// Host-side tests for the BLE protocol engine (components/precision_feeder_ble/engine.*): authentication, sequence
// numbers, retries, every command and record handler, Status and the event log, against the simulated feeder.
// Build and run from tests/ (one line; tools/check.sh does it):
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder -I../components/precision_feeder_ble
//   test_ble_engine.cpp ../components/precision_feeder_ble/engine.cpp ../components/precision_feeder/feeder_*.cpp
//   -o /tmp/test_ble_engine && /tmp/test_ble_engine
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

#include "engine.h"
#include "fake_feeder.h"

static int failures = 0;
static const char *current = "";
#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL [%s]: %s (line %d)\n", current, msg, __LINE__); \
      failures++;                                                         \
    }                                                                     \
  } while (0)

using Bytes = std::vector<uint8_t>;

/// Records what would have gone to each characteristic.
class FakeTransport : public BleTransport {
 public:
  void set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) override {
    this->value[channel] = Bytes(data, data + n);
    if (notify)
      this->notified[channel]++;
  }
  void random(uint8_t *out, size_t n) override {
    for (size_t i = 0; i < n; i++)
      out[i] = static_cast<uint8_t>(this->counter_++ * 37 + 11);  // distinct each time; the tests read it back
  }
  std::map<uint8_t, Bytes> value;
  std::map<uint8_t, int> notified;

 protected:
  uint32_t counter_{1};
};

/// A protocol-2 client, written from docs/PROTOCOL.md, driving the engine directly.
struct Client {
  BleEngine &engine;
  FakeTransport &transport;
  uint8_t key[crypto::KEY_SIZE];
  uint8_t session[crypto::SESSION_KEY_SIZE]{};
  uint8_t seq{0};

  uint8_t next() { return this->seq = static_cast<uint8_t>(this->seq % 255 + 1); }
  Bytes response() const { return this->transport.value.at(proto::CHANNEL_RESPONSE); }
  Bytes write(uint8_t channel, const Bytes &bytes) {
    this->engine.on_write(channel, bytes.data(), bytes.size());
    return this->response();
  }
  Bytes auth_bytes() {
    const Bytes nonce = this->transport.value.at(proto::CHANNEL_AUTH);
    uint8_t proof[crypto::PROOF_SIZE];
    crypto::auth_proof(this->key, nonce.data(), proof);
    crypto::session_key(this->key, nonce.data(), this->session);
    Bytes b{this->next(), proto::OP_AUTH};
    b.insert(b.end(), proof, proof + sizeof(proof));
    return b;
  }
  Bytes signed_(uint8_t channel, Bytes body) const {
    uint8_t tag[crypto::TAG_SIZE];
    crypto::write_tag(this->session, channel, body.data(), body.size(), tag);
    body.insert(body.end(), tag, tag + sizeof(tag));
    return body;
  }
  Bytes command_bytes(uint8_t opcode, const Bytes &args = {}) {
    Bytes body{this->next(), opcode};
    body.insert(body.end(), args.begin(), args.end());
    return this->signed_(proto::CHANNEL_COMMAND, body);
  }
  Bytes command(uint8_t opcode, const Bytes &args = {}) {
    return this->write(proto::CHANNEL_COMMAND, this->command_bytes(opcode, args));
  }
  Bytes record(uint8_t channel, const Bytes &rec) {
    Bytes body{this->next()};
    body.insert(body.end(), rec.begin(), rec.end());
    return this->write(channel, this->signed_(channel, body));
  }
};

/// A feeder, an engine with a known key (the build's, unless `build_key` is false: no key at all), and a client that
/// knows it; connected but not yet authenticated.
struct Rig {
  Flash flash;
  std::unique_ptr<FakeFeeder> feeder;
  FakeTransport transport;
  std::unique_ptr<BleEngine> engine;
  std::unique_ptr<Client> client;
  explicit Rig(bool clock_valid = true, bool build_key = true) {
    this->feeder = boot(this->flash, T0, clock_valid);
    this->engine = std::make_unique<BleEngine>(*this->feeder, this->transport);
    uint8_t key[crypto::KEY_SIZE];
    for (size_t i = 0; i < sizeof(key); i++)
      key[i] = static_cast<uint8_t>(i * 7 + 3);
    if (build_key) {
      this->feeder->set_build_ble_key(key);
    } else {
      const uint8_t none[DEVICE_KEY_SIZE] = {};
      this->feeder->set_build_ble_key(none);  // a BLE build with no `key:`, as the prebuilt binaries ship
    }
    this->feeder->set_feed_listener([this](const FeedRecord &r) {
      this->feeder->records.push_back(r);
      this->engine->on_feed(r);
    });
    this->engine->start();
    this->engine->on_connect();
    this->client.reset(new Client{*this->engine, this->transport, {}, {}, 0});
    std::memcpy(this->client->key, key, sizeof(key));
  }
  void advance(int64_t ms) {
    this->feeder->advance(ms);
    this->engine->loop(static_cast<uint32_t>(this->feeder->uptime()));
  }
};

