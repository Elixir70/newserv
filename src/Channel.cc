#include "Channel.hh"

#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <phosg/Network.hh>
#include <phosg/Time.hh>

#include "Loggers.hh"
#include "Version.hh"

using namespace std;



extern bool use_terminal_colors;



static void flush_and_free_bufferevent(struct bufferevent* bev) {
  bufferevent_flush(bev, EV_READ | EV_WRITE, BEV_FINISHED);
  bufferevent_free(bev);
}



Channel::Channel(
    GameVersion version,
    on_command_received_t on_command_received,
    on_error_t on_error,
    void* context_obj,
    const string& name,
    TerminalFormat terminal_send_color,
    TerminalFormat terminal_recv_color)
  : bev(nullptr, flush_and_free_bufferevent),
    version(version),
    name(name),
    terminal_send_color(terminal_send_color),
    terminal_recv_color(terminal_recv_color),
    on_command_received(on_command_received),
    on_error(on_error),
    context_obj(context_obj) {
}

Channel::Channel(
    struct bufferevent* bev,
    GameVersion version,
    on_command_received_t on_command_received,
    on_error_t on_error,
    void* context_obj,
    const string& name,
    TerminalFormat terminal_send_color,
    TerminalFormat terminal_recv_color)
  : bev(nullptr, flush_and_free_bufferevent),
    version(version),
    name(name),
    terminal_send_color(terminal_send_color),
    terminal_recv_color(terminal_recv_color),
    on_command_received(on_command_received),
    on_error(on_error),
    context_obj(context_obj) {
  this->set_bufferevent(bev);
}

void Channel::replace_with(
    Channel&& other,
    on_command_received_t on_command_received,
    on_error_t on_error,
    void* context_obj,
    const std::string& name) {
  this->set_bufferevent(other.bev.release());
  this->local_addr = other.local_addr;
  this->remote_addr = other.remote_addr;
  this->is_virtual_connection = other.is_virtual_connection;
  this->version = other.version;
  this->crypt_in = other.crypt_in;
  this->crypt_out = other.crypt_out;
  this->name = name;
  this->terminal_send_color = other.terminal_send_color;
  this->terminal_recv_color = other.terminal_recv_color;
  this->on_command_received = on_command_received;
  this->on_error = on_error;
  this->context_obj = context_obj;
  other.disconnect(); // Clears crypts, addrs, etc.
}

void Channel::set_bufferevent(struct bufferevent* bev) {
  this->bev.reset(bev);

  if (this->bev.get()) {
    int fd = bufferevent_getfd(this->bev.get());
    if (fd < 0) {
      this->is_virtual_connection = true;
      memset(&this->local_addr, 0, sizeof(this->local_addr));
      memset(&this->remote_addr, 0, sizeof(this->remote_addr));
    } else {
      this->is_virtual_connection = false;
      get_socket_addresses(fd, &this->local_addr, &this->remote_addr);
    }

    bufferevent_setcb(this->bev.get(),
        &Channel::dispatch_on_input, nullptr,
        &Channel::dispatch_on_error, this);
    bufferevent_enable(this->bev.get(), EV_READ | EV_WRITE);

  } else {
    this->is_virtual_connection = false;
    memset(&this->local_addr, 0, sizeof(this->local_addr));
    memset(&this->remote_addr, 0, sizeof(this->remote_addr));
  }
}



void Channel::disconnect() {
  if (this->bev.get()) {
    // If the output buffer is not empty, move the bufferevent into the draining
    // pool instead of disconnecting it, to make sure all the data gets sent.
    struct evbuffer* out_buffer = bufferevent_get_output(this->bev.get());
    if (evbuffer_get_length(out_buffer) == 0) {
      this->bev.reset(); // Destructor flushes and frees the bufferevent
    } else {
      // The callbacks will free it when all the data is sent or the client
      // disconnects

      auto on_output = +[](struct bufferevent* bev, void*) -> void {
        flush_and_free_bufferevent(bev);
      };

      auto on_error = +[](struct bufferevent* bev, short events, void*) -> void {
        if (events & BEV_EVENT_ERROR) {
          int err = EVUTIL_SOCKET_ERROR();
          channel_exceptions_log.warning(
              "Disconnecting channel caused error %d (%s)", err,
              evutil_socket_error_to_string(err));
        }
        if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
          bufferevent_flush(bev, EV_WRITE, BEV_FINISHED);
          bufferevent_free(bev);
        }
      };

      struct bufferevent* bev = this->bev.release();
      bufferevent_setcb(bev, nullptr, on_output, on_error, bev);
      bufferevent_disable(bev, EV_READ);
    }
  }

  memset(&this->local_addr, 0, sizeof(this->local_addr));
  memset(&this->remote_addr, 0, sizeof(this->remote_addr));
  this->is_virtual_connection = false;
  this->crypt_in.reset();
  this->crypt_out.reset();
}



