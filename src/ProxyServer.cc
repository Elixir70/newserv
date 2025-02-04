#include "ProxyServer.hh"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>
#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <phosg/Hash.hh>
#include <phosg/Network.hh>
#include <phosg/Random.hh>
#include <phosg/Strings.hh>
#include <phosg/Time.hh>

#include "IPStackSimulator.hh"
#include "Loggers.hh"
#include "NetworkAddresses.hh"
#include "PSOProtocol.hh"
#include "ProxyCommands.hh"
#include "ReceiveCommands.hh"
#include "ReceiveSubcommands.hh"
#include "SendCommands.hh"

using namespace std;
using namespace std::placeholders;

ProxyServer::ProxyServer(
    shared_ptr<struct event_base> base,
    shared_ptr<ServerState> state)
    : base(base),
      destroy_sessions_ev(event_new(this->base.get(), -1, EV_TIMEOUT, &ProxyServer::dispatch_destroy_sessions, this), event_free),
      state(state),
      next_unlicensed_session_id(0xFF00000000000001) {}

void ProxyServer::listen(const std::string& addr, uint16_t port, Version version, const struct sockaddr_storage* default_destination) {
  auto socket_obj = make_shared<ListeningSocket>(this, addr, port, version, default_destination);
  if (!this->listeners.emplace(port, socket_obj).second) {
    throw runtime_error("duplicate port in proxy server configuration");
  }
}

ProxyServer::ListeningSocket::ListeningSocket(
    ProxyServer* server,
    const std::string& addr,
    uint16_t port,
    Version version,
    const struct sockaddr_storage* default_destination)
    : server(server),
      log(string_printf("[ProxyServer:ListeningSocket:%hu] ", port), proxy_server_log.min_level),
      port(port),
      fd(::listen(addr, port, SOMAXCONN)),
      listener(nullptr, evconnlistener_free),
      version(version) {
  if (!this->fd.is_open()) {
    throw runtime_error("cannot listen on port");
  }
  this->listener.reset(evconnlistener_new(
      this->server->base.get(),
      &ProxyServer::ListeningSocket::dispatch_on_listen_accept,
      this,
      LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
      0,
      this->fd));
  if (!listener) {
    throw runtime_error("cannot create listener");
  }
  evconnlistener_set_error_cb(
      this->listener.get(), &ProxyServer::ListeningSocket::dispatch_on_listen_error);

  if (default_destination) {
    this->default_destination = *default_destination;
  } else {
    this->default_destination.ss_family = 0;
  }

  this->log.info("Listening on TCP port %hu (%s) on fd %d", this->port, name_for_enum(this->version), static_cast<int>(this->fd));
}

void ProxyServer::ListeningSocket::dispatch_on_listen_accept(
    struct evconnlistener*, evutil_socket_t fd, struct sockaddr*, int, void* ctx) {
  reinterpret_cast<ListeningSocket*>(ctx)->on_listen_accept(fd);
}

void ProxyServer::ListeningSocket::dispatch_on_listen_error(
    struct evconnlistener*, void* ctx) {
  reinterpret_cast<ListeningSocket*>(ctx)->on_listen_error();
}

void ProxyServer::ListeningSocket::on_listen_accept(int fd) {
  this->log.info("Client connected on fd %d (port %hu, version %s)", fd, this->port, name_for_enum(this->version));
  auto* bev = bufferevent_socket_new(this->server->base.get(), fd, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS);
  this->server->on_client_connect(bev, this->port, this->version,
      (this->default_destination.ss_family == AF_INET) ? &this->default_destination : nullptr);
}

void ProxyServer::ListeningSocket::on_listen_error() {
  int err = EVUTIL_SOCKET_ERROR();
  this->log.error("Failure on listening socket %d: %d (%s)",
      evconnlistener_get_fd(this->listener.get()),
      err, evutil_socket_error_to_string(err));
  event_base_loopexit(this->server->base.get(), nullptr);
}

void ProxyServer::connect_client(struct bufferevent* bev, uint16_t server_port) {
  // Look up the listening socket for the given port, and use that game version.
  // We don't support default-destination proxying for virtual connections (yet)
  Version version;
  try {
    version = this->listeners.at(server_port)->version;
  } catch (const out_of_range&) {
    proxy_server_log.info("Virtual connection received on unregistered port %hu; closing it",
        server_port);
    bufferevent_flush(bev, EV_READ | EV_WRITE, BEV_FINISHED);
    bufferevent_free(bev);
    return;
  }

  proxy_server_log.info("Client connected on virtual connection %p (port %hu)", bev,
      server_port);
  this->on_client_connect(bev, server_port, version, nullptr);
}