static Bytes u32(uint32_t v) { return {uint8_t(v), uint8_t(v >> 8), uint8_t(v >> 16), uint8_t(v >> 24)}; }
static Bytes i32(int32_t v) { return u32(static_cast<uint32_t>(v)); }
static Bytes cat(Bytes a, const Bytes &b) {
  a.insert(a.end(), b.begin(), b.end());
  return a;
}

// ---- tests -------------------------------------------------------------------------------------------

static void test_before_auth() {
  current = "before AUTH";
  Rig r;
  const Bytes info = r.transport.value.at(proto::CHANNEL_INFO);
  CHECK(info.size() == 6 && info[0] == 2 && (proto::get_u16(info.data() + 1) & proto::CAP_AUTH), "Info: protocol 2, auth bit");
  CHECK(r.transport.value.at(proto::CHANNEL_AUTH).size() == 16, "Auth: a 16-byte nonce");
  CHECK(r.transport.value.at(proto::CHANNEL_STATUS) == Bytes(20, 0), "Status reads as zeros");
  Bytes resp = r.client->write(proto::CHANNEL_COMMAND, {9, proto::OP_FEED, 1});
  CHECK(resp == (Bytes{9, proto::OP_FEED, proto::RES_NOT_AUTHORIZED, proto::AUTH_DETAIL_NOT_AUTHENTICATED}),
        "unsigned command refused, not authenticated");
  resp = r.client->write(proto::CHANNEL_CONFIG, Bytes(14, 0x85));
  CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == 0, "record write refused");
  r.advance(2000);
  CHECK(r.transport.notified[proto::CHANNEL_STATUS] == 0, "Status not notified");
  r.feeder->request_feed(1, Source::ESPHOME);  // a feed from Home Assistant
  CHECK(r.transport.notified[proto::CHANNEL_EVENT_LOG] == 0, "event log not notified");
  CHECK(r.feeder->position() == 0 || r.feeder->tray_state() == TrayState::MOVING, "(the ESPHome feed itself runs)");
}

static void test_auth_flow() {
  current = "AUTH";
  Rig r;
  const Bytes nonce1 = r.transport.value.at(proto::CHANNEL_AUTH);
  uint8_t good_key[crypto::KEY_SIZE];
  std::memcpy(good_key, r.client->key, sizeof(good_key));
  r.client->key[0] ^= 1;  // wrong key
  Bytes resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == proto::AUTH_DETAIL_BAD_PROOF, "wrong key refused");
  CHECK(r.transport.value.at(proto::CHANNEL_AUTH) != nonce1, "nonce changes after a failed AUTH");
  std::memcpy(r.client->key, good_key, sizeof(good_key));
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[3] == proto::AUTH_DETAIL_TOO_SOON, "right key refused within a second of a failure");
  r.advance(1100);
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[2] == proto::RES_OK && r.engine->authorized(), "right key accepted after the delay");
  r.advance(600);
  CHECK(r.transport.notified[proto::CHANNEL_STATUS] >= 1 && r.transport.value.at(proto::CHANNEL_STATUS) != Bytes(20, 0),
        "Status sent after AUTH");
}

static void test_feed_retry_and_replay() {
  current = "feed, retry, replay";
  Rig r;
  const Bytes auth = r.client->auth_bytes();
  CHECK(r.client->write(proto::CHANNEL_COMMAND, auth)[2] == proto::RES_OK, "authenticated");
  const Bytes feed = r.client->command_bytes(proto::OP_FEED, {1});
  Bytes resp = r.client->write(proto::CHANNEL_COMMAND, feed);
  CHECK(resp[2] == proto::RES_OK && resp[3] == 1, "FEED 1 accepted");
  r.advance(5000);
  CHECK(r.feeder->position() == 1, "fed once");
  CHECK(r.transport.notified[proto::CHANNEL_EVENT_LOG] == 1, "one event-log record");
  // A retry (same bytes) is answered from the cache and does not feed again.
  CHECK(r.client->write(proto::CHANNEL_COMMAND, feed) == resp, "exact retry: same answer");
  r.advance(65000);
  CHECK(r.feeder->position() == 1, "retry did not feed");
  // The bug the owner found: a repeated AUTH must not make the old FEED new again, even after the cache expires.
  CHECK(r.client->write(proto::CHANNEL_COMMAND, auth)[2] == proto::RES_OK, "repeated AUTH answered OK");
  r.advance(31000);
  resp = r.client->write(proto::CHANNEL_COMMAND, feed);
  CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == proto::AUTH_DETAIL_STALE_SEQ, "old FEED refused as stale");
  r.advance(5000);
  CHECK(r.feeder->position() == 1, "and did not feed a second time");
}

