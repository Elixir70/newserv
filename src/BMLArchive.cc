#include "BMLArchive.hh"

#include <phosg/Encoding.hh>
#include <phosg/Filesystem.hh>
#include <stdexcept>

#include "Text.hh"

using namespace std;

template <bool IsBigEndian>
struct BMLHeader {
  using U32T = typename std::conditional<IsBigEndian, be_uint32_t, le_uint32_t>::type;

  parray<uint8_t, 0x04> unknown_a1;
  U32T num_entries;
  parray<uint8_t, 0x38> unknown_a2;
} __attribute__((packed));

template <bool IsBigEndian>
struct BMLHeaderEntry {
  using U32T = typename std::conditional<IsBigEndian, be_uint32_t, le_uint32_t>::type;

  pstring<TextEncoding::ASCII, 0x20> filename;
  U32T compressed_size;
  parray<uint8_t, 0x04> unknown_a1;
  U32T decompressed_size;
  U32T compressed_gvm_size;
  U32T decompressed_gvm_size;
  parray<uint8_t, 0x0C> unknown_a2;
} __attribute__((packed));

template <bool IsBigEndian>
void BMLArchive::load_t() {
  StringReader r(*this->data);

  const auto& header = r.get<BMLHeader<IsBigEndian>>();

  size_t offset = 0x800;
  while (this->entries.size() < header.num_entries) {
    const auto& entry = r.get<BMLHeaderEntry<IsBigEndian>>();

    if (offset + entry.compressed_size > this->data->size()) {
      throw runtime_error("BML data entry extends beyond end of data");
    }
    size_t data_offset = offset;
    offset = (offset + entry.compressed_size + 0x1F) & (~0x1F);

    if (offset + entry.compressed_gvm_size > this->data->size()) {
      throw runtime_error("BML GVM entry extends beyond end of data");
    }
    size_t gvm_offset = offset;
    offset = (offset + entry.compressed_gvm_size + 0x1F) & (~0x1F);

    this->entries.emplace(entry.filename.decode(), Entry{data_offset, entry.compressed_size, gvm_offset, entry.compressed_gvm_size});
  }
}

BMLArchive::BMLArchive(shared_ptr<const string> data, bool big_endian)
    : data(data) {
  if (big_endian) {
    this->load_t<true>();
  } else {
    this->load_t<false>();
  }
}

const unordered_map<string, BMLArchive::Entry> BMLArchive::all_entries() const {
  return this->entries;
}

pair<const void*, size_t> BMLArchive::get(const std::string& name) const {
  try {
    const auto& entry = this->entries.at(name);
    return make_pair(this->data->data() + entry.offset, entry.size);
  } catch (const out_of_range&) {
    throw out_of_range("BML does not contain file: " + name);
  }
}

pair<const void*, size_t> BMLArchive::get_gvm(const std::string& name) const {
  try {
    const auto& entry = this->entries.at(name);
    return make_pair(this->data->data() + entry.gvm_offset, entry.gvm_size);
  } catch (const out_of_range&) {
    throw out_of_range("BML does not contain file: " + name);
  }
}

string BMLArchive::get_copy(const string& name) const {
  try {
    const auto& entry = this->entries.at(name);
    return this->data->substr(entry.offset, entry.size);
  } catch (const out_of_range&) {
    throw out_of_range("BML does not contain file: " + name);
  }
}

StringReader BMLArchive::get_reader(const string& name) const {
  try {
    const auto& entry = this->entries.at(name);
    return StringReader(this->data->data() + entry.offset, entry.size);
  } catch (const out_of_range&) {
    throw out_of_range("BML does not contain file: " + name);
  }
}
