// Precision Feeder BLE control plane: the ESPHome adapter (see precision_feeder_ble.h; the protocol is engine.cpp).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder_ble.h"

#include <vector>

#include <esp_random.h>

#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder_ble";

void PrecisionFeederBle::setup() {
  this->engine_ = new BleEngine(*this->feeder_, *this);  // lives as long as the device runs
  this->engine_->set_key(this->key_.data());
  this->key_.fill(0);  // the engine keeps its own copy
  this->engine_->set_time_policy(this->time_policy_);
  this->engine_->set_network_time_stale_ms(this->network_stale_ms_);
  this->engine_->start();

  for (uint8_t channel : {proto::CHANNEL_COMMAND, proto::CHANNEL_PROFILE, proto::CHANNEL_CONFIG,
                          proto::CHANNEL_CALIBRATION}) {
    // `data` points into the characteristic's own stored value; the engine copies it before answering.
    this->chars_[channel]->on_write([this, channel](std::span<const uint8_t> data, uint16_t) {
      this->engine_->on_write(channel, data.data(), data.size());
    });
  }
  this->server_->on_connect([this](uint16_t) { this->engine_->on_connect(); });
  this->server_->on_disconnect([this](uint16_t) { this->engine_->on_disconnect(); });
  this->feeder_->set_feed_listener([this](const FeedRecord &record) { this->engine_->on_feed(record); });
}

void PrecisionFeederBle::loop() { this->engine_->loop(millis()); }

void PrecisionFeederBle::dump_config() {
  const char *policy = "fallback_only";
  if (this->time_policy_ == proto::TimePolicy::ALWAYS)
    policy = "always";
  else if (this->time_policy_ == proto::TimePolicy::OFF)
    policy = "off";
  ESP_LOGCONFIG(TAG, "Precision Feeder BLE:");
  ESP_LOGCONFIG(TAG, "  Protocol version: %u", proto::PROTOCOL_VERSION);
  ESP_LOGCONFIG(TAG, "  Authentication: required (shared key set)");
  ESP_LOGCONFIG(TAG, "  Time policy: %s (network time counts as fresh for %u h)", policy,
                (unsigned) (this->network_stale_ms_ / 3600000u));
}

void PrecisionFeederBle::set_value(uint8_t channel, const uint8_t *data, size_t n, bool notify) {
  Characteristic *c = channel < sizeof(this->chars_) / sizeof(this->chars_[0]) ? this->chars_[channel] : nullptr;
  if (c == nullptr)
    return;
  c->set_value(std::vector<uint8_t>(data, data + n));
  if (notify)
    c->notify();
}

void PrecisionFeederBle::random(uint8_t *out, size_t n) {
  esp_fill_random(out, n);  // hardware RNG; the radio is on, so it is a true random source
}

void PrecisionFeederBle::log(LogLevel level, const char *message) {
  switch (level) {
    case LogLevel::kError:
      ESP_LOGE(TAG, "%s", message);
      break;
    case LogLevel::kWarn:
      ESP_LOGW(TAG, "%s", message);
      break;
    case LogLevel::kInfo:
      ESP_LOGI(TAG, "%s", message);
      break;
    default:
      ESP_LOGD(TAG, "%s", message);
      break;
  }
}

}  // namespace precision_feeder
}  // namespace esphome