static void test_signing_rules() {
  current = "signing rules";
  Rig r;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  Bytes tampered = r.client->command_bytes(proto::OP_SET_PROFILE, {0});
  tampered[2] ^= 1;
  CHECK(r.client->write(proto::CHANNEL_COMMAND, tampered)[3] == proto::AUTH_DETAIL_BAD_TAG, "tampered write refused");
  Bytes body{r.client->next(), proto::OP_SET_PROFILE, 0};
  CHECK(r.client->write(proto::CHANNEL_COMMAND, r.client->signed_(proto::CHANNEL_CONFIG, body))[3] ==
            proto::AUTH_DETAIL_BAD_TAG,
        "a write signed for another characteristic is refused");
  Bytes short_cmd = r.client->signed_(proto::CHANNEL_COMMAND, {r.client->next()});  // seq and tag, no opcode
  const Bytes resp = r.client->write(proto::CHANNEL_COMMAND, short_cmd);
  CHECK(resp[2] == proto::RES_INVALID_ARG, "a signed command with no opcode is INVALID_ARG");
  CHECK(r.client->command(proto::OP_SET_TIMEZONE)[2] == proto::RES_UNSUPPORTED, "SET_TIMEZONE is UNSUPPORTED");
  CHECK(r.client->command(0x3F)[2] == proto::RES_UNSUPPORTED, "unknown opcode is UNSUPPORTED");
  CHECK(r.client->command(proto::OP_FEED, {1, 0})[2] == proto::RES_INVALID_ARG, "wrong argument length");
}

static void test_commands() {
  current = "commands";
  Rig r;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  Client &c = *r.client;
  CHECK(c.command(proto::OP_SET_PROFILE, {1})[2] == proto::RES_OK && r.feeder->cfg.active_profile == 1, "SET_PROFILE");
  CHECK(c.command(proto::OP_SET_PROFILE, {3})[2] == proto::RES_INVALID_ARG, "SET_PROFILE 3 refused");
  CHECK(c.command(proto::OP_HOLD, u32(30))[2] == proto::RES_OK && r.feeder->hold_active(), "HOLD 30");
  CHECK(c.command(proto::OP_HOLD, u32(0))[2] == proto::RES_OK && !r.feeder->hold_active(), "HOLD cancel");
  CHECK(c.command(proto::OP_HOLD, u32(0xFFFFFFFF))[2] == proto::RES_OK && r.feeder->hold_active(), "HOLD forever");
  c.command(proto::OP_HOLD, u32(0));
  Bytes loc = i32(5150740);
  Bytes lon = i32(-12780);
  loc.insert(loc.end(), lon.begin(), lon.end());
  CHECK(c.command(proto::OP_SET_LOCATION, loc)[2] == proto::RES_OK && r.feeder->cfg.latitude > 51.5f, "SET_LOCATION");
  CHECK(c.command(proto::OP_JOG, {1, 1, 0})[2] == proto::RES_NOT_CALIBRATING, "JOG needs calibration mode");
  CHECK(c.command(proto::OP_CALIBRATE, {1})[2] == proto::RES_OK && r.feeder->calibration_mode(), "CALIBRATE on");
  CHECK(c.command(proto::OP_FEED, {1})[2] == static_cast<uint8_t>(Result::CALIBRATING), "FEED refused while calibrating");
  CHECK(c.command(proto::OP_JOG, {1, 5, 0})[2] == proto::RES_OK && r.feeder->calib.offsets[1] == 5, "JOG +0.5");
  CHECK(c.command(proto::OP_SET_POSITION, {2})[2] == static_cast<uint8_t>(Result::CALIBRATING), "SET_POSITION refused");
  CHECK(c.command(proto::OP_SAVE_CALIBRATION)[2] == proto::RES_OK, "SAVE_CALIBRATION");
  CHECK(c.command(proto::OP_CALIBRATE, {0})[2] == proto::RES_OK && !r.feeder->calibration_mode(), "CALIBRATE off");
  Bytes resp = c.command(proto::OP_SET_POSITION, {6});
  CHECK(resp[2] == proto::RES_OK && resp[3] == 0, "SET_POSITION 6: 0 remaining");
  r.advance(3000);
  CHECK(c.command(proto::OP_FEED, {1})[2] == static_cast<uint8_t>(Result::EMPTY), "FEED refused: empty");
  resp = c.command(proto::OP_TRAY_RELOAD);
  CHECK(resp[2] == proto::RES_OK && resp[3] == 6 && r.feeder->position() == 0, "TRAY_RELOAD");
  CHECK(c.command(proto::OP_STOP)[2] == proto::RES_OK, "STOP");
}