void ProxyServer::on_client_connect(
    struct bufferevent* bev,
    uint16_t listen_port,
    Version version,
    const struct sockaddr_storage* default_destination) {
  // If a default destination exists for this client and the client is a patch
  // client, create a linked session immediately and connect to the remote
  // server. This creates a direct session.
  if (default_destination && is_patch(version)) {
    uint64_t session_id = this->next_unlicensed_session_id++;
    if (this->next_unlicensed_session_id == 0) {
      this->next_unlicensed_session_id = 0xFF00000000000001;
    }

    auto emplace_ret = this->id_to_session.emplace(session_id, make_shared<LinkedSession>(this->shared_from_this(), session_id, listen_port, version, *default_destination));
    if (!emplace_ret.second) {
      throw logic_error("linked session already exists for unlicensed client");
    }
    auto ses = emplace_ret.first->second;
    ses->log.info("Opened linked session");

    Channel ch(bev, version, 1, nullptr, nullptr, ses.get(), "", TerminalFormat::FG_YELLOW, TerminalFormat::FG_GREEN);
    ses->resume(std::move(ch));

    // If no default destination exists, or the client is not a patch client,
    // create an unlinked session - we'll have to get the destination from the
    // client's config, which we'll get via a 9E command soon.
  } else {
    auto emplace_ret = this->bev_to_unlinked_session.emplace(bev, make_shared<UnlinkedSession>(this->shared_from_this(), bev, listen_port, version));
    if (!emplace_ret.second) {
      throw logic_error("stale unlinked session exists");
    }
    auto ses = emplace_ret.first->second;
    proxy_server_log.info("Opened unlinked session");

    // Note that this should only be set when the linked session is created, not
    // when it is resumed!
    if (default_destination) {
      ses->next_destination = *default_destination;
    }

    switch (version) {
      case Version::PC_PATCH:
      case Version::BB_PATCH:
        throw logic_error("cannot create unlinked patch session");
      case Version::DC_NTE:
      case Version::DC_V1_11_2000_PROTOTYPE:
      case Version::DC_V1:
      case Version::DC_V2:
      case Version::PC_NTE:
      case Version::PC_V2:
      case Version::GC_NTE:
      case Version::GC_V3:
      case Version::GC_EP3_NTE:
      case Version::GC_EP3:
      case Version::XB_V3: {
        uint32_t server_key = random_object<uint32_t>();
        uint32_t client_key = random_object<uint32_t>();
        auto cmd = prepare_server_init_contents_console(server_key, client_key, 0);
        ses->channel.send(0x02, 0x00, &cmd, sizeof(cmd));
        if (uses_v2_encryption(version)) {
          ses->channel.crypt_out = make_shared<PSOV2Encryption>(server_key);
          ses->channel.crypt_in = make_shared<PSOV2Encryption>(client_key);
        } else {
          ses->channel.crypt_out = make_shared<PSOV3Encryption>(server_key);
          ses->channel.crypt_in = make_shared<PSOV3Encryption>(client_key);
        }
        break;
      }
      case Version::BB_V4: {
        parray<uint8_t, 0x30> server_key;
        parray<uint8_t, 0x30> client_key;
        random_data(server_key.data(), server_key.bytes());
        random_data(client_key.data(), client_key.bytes());
        auto cmd = prepare_server_init_contents_bb(server_key, client_key, 0);
        ses->channel.send(0x03, 0x00, &cmd, sizeof(cmd));
        ses->detector_crypt = make_shared<PSOBBMultiKeyDetectorEncryption>(
            this->state->bb_private_keys,
            bb_crypt_initial_client_commands,
            cmd.basic_cmd.client_key.data(),
            sizeof(cmd.basic_cmd.client_key));
        ses->channel.crypt_in = ses->detector_crypt;
        ses->channel.crypt_out = make_shared<PSOBBMultiKeyImitatorEncryption>(
            ses->detector_crypt,
            cmd.basic_cmd.server_key.data(),
            sizeof(cmd.basic_cmd.server_key),
            true);
        break;
      }
      default:
        throw logic_error("unsupported game version on proxy server");
    }
  }
}

