// Host-side tests for the BLE protocol layer (components/precision_feeder_ble/protocol.h).
// The golden byte vectors are shared with tools/test_precision_feeder_ble.py, so the firmware and the
// Python client are checked against the same bytes.
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder_ble test_protocol.cpp -o /tmp/test_protocol && /tmp/test_protocol
#include <cstdio>
#include <cstring>

#include "protocol.h"

using namespace esphome::precision_feeder::proto;

static int failures = 0;
#define CHECK(cond, msg)                                  \
  do {                                                    \
    if (!(cond)) {                                        \
      std::printf("FAIL: %s (line %d)\n", msg, __LINE__); \
      failures++;                                         \
    }                                                     \
  } while (0)

template<size_t N> static bool same(const uint8_t *got, const uint8_t (&want)[N]) { return std::memcmp(got, want, N) == 0; }

int main() {
  // ---- Status ----
  {
    StatusSnapshot s;
    s.tray_position = 2;
    s.remaining = 4;
    s.tray_state = 0;
    s.active_profile = 1;
    s.flags = FLAG_TIME_VALID | FLAG_SCHEDULER_ENABLED;
    s.last_feed_epoch = 1760380800;
    s.last_feed_source = 2;
    s.feeds_today = 1;
    s.next_event_epoch = 1760427600;
    s.runway_days = 3;
    s.time_source = 2;
    s.time_age_min = 125;
    uint8_t out[STATUS_SIZE];
    encode_status(s, out);
    const uint8_t want[] = {0x02, 0x04, 0x00, 0x01, 0x11, 0x00, 0x80, 0x47, 0xED, 0x68,
                            0x02, 0x01, 0x50, 0xFE, 0xED, 0x68, 0x03, 0x02, 0x7D, 0x00};
    CHECK(sizeof(want) == STATUS_SIZE, "status vector size");
    CHECK(same(out, want), "status golden bytes");
  }

  // ---- Profile header ----
  {
    ProfileHeaderRec r;
    r.profile = 1;
    r.every_n = 2;
    r.fast_mask = 0x01;
    r.anchor_day = 20739;
    uint8_t out[PROFILE_HEADER_SIZE];
    encode_profile_header(r, out);
    const uint8_t want[] = {0x00, 0x01, 0x02, 0x01, 0x03, 0x51};
    CHECK(same(out, want), "profile header golden bytes");
    ProfileHeaderRec d;
    CHECK(decode_profile_header(want, sizeof(want), d), "header decodes");
    CHECK(d.profile == 1 && d.every_n == 2 && d.fast_mask == 1 && d.anchor_day == 20739, "header fields");
    CHECK(valid_profile_header(d), "header valid");
    CHECK(!decode_profile_header(want, sizeof(want) - 1, d), "short header rejected");
    uint8_t bad_tag[] = {0x01, 0x01, 0x02, 0x01, 0x03, 0x51};
    CHECK(!decode_profile_header(bad_tag, sizeof(bad_tag), d), "wrong tag rejected");
    d.every_n = 0;
    CHECK(!valid_profile_header(d), "every_n 0 invalid");
    d.every_n = 15;
    CHECK(!valid_profile_header(d), "every_n 15 invalid");
    d.every_n = 14;
    d.profile = 3;
    CHECK(!valid_profile_header(d), "profile 3 invalid");
    d.profile = 2;
    d.fast_mask = 0x80;
    CHECK(!valid_profile_header(d), "fast mask bit 7 invalid");
  }

  // ---- Profile event ----
  {
    ProfileEventRec r;
    r.profile = 0;
    r.event = 2;
    r.enabled = 1;
    r.anchor = 2;  // sunset
    r.offset_min = 180;
    r.portions = 2;
    r.days = 0x3E;
    uint8_t out[PROFILE_EVENT_SIZE];
    encode_profile_event(r, out);
    const uint8_t want[] = {0x01, 0x00, 0x02, 0x05, 0xB4, 0x00, 0x02, 0x3E};
    CHECK(same(out, want), "event golden bytes (sunset +180)");
    ProfileEventRec d;
    CHECK(decode_profile_event(want, sizeof(want), d), "event decodes");
    CHECK(d.enabled == 1 && d.anchor == 2 && d.offset_min == 180 && d.portions == 2 && d.days == 0x3E, "event fields");
    CHECK(valid_profile_event(d), "event valid");

    const uint8_t clock_ev[] = {0x01, 0x02, 0x03, 0x01, 0xE0, 0x01, 0x01, 0x7F};  // clock 08:00
    CHECK(decode_profile_event(clock_ev, sizeof(clock_ev), d) && d.anchor == 0 && d.offset_min == 480 && valid_profile_event(d),
          "clock 08:00 event");
    const uint8_t sunrise_ev[] = {0x01, 0x01, 0x00, 0x02, 0xE2, 0xFF, 0x01, 0x7F};  // sunrise -30
    CHECK(decode_profile_event(sunrise_ev, sizeof(sunrise_ev), d) && d.anchor == 1 && d.offset_min == -30 && d.enabled == 0 &&
              valid_profile_event(d),
          "sunrise -30 event");

    // Range checks.
    d.anchor = 0;
    d.offset_min = 1440;
    CHECK(!valid_profile_event(d), "clock 24:00 invalid");
    d.offset_min = -1;
    CHECK(!valid_profile_event(d), "negative clock invalid");
    d.offset_min = 1439;
    CHECK(valid_profile_event(d), "clock 23:59 valid");
    d.anchor = 1;
    d.offset_min = 721;
    CHECK(!valid_profile_event(d), "solar +721 invalid");
    d.offset_min = -720;
    CHECK(valid_profile_event(d), "solar -720 valid");
    d.portions = 0;
    CHECK(!valid_profile_event(d), "0 portions invalid");
    d.portions = 7;
    CHECK(!valid_profile_event(d), "7 portions invalid");
    d.portions = 6;
    d.event = 4;
    CHECK(!valid_profile_event(d), "event 4 invalid");
    d.event = 3;
    d.anchor = 3;
    CHECK(!valid_profile_event(d), "anchor 3 invalid");
  }

  // ---- Config and calibration ranges ----
  {
    CHECK(config_key_known(CFG_MIN_GAP_S) && config_key_known(CFG_LONGITUDE_E5), "known keys");
    CHECK(config_key_known(CFG_MIN_PULSE_US) && config_key_known(CFG_MAX_PULSE_US), "pulse keys known");
    CHECK(!config_key_known(0) && !config_key_known(12), "unknown keys");
    CHECK(config_value_ok(CFG_MIN_PULSE_US, 300) && config_value_ok(CFG_MIN_PULSE_US, 1500) &&
              !config_value_ok(CFG_MIN_PULSE_US, 299) && !config_value_ok(CFG_MIN_PULSE_US, 1501),
          "min pulse range");
    CHECK(config_value_ok(CFG_MAX_PULSE_US, 1500) && config_value_ok(CFG_MAX_PULSE_US, 3000) &&
              !config_value_ok(CFG_MAX_PULSE_US, 1499) && !config_value_ok(CFG_MAX_PULSE_US, 3001),
          "max pulse range");
    CHECK(config_value_ok(CFG_DAILY_CAP, 3) && !config_value_ok(CFG_DAILY_CAP, 0) && !config_value_ok(CFG_DAILY_CAP, 7),
          "daily cap range");
    CHECK(config_value_ok(CFG_MIN_GAP_S, 10) && !config_value_ok(CFG_MIN_GAP_S, 9) && config_value_ok(CFG_MIN_GAP_S, 3600) &&
              !config_value_ok(CFG_MIN_GAP_S, 3601),
          "min gap range");
    CHECK(config_value_ok(CFG_RELOAD_PAUSE_MIN, 0) && config_value_ok(CFG_RELOAD_PAUSE_MIN, 60), "reload pause range");
    CHECK(config_value_ok(CFG_SCHEDULER_ENABLED, 1) && !config_value_ok(CFG_SCHEDULER_ENABLED, 2), "scheduler flag");
    CHECK(config_value_ok(CFG_LATITUDE_E5, -9000000) && !config_value_ok(CFG_LATITUDE_E5, 9000001), "latitude range");
    CHECK(config_value_ok(CFG_LONGITUDE_E5, 18000000) && !config_value_ok(CFG_LONGITUDE_E5, -18000001), "longitude range");
    CHECK(!config_value_ok(99, 1), "unknown key has no valid value");

    CHECK(calibration_ok(0, 0) && calibration_ok(6, 150) && calibration_ok(2, -150), "calibration ok");
    CHECK(!calibration_ok(7, 0) && !calibration_ok(0, 151) && !calibration_ok(0, -151), "calibration range");
  }

  // ---- Event log ----
  {
    uint8_t out[EVENT_LOG_SIZE];
    encode_event_log(1760380800, 2, 1, 0, out);
    const uint8_t want[] = {0x80, 0x47, 0xED, 0x68, 0x02, 0x01, 0x00};
    CHECK(same(out, want), "event log golden bytes");
  }

  // ---- Little-endian helpers and command vectors ----
  {
    const uint8_t hold[] = {0x06, 0x04, 0xFF, 0xFF, 0xFF, 0xFF};
    CHECK(get_u32(hold + 2) == 0xFFFFFFFFu, "hold forever decodes");
    const uint8_t set_time[] = {0x07, 0x06, 0x80, 0x47, 0xED, 0x68, 0x98, 0xFE};
    CHECK(get_u32(set_time + 2) == 1760380800u && get_i16(set_time + 6) == -360, "set_time decodes");
    const uint8_t loc[] = {0x08, 0x07, 0x88, 0xAE, 0x3C, 0x00, 0x7E, 0x88, 0x7C, 0xFF};
    CHECK(get_i32(loc + 2) == 3976840 && get_i32(loc + 6) == -8615810, "set_location decodes");
    const uint8_t cfg[] = {0x05, 0x03, 0x00, 0x00, 0x00};
    CHECK(get_i32(cfg + 1) == 3, "config value decodes");
    const uint8_t pulse[] = {0x0B, 0xD2, 0x0A, 0x00, 0x00};
    CHECK(pulse[0] == CFG_MAX_PULSE_US && get_i32(pulse + 1) == 2770, "max pulse config decodes");
    const uint8_t cal[] = {0x02, 0xF4, 0xFF};
    CHECK(get_i16(cal + 1) == -12, "calibration offset decodes");
    uint8_t buf[4];
    put_i32(buf, -2);
    CHECK(buf[0] == 0xFE && buf[1] == 0xFF && buf[2] == 0xFF && buf[3] == 0xFF, "put_i32 negative");
  }

  // ---- BLE time policy ----
  {
    const int64_t hour = 3600LL * 1000;
    const uint32_t stale = 24u * 3600u * 1000u;
    const uint32_t ok_epoch = 1760380800;
    CHECK(decide_ble_time(TimePolicy::OFF, ok_epoch, -1, stale) == RES_BLE_TIME_DISABLED, "policy off refuses");
    CHECK(decide_ble_time(TimePolicy::FALLBACK_ONLY, ok_epoch, -1, stale) == RES_OK, "fallback: never synced accepts");
    CHECK(decide_ble_time(TimePolicy::FALLBACK_ONLY, ok_epoch, 2 * hour, stale) == RES_IGNORED_NTP_ACTIVE,
          "fallback: fresh network time ignores BLE");
    CHECK(decide_ble_time(TimePolicy::FALLBACK_ONLY, ok_epoch, 25 * hour, stale) == RES_OK,
          "fallback: stale network time accepts");
    CHECK(decide_ble_time(TimePolicy::ALWAYS, ok_epoch, 2 * hour, stale) == RES_OK, "always accepts");
    CHECK(decide_ble_time(TimePolicy::ALWAYS, 1000, -1, stale) == RES_INVALID_ARG, "implausible epoch rejected");
    CHECK(decide_ble_time(TimePolicy::FALLBACK_ONLY, 1000, -1, stale) == RES_INVALID_ARG, "implausible epoch rejected (fallback)");
    CHECK(decide_ble_time(TimePolicy::OFF, 1000, -1, stale) == RES_BLE_TIME_DISABLED, "off wins over invalid epoch");
  }

  // ---- Retry cache ----
  {
    ReplayCache cache(30000);
    uint8_t out[RESPONSE_SIZE];
    const uint8_t feed5[] = {5, 0x01, 1};        // seq 5: FEED 1
    const uint8_t r5[] = {5, 0x01, 0, 1};
    CHECK(!cache.find(feed5, sizeof(feed5), 1000, out), "empty cache has nothing");
    cache.store(feed5, sizeof(feed5), r5, 1000);
    CHECK(cache.find(feed5, sizeof(feed5), 2000, out) && same(out, r5), "an exact retry is replayed");
    const uint8_t feed6[] = {6, 0x01, 1};
    CHECK(!cache.find(feed6, sizeof(feed6), 2000, out), "other seq not found");
    CHECK(cache.find(feed5, sizeof(feed5), 30999, out), "still valid just inside the TTL");
    CHECK(!cache.find(feed5, sizeof(feed5), 31000, out), "expired at the TTL");

    // The bug this guards against: a client restarts and reuses seq 5 for a different command.
    const uint8_t time5[] = {5, 0x06, 0x80, 0x47, 0xED, 0x68, 0x98, 0xFE};  // seq 5: SET_TIME
    CHECK(!cache.find(time5, sizeof(time5), 2000, out), "same seq, different opcode is a new command");
    const uint8_t feed5x2[] = {5, 0x01, 2};
    CHECK(!cache.find(feed5x2, sizeof(feed5x2), 2000, out), "same seq and opcode, different argument is new");
    const uint8_t feed5_long[] = {5, 0x01, 1, 0};
    CHECK(!cache.find(feed5_long, sizeof(feed5_long), 2000, out), "different length is new");

    // Storing a new command under the same seq replaces the old entry.
    const uint8_t rt[] = {5, 0x06, 0, 0};
    cache.store(time5, sizeof(time5), rt, 3000);
    CHECK(cache.find(time5, sizeof(time5), 3001, out) && same(out, rt), "new command stored under seq 5");
    CHECK(!cache.find(feed5, sizeof(feed5), 3001, out), "old command under seq 5 forgotten");

    // The ring holds 8 entries: a ninth distinct seq evicts the oldest.
    ReplayCache ring(1000000);
    for (uint8_t i = 0; i < ReplayCache::SIZE; i++) {
      const uint8_t cmd[] = {i, 0x05};
      const uint8_t r[] = {i, 0x05, 0, i};
      ring.store(cmd, sizeof(cmd), r, 100 + i);
    }
    for (uint8_t i = 0; i < ReplayCache::SIZE; i++) {
      const uint8_t cmd[] = {i, 0x05};
      CHECK(ring.find(cmd, sizeof(cmd), 200, out) && out[3] == i, "all eight kept");
    }
    const uint8_t cmd100[] = {100, 0x05};
    const uint8_t r100[] = {100, 0x05, 0, 9};
    ring.store(cmd100, sizeof(cmd100), r100, 300);
    const uint8_t cmd0[] = {0, 0x05};
    const uint8_t cmd1[] = {1, 0x05};
    CHECK(!ring.find(cmd0, sizeof(cmd0), 301, out), "oldest evicted by the ninth");
    CHECK(ring.find(cmd1, sizeof(cmd1), 301, out) && ring.find(cmd100, sizeof(cmd100), 301, out), "newer entries survive");
  }

  if (failures == 0)
    std::printf("All protocol tests passed\n");
  return failures == 0 ? 0 : 1;
}