static void test_set_time_policy() {
  current = "SET_TIME";
  Rig r(false);  // clock not set
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  Bytes t = u32(1792000000);
  t.push_back(0);
  t.push_back(0);
  CHECK(r.client->command(proto::OP_SET_TIME, t)[2] == proto::RES_OK, "accepted with no network time");
  CHECK(r.feeder->time_valid() && r.feeder->time_source() == TimeSource::BLE, "clock set, source BLE");
  r.feeder->note_time_sync(TimeSource::NTP);
  CHECK(r.client->command(proto::OP_SET_TIME, t)[2] == proto::RES_IGNORED_NTP_ACTIVE, "ignored while NTP is fresh");
  Bytes early = u32(1000);
  early.push_back(0);
  early.push_back(0);
  CHECK(r.client->command(proto::OP_SET_TIME, early)[2] == proto::RES_INVALID_ARG, "before 2019 refused");
}

static void test_records() {
  current = "records";
  Rig r;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  Client &c = *r.client;
  Bytes resp = c.record(proto::CHANNEL_CONFIG, {proto::CFG_DAILY_CAP, 3, 0, 0, 0});
  CHECK(resp[0] == c.seq && resp[1] == proto::OP_CONFIG_WRITE && resp[2] == proto::RES_OK && r.feeder->cfg.daily_cap == 3,
        "Config write, answered with its seq");
  CHECK(c.record(proto::CHANNEL_CONFIG, {proto::CFG_DAILY_CAP, 7, 0, 0, 0})[2] == proto::RES_INVALID_ARG,
        "out-of-range Config refused");
  c.record(proto::CHANNEL_CONFIG, {proto::CFG_MIN_GAP_S | proto::QUERY_FLAG});
  CHECK(r.transport.value.at(proto::CHANNEL_CONFIG) == (Bytes{proto::CFG_MIN_GAP_S, 60, 0, 0, 0}), "Config query read");
  CHECK(c.record(proto::CHANNEL_CONFIG, {proto::CFG_MIN_PULSE_US, 0x60, 0x09, 0, 0})[2] == proto::RES_INVALID_ARG,
        "pulse pair closer than 500 us refused");
  resp = c.record(proto::CHANNEL_PROFILE, {0x01, 0, 2, 0x05, 0xB4, 0x00, 2, 0x3E});
  const Event &e = r.feeder->cfg.profiles[0].ev[2];
  CHECK(resp[2] == proto::RES_OK && e.enabled && e.anchor == 2 && e.offset_min == 180 && e.portions == 2,
        "Profile event write");
  c.record(proto::CHANNEL_PROFILE, {0x02, 0, 2});
  CHECK(r.transport.value.at(proto::CHANNEL_PROFILE) == (Bytes{0x01, 0, 2, 0x05, 0xB4, 0x00, 2, 0x3E}),
        "Profile query reads the event back");
  CHECK(c.record(proto::CHANNEL_PROFILE, {0x01, 0, 2, 0x01, 0xA0, 0x05, 2, 0x3E})[2] == proto::RES_INVALID_ARG,
        "clock offset 1440 refused");
  resp = c.record(proto::CHANNEL_CALIBRATION, {2, 0xF4, 0xFF});
  CHECK(resp[2] == proto::RES_OK && r.feeder->calib.offsets[2] == -12, "Calibration write");
  c.record(proto::CHANNEL_CALIBRATION, {2 | proto::QUERY_FLAG});
  CHECK(r.transport.value.at(proto::CHANNEL_CALIBRATION) == (Bytes{2, 0xF4, 0xFF}), "Calibration query read");
  // A record retry is answered from the cache.
  Bytes body{c.next(), proto::CFG_DAILY_CAP, 4, 0, 0, 0};
  const Bytes w = c.signed_(proto::CHANNEL_CONFIG, body);
  const Bytes first = c.write(proto::CHANNEL_CONFIG, w);
  r.feeder->cfg.daily_cap = 5;  // changed meanwhile by Home Assistant
  CHECK(c.write(proto::CHANNEL_CONFIG, w) == first && r.feeder->cfg.daily_cap == 5, "record retry not re-applied");
}

