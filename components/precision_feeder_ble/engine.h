// Precision Feeder BLE protocol engine: everything the BLE protocol does, as plain C++.
//
// Authentication, sequence numbers and the retry cache, every command and record handler, Status and the event
// log live here, on top of FeederCore. It talks to the radio only through BleTransport (set a characteristic's
// value, optionally notify; random bytes; log), so the whole protocol runs in host tests against the simulated
// feeder (tests/test_ble_engine.cpp). PrecisionFeederBle is the thin ESPHome adapter that implements the transport
// with real GATT characteristics.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstddef>
#include <cstdint>

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

class BleEngine {
 public:
  static constexpr uint32_t STATUS_PERIOD_MS = 500;

  BleEngine(FeederCore &feeder, BleTransport &transport) : feeder_(feeder), transport_(transport) {}

  void set_key(const uint8_t key[crypto::KEY_SIZE]) { this->session_.set_key(key); }
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

 protected:
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
  proto::ReplayCache replay_;
  bool connected_{false};
  uint8_t last_status_[proto::STATUS_SIZE]{};
  uint32_t last_status_ms_{0};
  bool force_status_{true};
};

}  // namespace precision_feeder
}  // namespace esphome
