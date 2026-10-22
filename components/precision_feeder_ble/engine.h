// Precision Feeder BLE protocol engine: everything the BLE protocol does, as plain C++.
//
// Authentication, sequence numbers and the retry cache, every command and record handler, Status and the event
// log live here, on top of FeederCore. It talks to the radio only through BleTransport (set a characteristic's
// value, optionally notify; random bytes; log), so the whole protocol runs in host tests against the simulated
// feeder (tests/test_ble_engine.cpp). PrecisionFeederBle is the thin ESPHome adapter that implements the transport
// with real GATT characteristics.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#if __has_include("esphome/components/precision_feeder/feeder_core.h")
#include "esphome/components/precision_feeder/feeder_core.h"
#else
#include "feeder_core.h"  // host tests: -I components/precision_feeder
#endif

#include "protocol.h"

namespace esphome {
namespace precision_feeder {

/// What the engine needs from the BLE stack. Characteristics are named by their channel (the low byte of the UUID,
/// proto::CHANNEL_*).
class BleTransport {
 public:
  virtual ~BleTransport() = default;
  /// Set a characteristic's readable value; with `notify`, also send it to the subscribed client.
  virtual void set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) = 0;
  /// Fill `out` from a cryptographically strong random source.
  virtual void random(uint8_t *out, size_t n) = 0;
  virtual void log(LogLevel /*level*/, const char * /*message*/) {}
};

/// Counters since boot, for diagnostics (the web app's Bluetooth page). Not part of the protocol.
struct BleStats {
  uint32_t connections{0};
  uint32_t writes{0};         // writes received: commands, AUTH and records
  uint32_t write_bytes{0};
  uint32_t reads{0};          // reads of any characteristic, as the transport reports them
  uint32_t read_bytes{0};
  uint32_t notifications{0};  // Response, Status and Event log notifications sent
  uint32_t notify_bytes{0};
  uint32_t auth_ok{0};
  uint32_t auth_failures{0};
  uint32_t refused{0};  // writes refused as not authorized: no AUTH yet, bad tag, stale sequence number, too short,
                        // or a setting turned off for BLE
  uint32_t invalid{0};  // writes answered INVALID_ARG or UNSUPPORTED
};

/// What an activity entry records.
enum class ActivityKind : uint8_t { CONNECT = 0, DISCONNECT = 1, AUTH = 2, COMMAND = 3, RECORD = 4, REPLAY = 5 };

/// One entry of the activity log: connections and every write with its answer. Reads and Status notifications are
/// only counted (BleStats), so a client that polls cannot push everything else out of the log.
struct BleActivity {
  uint32_t id;        // 1, 2, 3, ... (0 = unused slot)
  uint32_t uptime_s;  // when, in seconds since boot
  ActivityKind kind;
  uint8_t code;  // COMMAND, RECORD, REPLAY: the opcode (records: OP_PROFILE_WRITE and so on)
  uint8_t seq;
  uint8_t result;
  uint8_t detail;
};

class BleEngine {
 public:
  static constexpr uint32_t STATUS_PERIOD_MS = 500;
  static constexpr uint8_t ACTIVITY_SIZE = 32;
  static constexpr uint16_t DEFAULT_MTU = 23;

  /// The key comes from the feeder (FeederCore::ble_key(): saved on the device, or the build's), read at each AUTH.
  BleEngine(FeederCore &feeder, BleTransport &transport) : feeder_(feeder), transport_(transport) {}

  void set_time_policy(proto::TimePolicy policy) { this->time_policy_ = policy; }
  void set_network_time_stale_ms(uint32_t ms) { this->network_stale_ms_ = ms; }
  proto::TimePolicy time_policy() const { return this->time_policy_; }
  uint32_t network_time_stale_ms() const { return this->network_stale_ms_; }

  /// Once at start-up: publish Info, clear everything else, issue the first nonce.
  void start();
  /// A client connected (one at a time): new nonce, nothing authorized yet, empty retry cache.
  void on_connect();
  /// The client went away: the session ends.
  void on_disconnect();
  /// A write to the Command, Profile, Config or Calibration characteristic.
  void on_write(uint8_t channel, const uint8_t *data, size_t n);
  /// Call every main-loop pass: sends Status when it changed (at most twice a second).
  void loop(uint32_t now_ms);
  /// Wired to FeederCore's feed listener: one Event log notification per feed request.
  void on_feed(const FeedRecord &record);