ProxyServer::UnlinkedSession::UnlinkedSession(
    shared_ptr<ProxyServer> server,
    struct bufferevent* bev,
    uint16_t local_port,
    Version version)
    : server(server),
      log(string_printf("[ProxyServer:UnlinkedSession:%p] ", bev), proxy_server_log.min_level),
      channel(
          bev,
          version,
          1,
          ProxyServer::UnlinkedSession::on_input,
          ProxyServer::UnlinkedSession::on_error,
          this,
          string_printf("UnlinkedSession:%p", bev),
          TerminalFormat::FG_YELLOW,
          TerminalFormat::FG_GREEN),
      local_port(local_port) {
  memset(&this->next_destination, 0, sizeof(this->next_destination));
}

std::shared_ptr<ProxyServer> ProxyServer::UnlinkedSession::require_server() const {
  auto server = this->server.lock();
  if (!server) {
    throw logic_error("server is deleted");
  }
  return server;
}

std::shared_ptr<ServerState> ProxyServer::UnlinkedSession::require_server_state() const {
  return this->require_server()->state;
}

void ProxyServer::UnlinkedSession::on_input(Channel& ch, uint16_t command, uint32_t, std::string& data) {
  auto* ses = reinterpret_cast<UnlinkedSession*>(ch.context_obj);
  auto server = ses->require_server();
  auto s = server->state;

  bool should_close_unlinked_session = false;

  try {
    switch (ses->version()) {
      case Version::DC_NTE:
      case Version::DC_V1_11_2000_PROTOTYPE:
      case Version::DC_V1:
      case Version::DC_V2:
      case Version::GC_NTE:
        // We should only get an 8B, 93 or 9D while the session is unlinked
        if (command == 0x8B) {
          ses->channel.version = Version::DC_NTE;
          ses->log.info("Version changed to DC_NTE");
          const auto& cmd = check_size_t<C_Login_DCNTE_8B>(data, sizeof(C_LoginExtended_DCNTE_8B));
          ses->license = s->license_index->verify_v1_v2(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
          ses->sub_version = cmd.sub_version;
          ses->channel.language = cmd.language;
          ses->character_name = cmd.name.decode(ses->channel.language);
          // TODO: Parse cmd.hardware_id
        } else if (command == 0x93) { // 11/2000 proto through DC V1
          ses->channel.version = Version::DC_V1;
          ses->log.info("Version changed to DC_V1");
          const auto& cmd = check_size_t<C_LoginV1_DC_93>(data);
          ses->license = s->license_index->verify_v1_v2(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
          ses->sub_version = cmd.sub_version;
          ses->channel.language = cmd.language;
          ses->character_name = cmd.name.decode(ses->channel.language);
          ses->hardware_id = cmd.hardware_id.decode();
        } else if (command == 0x9D) {
          const auto& cmd = check_size_t<C_Login_DC_PC_GC_9D>(data, sizeof(C_LoginExtended_DC_GC_9D));
          if (cmd.sub_version >= 0x30) {
            ses->log.info("Version changed to GC_NTE");
            ses->channel.version = Version::GC_NTE;
            ses->license = s->license_index->verify_gc(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
          } else { // DC V2
            ses->log.info("Version changed to DC_V2");
            ses->channel.version = Version::DC_V2;
            ses->license = s->license_index->verify_v1_v2(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
          }
          ses->sub_version = cmd.sub_version;
          ses->channel.language = cmd.language;
          ses->character_name = cmd.name.decode(ses->channel.language);
          ses->config.set_flags_for_version(ses->version(), cmd.sub_version);
        } else {
          throw runtime_error("command is not 93 or 9D");
        }
        break;

      case Version::PC_NTE:
      case Version::PC_V2: {
        // We should only get a 9D while the session is unlinked
        if (command != 0x9D) {
          throw runtime_error("command is not 9D");
        }
        const auto& cmd = check_size_t<C_Login_DC_PC_GC_9D>(data, sizeof(C_LoginExtended_PC_9D));
        ses->license = s->license_index->verify_v1_v2(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
        ses->sub_version = cmd.sub_version;
        ses->channel.language = cmd.language;
        ses->character_name = cmd.name.decode(ses->channel.language);
        break;
      }

      case Version::GC_V3:
      case Version::GC_EP3_NTE:
      case Version::GC_EP3:
        // We should only get a 9E while the session is unlinked
        if (command == 0x9E) {
          const auto& cmd = check_size_t<C_Login_GC_9E>(data, sizeof(C_LoginExtended_GC_9E));
          ses->license = s->license_index->verify_gc(stoul(cmd.serial_number.decode(), nullptr, 16), cmd.access_key.decode());
          ses->sub_version = cmd.sub_version;
          ses->channel.language = cmd.language;
          ses->character_name = cmd.name.decode(ses->channel.language);
          ses->config.parse_from(cmd.client_config);
          if (cmd.sub_version >= 0x40) {
            ses->log.info("Version changed to GC_EP3");
            ses->channel.version = Version::GC_EP3;
          }
        } else {
          throw runtime_error("command is not 9D or 9E");
        }
        break;

      case Version::XB_V3:
        // We should only get a 9E or 9F while the session is unlinked
        if (command == 0x9E) {
          const auto& cmd = check_size_t<C_Login_XB_9E>(data, sizeof(C_LoginExtended_XB_9E));
          string xb_gamertag = cmd.serial_number.decode();
          uint64_t xb_user_id = stoull(cmd.access_key.decode(), nullptr, 16);
          uint64_t xb_account_id = cmd.netloc.account_id;
          ses->license = s->license_index->verify_xb(xb_gamertag, xb_user_id, xb_account_id);
          ses->sub_version = cmd.sub_version;
          ses->channel.language = cmd.language;
          ses->character_name = cmd.name.decode(ses->channel.language);
          ses->xb_netloc = cmd.netloc;
          ses->xb_9E_unknown_a1a = cmd.unknown_a1a;
          ses->channel.send(0x9F, 0x00);
          return;
        } else if (command == 0x9F) {
          const auto& cmd = check_size_t<C_ClientConfig_V3_9F>(data);
          ses->config.parse_from(cmd.data);
        } else {
          throw runtime_error("command is not 9E or 9F");
        }
        break;

      case Version::BB_V4: {
        // We should only get a 93 while the session is unlinked; if we get
        // anything else, disconnect
        if (command != 0x93) {
          throw runtime_error("command is not 93");
        }
        const auto& cmd = check_size_t<C_LoginBase_BB_93>(data, 0xFFFF);
        try {
          ses->license = s->license_index->verify_bb(cmd.username.decode(), cmd.password.decode());
        } catch (const LicenseIndex::missing_license&) {
          if (!s->allow_unregistered_users) {
            throw;
          }
          auto l = s->license_index->create_license();
          l->serial_number = fnv1a32(cmd.username.decode()) & 0x7FFFFFFF;
          l->bb_username = cmd.username.decode();
          l->bb_password = cmd.password.decode();
          s->license_index->add(l);
          l->save();
          ses->license = l;
          string l_str = l->str();
          ses->log.info("Created license %s", l_str.c_str());
        }
        ses->login_command_bb = std::move(data);
        break;
      }

      default:
        throw runtime_error("unvalid unlinked session version");
    }
  } catch (const exception& e) {
    ses->log.error("Failed to process command from unlinked client: %s", e.what());
    should_close_unlinked_session = true;
  }

  // Note that ch.bev will be moved from when the linked session is resumed, so
  // we need to retain a copy of it in order to close the unlinked session
  // afterward.
  struct bufferevent* session_key = ch.bev.get();

  // If license is non-null, then the client has a password and can be connected
  // to the remote lobby server.
  if (ses->license.get()) {
    // At this point, we will always close the unlinked session, even if it
    // doesn't get converted/merged to a linked session
    should_close_unlinked_session = true;

    // Look up the linked session for this license (if any)
    shared_ptr<LinkedSession> linked_ses;
    try {
      linked_ses = server->id_to_session.at(ses->license->serial_number);
      linked_ses->log.info("Resuming linked session from unlinked session");

    } catch (const out_of_range&) {
      // If there's no open session for this license, then there must be a valid
      // destination somewhere - either in the client config or in the unlinked
      // session
      if (ses->config.proxy_destination_address != 0) {
        linked_ses = make_shared<LinkedSession>(server, ses->local_port, ses->version(), ses->license, ses->config);
        linked_ses->log.info("Opened licensed session for unlinked session based on client config");
      } else if (ses->next_destination.ss_family == AF_INET) {
        linked_ses = make_shared<LinkedSession>(server, ses->local_port, ses->version(), ses->license, ses->next_destination);
        linked_ses->log.info("Opened licensed session for unlinked session based on unlinked default destination");
      } else {
        ses->log.error("Cannot open linked session: no valid destination in client config or unlinked session");
      }
    }

    if (linked_ses.get()) {
      server->id_to_session.emplace(ses->license->serial_number, linked_ses);
      // Resume the linked session using the unlinked session
      try {
        if (ses->version() == Version::BB_V4) {
          linked_ses->resume(
              std::move(ses->channel),
              ses->detector_crypt,
              std::move(ses->login_command_bb));
        } else {
          linked_ses->resume(
              std::move(ses->channel),
              ses->detector_crypt,
              ses->sub_version,
              ses->character_name,
              ses->hardware_id,
              ses->xb_netloc,
              ses->xb_9E_unknown_a1a);
        }
      } catch (const exception& e) {
        linked_ses->log.error("Failed to resume linked session: %s", e.what());
      }
    }
  }

  if (should_close_unlinked_session) {
    server->delete_session(session_key);
  }
}

void ProxyServer::UnlinkedSession::on_error(Channel& ch, short events) {
  auto* ses = reinterpret_cast<UnlinkedSession*>(ch.context_obj);

  if (events & BEV_EVENT_ERROR) {
    int err = EVUTIL_SOCKET_ERROR();
    ses->log.warning("Error %d (%s) in unlinked client stream", err,
        evutil_socket_error_to_string(err));
  }
  if (events & (BEV_EVENT_ERROR | BEV_EVENT_EOF)) {
    ses->log.info("Client has disconnected");
    ses->require_server()->delete_session(ses->channel.bev.get());
  }
}

ProxyServer::LinkedSession::LinkedSession(
    shared_ptr<ProxyServer> server,
    uint64_t id,
    uint16_t local_port,
    Version version)
    : server(server),
      id(id),
      log(string_printf("[ProxyServer:LinkedSession:%08" PRIX64 "] ", this->id), proxy_server_log.min_level),
      timeout_event(event_new(server->base.get(), -1, EV_TIMEOUT, &LinkedSession::dispatch_on_timeout, this), event_free),
      license(nullptr),
      client_channel(
          version,
          1,
          nullptr,
          nullptr,
          this,
          string_printf("LinkedSession:%08" PRIX64 ":client", this->id),
          TerminalFormat::FG_YELLOW,
          TerminalFormat::FG_GREEN),
      server_channel(
          version,
          1,
          nullptr,
          nullptr,
          this,
          string_printf("LinkedSession:%08" PRIX64 ":server", this->id),
          TerminalFormat::FG_YELLOW,
          TerminalFormat::FG_RED),
      local_port(local_port),
      disconnect_action(DisconnectAction::LONG_TIMEOUT),
      remote_ip_crc(0),
      enable_remote_ip_crc_patch(false),
      sub_version(0), // This is set during resume()
      remote_guild_card_number(-1),
      next_item_id(0x0F000000),
      lobby_players(12),
      lobby_client_id(0),
      leader_client_id(0),
      floor(0),
      x(0.0),
      z(0.0),
      is_in_game(false),
      is_in_quest(false) {
  this->last_switch_enabled_command.header.subcommand = 0;
  memset(this->prev_server_command_bytes, 0, sizeof(this->prev_server_command_bytes));
}

ProxyServer::LinkedSession::LinkedSession(
    shared_ptr<ProxyServer> server,
    uint16_t local_port,
    Version version,
    shared_ptr<License> license,
    const Client::Config& config)
    : LinkedSession(server, license->serial_number, local_port, version) {
  this->license = license;
  this->config = config;
  memset(&this->next_destination, 0, sizeof(this->next_destination));
  struct sockaddr_in* dest_sin = reinterpret_cast<struct sockaddr_in*>(&this->next_destination);
  dest_sin->sin_family = AF_INET;
  dest_sin->sin_port = htons(this->config.proxy_destination_port);
  dest_sin->sin_addr.s_addr = htonl(this->config.proxy_destination_address);
}

ProxyServer::LinkedSession::LinkedSession(
    shared_ptr<ProxyServer> server,
    uint16_t local_port,
    Version version,
    std::shared_ptr<License> license,
    const struct sockaddr_storage& next_destination)
    : LinkedSession(server, license->serial_number, local_port, version) {
  this->license = license;
  this->next_destination = next_destination;
}

ProxyServer::LinkedSession::LinkedSession(
    shared_ptr<ProxyServer> server,
    uint64_t id,
    uint16_t local_port,
    Version version,
    const struct sockaddr_storage& destination)
    : LinkedSession(server, id, local_port, version) {
  this->next_destination = destination;
}

shared_ptr<ProxyServer> ProxyServer::LinkedSession::require_server() const {
  auto server = this->server.lock();
  if (!server) {
    throw logic_error("server is deleted");
  }
  return server;
}

std::shared_ptr<ServerState> ProxyServer::LinkedSession::require_server_state() const {
  return this->require_server()->state;
}

void ProxyServer::LinkedSession::set_version(Version v) {
  this->client_channel.version = v;
  this->server_channel.version = v;
}

void ProxyServer::LinkedSession::resume(
    Channel&& client_channel,
    shared_ptr<PSOBBMultiKeyDetectorEncryption> detector_crypt,
    uint32_t sub_version,
    const string& character_name,
    const string& hardware_id,
    const XBNetworkLocation& xb_netloc,
    const parray<le_uint32_t, 3>& xb_9E_unknown_a1a) {
  this->sub_version = sub_version;
  this->character_name = character_name;
  this->hardware_id = hardware_id;
  this->xb_netloc = xb_netloc;
  this->xb_9E_unknown_a1a = xb_9E_unknown_a1a;
  this->resume_inner(std::move(client_channel), detector_crypt);
}

void ProxyServer::LinkedSession::resume(
    Channel&& client_channel,
    shared_ptr<PSOBBMultiKeyDetectorEncryption> detector_crypt,
    string&& login_command_bb) {
  this->login_command_bb = std::move(login_command_bb);
  this->resume_inner(std::move(client_channel), detector_crypt);
}

void ProxyServer::LinkedSession::resume(Channel&& client_channel) {
  this->sub_version = 0;
  this->character_name.clear();
  this->resume_inner(std::move(client_channel), nullptr);
}

void ProxyServer::LinkedSession::resume_inner(
    Channel&& client_channel,
    shared_ptr<PSOBBMultiKeyDetectorEncryption> detector_crypt) {
  if (this->client_channel.connected()) {
    throw runtime_error("client connection is already open for this session");
  }
  if (this->next_destination.ss_family != AF_INET) {
    throw logic_error("attempted to resume an unlicensed linked session without destination set");
  }

  this->client_channel.replace_with(
      std::move(client_channel),
      ProxyServer::LinkedSession::on_input,
      ProxyServer::LinkedSession::on_error,
      this,
      string_printf("LinkedSession:%08" PRIX64 ":client", this->id));
  this->server_channel.language = this->client_channel.language;
  this->server_channel.version = this->client_channel.version;

  this->detector_crypt = detector_crypt;
  this->server_channel.disconnect();
  this->saving_files.clear();

  this->connect();
}

void ProxyServer::LinkedSession::connect() {
  // Connect to the remote server. The command handlers will do the login steps
  // and set up forwarding
  const struct sockaddr_in* dest_sin = reinterpret_cast<const sockaddr_in*>(
      &this->next_destination);
  if (dest_sin->sin_family != AF_INET) {
    throw runtime_error("destination is not AF_INET");
  }

  string netloc_str = render_sockaddr_storage(this->next_destination);
  this->log.info("Connecting to %s", netloc_str.c_str());

  this->server_channel.set_bufferevent(bufferevent_socket_new(
      this->require_server()->base.get(), -1, BEV_OPT_CLOSE_ON_FREE | BEV_OPT_DEFER_CALLBACKS));
  if (bufferevent_socket_connect(this->server_channel.bev.get(),
          reinterpret_cast<const sockaddr*>(dest_sin), sizeof(*dest_sin)) != 0) {
    throw runtime_error(string_printf("failed to connect (%d)", EVUTIL_SOCKET_ERROR()));
  }

  this->server_channel.on_command_received = ProxyServer::LinkedSession::on_input;
  this->server_channel.on_error = ProxyServer::LinkedSession::on_error;
  this->server_channel.context_obj = this;

  // Cancel the session delete timeout
  event_del(this->timeout_event.get());
}

ProxyServer::LinkedSession::SavingFile::SavingFile(
    const string& basename,
    const string& output_filename,
    size_t remaining_bytes,
    bool is_download)
    : basename(basename),
      output_filename(output_filename),
      is_download(is_download),
      remaining_bytes(remaining_bytes) {}

void ProxyServer::LinkedSession::SavingFile::write() const {
  string data = join(this->blocks);
  if (is_download && (ends_with(this->basename, ".bin") || ends_with(this->basename, ".dat"))) {
    data = decode_dlq_data(data);
  }
  save_file(this->output_filename, data);
}

void ProxyServer::LinkedSession::dispatch_on_timeout(
    evutil_socket_t, short, void* ctx) {
  reinterpret_cast<LinkedSession*>(ctx)->on_timeout();
}

void ProxyServer::LinkedSession::on_timeout() {
  this->require_server()->delete_session(this->id);
}

void ProxyServer::LinkedSession::on_error(Channel& ch, short events) {
  auto* ses = reinterpret_cast<LinkedSession*>(ch.context_obj);
  bool is_server_stream = (&ch == &ses->server_channel);

  if (events & BEV_EVENT_CONNECTED) {
    ses->log.info("%s channel connected", is_server_stream ? "Server" : "Client");

    if (is_server_stream && (ses->config.override_lobby_event != 0xFF) && (is_v3(ses->version()) || is_v4(ses->version()))) {
      ses->client_channel.send(0xDA, ses->config.override_lobby_event);
    }
  }
  if (events & BEV_EVENT_ERROR) {
    int err = EVUTIL_SOCKET_ERROR();
    ses->log.warning("Error %d (%s) in %s stream",
        err, evutil_socket_error_to_string(err),
        is_server_stream ? "server" : "client");
  }
  if (events & (BEV_EVENT_EOF | BEV_EVENT_ERROR)) {
    ses->log.info("%s has disconnected",
        is_server_stream ? "Server" : "Client");
    // If the server disconnected, send the client back to the game server so
    // they're not disconnected completely.
    if (is_server_stream) {
      ses->send_to_game_server("The server has\ndisconnected.");
    }
    ses->disconnect();
  }
}

void ProxyServer::LinkedSession::clear_lobby_players(size_t num_slots) {
  this->lobby_players.clear();
  this->lobby_players.resize(num_slots);
  this->log.info("Cleared lobby players");
}

void ProxyServer::LinkedSession::send_to_game_server(const char* error_message) {
  // If there is no license, do nothing - we can't return to the game server
  // from unlicensed sessions
  if (!this->license) {
    this->disconnect();
    return;
  }
  // On BB, do nothing - we can't return to the game server since the remote
  // server likely sent different game data than what newserv would have sent
  if (this->version() == Version::BB_V4) {
    this->disconnect();
    return;
  }

  // Delete all the other players
  for (size_t x = 0; x < this->lobby_players.size(); x++) {
    if (this->lobby_players[x].guild_card_number == 0) {
      continue;
    }
    uint8_t leaving_id = x;
    uint8_t leader_id = this->lobby_client_id;
    S_LeaveLobby_66_69_Ep3_E9 cmd = {leaving_id, leader_id, 1, 0};
    this->client_channel.send(this->is_in_game ? 0x66 : 0x69, leaving_id, &cmd, sizeof(cmd));
  }

  auto s = this->require_server_state();
  if (this->is_in_game) {
    send_ship_info(this->client_channel, string_printf("You cannot return\nto $C6%s$C7\nwhile in a game.\n\n%s", s->name.c_str(), error_message ? error_message : ""));
    this->disconnect();

  } else {
    send_ship_info(this->client_channel, string_printf("You\'ve returned to\n\tC6%s$C7\n\n%s", s->name.c_str(), error_message ? error_message : ""));

    // Restore newserv_client_config, so the login server gets the client flags
    if (is_v3(this->version())) {
      S_UpdateClientConfig_V3_04 update_client_config_cmd;
      update_client_config_cmd.player_tag = 0x00010000;
      update_client_config_cmd.guild_card_number = this->license->serial_number;
      this->config.serialize_into(update_client_config_cmd.client_config);
      this->client_channel.send(0x04, 0x00, &update_client_config_cmd, sizeof(update_client_config_cmd));
    }

    string port_name = login_port_name_for_version(this->version());
    S_Reconnect_19 reconnect_cmd = {{0, s->name_to_port_config.at(port_name)->port, 0}};

    // If the client is on a virtual connection, we can use any address
    // here and they should be able to connect back to the game server
    if (this->client_channel.is_virtual_connection) {
      struct sockaddr_in* dest_sin = reinterpret_cast<struct sockaddr_in*>(&this->next_destination);
      if (dest_sin->sin_family != AF_INET) {
        throw logic_error("ss not AF_INET");
      }
      reconnect_cmd.address = IPStackSimulator::connect_address_for_remote_address(ntohl(dest_sin->sin_addr.s_addr));
    } else {
      const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(&this->client_channel.remote_addr);
      if (sin->sin_family != AF_INET) {
        throw logic_error("existing connection is not ipv4");
      }
      reconnect_cmd.address = is_local_address(ntohl(sin->sin_addr.s_addr)) ? s->local_address : s->external_address;
    }

    this->client_channel.send(0x19, 0x00, &reconnect_cmd, sizeof(reconnect_cmd));
    this->disconnect_action = DisconnectAction::CLOSE_IMMEDIATELY;
  }
}

uint64_t ProxyServer::LinkedSession::timeout_for_disconnect_action(
    DisconnectAction action) {
  switch (action) {
    case DisconnectAction::LONG_TIMEOUT:
      return 5 * 60 * 1000 * 1000; // 5 minutes
    case DisconnectAction::MEDIUM_TIMEOUT:
      return 30 * 1000 * 1000; // 30 seconds
    case DisconnectAction::SHORT_TIMEOUT:
      return 10 * 1000 * 1000; // 10 seconds
    case DisconnectAction::CLOSE_IMMEDIATELY:
      return 0;
    default:
      throw logic_error("disconnect action does not have a timeout");
  }
}

void ProxyServer::LinkedSession::disconnect() {
  // Disconnect both ends
  this->client_channel.disconnect();
  this->server_channel.disconnect();

  // Set a timeout to delete the session entirely (in case the client doesn't
  // reconnect)
  struct timeval tv = usecs_to_timeval(this->timeout_for_disconnect_action(this->disconnect_action));
  event_add(this->timeout_event.get(), &tv);
}

bool ProxyServer::LinkedSession::is_connected() const {
  return (this->server_channel.connected() && this->client_channel.connected());
}

void ProxyServer::LinkedSession::on_input(Channel& ch, uint16_t command, uint32_t flag, std::string& data) {
  auto* ses = reinterpret_cast<LinkedSession*>(ch.context_obj);
  bool is_server_stream = (&ch == &ses->server_channel);

  try {
    if (is_server_stream) {
      size_t bytes_to_save = min<size_t>(data.size(), sizeof(ses->prev_server_command_bytes));
      memcpy(ses->prev_server_command_bytes, data.data(), bytes_to_save);
    }
    on_proxy_command(
        ses->shared_from_this(),
        is_server_stream,
        command,
        flag,
        data);
  } catch (const exception& e) {
    ses->log.error("Failed to process command from %s: %s",
        is_server_stream ? "server" : "client", e.what());
    ses->disconnect();
  }
}

shared_ptr<ProxyServer::LinkedSession> ProxyServer::get_session() {
  if (this->id_to_session.empty()) {
    throw runtime_error("no sessions exist");
  }
  if (this->id_to_session.size() > 1) {
    throw runtime_error("multiple sessions exist");
  }
  return this->id_to_session.begin()->second;
}

shared_ptr<ProxyServer::LinkedSession> ProxyServer::get_session_by_name(
    const std::string& name) {
  try {
    uint64_t session_id = stoull(name, nullptr, 16);
    return this->id_to_session.at(session_id);
  } catch (const invalid_argument&) {
    throw runtime_error("invalid session name");
  } catch (const out_of_range&) {
    throw runtime_error("no such session");
  }
}

shared_ptr<ProxyServer::LinkedSession> ProxyServer::create_licensed_session(
    shared_ptr<License> l,
    uint16_t local_port,
    Version version,
    const Client::Config& config) {
  auto session = make_shared<LinkedSession>(this->shared_from_this(), local_port, version, l, config);
  auto emplace_ret = this->id_to_session.emplace(session->id, session);
  if (!emplace_ret.second) {
    throw runtime_error("session already exists for this license");
  }
  session->log.info("Opening licensed session");
  return emplace_ret.first->second;
}

void ProxyServer::delete_session(uint64_t id) {
  if (this->id_to_session.erase(id)) {
    proxy_server_log.info("Closed LinkedSession:%08" PRIX64, id);
  }
}

void ProxyServer::delete_session(struct bufferevent* bev) {
  auto it = this->bev_to_unlinked_session.find(bev);
  if (it == this->bev_to_unlinked_session.end()) {
    throw logic_error("unlinked session exists but is not registered");
  }
  it->second->log.info("Closing session");
  this->unlinked_sessions_to_destroy.emplace(std::move(it->second));
  this->bev_to_unlinked_session.erase(it);

  auto tv = usecs_to_timeval(0);
  event_add(this->destroy_sessions_ev.get(), &tv);
}

void ProxyServer::dispatch_destroy_sessions(evutil_socket_t, short, void* ctx) {
  reinterpret_cast<ProxyServer*>(ctx)->destroy_sessions();
}

void ProxyServer::destroy_sessions() {
  this->unlinked_sessions_to_destroy.clear();
}

size_t ProxyServer::num_sessions() const {
  return this->id_to_session.size();
}

size_t ProxyServer::delete_disconnected_sessions() {
  size_t count = 0;
  for (auto it = this->id_to_session.begin(); it != this->id_to_session.end();) {
    if (!it->second->is_connected()) {
      it = this->id_to_session.erase(it);
      count++;
    } else {
      it++;
    }
  }
  return count;
}
