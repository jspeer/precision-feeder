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
#ifdef USE_WEBSERVER
#include "esphome/components/web_server_base/web_server_base.h"
#endif

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder_ble";

#ifdef USE_WEBSERVER
/// GET /feeder/ble: the BLE activity log as JSON, for the web app. Added with add_handler(), so web auth applies
/// when it is configured. Runs on the web server's task, so it only reads the copy the main loop prepared.
class BleActivityHandler : public AsyncWebHandler {
 public:
  explicit BleActivityHandler(PrecisionFeederBle *parent) : parent_(parent) {}
  bool canHandle(AsyncWebServerRequest *request) const override {
    if (request->method() != HTTP_GET)
      return false;
    char url[AsyncWebServerRequest::URL_BUF_SIZE];
    return request->url_to(url) == "/feeder/ble";
  }
  void handleRequest(AsyncWebServerRequest *request) override {
    const std::string body = this->parent_->activity_json_copy();
    request->send(200, "application/json", body.c_str());
  }

 protected:
  PrecisionFeederBle *parent_;
};
#endif

void PrecisionFeederBle::setup() {
  this->engine_ = new BleEngine(*this->feeder_, *this);  // lives as long as the device runs
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
  // Count reads (the engine only sees writes); the size is what the client received.
  for (uint8_t channel : {proto::CHANNEL_INFO, proto::CHANNEL_RESPONSE, proto::CHANNEL_STATUS, proto::CHANNEL_PROFILE,
                          proto::CHANNEL_CONFIG, proto::CHANNEL_CALIBRATION, proto::CHANNEL_AUTH}) {
    Characteristic *c = this->chars_[channel];
    this->chars_[channel]->on_read([this, channel, c](uint16_t) { this->engine_->on_read(channel, c->get_value().size()); });
  }
#ifdef USE_WEBSERVER
  if (web_server_base::global_web_server_base != nullptr)
    web_server_base::global_web_server_base->add_handler(new BleActivityHandler(this));  // lives as long as the device
#endif
  this->server_->on_connect([this](uint16_t) { this->engine_->on_connect(); });
  this->server_->on_disconnect([this](uint16_t) { this->engine_->on_disconnect(); });
  this->feeder_->set_feed_listener([this](const FeedRecord &record) { this->engine_->on_feed(record); });
}

void PrecisionFeederBle::loop() {
  this->engine_->loop(millis());
  if (this->engine_->activity_last_id() != this->activity_json_id_) {
    std::string json = this->engine_->activity_json();
    LockGuard guard(this->activity_lock_);
    this->activity_json_ = std::move(json);
    this->activity_json_id_ = this->engine_->activity_last_id();
  }
}

std::string PrecisionFeederBle::activity_json_copy() {
  LockGuard guard(this->activity_lock_);
  return this->activity_json_;
}

void PrecisionFeederBle::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t /*gatts_if*/,
                                             esp_ble_gatts_cb_param_t *param) {
  if (this->engine_ == nullptr)
    return;
  if (event == ESP_GATTS_CONNECT_EVT) {
    this->engine_->set_peer(param->connect.remote_bda);
  } else if (event == ESP_GATTS_MTU_EVT) {
    this->engine_->set_mtu(param->mtu.mtu);
  }
}

void PrecisionFeederBle::dump_config() {
  const char *policy = "fallback_only";
  if (this->time_policy_ == proto::TimePolicy::ALWAYS)
    policy = "always";
  else if (this->time_policy_ == proto::TimePolicy::OFF)
    policy = "off";
  ESP_LOGCONFIG(TAG, "Precision Feeder BLE:");
  ESP_LOGCONFIG(TAG, "  Protocol version: %u", proto::PROTOCOL_VERSION);
  static const char *const KEYS[] = {"none set: every AUTH is refused until one is set in the web app",
                                     "from the build (YAML)", "saved on the device"};
  ESP_LOGCONFIG(TAG, "  Authentication: required; key: %s", KEYS[static_cast<uint8_t>(this->feeder_->ble_key_source())]);
  ESP_LOGCONFIG(TAG, "  BLE may change: guard limits %s, calibration %s", YESNO(this->feeder_->ble_may_change_guard()),
                YESNO(this->feeder_->ble_may_change_calibration()));
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
