// Precision Feeder BLE control plane: serves the GATT table declared in YAML.
//
// The GATT table (UUIDs, properties) lives in packages/ble_gatt.yaml; this component owns what the
// characteristics mean. The byte layouts are in protocol.h and docs/PROTOCOL.md.
//
// Protocol 2 authenticates every write: a client reads the Auth characteristic (a random nonce, new for every
// connection), proves it knows the shared key with AUTH, and then signs each write with a sequence number and a
// tag (crypto.h). Until then it can read Info and Auth only; everything else reads as zeros and is not notified.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <array>
#include <span>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/components/esp32_ble_server/ble_characteristic.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/components/precision_feeder/precision_feeder.h"

#include "protocol.h"

namespace esphome {
namespace precision_feeder {

class PrecisionFeederBle : public Component {
 public:
  using Characteristic = esp32_ble_server::BLECharacteristic;

  // ---- configuration (set from Python) ----
  void set_feeder(PrecisionFeeder *feeder) { this->feeder_ = feeder; }
  void set_server(esp32_ble_server::BLEServer *server) { this->server_ = server; }
  void set_info(Characteristic *c) { this->info_ = c; }
  void set_command(Characteristic *c) { this->command_ = c; }
  void set_response(Characteristic *c) { this->response_ = c; }
  void set_status(Characteristic *c) { this->status_ = c; }
  void set_profile(Characteristic *c) { this->profile_ = c; }
  void set_config(Characteristic *c) { this->config_ = c; }
  void set_calibration(Characteristic *c) { this->calibration_ = c; }
  void set_event_log(Characteristic *c) { this->event_log_ = c; }
  void set_auth(Characteristic *c) { this->auth_ = c; }
  void set_key(const std::array<uint8_t, crypto::KEY_SIZE> &key) { this->session_.set_key(key.data()); }
  void set_time_policy(proto::TimePolicy policy) { this->time_policy_ = policy; }
  void set_network_time_stale_ms(uint32_t ms) { this->network_stale_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  // Command characteristic
  void on_command_(std::span<const uint8_t> data);
  void handle_auth_(uint8_t seq, const uint8_t *proof, size_t n, uint64_t now);
  uint8_t execute_(uint8_t opcode, const uint8_t *args, size_t n, uint8_t &detail);
  void send_response_(const uint8_t response[proto::RESPONSE_SIZE]);
  void respond_write_(uint8_t opcode, uint8_t result, uint8_t detail);

  // Profile, Config and Calibration characteristics. on_record_write_ checks the sequence number and tag (or
  // replays the answer to a retry), then hands the record in between to the matching handler.
  void on_record_write_(uint8_t channel, uint8_t opcode, std::span<const uint8_t> data);
  void handle_profile_(std::span<const uint8_t> data);
  void handle_config_(std::span<const uint8_t> data);
  void handle_calibration_(std::span<const uint8_t> data);
  void set_profile_read_(uint8_t profile, uint8_t event);
  bool get_config_(uint8_t key, int32_t &value) const;
  void set_config_read_(uint8_t key);
  void set_calibration_read_(uint8_t index);

  // Status and event log
  proto::StatusSnapshot snapshot_();
  void publish_status_(bool force);
  void publish_event_(const FeedRecord &record);
  bool has_client_() const { return this->server_ != nullptr && this->server_->get_connected_client_count() > 0; }
  bool authorized_() const { return this->has_client_() && this->session_.authenticated(); }

  // Authentication
  void new_nonce_();
  void clear_reads_();

  PrecisionFeeder *feeder_{nullptr};
  esp32_ble_server::BLEServer *server_{nullptr};
  Characteristic *info_{nullptr};
  Characteristic *command_{nullptr};
  Characteristic *response_{nullptr};
  Characteristic *status_{nullptr};
  Characteristic *profile_{nullptr};
  Characteristic *config_{nullptr};
  Characteristic *calibration_{nullptr};
  Characteristic *event_log_{nullptr};
  Characteristic *auth_{nullptr};

  proto::TimePolicy time_policy_{proto::TimePolicy::FALLBACK_ONLY};
  uint32_t network_stale_ms_{24u * 3600u * 1000u};

  proto::ReplayCache replay_;
  proto::Session session_;
  // The record write being answered: its sequence number, channel and bytes, for the answer and the retry cache.
  uint8_t record_seq_{0};
  uint8_t record_channel_{0};
  uint8_t record_write_[proto::ReplayCache::MAX_COMMAND]{};
  size_t record_write_len_{0};
  uint8_t last_status_[proto::STATUS_SIZE]{};
  uint32_t last_status_ms_{0};
  bool force_status_{true};
};

}  // namespace precision_feeder
}  // namespace esphome
