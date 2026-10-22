// Precision Feeder: MQTT's own topics (firmware 2.2.0, docs/MQTT.md): the schedule, the one-shot feeds and, when
// allowed, the feed guard's limits, as plain topics with one value each under "<prefix>/feeder/". The entities Home
// Assistant sees (the slim set) are ESPHome's own MQTT topics; these are the ones it does not offer.
//
// Pure C++ with no ESPHome includes, so it is tested on the host (tests/test_mqtt.cpp). The adapter
// (components/precision_feeder_mqtt) subscribes to "<prefix>/feeder/#", hands every message here with the prefix taken
// off, and publishes what comes back, retained, under the prefix.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "feeder_core.h"

namespace esphome {
namespace precision_feeder {

/// One message to publish: `topic` below "<prefix>/feeder/".
struct MqttOut {
  std::string topic;
  std::string payload;
  bool retain;
};

class FeederMqtt {
 public:
  explicit FeederMqtt(FeederCore &feeder) : f_(feeder) {}

  /// A message on "<prefix>/feeder/<topic>". Commands are the topics ending in "/set", plus "oneshot/add",
  /// "oneshot/clear" and "schedule/<P>/restart_cycle"; anything else (our own state topics, coming back from the
  /// broker) is ignored. Appends what to publish: the state topics that changed, the command's own state topic even
  /// when nothing changed (a refused value is answered with the value in force), and "result" (not retained).
  /// Returns the command's result, or Result::NONE when the topic was not a command.
  Result on_message(const std::string &topic, const std::string &payload, std::vector<MqttOut> &out);
  /// The state topics whose value changed since the last call (every one after forget()): call it about once a
  /// second, so changes from the web app, BLE or the schedule reach MQTT too.
  void changed(std::vector<MqttOut> &out);
  /// Publish everything again at the next changed() (after a reconnect: the broker may have lost them).
  void forget() { this->last_.clear(); }
  /// Every state topic and its value now.
  void states(std::vector<MqttOut> &out);

 protected:
  Result command_(const std::string &topic, const std::string &payload, std::string &state_topic);
  FeederCore &f_;
  std::map<std::string, std::string> last_;
};

}  // namespace precision_feeder
}  // namespace esphome
