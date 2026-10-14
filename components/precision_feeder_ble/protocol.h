// Precision Feeder BLE protocol: constants, packing, validation, retry cache.
//
// Pure functions with no ESPHome dependencies, so they can be tested on a host
// (tests/test_protocol.cpp). The byte layouts here are the protocol; they are
// documented in docs/PROTOCOL.md and mirrored by tools/precision_feeder_ble.py.
// All multi-byte values are little-endian.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>

namespace esphome {
namespace precision_feeder {
namespace proto {

constexpr uint8_t PROTOCOL_VERSION = 1;

// Capability bits reported in the Info characteristic.
constexpr uint16_t CAP_SOLAR = 1u << 0;          // sunrise/sunset event anchors
constexpr uint16_t CAP_TIME_PUSH = 1u << 1;      // SET_TIME is available
constexpr uint16_t CAP_PROFILE_EDIT = 1u << 2;   // Profile characteristic can edit the schedule
constexpr uint16_t CAP_CALIBRATION = 1u << 3;    // calibration commands
constexpr uint16_t CAP_HOLD_FOREVER = 1u << 4;   // HOLD accepts "until cancelled"
constexpr uint16_t CAP_SERVO_PULSE = 1u << 5;    // Config keys 10 and 11 (servo pulse range) exist

// Layout constants (must match the feeder component; checked in precision_feeder_ble.cpp).
constexpr uint8_t POSITIONS = 7;
constexpr uint8_t PROFILES = 3;
constexpr uint8_t EVENTS_PER_PROFILE = 4;

// Command opcodes (Command characteristic: seq u8, opcode u8, arguments).
constexpr uint8_t OP_FEED = 0x01;              // portions u8
constexpr uint8_t OP_TRAY_RELOAD = 0x02;       // none
constexpr uint8_t OP_SET_PROFILE = 0x03;       // index u8
constexpr uint8_t OP_HOLD = 0x04;              // minutes u32; 0 cancels; 0xFFFFFFFF = until cancelled
constexpr uint8_t OP_STOP = 0x05;              // none
constexpr uint8_t OP_SET_TIME = 0x06;          // epoch u32 (UTC), utc_offset_min i16
constexpr uint8_t OP_SET_LOCATION = 0x07;      // latitude i32, longitude i32 (units of 1e-5 degree)
constexpr uint8_t OP_CALIBRATE = 0x08;         // on u8 (0 or 1)
constexpr uint8_t OP_JOG = 0x09;               // position u8, delta i16 (tenths of a degree)
constexpr uint8_t OP_SAVE_CALIBRATION = 0x0A;  // none
constexpr uint8_t OP_SET_POSITION = 0x0B;      // position u8
constexpr uint8_t OP_SET_TIMEZONE = 0x0C;      // reserved; answered UNSUPPORTED (see DESIGN.md section 6)

// Opcodes used in the Response for writes to the Profile, Config and Calibration characteristics.
constexpr uint8_t OP_PROFILE_WRITE = 0x80;
constexpr uint8_t OP_CONFIG_WRITE = 0x81;
constexpr uint8_t OP_CALIBRATION_WRITE = 0x82;

// Result codes (the first twelve match the feeder component's Result enum).
constexpr uint8_t RES_OK = 0;
constexpr uint8_t RES_INVALID_ARG = 8;
constexpr uint8_t RES_IGNORED_NTP_ACTIVE = 12;  // BLE time refused: network time is fresh
constexpr uint8_t RES_BLE_TIME_DISABLED = 13;   // BLE time refused: policy is "off"
constexpr uint8_t RES_UNSUPPORTED = 14;         // unknown or reserved opcode
constexpr uint8_t RES_NOT_CALIBRATING = 15;     // the command needs calibration mode

constexpr size_t RESPONSE_SIZE = 4;  // seq, opcode, result, detail

// ---- little-endian helpers ---------------------------------------------------------------------

inline uint16_t get_u16(const uint8_t *p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline int16_t get_i16(const uint8_t *p) { return static_cast<int16_t>(get_u16(p)); }
inline uint32_t get_u32(const uint8_t *p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline int32_t get_i32(const uint8_t *p) { return static_cast<int32_t>(get_u32(p)); }
inline void put_u16(uint8_t *p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
}
inline void put_i16(uint8_t *p, int16_t v) { put_u16(p, static_cast<uint16_t>(v)); }
inline void put_u32(uint8_t *p, uint32_t v) {
  p[0] = static_cast<uint8_t>(v);
  p[1] = static_cast<uint8_t>(v >> 8);
  p[2] = static_cast<uint8_t>(v >> 16);
  p[3] = static_cast<uint8_t>(v >> 24);
}
inline void put_i32(uint8_t *p, int32_t v) { put_u32(p, static_cast<uint32_t>(v)); }

// ---- Status characteristic (20 bytes, fits the default 23-byte MTU) --------------------------------

constexpr size_t STATUS_SIZE = 20;

// Status flag bits.
constexpr uint16_t FLAG_TIME_VALID = 1u << 0;
constexpr uint16_t FLAG_HOLD = 1u << 1;
constexpr uint16_t FLAG_CALIBRATING = 1u << 2;
constexpr uint16_t FLAG_MOVING = 1u << 3;
constexpr uint16_t FLAG_SCHEDULER_ENABLED = 1u << 4;
constexpr uint16_t FLAG_RELOAD_PAUSE = 1u << 5;
constexpr uint16_t FLAG_SOLAR_FAULT = 1u << 6;
constexpr uint16_t FLAG_OVERSUBSCRIBED = 1u << 7;
constexpr uint16_t FLAG_TIME_STALE = 1u << 8;
constexpr uint16_t FLAG_POSITION_UNKNOWN = 1u << 9;
constexpr uint16_t FLAG_TRAY_EMPTY = 1u << 10;

struct StatusSnapshot {
  uint8_t tray_position{0};
  uint8_t remaining{0};
  uint8_t tray_state{0};  // 0 loaded, 1 empty, 2 unknown, 3 calibrating, 4 moving
  uint8_t active_profile{0};
  uint16_t flags{0};
  uint32_t last_feed_epoch{0};
  uint8_t last_feed_source{0};  // 0 schedule, 1 home assistant, 2 ble, 3 button, 4 one-shot
  uint8_t feeds_today{0};
  uint32_t next_event_epoch{0};
  uint8_t runway_days{255};  // 255 = more than the planning horizon
  uint8_t time_source{0};    // 0 none, 1 BLE, 2 NTP, 3 Home Assistant
  uint16_t time_age_min{0};  // minutes since the last clock sync, saturating
};

inline void encode_status(const StatusSnapshot &s, uint8_t out[STATUS_SIZE]) {
  out[0] = s.tray_position;
  out[1] = s.remaining;
  out[2] = s.tray_state;
  out[3] = s.active_profile;
  put_u16(out + 4, s.flags);
  put_u32(out + 6, s.last_feed_epoch);
  out[10] = s.last_feed_source;
  out[11] = s.feeds_today;
  put_u32(out + 12, s.next_event_epoch);
  out[16] = s.runway_days;
  out[17] = s.time_source;
  put_u16(out + 18, s.time_age_min);
}

// ---- Profile characteristic ------------------------------------------------------------------------

constexpr uint8_t PROFILE_TAG_HEADER = 0x00;
constexpr uint8_t PROFILE_TAG_EVENT = 0x01;
constexpr uint8_t PROFILE_TAG_QUERY = 0x02;  // [0x02, profile, event]; event 0xFF asks for the header
constexpr uint8_t QUERY_HEADER = 0xFF;
constexpr size_t PROFILE_HEADER_SIZE = 6;
constexpr size_t PROFILE_EVENT_SIZE = 8;
constexpr size_t PROFILE_QUERY_SIZE = 3;

struct ProfileHeaderRec {
  uint8_t profile{0};
  uint8_t every_n{1};
  uint8_t fast_mask{0};
  uint16_t anchor_day{0};  // days since 1970-01-01 that count as cycle day 0; 0 = restart today
};

struct ProfileEventRec {
  uint8_t profile{0};
  uint8_t event{0};
  uint8_t enabled{0};
  uint8_t anchor{0};  // 0 clock, 1 sunrise, 2 sunset
  int16_t offset_min{0};
  uint8_t portions{1};
  uint8_t days{0x7F};
};

inline void encode_profile_header(const ProfileHeaderRec &r, uint8_t out[PROFILE_HEADER_SIZE]) {
  out[0] = PROFILE_TAG_HEADER;
  out[1] = r.profile;
  out[2] = r.every_n;
  out[3] = r.fast_mask;
  put_u16(out + 4, r.anchor_day);
}

inline bool decode_profile_header(const uint8_t *p, size_t n, ProfileHeaderRec &r) {
  if (n != PROFILE_HEADER_SIZE || p[0] != PROFILE_TAG_HEADER)
    return false;
  r.profile = p[1];
  r.every_n = p[2];
  r.fast_mask = p[3];
  r.anchor_day = get_u16(p + 4);
  return true;
}

inline bool valid_profile_header(const ProfileHeaderRec &r) {
  return r.profile < PROFILES && r.every_n >= 1 && r.every_n <= 14 && r.fast_mask <= 0x7F;
}

inline void encode_profile_event(const ProfileEventRec &r, uint8_t out[PROFILE_EVENT_SIZE]) {
  out[0] = PROFILE_TAG_EVENT;
  out[1] = r.profile;
  out[2] = r.event;
  out[3] = static_cast<uint8_t>((r.enabled & 1) | ((r.anchor & 3) << 1));
  put_i16(out + 4, r.offset_min);
  out[6] = r.portions;
  out[7] = r.days;
}

inline bool decode_profile_event(const uint8_t *p, size_t n, ProfileEventRec &r) {
  if (n != PROFILE_EVENT_SIZE || p[0] != PROFILE_TAG_EVENT)
    return false;
  r.profile = p[1];
  r.event = p[2];
  r.enabled = p[3] & 1;
  r.anchor = (p[3] >> 1) & 3;
  r.offset_min = get_i16(p + 4);
  r.portions = p[6];
  r.days = p[7];
  return true;
}

inline bool valid_profile_event(const ProfileEventRec &r) {
  if (r.profile >= PROFILES || r.event >= EVENTS_PER_PROFILE || r.anchor > 2 || r.portions < 1 || r.portions > 6 ||
      r.days > 0x7F)
    return false;
  if (r.anchor == 0)
    return r.offset_min >= 0 && r.offset_min <= 24 * 60 - 1;
  return r.offset_min >= -720 && r.offset_min <= 720;
}

// ---- Config characteristic -----------------------------------------------------------------------

constexpr uint8_t CFG_MIN_GAP_S = 1;
constexpr uint8_t CFG_PORTION_GAP_S = 2;
constexpr uint8_t CFG_RELOAD_PAUSE_MIN = 3;
constexpr uint8_t CFG_REFILL_INTERVAL_DAYS = 4;
constexpr uint8_t CFG_DAILY_CAP = 5;
constexpr uint8_t CFG_MAX_PORTIONS_CMD = 6;
constexpr uint8_t CFG_SCHEDULER_ENABLED = 7;
constexpr uint8_t CFG_LATITUDE_E5 = 8;
constexpr uint8_t CFG_LONGITUDE_E5 = 9;
constexpr uint8_t CFG_MIN_PULSE_US = 10;  // servo pulse for 0 degrees; at least 500 us below the max
constexpr uint8_t CFG_MAX_PULSE_US = 11;  // servo pulse for 180 degrees
constexpr uint8_t QUERY_FLAG = 0x80;  // OR into a key (or index) to ask for it instead of setting it
constexpr size_t CONFIG_SET_SIZE = 5;  // key u8, value i32
constexpr size_t QUERY_SIZE = 1;       // key|0x80

/// True if `key` exists and `value` is in range for it.
inline bool config_value_ok(uint8_t key, int32_t v) {
  switch (key) {
    case CFG_MIN_GAP_S:
      return v >= 10 && v <= 3600;
    case CFG_PORTION_GAP_S:
      return v >= 1 && v <= 60;
    case CFG_RELOAD_PAUSE_MIN:
      return v >= 0 && v <= 60;
    case CFG_REFILL_INTERVAL_DAYS:
      return v >= 1 && v <= 30;
    case CFG_DAILY_CAP:
    case CFG_MAX_PORTIONS_CMD:
      return v >= 1 && v <= 6;
    case CFG_SCHEDULER_ENABLED:
      return v == 0 || v == 1;
    case CFG_LATITUDE_E5:
      return v >= -9000000 && v <= 9000000;
    case CFG_LONGITUDE_E5:
      return v >= -18000000 && v <= 18000000;
    case CFG_MIN_PULSE_US:
      return v >= 300 && v <= 1500;
    case CFG_MAX_PULSE_US:
      return v >= 1500 && v <= 3000;
    default:
      return false;
  }
}

inline bool config_key_known(uint8_t key) { return key >= CFG_MIN_GAP_S && key <= CFG_MAX_PULSE_US; }

// ---- Calibration characteristic -------------------------------------------------------------------

constexpr size_t CALIBRATION_SET_SIZE = 3;  // index u8, offset i16 (tenths of a degree)
constexpr int16_t CAL_MIN = -150;           // +/- 15 degrees
constexpr int16_t CAL_MAX = 150;

inline bool calibration_ok(uint8_t index, int16_t tenths) {
  return index < POSITIONS && tenths >= CAL_MIN && tenths <= CAL_MAX;
}

// ---- Event log characteristic -----------------------------------------------------------------------

constexpr size_t EVENT_LOG_SIZE = 7;  // epoch u32 (0 = clock unknown), source u8, portions u8, result u8

inline void encode_event_log(uint32_t epoch, uint8_t source, uint8_t portions, uint8_t result,
                             uint8_t out[EVENT_LOG_SIZE]) {
  put_u32(out, epoch);
  out[4] = source;
  out[5] = portions;
  out[6] = result;
}

// ---- Setting the clock over BLE ---------------------------------------------------------------------

/// How the device treats SET_TIME.
enum class TimePolicy : uint8_t {
  FALLBACK_ONLY = 0,  // accepted only if network time (NTP or Home Assistant) has not synced recently
  ALWAYS = 1,         // accepted whenever the value is plausible
  OFF = 2,            // refused
};

constexpr uint32_t MIN_VALID_EPOCH = 1546300800;  // 2019-01-01 00:00:00 UTC, the same floor ESPHome uses

/// Decide whether to accept a time pushed over BLE.
/// @param network_age_ms  milliseconds since the last NTP or Home Assistant sync; negative = never synced
/// @param stale_ms        how old a network sync may be before BLE time is accepted again
inline uint8_t decide_ble_time(TimePolicy policy, uint32_t epoch, int64_t network_age_ms, uint32_t stale_ms) {
  if (policy == TimePolicy::OFF)
    return RES_BLE_TIME_DISABLED;
  if (epoch < MIN_VALID_EPOCH)
    return RES_INVALID_ARG;
  if (policy == TimePolicy::FALLBACK_ONLY && network_age_ms >= 0 && network_age_ms < static_cast<int64_t>(stale_ms))
    return RES_IGNORED_NTP_ACTIVE;
  return RES_OK;
}

// ---- Retry cache (BLE command idempotency) -----------------------------------------------------------

/// Remembers the answers to recent commands so a client that retries a command whose response was lost gets the
/// same answer back and the command is not executed twice. A retry is the *same bytes* (sequence number, opcode
/// and arguments) within the TTL; a command that reuses a sequence number with different contents is new.
/// Entries also expire, so a client that restarts and repeats an old command much later is not mistaken for a retry.
class ReplayCache {
 public:
  static constexpr size_t SIZE = 8;
  static constexpr size_t MAX_COMMAND = 20;  // a whole Command write fits the default 20-byte payload
  static constexpr uint32_t DEFAULT_TTL_MS = 30000;

  explicit ReplayCache(uint32_t ttl_ms = DEFAULT_TTL_MS) : ttl_ms_(ttl_ms) {}

  /// If this exact command (`command[0]` is the sequence number) was answered within the TTL, copy the stored
  /// response and return true.
  bool find(const uint8_t *command, size_t len, uint64_t now_ms, uint8_t out[RESPONSE_SIZE]) const {
    for (const Entry &e : this->entries_) {
      if (e.used && same_(e, command, len) && now_ms - e.at_ms < this->ttl_ms_) {
        for (size_t i = 0; i < RESPONSE_SIZE; i++)
          out[i] = e.response[i];
        return true;
      }
    }
    return false;
  }

  void store(const uint8_t *command, size_t len, const uint8_t response[RESPONSE_SIZE], uint64_t now_ms) {
    if (len == 0)
      return;
    Entry *slot = nullptr;
    for (Entry &e : this->entries_) {
      if (e.used && e.len > 0 && e.command[0] == command[0]) {  // same sequence number: replace it
        slot = &e;
        break;
      }
    }
    if (slot == nullptr) {
      slot = &this->entries_[this->next_];
      this->next_ = (this->next_ + 1) % SIZE;
    }
    slot->used = true;
    slot->len = static_cast<uint8_t>(len < MAX_COMMAND ? len : MAX_COMMAND);
    for (size_t i = 0; i < slot->len; i++)
      slot->command[i] = command[i];
    slot->at_ms = now_ms;
    for (size_t i = 0; i < RESPONSE_SIZE; i++)
      slot->response[i] = response[i];
  }

 protected:
  struct Entry {
    bool used{false};
    uint8_t len{0};
    uint8_t command[MAX_COMMAND]{};
    uint8_t response[RESPONSE_SIZE]{};
    uint64_t at_ms{0};
  };

  static bool same_(const Entry &e, const uint8_t *command, size_t len) {
    if (len != e.len || len == 0 || len > MAX_COMMAND)
      return false;
    for (size_t i = 0; i < len; i++) {
      if (e.command[i] != command[i])
        return false;
    }
    return true;
  }

  Entry entries_[SIZE];
  size_t next_{0};
  uint32_t ttl_ms_;
};

}  // namespace proto
}  // namespace precision_feeder
}  // namespace esphome
