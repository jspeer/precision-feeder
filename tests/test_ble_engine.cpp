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

/// A feeder, an engine with a known key, and a client that knows it; connected but not yet authenticated.
struct Rig {
  Flash flash;
  std::unique_ptr<FakeFeeder> feeder;
  FakeTransport transport;
  std::unique_ptr<BleEngine> engine;
  std::unique_ptr<Client> client;
  explicit Rig(bool clock_valid = true) {
    this->feeder = boot(this->flash, T0, clock_valid);
    this->engine = std::make_unique<BleEngine>(*this->feeder, this->transport);
    uint8_t key[crypto::KEY_SIZE];
    for (size_t i = 0; i < sizeof(key); i++)
      key[i] = static_cast<uint8_t>(i * 7 + 3);
    this->engine->set_key(key);
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

int main() {
  test_before_auth();
  test_auth_flow();
  test_feed_retry_and_replay();
  test_signing_rules();
  test_commands();
  test_set_time_policy();
  test_records();
  test_disconnect();
  if (failures == 0)
    std::printf("All BLE engine tests passed\n");
  return failures == 0 ? 0 : 1;
}