static Bytes cfg(uint8_t key, int32_t v) {
  Bytes b{key};
  const Bytes x = i32(v);
  b.insert(b.end(), x.begin(), x.end());
  return b;
}

// The value ranges BLE enforces are FeederCore's (feeder_core.h): each limit and one step past it.
static void test_ranges() {
  current = "ranges";
  Rig r;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  Client &c = *r.client;
  struct Case {
    uint8_t key;
    int32_t lo, hi;
  };
  const Case cases[] = {
      {proto::CFG_MIN_GAP_S, MIN_GAP_S_LO, MIN_GAP_S_HI},
      {proto::CFG_PORTION_GAP_S, PORTION_GAP_S_LO, PORTION_GAP_S_HI},
      {proto::CFG_RELOAD_PAUSE_MIN, RELOAD_PAUSE_MIN_LO, RELOAD_PAUSE_MIN_HI},
      {proto::CFG_REFILL_INTERVAL_DAYS, REFILL_DAYS_LO, REFILL_DAYS_HI},
      {proto::CFG_DAILY_CAP, DAILY_CAP_LO, DAILY_CAP_HI},
      {proto::CFG_MAX_PORTIONS_CMD, PORTIONS_CMD_LO, PORTIONS_CMD_HI},
      {proto::CFG_SCHEDULER_ENABLED, 0, 1},
      {proto::CFG_LATITUDE_E5, -9000000, 9000000},
      {proto::CFG_LONGITUDE_E5, -18000000, 18000000},
  };
  for (const Case &k : cases) {
    char what[128];
    std::snprintf(what, sizeof(what), "Config key %u: %d and %d accepted, %d and %d refused", k.key, k.lo, k.hi,
                  k.lo - 1, k.hi + 1);
    const bool ok = c.record(proto::CHANNEL_CONFIG, cfg(k.key, k.lo))[2] == proto::RES_OK &&
                    c.record(proto::CHANNEL_CONFIG, cfg(k.key, k.hi))[2] == proto::RES_OK &&
                    c.record(proto::CHANNEL_CONFIG, cfg(k.key, k.lo - 1))[2] == proto::RES_INVALID_ARG &&
                    c.record(proto::CHANNEL_CONFIG, cfg(k.key, k.hi + 1))[2] == proto::RES_INVALID_ARG;
    CHECK(ok, what);
  }
  CHECK(c.record(proto::CHANNEL_CONFIG, cfg(12, 1))[2] == proto::RES_INVALID_ARG, "unknown key refused");
  CHECK(c.record(proto::CHANNEL_CONFIG, cfg(proto::CFG_MIN_PULSE_US, PULSE_MIN_LO - 1))[2] == proto::RES_INVALID_ARG,
        "min pulse below its limit");
  CHECK(c.record(proto::CHANNEL_CONFIG, cfg(proto::CFG_MAX_PULSE_US, PULSE_MAX_HI + 1))[2] == proto::RES_INVALID_ARG,
        "max pulse above its limit");
  CHECK(c.record(proto::CHANNEL_CALIBRATION, {6, uint8_t(CAL_OFFSET_HI), 0})[2] == proto::RES_OK &&
            c.record(proto::CHANNEL_CALIBRATION, {6, uint8_t(CAL_OFFSET_HI + 1), 0})[2] == proto::RES_INVALID_ARG &&
            c.record(proto::CHANNEL_CALIBRATION, {7, 0, 0})[2] == proto::RES_INVALID_ARG,
        "calibration offset and index limits");
}

static void test_disconnect() {
  current = "disconnect";
  Rig r;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  r.advance(600);
  CHECK(r.transport.value.at(proto::CHANNEL_STATUS) != Bytes(20, 0), "Status readable while connected");
  r.engine->on_disconnect();
  CHECK(!r.engine->authorized() && r.transport.value.at(proto::CHANNEL_STATUS) == Bytes(20, 0),
        "disconnect ends the session and clears reads");
  r.engine->on_connect();
  CHECK(r.client->command(proto::OP_SET_PROFILE, {0})[3] == proto::AUTH_DETAIL_NOT_AUTHENTICATED,
        "the old session key is useless on a new connection");
}

