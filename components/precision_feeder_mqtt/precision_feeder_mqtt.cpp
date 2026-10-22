// Precision Feeder: MQTT's own topics on the device (see precision_feeder_mqtt.h).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder_mqtt.h"

#include "esphome/components/mqtt/mqtt_client.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder.mqtt";
// Published per loop pass: ESPHome's MQTT queue drops messages when it is full, and a reconnect republishes 72.
static constexpr size_t PUBLISH_PER_LOOP = 6;

void PrecisionFeederMqtt::setup() {
  this->topics_ = std::make_unique<FeederMqtt>(*this->feeder_);
  // The prefix in use is known now (the boot lambda set it). ESPHome keeps the subscription and makes it again after
  // every reconnect; '#' is matched by its client.
  this->base_ = mqtt::global_mqtt_client->get_topic_prefix() + "/feeder/";
  mqtt::global_mqtt_client->subscribe(this->base_ + "#", [this](const std::string &topic, const std::string &payload) {
    this->on_message_(topic, payload);
  });
  ESP_LOGI(TAG, "Schedule and policy topics under %s", this->base_.c_str());
}

void PrecisionFeederMqtt::on_message_(const std::string &topic, const std::string &payload) {
  if (topic.compare(0, this->base_.size(), this->base_) != 0)
    return;
  const std::string rel = topic.substr(this->base_.size());
  std::vector<MqttOut> out;
  const Result r = this->topics_->on_message(rel, payload, out);
  if (r == Result::NONE)
    return;  // one of our state topics coming back, or not ours
  if (r == Result::OK) {
    ESP_LOGI(TAG, "%s: %s", rel.c_str(), payload.c_str());
  } else {
    ESP_LOGW(TAG, "%s: %s refused (%s)", rel.c_str(), payload.c_str(), FeederCore::result_name(r));
  }
  this->queue_(out);
}

void PrecisionFeederMqtt::queue_(std::vector<MqttOut> &out) {
  for (auto &m : out)
    this->pending_.push_back(std::move(m));
}

void PrecisionFeederMqtt::loop() {
  auto *client = mqtt::global_mqtt_client;
  const bool connected = client->is_connected();
  if (connected != this->was_connected_) {
    this->was_connected_ = connected;
    this->pending_.clear();
    if (connected)
      this->topics_->forget();  // the broker may have lost them: everything again
  }
  if (!connected)
    return;
  const uint32_t now = millis();
  if (now - this->last_check_ >= 1000) {
    this->last_check_ = now;
    std::vector<MqttOut> out;
    this->topics_->changed(out);  // changes from the web app, BLE or the schedule too
    this->queue_(out);
  }
  for (size_t i = 0; i < PUBLISH_PER_LOOP && !this->pending_.empty(); i++) {
    const MqttOut &m = this->pending_.front();
    if (!client->publish(this->base_ + m.topic, m.payload, 0, m.retain))
      break;  // the client's queue is full: the rest next time
    this->pending_.pop_front();
  }
}

}  // namespace precision_feeder
}  // namespace esphome