Channel::Message Channel::recv(bool print_contents) {
  struct evbuffer* buf = bufferevent_get_input(this->bev.get());

  size_t header_size = (this->version == GameVersion::BB) ? 8 : 4;
  PSOCommandHeader header;
  if (evbuffer_copyout(buf, &header, header_size)
      < static_cast<ssize_t>(header_size)) {
    throw out_of_range("no command available");
  }

  if (this->crypt_in.get()) {
    this->crypt_in->decrypt(&header, header_size, false);
  }

  size_t command_logical_size = header.size(version);

  // If encryption is enabled, BB pads commands to 8-byte boundaries, and this
  // is not reflected in the size field. This logic does not occur if encryption
  // is not yet enabled.
  size_t command_physical_size = (this->crypt_in.get() && (version == GameVersion::BB))
      ? ((command_logical_size + 7) & ~7) : command_logical_size;
  if (evbuffer_get_length(buf) < command_physical_size) {
    throw out_of_range("no command available");
  }

  // If we get here, then there is a full command in the buffer. Some encryption
  // algorithms' advancement depends on the decrypted data, so we have to
  // actually decrypt the header again (with advance=true) to keep them in a
  // consistent state.

  string header_data(header_size, '\0');
  if (evbuffer_remove(buf, header_data.data(), header_data.size())
      < static_cast<ssize_t>(header_data.size())) {
    throw logic_error("enough bytes available, but could not remove them");
  }
  if (this->crypt_in.get()) {
    this->crypt_in->decrypt(header_data.data(), header_data.size());
  }

  string command_data(command_physical_size - header_size, '\0');
  if (evbuffer_remove(buf, command_data.data(), command_data.size())
      < static_cast<ssize_t>(command_data.size())) {
    throw logic_error("enough bytes available, but could not remove them");
  }
  if (this->crypt_in.get()) {
    this->crypt_in->decrypt(command_data.data(), command_data.size());
  }
  command_data.resize(command_logical_size - header_size);

  if (print_contents && (this->terminal_recv_color != TerminalFormat::END)) {
    if (use_terminal_colors && this->terminal_recv_color != TerminalFormat::NORMAL) {
      print_color_escape(stderr, this->terminal_recv_color, TerminalFormat::BOLD, TerminalFormat::END);
    }

    string name_token;
    if (!this->name.empty()) {
      name_token = " from " + this->name;
    }
    command_data_log.info("Received%s (version=%s command=%04hX flag=%08X)",
        name_token.c_str(),
        name_for_version(this->version),
        header.command(this->version),
        header.flag(this->version));

    vector<struct iovec> iovs;
    iovs.emplace_back(iovec{.iov_base = header_data.data(), .iov_len = header_data.size()});
    iovs.emplace_back(iovec{.iov_base = command_data.data(), .iov_len = command_data.size()});
    print_data(stderr, iovs, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::DISABLE_COLOR);

    if (use_terminal_colors && this->terminal_recv_color != TerminalFormat::NORMAL) {
      print_color_escape(stderr, TerminalFormat::NORMAL, TerminalFormat::END);
    }
  }

  return {
    .command = header.command(this->version),
    .flag = header.flag(this->version),
    .data = move(command_data),
  };
}