static bool has(const std::string &text, const char *part) { return text.find(part) != std::string::npos; }

static void test_diagnostics() {
  current = "diagnostics (1.1.0)";
  Rig r;  // connected, not authenticated
  const uint8_t peer[6] = {0x11, 0x22, 0x33, 0x44, 0x4A, 0x21};
  r.engine->set_peer(peer);
  r.engine->set_mtu(185);
  std::string s = r.engine->session_text();
  CHECK(has(s, "connected=1;auth=0;peer=4A:21;mtu=185;since=1;"), "connected, masked peer, MTU, since");
  CHECK(!has(s, "11:22") && !has(s, "33:44"), "only the last two address bytes");
  CHECK(r.engine->info_text() == "protocol=2;auth=hmac-sha256;time=fallback_only;stale_h=24;key=build;guard=1;"
                                 "calibration=1", "info");

  // A refused write, a bad AUTH, a good AUTH, a feed, an invalid command, a retry, a record and a read.
  r.client->write(proto::CHANNEL_COMMAND, {9, proto::OP_FEED, 1});  // unsigned: refused
  r.client->key[0] ^= 1;
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());  // wrong key
  r.client->key[0] ^= 1;
  r.advance(1100);
  r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  const Bytes feed = r.client->command_bytes(proto::OP_FEED, {1});
  r.client->write(proto::CHANNEL_COMMAND, feed);
  r.client->write(proto::CHANNEL_COMMAND, feed);  // an exact retry
  r.client->command(proto::OP_SET_PROFILE, {9});   // invalid profile
  r.client->command(0x7E);                         // unknown opcode
  r.client->record(proto::CHANNEL_CONFIG, {proto::CFG_DAILY_CAP | proto::QUERY_FLAG});
  r.engine->on_read(proto::CHANNEL_STATUS, 20);
  const BleStats &st = r.engine->stats();
  CHECK(st.connections == 1, "connections");
  CHECK(st.writes == 8, "every write counted");
  CHECK(st.reads == 1 && st.read_bytes == 20, "reads");
  CHECK(st.auth_ok == 1 && st.auth_failures == 1, "AUTH results");
  CHECK(st.refused == 1, "not authorized");
  CHECK(st.invalid == 2, "invalid argument and unsupported");
  CHECK(st.notifications >= 8 && st.notify_bytes >= 32, "notifications counted (Response at least)");

  // The activity log: connection, then every write with its answer; reads are only counted.
  CHECK(r.engine->activity_count() == 9, "nine entries");
  CHECK(r.engine->activity(0).kind == ActivityKind::CONNECT, "connect first");
  CHECK(r.engine->activity(1).kind == ActivityKind::COMMAND && r.engine->activity(1).result == proto::RES_NOT_AUTHORIZED,
        "refused command");
  CHECK(r.engine->activity(3).kind == ActivityKind::AUTH && r.engine->activity(3).result == proto::RES_OK, "AUTH ok");
  CHECK(r.engine->activity(5).kind == ActivityKind::REPLAY && r.engine->activity(5).code == proto::OP_FEED, "retry");
  CHECK(r.engine->activity(8).kind == ActivityKind::RECORD && r.engine->activity(8).code == proto::OP_CONFIG_WRITE,
        "record");
  CHECK(r.engine->activity_last_id() == 9, "ids count from 1");
  const std::string json = r.engine->activity_json();
  CHECK(json.rfind("{\"entries\":[{\"id\":1,\"t\":1,\"kind\":\"connect\"}", 0) == 0, "JSON starts with the connection");
  CHECK(has(json, "\"kind\":\"command\",\"name\":\"FEED\",\"seq\":3,\"result\":\"ok\",\"detail\":1}"), "the feed");
  CHECK(has(json, "\"name\":\"0x7E\"") && has(json, "\"result\":\"unsupported\""), "unknown opcode by number");
  CHECK(has(json, "\"result\":\"not authorized\",\"detail\":0}"), "refusal named");
  CHECK(json.substr(json.size() - 2) == "]}", "JSON closes");
  s = r.engine->session_text();
  CHECK(has(s, "auth=1;") && has(s, "conns=1;wr=8;") && has(s, "authok=1;authfail=1;refused=1;invalid=2"), "counters");
  CHECK(s.size() < 255, "fits Home Assistant's state");

  // The ring keeps the newest entries; a disconnect forgets the peer and resets the MTU on the next connection.
  for (int i = 0; i < 40; i++)
    r.client->command(proto::OP_STOP);
  CHECK(r.engine->activity_count() == BleEngine::ACTIVITY_SIZE, "ring full");
  CHECK(r.engine->activity(0).id == r.engine->activity_last_id() - BleEngine::ACTIVITY_SIZE + 1, "oldest dropped");
  r.engine->on_disconnect();
  CHECK(r.engine->activity(BleEngine::ACTIVITY_SIZE - 1).kind == ActivityKind::DISCONNECT, "disconnect logged");
  CHECK(has(r.engine->session_text(), "connected=0;auth=0;peer=;mtu=185;"), "disconnected");
  r.engine->on_connect();
  CHECK(has(r.engine->session_text(), "connected=1;auth=0;peer=;mtu=23;") && r.engine->stats().connections == 2,
        "new connection");
  CHECK(BleEngine::opcode_name(proto::OP_CALIBRATION_WRITE) == "CALIBRATION_WRITE", "record opcode names");
}

