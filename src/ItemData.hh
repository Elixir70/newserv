#pragma once

#include <phosg/Encoding.hh>
#include <string>

#include "Text.hh"
#include "Version.hh"

class ItemParameterTable;

enum class EquipSlot {
  // When equipping items through the Item Pack pause menu, the client sends
  // UNKNOWN for the slot. The receiving client (and server, in our case) have
  // to analyze the item being equipped and put it in the appropriate slot in
  // this case. See ItemData::default_equip_slot() for this computation.
  UNKNOWN = 0x00,
  // When equipping items through the quick menu or Equip pause menu, the client
  // sends one of the slots below.
  MAG = 0x01,
  ARMOR = 0x02,
  SHIELD = 0x03,
  WEAPON = 0x06,
  UNIT_1 = 0x09,
  UNIT_2 = 0x0A,
  UNIT_3 = 0x0B,
  UNIT_4 = 0x0C,
};

struct ItemMagStats {
  uint16_t iq = 0;
  uint16_t synchro = 40;
  uint16_t def = 500;
  uint16_t pow = 0;
  uint16_t dex = 0;
  uint16_t mind = 0;
  uint8_t flags = 0;
  uint8_t photon_blasts = 0;
  uint8_t color = 14;

  inline uint16_t def_level() const {
    return this->def / 100;
  }
  inline uint16_t pow_level() const {
    return this->pow / 100;
  }
  inline uint16_t dex_level() const {
    return this->dex / 100;
  }
  inline uint16_t mind_level() const {
    return this->mind / 100;
  }
  inline uint16_t level() const {
    return this->def_level() + this->pow_level() + this->dex_level() + this->mind_level();
  }
};

struct ItemData { // 0x14 bytes
  // QUICK ITEM FORMAT REFERENCE
  //           data1/0  data1/4  data1/8  data2
  //   Weapon: 00ZZZZGG SS00AABB AABBAABB 00000000
  //   Armor:  0101ZZ00 FFTTDDDD EEEE0000 00000000
  //   Shield: 0102ZZ00 FFTTDDDD EEEE0000 00000000
  //   Unit:   0103ZZ00 FF00RRRR 00000000 00000000
  //   Mag:    02ZZLLWW HHHHIIII JJJJKKKK YYQQPPVV
  //   Tool:   03ZZZZFF 00CC0000 00000000 00000000
  //   Meseta: 04000000 00000000 00000000 MMMMMMMM
  // A = attribute type (for S-ranks, custom name)
  // B = attribute amount (for S-ranks, custom name)
  // C = stack size (for tools)
  // D = DEF bonus
  // E = EVP bonus
  // F = flags (40=present; for tools, unused if item is stackable)
  // G = weapon grind
  // H = mag DEF
  // I = mag POW
  // J = mag DEX
  // K = mag MIND
  // L = mag level
  // M = meseta amount
  // P = mag flags (40=present, 04=has left pb, 02=has right pb, 01=has center pb)
  // Q = mag IQ
  // R = unit modifier (little-endian)
  // S = weapon flags (80=unidentified, 40=present) and special (low 6 bits)
  // T = slot count
  // V = mag color
  // W = photon blasts
  // Y = mag synchro
  // Z = item ID
  // Note: PSO GC erroneously byteswaps data2 even when the item is a mag. This
  // makes it incompatible with little-endian versions of PSO (i.e. all other
  // versions). We manually byteswap data2 upon receipt and immediately before
  // sending where needed.
  // Related note: PSO V2 has an annoyingly complicated format for mags that
  // doesn't match the above table. We decode this upon receipt and encode it
  // imemdiately before sending when interacting with V2 clients; see the
  // implementation of decode_for_version() for details.

  union {
    parray<uint8_t, 12> data1;
    parray<le_uint16_t, 6> data1w;
    parray<be_uint16_t, 6> data1wb;
    parray<le_uint32_t, 3> data1d;
    parray<be_uint32_t, 3> data1db;
  } __attribute__((packed));
  le_uint32_t id;
  union {
    parray<uint8_t, 4> data2;
    parray<le_uint16_t, 2> data2w;
    parray<be_uint16_t, 2> data2wb;
    le_uint32_t data2d;
    be_uint32_t data2db;
  } __attribute__((packed));

  ItemData();
  ItemData(const ItemData& other);
  ItemData(uint64_t first, uint64_t second = 0);
  ItemData& operator=(const ItemData& other);

  bool operator==(const ItemData& other) const;
  bool operator!=(const ItemData& other) const;

  bool operator<(const ItemData& other) const;

  void clear();

  static ItemData from_data(const std::string& data);
  static ItemData from_primary_identifier(Version version, uint32_t primary_identifier);
  std::string hex() const;
  uint32_t primary_identifier() const;

  bool is_wrapped(Version version) const;
  void wrap(Version version);
  void unwrap(Version version);

  bool is_stackable(Version version) const;
  size_t stack_size(Version version) const;
  size_t max_stack_size(Version version) const;
  void enforce_min_stack_size(Version version);

  static bool is_common_consumable(uint32_t primary_identifier);
  bool is_common_consumable() const;

  void assign_mag_stats(const ItemMagStats& mag);
  void clear_mag_stats();
  uint16_t compute_mag_level() const;
  uint16_t compute_mag_strength_flags() const;
  uint8_t mag_photon_blast_for_slot(uint8_t slot) const;
  bool mag_has_photon_blast_in_any_slot(uint8_t pb_num) const;
  void add_mag_photon_blast(uint8_t pb_num);
  void decode_for_version(Version version);
  void encode_for_version(Version version, std::shared_ptr<const ItemParameterTable> item_parameter_table);
  uint8_t get_encoded_v2_data() const;
  bool has_encoded_v2_data() const;

  uint16_t get_sealed_item_kill_count() const;
  void set_sealed_item_kill_count(uint16_t v);
  uint8_t get_tool_item_amount(Version version) const;
  void set_tool_item_amount(Version version, uint8_t amount);
  int16_t get_armor_or_shield_defense_bonus() const;
  void set_armor_or_shield_defense_bonus(int16_t bonus);
  int16_t get_common_armor_evasion_bonus() const;
  void set_common_armor_evasion_bonus(int16_t bonus);
  int16_t get_unit_bonus() const;
  void set_unit_bonus(int16_t bonus);

  bool has_bonuses() const;
  bool is_s_rank_weapon() const;

  EquipSlot default_equip_slot() const;
  bool can_be_equipped_in_slot(EquipSlot slot) const;

  bool empty() const;

  static bool compare_for_sort(const ItemData& a, const ItemData& b);
} __attribute__((packed));