void Channel::send(uint16_t cmd, uint32_t flag, const void* data, size_t size,
    bool print_contents) {
  if (!this->connected()) {
    channel_exceptions_log.warning("Attempted to send command on closed channel; dropping data");
  }

  string send_data;
  size_t logical_size;
  size_t send_data_size = 0;
  switch (this->version) {
    case GameVersion::GC:
    case GameVersion::DC: {
      PSOCommandHeaderDCGC header;
      if (this->crypt_out.get()) {
        send_data_size = (sizeof(header) + size + 3) & ~3;
      } else {
        send_data_size = (sizeof(header) + size);
      }
      logical_size = send_data_size;
      header.command = cmd;
      header.flag = flag;
      header.size = send_data_size;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      break;
    }

    case GameVersion::PC:
    case GameVersion::PATCH: {
      PSOCommandHeaderPC header;
      if (this->crypt_out.get()) {
        send_data_size = (sizeof(header) + size + 3) & ~3;
      } else {
        send_data_size = (sizeof(header) + size);
      }
      logical_size = send_data_size;
      header.size = send_data_size;
      header.command = cmd;
      header.flag = flag;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      break;
    }

    case GameVersion::BB: {
      // BB has an annoying behavior here: command lengths must be multiples of
      // 4, but the actual data length must be a multiple of 8. If the size
      // field is not divisible by 8, 4 extra bytes are sent anyway. This
      // behavior only applies when encryption is enabled - any commands sent
      // before encryption is enabled have no size restrictions (except they
      // must include a full header and must fit in the client's receive
      // buffer), and no implicit extra bytes are sent.
      PSOCommandHeaderBB header;
      if (this->crypt_out.get()) {
        send_data_size = (sizeof(header) + size + 7) & ~7;
      } else {
        send_data_size = (sizeof(header) + size);
      }
      logical_size = (sizeof(header) + size + 3) & ~3;
      header.size = logical_size;
      header.command = cmd;
      header.flag = flag;
      send_data.append(reinterpret_cast<const char*>(&header), sizeof(header));
      break;
    }

    default:
      throw logic_error("unimplemented game version in send_command");
  }

  // All versions of PSO I've seen (PC, GC, BB) have a receive buffer 0x7C00
  // bytes in size
  if (send_data_size > 0x7C00) {
    throw runtime_error("outbound command too large");
  }

  if (send_data.size() < send_data_size) {
    send_data.append(reinterpret_cast<const char*>(data), size);
    send_data.resize(send_data_size, '\0');
  }

  if (print_contents && (this->terminal_send_color != TerminalFormat::END)) {
    string name_token;
    if (!this->name.empty()) {
      name_token = " to " + this->name;
    }
    if (use_terminal_colors && this->terminal_send_color != TerminalFormat::NORMAL) {
      print_color_escape(stderr, TerminalFormat::FG_YELLOW, TerminalFormat::BOLD, TerminalFormat::END);
    }
    command_data_log.info("Sending%s (version=%s command=%04hX flag=%08X)",
        name_token.c_str(), name_for_version(version), cmd, flag);
    print_data(stderr, send_data.data(), logical_size, 0, nullptr, PrintDataFlags::PRINT_ASCII | PrintDataFlags::DISABLE_COLOR);
    if (use_terminal_colors && this->terminal_send_color != TerminalFormat::NORMAL) {
      print_color_escape(stderr, TerminalFormat::NORMAL, TerminalFormat::END);
    }
  }

  if (this->crypt_out.get()) {
    this->crypt_out->encrypt(send_data.data(), send_data.size());
  }

  struct evbuffer* buf = bufferevent_get_output(this->bev.get());
  evbuffer_add(buf, send_data.data(), send_data.size());
}

void Channel::send(uint16_t cmd, uint32_t flag, const string& data, bool print_contents) {
  this->send(cmd, flag, data.data(), data.size(), print_contents);
}

void Channel::send(const void* data, size_t size, bool print_contents) {
  size_t header_size = (this->version == GameVersion::BB) ? 8 : 4;
  const auto* header = reinterpret_cast<const PSOCommandHeader*>(data);
  this->send(
      header->command(this->version),
      header->flag(this->version),
      reinterpret_cast<const uint8_t*>(data) + header_size,
      size - header_size,
      print_contents);
}

void Channel::send(const string& data, bool print_contents) {
  return this->send(data.data(), data.size(), print_contents);
}



void Channel::dispatch_on_input(struct bufferevent*, void* ctx) {
  Channel* ch = reinterpret_cast<Channel*>(ctx);
  // The client can be disconnected during on_command_received, so we have to
  // make sure ch->bev is valid every time before calling recv()
  while (ch->bev.get()) {
    Message msg;
    try {
      msg = ch->recv();
    } catch (const out_of_range&) {
      break;
    } catch (const exception& e) {
      channel_exceptions_log.warning("Error receiving on channel: %s", e.what());
      ch->on_error(*ch, BEV_EVENT_ERROR);
      break;
    }
    if (ch->on_command_received) {
      ch->on_command_received(*ch, msg.command, msg.flag, msg.data);
    }
  }
}

void Channel::dispatch_on_error(struct bufferevent*, short events, void* ctx) {
  Channel* ch = reinterpret_cast<Channel*>(ctx);
  if (ch->on_error) {
    ch->on_error(*ch, events);
  } else {
    ch->disconnect();
  }
}
