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

  // ---- Retry cache: channels are separate ----
  {
    ReplayCache cache(30000);
    uint8_t out[RESPONSE_SIZE];
    const uint8_t w[] = {7, 0x05, 0x03, 0x00, 0x00, 0x00};
    const uint8_t r[] = {7, 0x81, 0, 5};
    cache.store(CHANNEL_CONFIG, w, sizeof(w), r, 100);
    CHECK(cache.find(CHANNEL_CONFIG, w, sizeof(w), 200, out) && same(out, r), "record retry replayed");
    CHECK(!cache.find(CHANNEL_PROFILE, w, sizeof(w), 200, out), "same bytes on another channel are new");
    const uint8_t c7[] = {7, 0x01, 1};
    const uint8_t rc[] = {7, 0x01, 0, 1};
    cache.store(CHANNEL_COMMAND, c7, sizeof(c7), rc, 300);
    CHECK(cache.find(CHANNEL_CONFIG, w, sizeof(w), 400, out), "a command with the same seq does not evict a record");
    cache.clear();
    CHECK(!cache.find(CHANNEL_COMMAND, c7, sizeof(c7), 400, out), "clear forgets everything");
  }

  // ---- SHA-256 and HMAC-SHA256 (FIPS 180-4, RFC 4231) ----
  {
    using namespace esphome::precision_feeder::crypto;
    auto hex = [](const char *h, uint8_t *out) {
      for (size_t i = 0; h[2 * i]; i++) {
        unsigned v;
        std::sscanf(h + 2 * i, "%2x", &v);
        out[i] = static_cast<uint8_t>(v);
      }
    };
    uint8_t want[32], got[32];
    sha256(nullptr, 0, got);
    hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", want);
    CHECK(std::memcmp(got, want, 32) == 0, "sha256 empty");
    sha256(reinterpret_cast<const uint8_t *>("abc"), 3, got);
    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", want);
    CHECK(std::memcmp(got, want, 32) == 0, "sha256 abc");
    const char *m448 = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha256(reinterpret_cast<const uint8_t *>(m448), std::strlen(m448), got);
    hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1", want);
    CHECK(std::memcmp(got, want, 32) == 0, "sha256 448-bit message (two blocks)");
    {
      Sha256 h;
      uint8_t a[1000];
      std::memset(a, 'a', sizeof(a));
      for (int i = 0; i < 1000; i++)
        h.update(a, sizeof(a));
      h.finish(got);
      hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0", want);
      CHECK(std::memcmp(got, want, 32) == 0, "sha256 one million 'a'");
    }
    uint8_t k1[20];
    std::memset(k1, 0x0b, sizeof(k1));
    hmac_sha256(k1, sizeof(k1), reinterpret_cast<const uint8_t *>("Hi There"), 8, got);
    hex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", want);
    CHECK(std::memcmp(got, want, 32) == 0, "hmac rfc4231 case 1");
    const char *jefe = "what do ya want for nothing?";
    hmac_sha256(reinterpret_cast<const uint8_t *>("Jefe"), 4, reinterpret_cast<const uint8_t *>(jefe),
                std::strlen(jefe), got);
    hex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", want);
    CHECK(std::memcmp(got, want, 32) == 0, "hmac rfc4231 case 2");
    uint8_t k6[131];
    std::memset(k6, 0xaa, sizeof(k6));
    const char *m6 = "Test Using Larger Than Block-Size Key - Hash Key First";
    hmac_sha256(k6, sizeof(k6), reinterpret_cast<const uint8_t *>(m6), std::strlen(m6), got);
    hex("60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54", want);
    CHECK(std::memcmp(got, want, 32) == 0, "hmac rfc4231 case 6 (key longer than a block)");
    const uint8_t x[] = {1, 2, 3}, y[] = {1, 2, 4};
    CHECK(equal_ct(x, x, 3) && !equal_ct(x, y, 3), "constant-time compare");
  }

  // ---- Authentication golden vectors (shared with tools/test_precision_feeder_ble.py) ----
  {
    using namespace esphome::precision_feeder::crypto;
    uint8_t key[KEY_SIZE], nonce[NONCE_SIZE];
    for (size_t i = 0; i < KEY_SIZE; i++)
      key[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < NONCE_SIZE; i++)
      nonce[i] = static_cast<uint8_t>(0xA0 + i);
    uint8_t proof[PROOF_SIZE];
    auth_proof(key, nonce, proof);
    const uint8_t want_proof[] = {0xFA, 0xE3, 0x72, 0xF2, 0xC6, 0xD3, 0xC9, 0xD6,
                                  0xEE, 0x0E, 0x49, 0x74, 0x57, 0x4D, 0x6C, 0x7F};
    CHECK(same(proof, want_proof), "auth proof golden bytes");
    uint8_t ks[SESSION_KEY_SIZE];
    session_key(key, nonce, ks);
    const uint8_t want_ks[] = {0x0C, 0x31, 0x22, 0xB4, 0xB9, 0xCC, 0x94, 0x02, 0xF4, 0x17, 0x94,
                               0x4D, 0xDA, 0x88, 0x31, 0x2C, 0x46, 0x47, 0x9E, 0x89, 0xD2, 0x30,
                               0xC4, 0x86, 0x50, 0x69, 0x35, 0x5A, 0xAD, 0xF3, 0x91, 0xC3};
    CHECK(same(ks, want_ks), "session key golden bytes");
    uint8_t tag[TAG_SIZE];
    const uint8_t feed[] = {5, 0x01, 0x01};
    write_tag(ks, CHANNEL_COMMAND, feed, sizeof(feed), tag);
    const uint8_t want_feed_tag[] = {0x2A, 0x8F, 0x78, 0x9B, 0x5E, 0xD3, 0x34, 0xF6};
    CHECK(same(tag, want_feed_tag), "FEED seq 5 tag golden bytes");
    const uint8_t cfg[] = {6, 0x05, 0x03, 0x00, 0x00, 0x00};
    write_tag(ks, CHANNEL_CONFIG, cfg, sizeof(cfg), tag);
    const uint8_t want_cfg_tag[] = {0xC4, 0x79, 0xCE, 0xE3, 0x22, 0x0F, 0xCE, 0x8B};
    CHECK(same(tag, want_cfg_tag), "Config write seq 6 tag golden bytes");

    // Sequence numbers: 1..255, wrapping, window 127.
    CHECK(seq_is_newer(5, 6) && seq_is_newer(5, 132) && !seq_is_newer(5, 133), "window of 127 ahead");
    CHECK(!seq_is_newer(5, 5) && !seq_is_newer(5, 4) && !seq_is_newer(5, 0), "not equal, not behind, never 0");
    CHECK(seq_is_newer(255, 1) && seq_is_newer(250, 3), "wraps from 255 to 1");
  }

  // ---- Session ----
  {
    using namespace esphome::precision_feeder::crypto;
    uint8_t key[KEY_SIZE], nonce[NONCE_SIZE], proof[PROOF_SIZE], ks[SESSION_KEY_SIZE];
    for (size_t i = 0; i < KEY_SIZE; i++)
      key[i] = static_cast<uint8_t>(i);
    for (size_t i = 0; i < NONCE_SIZE; i++)
      nonce[i] = static_cast<uint8_t>(0xA0 + i);
    auth_proof(key, nonce, proof);
    session_key(key, nonce, ks);
    Session s;
    s.set_key(key);
    s.start(nonce);
    uint8_t detail;
    uint8_t w[12] = {5, 0x01, 0x01};
    write_tag(ks, CHANNEL_COMMAND, w, 3, w + 3);
    CHECK(s.check(CHANNEL_COMMAND, w, 11, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_NOT_AUTHENTICATED,
          "nothing accepted before AUTH");
    uint8_t bad[PROOF_SIZE];
    std::memcpy(bad, proof, sizeof(bad));
    bad[0] ^= 1;
    CHECK(s.authenticate(1, bad, sizeof(bad), 1000, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_BAD_PROOF,
          "wrong proof refused");
    CHECK(s.authenticate(1, proof, sizeof(proof), 1500, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_TOO_SOON,
          "even the right proof waits a second after a failure");
    CHECK(s.authenticate(1, proof, sizeof(proof), 2000, detail) == RES_OK && s.authenticated(), "AUTH accepted");
    CHECK(s.check(CHANNEL_COMMAND, w, 11, detail) == RES_OK, "tagged FEED with seq 5 after AUTH seq 1");
    CHECK(s.check(CHANNEL_CONFIG, w, 11, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_BAD_TAG,
          "the tag binds the channel");
    uint8_t w2[11];
    std::memcpy(w2, w, 11);
    w2[2] = 2;  // FEED 2 with FEED 1's tag
    CHECK(s.check(CHANNEL_COMMAND, w2, 11, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_BAD_TAG,
          "changed bytes break the tag");
    s.accept(5);
    CHECK(s.check(CHANNEL_COMMAND, w, 11, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_STALE_SEQ,
          "the same seq again is stale (a retry is answered from the retry cache instead)");
    CHECK(s.check(CHANNEL_COMMAND, w, 8, detail) == RES_NOT_AUTHORIZED && detail == AUTH_DETAIL_TOO_SHORT,
          "too short for a tag");
    uint8_t other[NONCE_SIZE] = {};
    s.start(other);
    CHECK(!s.authenticated() && s.check(CHANNEL_COMMAND, w, 11, detail) == RES_NOT_AUTHORIZED,
          "a new nonce (new connection) ends the session");
    CHECK(s.authenticate(1, proof, sizeof(proof), 5000, detail) == RES_NOT_AUTHORIZED,
          "a proof for the old nonce does not work on the new one");
  }

  if (failures == 0)
    std::printf("All protocol tests passed\n");
  return failures == 0 ? 0 : 1;
}