static void test_keys() {
  current = "BLE key on the device (2.0.0)";
  Rig r(true, false);  // no key anywhere
  CHECK(r.feeder->ble_key_source() == KeySource::NONE, "no key");
  Bytes resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == proto::AUTH_DETAIL_NO_KEY, "AUTH refused: no key (6)");
  std::memset(r.client->key, 0, sizeof(r.client->key));
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[3] == proto::AUTH_DETAIL_NO_KEY && !r.engine->authorized(), "the all-zero key is not a key");
  CHECK(has(r.engine->info_text(), ";key=none;"), "info: no key");

  uint8_t zero[DEVICE_KEY_SIZE] = {}, key[DEVICE_KEY_SIZE];
  CHECK(r.feeder->set_ble_key(zero) == Result::INVALID_ARG, "the all-zero key is refused");
  for (size_t i = 0; i < sizeof(key); i++)
    key[i] = static_cast<uint8_t>(0xA0 + i);
  CHECK(r.feeder->set_ble_key(key) == Result::OK && r.feeder->ble_key_source() == KeySource::DEVICE, "key saved");
  r.advance(10);
  std::memcpy(r.client->key, key, sizeof(key));
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[2] == proto::RES_OK && r.engine->authorized(), "AUTH with the device's key");
  CHECK(r.client->command(proto::OP_STOP)[2] == proto::RES_OK, "a signed command works");

  // Changing the key ends the session: the old session key no longer signs anything.
  const Bytes old_nonce = r.transport.value.at(proto::CHANNEL_AUTH);
  key[0] ^= 0xFF;
  r.feeder->set_ble_key(key);
  r.advance(10);
  CHECK(!r.engine->authorized() && r.transport.value.at(proto::CHANNEL_AUTH) != old_nonce, "session ended, new nonce");
  resp = r.client->command(proto::OP_STOP);
  CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == proto::AUTH_DETAIL_NOT_AUTHENTICATED, "old session refused");
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[3] == proto::AUTH_DETAIL_BAD_PROOF, "the old key is refused");
  r.advance(1100);  // past the wait after a failed AUTH
  std::memcpy(r.client->key, key, sizeof(key));
  resp = r.client->write(proto::CHANNEL_COMMAND, r.client->auth_bytes());
  CHECK(resp[2] == proto::RES_OK, "the new key is accepted");

  // A key saved on the device wins over the build's, and survives a restart.
  Rig b;  // build key i * 7 + 3
  CHECK(b.feeder->ble_key_source() == KeySource::BUILD, "build key");
  CHECK(b.feeder->set_ble_key(key) == Result::OK && b.feeder->ble_key_source() == KeySource::DEVICE, "device wins");
  auto again = boot(b.flash, T0 + 60, true);
  uint8_t build[DEVICE_KEY_SIZE];
  for (size_t i = 0; i < sizeof(build); i++)
    build[i] = static_cast<uint8_t>(i * 7 + 3);
  again->set_build_ble_key(build);
  uint8_t in_use[DEVICE_KEY_SIZE];
  CHECK(again->ble_key(in_use) && std::memcmp(in_use, key, sizeof(key)) == 0, "kept across a restart");

  Flash flash;
  auto no_ble = boot(flash, T0);  // set_build_ble_key() never called: a build without BLE
  CHECK(no_ble->set_ble_key(key) == Result::UNSUPPORTED, "no BLE in the build");
}

