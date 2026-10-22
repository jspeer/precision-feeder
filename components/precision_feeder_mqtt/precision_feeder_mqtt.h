// Precision Feeder: MQTT's own topics on the device (see __init__.py; the logic is FeederMqtt, feeder_mqtt.h).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <deque>
#include <memory>
#include <string>

#include "esphome/core/component.h"
#include "esphome/components/precision_feeder/precision_feeder.h"
#include "esphome/components/precision_feeder/feeder_mqtt.h"

namespace esphome {
namespace precision_feeder {

class PrecisionFeederMqtt : public Component {
 public:
  void set_feeder(PrecisionFeeder *feeder) { this->feeder_ = feeder; }
  void setup() override;
  void loop() override;
  // After the boot lambda that sets the topic prefix (packages/mqtt.yaml, priority 600), with the MQTT client.
  float get_setup_priority() const override { return setup_priority::AFTER_WIFI; }

 protected:
  void on_message_(const std::string &topic, const std::string &payload);
  void queue_(std::vector<MqttOut> &out);
  PrecisionFeeder *feeder_{nullptr};
  std::unique_ptr<FeederMqtt> topics_;
  std::string base_;  // "<prefix>/feeder/"
  std::deque<MqttOut> pending_;
  bool was_connected_{false};
  uint32_t last_check_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
