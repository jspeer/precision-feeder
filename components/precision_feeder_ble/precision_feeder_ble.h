// Precision Feeder BLE control plane: the ESPHome adapter.
//
// The GATT table (UUIDs, properties) lives in packages/ble_gatt.yaml. Everything the protocol does lives in
// BleEngine (engine.h), plain C++ that is tested on a host; this class only connects it to ESPHome: the
// characteristics, the hardware random number generator, the log, and connect/disconnect events. The byte layouts
// are in protocol.h and docs/PROTOCOL.md.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <array>

#include "esphome/core/component.h"
#include "esphome/components/esp32_ble_server/ble_characteristic.h"
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "esphome/components/precision_feeder/precision_feeder.h"

#include "engine.h"

namespace esphome {
namespace precision_feeder {

class PrecisionFeederBle : public Component, public BleTransport {
 public:
  using Characteristic = esp32_ble_server::BLECharacteristic;

  // ---- configuration (set from Python) ----
  void set_feeder(PrecisionFeeder *feeder) { this->feeder_ = feeder; }
  void set_server(esp32_ble_server::BLEServer *server) { this->server_ = server; }
  void set_info(Characteristic *c) { this->chars_[proto::CHANNEL_INFO] = c; }
  void set_command(Characteristic *c) { this->chars_[proto::CHANNEL_COMMAND] = c; }
  void set_response(Characteristic *c) { this->chars_[proto::CHANNEL_RESPONSE] = c; }
  void set_status(Characteristic *c) { this->chars_[proto::CHANNEL_STATUS] = c; }
  void set_profile(Characteristic *c) { this->chars_[proto::CHANNEL_PROFILE] = c; }
  void set_config(Characteristic *c) { this->chars_[proto::CHANNEL_CONFIG] = c; }
  void set_calibration(Characteristic *c) { this->chars_[proto::CHANNEL_CALIBRATION] = c; }
  void set_event_log(Characteristic *c) { this->chars_[proto::CHANNEL_EVENT_LOG] = c; }
  void set_auth(Characteristic *c) { this->chars_[proto::CHANNEL_AUTH] = c; }
  void set_key(const std::array<uint8_t, crypto::KEY_SIZE> &key) { this->key_ = key; }
  void set_time_policy(proto::TimePolicy policy) { this->time_policy_ = policy; }
  void set_network_time_stale_ms(uint32_t ms) { this->network_stale_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ---- BleTransport ----
  void set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) override;
  void random(uint8_t *out, size_t n) override;
  void log(LogLevel level, const char *message) override;

 protected:
  PrecisionFeeder *feeder_{nullptr};
  esp32_ble_server::BLEServer *server_{nullptr};
  Characteristic *chars_[proto::CHANNEL_AUTH + 1]{};
  std::array<uint8_t, crypto::KEY_SIZE> key_{};
  proto::TimePolicy time_policy_{proto::TimePolicy::FALLBACK_ONLY};
  uint32_t network_stale_ms_{24u * 3600u * 1000u};
  BleEngine *engine_{nullptr};  // created in setup(), once the feeder is known
};

}  // namespace precision_feeder
}  // namespace esphome