static void test_permissions() {
  current = "BLE permissions (2.0.0)";
  Rig r;
  Client &c = *r.client;
  c.write(proto::CHANNEL_COMMAND, c.auth_bytes());
  r.feeder->set_ble_may_change_guard(false);
  r.feeder->set_ble_may_change_calibration(false);
  CHECK(has(r.engine->info_text(), ";guard=0;calibration=0"), "info shows both off");
  const uint32_t refused = r.engine->stats().refused;

  Bytes resp = c.record(proto::CHANNEL_CONFIG, cat({proto::CFG_DAILY_CAP}, i32(3)));
  CHECK(resp[1] == proto::OP_CONFIG_WRITE && resp[2] == proto::RES_NOT_AUTHORIZED &&
            resp[3] == proto::AUTH_DETAIL_SETTING_OFF, "guard limit refused (7)");
  for (uint8_t key : {proto::CFG_MIN_GAP_S, proto::CFG_PORTION_GAP_S, proto::CFG_MAX_PORTIONS_CMD})
    CHECK(c.record(proto::CHANNEL_CONFIG, cat({key}, i32(5)))[3] == proto::AUTH_DETAIL_SETTING_OFF, "each guard key");
  CHECK(c.record(proto::CHANNEL_CONFIG, cat({proto::CFG_RELOAD_PAUSE_MIN}, i32(5)))[2] == proto::RES_OK,
        "reload pause is not a guard limit");
  CHECK(c.record(proto::CHANNEL_CONFIG, {uint8_t(proto::CFG_DAILY_CAP | proto::QUERY_FLAG)})[2] == proto::RES_OK,
        "queries still answered");
  CHECK(c.record(proto::CHANNEL_CONFIG, cat({proto::CFG_MIN_PULSE_US}, i32(600)))[3] == proto::AUTH_DETAIL_SETTING_OFF,
        "pulse range is calibration");
  CHECK(c.record(proto::CHANNEL_CALIBRATION, {2, 0xF4, 0xFF})[3] == proto::AUTH_DETAIL_SETTING_OFF, "offset refused");
  CHECK(c.record(proto::CHANNEL_CALIBRATION, {uint8_t(2 | proto::QUERY_FLAG)})[2] == proto::RES_OK, "offset query");
  for (uint8_t op : {proto::OP_CALIBRATE, proto::OP_JOG, proto::OP_SAVE_CALIBRATION}) {
    const Bytes args = op == proto::OP_CALIBRATE ? Bytes{1} : op == proto::OP_JOG ? Bytes{1, 5, 0} : Bytes{};
    resp = c.command(op, args);
    CHECK(resp[2] == proto::RES_NOT_AUTHORIZED && resp[3] == proto::AUTH_DETAIL_SETTING_OFF, "calibration command");
  }
  CHECK(!r.feeder->calibration_mode(), "calibration mode stayed off");
  CHECK(c.command(proto::OP_SET_POSITION, {0})[2] == proto::RES_OK, "SET_POSITION is everyday recovery");
  CHECK(c.command(proto::OP_FEED, {1})[2] == proto::RES_OK, "feeding is unaffected");
  CHECK(r.engine->stats().refused == refused + 9, "counted as refused");

  // A retry of a refused write is answered from the cache, and a re-enabled setting works with the next seq.
  const Bytes w = c.signed_(proto::CHANNEL_CONFIG, Bytes{c.next(), proto::CFG_DAILY_CAP, 4, 0, 0, 0});
  CHECK(c.write(proto::CHANNEL_CONFIG, w)[3] == proto::AUTH_DETAIL_SETTING_OFF, "refused");
  r.feeder->set_ble_may_change_guard(true);
  CHECK(c.write(proto::CHANNEL_CONFIG, w)[3] == proto::AUTH_DETAIL_SETTING_OFF, "the retry gets the same answer");
  CHECK(c.record(proto::CHANNEL_CONFIG, cat({proto::CFG_DAILY_CAP}, i32(4)))[2] == proto::RES_OK &&
            r.feeder->cfg.daily_cap == 4, "allowed again");
}

int main() {
  test_before_auth();
  test_auth_flow();
  test_feed_retry_and_replay();
  test_signing_rules();
  test_commands();
  test_set_time_policy();
  test_records();
  test_ranges();
  test_disconnect();
  test_diagnostics();
  test_keys();
  test_permissions();
  if (failures == 0)
    std::printf("All BLE engine tests passed\n");
  return failures == 0 ? 0 : 1;
}