  bool connected() const { return this->connected_; }
  bool authorized() const { return this->connected_ && this->session_.authenticated(); }
  uint16_t capabilities() const;

  // ---- diagnostics (read-only for the protocol) ----
  /// The connected client's Bluetooth address, as the stack reports it (6 bytes, most significant first).
  void set_peer(const uint8_t address[6]);
  /// The ATT MTU agreed with the client (23 until the client negotiates).
  void set_mtu(uint16_t mtu) { this->mtu_ = mtu; }
  /// A client read `size` bytes of a characteristic.
  void on_read(uint8_t channel, size_t size);
  const BleStats &stats() const { return this->stats_; }
  /// Activity entry `i`, oldest first (i < activity_count()).
  const BleActivity &activity(uint8_t i) const;
  uint8_t activity_count() const { return this->activity_count_; }
  /// Id of the newest activity entry (0 if none); changes whenever an entry is added.
  uint32_t activity_last_id() const { return this->next_activity_id_ - 1; }
  /// Facts for diagnostics: "protocol=2;auth=hmac-sha256;time=fallback_only;stale_h=24;key=device;guard=1;
  /// calibration=1" (key: none, build or device; guard and calibration: the two BLE permissions). Never the key.
  std::string info_text() const;
  /// The session and the counters in one line, "connected=1;auth=1;peer=4A:21;mtu=23;since=12;last=40;conns=1;..."
  /// (times in seconds since boot; only the last two bytes of the address). Under 255 characters.
  std::string session_text() const;
  /// The activity log as JSON, oldest first: {"entries":[{"id":1,"t":12,"kind":"command","name":"FEED","seq":5,
  /// "result":"ok","detail":0},...]}.
  std::string activity_json() const;
  /// The protocol's name for an opcode ("FEED", ...), or "0x" and its hex value.
  static std::string opcode_name(uint8_t opcode);

 protected:
  void notify_(uint8_t channel, const uint8_t *data, size_t n);
  void add_activity_(ActivityKind kind, uint8_t code = 0, uint8_t seq = 0, uint8_t result = 0, uint8_t detail = 0);
  void count_answer_(uint8_t result);

  struct Answer {
    uint8_t result;
    uint8_t detail;
  };

  void log_(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void respond_(uint8_t seq, uint8_t opcode, uint8_t result, uint8_t detail);
  void new_nonce_();
  void clear_reads_();

  void on_command_(const uint8_t *command, size_t n);
  void handle_auth_(uint8_t seq, const uint8_t *proof, size_t n);
  Answer execute_(uint8_t opcode, const uint8_t *args, size_t n);
  void on_record_(uint8_t channel, const uint8_t *write, size_t n);
  Answer handle_profile_(const uint8_t *d, size_t n);
  Answer handle_config_(const uint8_t *d, size_t n);
  Answer handle_calibration_(const uint8_t *d, size_t n);

  void set_profile_read_(uint8_t profile, uint8_t event);
  bool get_config_(uint8_t key, int32_t &value) const;
  void set_config_read_(uint8_t key);
  void set_calibration_read_(uint8_t index);

  proto::StatusSnapshot snapshot_();
  void publish_status_(bool force);

  uint64_t now_() { return static_cast<uint64_t>(this->feeder_.uptime()); }

  FeederCore &feeder_;
  BleTransport &transport_;
  proto::TimePolicy time_policy_{proto::TimePolicy::FALLBACK_ONLY};
  uint32_t network_stale_ms_{24u * 3600u * 1000u};

  proto::Session session_;
  uint32_t key_serial_{0};  // FeederCore::ble_key_serial() when the engine last looked
  proto::ReplayCache replay_;
  bool connected_{false};
  uint8_t last_status_[proto::STATUS_SIZE]{};
  uint32_t last_status_ms_{0};
  bool force_status_{true};

  BleStats stats_{};
  BleActivity activity_[ACTIVITY_SIZE]{};
  uint8_t activity_head_{0};  // where the next entry goes
  uint8_t activity_count_{0};
  uint32_t next_activity_id_{1};
  uint8_t peer_[6]{};
  bool have_peer_{false};
  uint16_t mtu_{DEFAULT_MTU};
  int64_t connected_ms_{-1};      // uptime of the current (or last) connection; -1 = never
  int64_t last_activity_ms_{-1};  // uptime of the last write or read; -1 = never
};

}  // namespace precision_feeder
}  // namespace esphome
