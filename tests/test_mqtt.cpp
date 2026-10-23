// Host tests for MQTT's own topics (components/precision_feeder/feeder_mqtt.*, firmware 2.2.0, docs/MQTT.md).
// Build and run from the repository root (tools/check.sh does): c++ -std=c++17 -Icomponents/precision_feeder -Itests
// tests/test_mqtt.cpp components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include <cstdio>
#include <string>
#include <vector>

#include "fake_feeder.h"
#include "feeder_mqtt.h"

static int failures = 0;
static const char *current = "";

#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL [%s]: %s (line %d)\n", current, msg, __LINE__);   \
      failures++;                                                         \
    }                                                                     \
  } while (0)

/// The payload published for `topic` in `out`, or "<none>".
static std::string sent(const std::vector<MqttOut> &out, const std::string &topic) {
  for (const auto &m : out) {
    if (m.topic == topic)
      return m.payload;
  }
  return "<none>";
}

struct Rig {
  Flash flash;
  std::unique_ptr<FakeFeeder> f;
  std::unique_ptr<FeederMqtt> m;
  std::vector<MqttOut> out;
  Rig() {
    this->f = boot(this->flash, T0);
    this->m = std::make_unique<FeederMqtt>(*this->f);
  }
  Result send(const std::string &topic, const std::string &payload) {
    this->out.clear();
    return this->m->on_message(topic, payload, this->out);
  }
};

static void test_states() {
  current = "the state topics";
  Rig r;
  std::vector<MqttOut> all;
  r.m->states(all);
  CHECK(all.size() == 3 * (4 * 5 + 2) + 1 + 4 + 1, "72 topics: 3 profiles of 4 events, one-shots, policy");
  for (const auto &s : all)
    CHECK(s.retain, "every state topic retained");
  CHECK(sent(all, "schedule/A/event/1/enabled") == "OFF" && sent(all, "schedule/A/event/1/anchor") == "clock",
        "an event's fields");
  CHECK(sent(all, "schedule/C/every_n_days") == "1" && sent(all, "schedule/B/fast_days") == "none", "a profile's");
  CHECK(sent(all, "oneshot/list") == "none", "no one-shots");
  CHECK(sent(all, "policy/daily_cap") == "4" && sent(all, "policy/writable") == "OFF", "the policy, read only");
}

static void test_events() {
  current = "editing an event";
  Rig r;
  CHECK(r.send("schedule/A/event/1/time/set", "07:30") == Result::OK, "time");
  CHECK(r.f->cfg.profiles[0].ev[0].offset_min == 7 * 60 + 30, "applied");
  CHECK(sent(r.out, "schedule/A/event/1/time") == "07:30", "its state published");
  CHECK(sent(r.out, "result") == "schedule/A/event/1/time/set: ok", "and the result");
  CHECK(r.send("schedule/A/event/1/time/set", "25:00") == Result::INVALID_ARG, "a bad time refused");
  CHECK(sent(r.out, "schedule/A/event/1/time") == "07:30", "answered with the time in force");
  for (const char *bad : {"08:00 PM", "08:00garbage", "71582789:00"}) {
    CHECK(r.send("schedule/A/event/1/time/set", bad) == Result::INVALID_ARG, bad);
    CHECK(r.f->cfg.profiles[0].ev[0].offset_min == 7 * 60 + 30, "the time in force kept (2.3.1)");
  }
  CHECK(r.send("schedule/a/event/1/enabled/set", "on") == Result::OK && r.f->cfg.profiles[0].ev[0].enabled == 1,
        "enabled, lower-case profile and value");
  CHECK(r.send("schedule/A/event/1/enabled/set", "maybe") == Result::INVALID_ARG, "not a switch value");
  CHECK(r.send("schedule/A/event/1/anchor/set", "sunrise") == Result::OK, "anchor");
  CHECK(sent(r.out, "schedule/A/event/1/time") == "+00:00", "the time follows the anchor (0 from the sun)");
  CHECK(r.send("schedule/A/event/1/time/set", "-01:15") == Result::OK &&
            r.f->cfg.profiles[0].ev[0].offset_min == -75, "before sunrise");
  CHECK(r.send("schedule/A/event/1/anchor/set", "noon") == Result::INVALID_ARG, "no such anchor");
  CHECK(r.send("schedule/B/event/4/portions/set", "6") == Result::OK, "6 portions on the servo");
  CHECK(r.send("schedule/B/event/4/portions/set", "7") == Result::INVALID_ARG, "7: more than a load");
  CHECK(r.send("schedule/B/event/4/portions/set", "2.5") == Result::INVALID_ARG, "whole numbers only");
  CHECK(r.send("schedule/B/event/4/days/set", "mon,wed") == Result::OK && r.f->cfg.profiles[1].ev[3].days == 0x0A,
        "days");
  CHECK(sent(r.out, "schedule/B/event/4/days") == "mon,wed", "published as the device writes them");
  CHECK(r.send("schedule/B/event/4/days/set", "someday") == Result::INVALID_ARG, "days not understood");
}

static void test_profiles() {
  current = "a profile's cadence";
  Rig r;
  CHECK(r.send("schedule/C/every_n_days/set", "3") == Result::OK && r.f->cfg.profiles[2].every_n == 3, "every 3 days");
  CHECK(r.send("schedule/C/every_n_days/set", "0") == Result::INVALID_ARG, "0 refused");
  CHECK(r.send("schedule/C/every_n_days/set", "15") == Result::INVALID_ARG, "15 refused");
  CHECK(sent(r.out, "schedule/C/every_n_days") == "3", "answered with the value in force");
  CHECK(r.send("schedule/C/fast_days/set", "sun") == Result::OK && r.f->cfg.profiles[2].fast_days == 0x01, "fast days");
  CHECK(r.send("schedule/C/restart_cycle", "") == Result::OK, "the cycle restarts today");
}

static void test_oneshots() {
  current = "one-shot feeds";
  Rig r;
  CHECK(r.send("oneshot/add", "2026-10-14 09:00 2") == Result::OK, "added");
  CHECK(sent(r.out, "oneshot/list").find("x2") != std::string::npos, "listed");
  CHECK(r.send("oneshot/add", "2026-10-14 09:00 2") == Result::OK, "the same again (a retained command)");
  CHECK(r.f->oneshots_text().find(';') == std::string::npos, "still one");
  CHECK(r.send("oneshot/add", "2026-10-14 10:00") == Result::OK, "portions default to 1");
  CHECK(r.send("oneshot/add", "tomorrow") == Result::INVALID_ARG, "not a date");
  CHECK(r.send("oneshot/add", "2026-10-14 09:30x 1") == Result::INVALID_ARG, "not a time (2.3.1)");
  CHECK(r.send("oneshot/add", "2026-10-14 11:00 7") == Result::INVALID_ARG, "more than a load");
  CHECK(r.send("oneshot/clear", "") == Result::OK && sent(r.out, "oneshot/list") == "none", "cleared");
}

static void test_policy() {
  current = "the guard's limits: read only (D28)";
  Rig r;
  const int32_t before[4] = {r.f->cfg.daily_cap, r.f->cfg.min_gap_s, r.f->cfg.portion_gap_s, r.f->cfg.max_portions_cmd};
  const char *keys[4] = {"daily_cap", "min_gap_s", "portion_gap_s", "max_portions_cmd"};
  for (const char *value : {"3", "120", "8", "2", "0", "999", "x", ""}) {
    for (const char *key : keys) {
      const std::string topic = std::string("policy/") + key + "/set";
      CHECK(r.send(topic, value) == Result::NOT_ALLOWED, "every limit, every value: not allowed");
    }
  }
  CHECK(r.f->cfg.daily_cap == before[0] && r.f->cfg.min_gap_s == before[1] && r.f->cfg.portion_gap_s == before[2] &&
            r.f->cfg.max_portions_cmd == before[3],
        "nothing changed");
  r.send("policy/daily_cap/set", "3");
  CHECK(sent(r.out, "policy/daily_cap") == "4" && sent(r.out, "result") == "policy/daily_cap/set: not allowed",
        "answered with the cap in force");
  r.out.clear();
  r.m->states(r.out);
  CHECK(sent(r.out, "policy/writable") == "OFF", "policy/writable is always OFF");
  CHECK(r.send("policy/portions_24h/set", "0") == Result::NONE, "the 24 h count is never on MQTT (D25)");
  // Changed in the web app (or over BLE), the new value is published; MQTT still cannot change it.
  r.f->set_daily_cap(5);
  r.out.clear();
  r.m->changed(r.out);
  CHECK(sent(r.out, "policy/daily_cap") == "5", "a change elsewhere is published");
  CHECK(r.send("policy/daily_cap/set", "6") == Result::NOT_ALLOWED && r.f->cfg.daily_cap == 5, "and still read only");
  // A feeder upgraded from 2.2.0 with its "MQTT Can Change Guard Limits" switch on: the retired bit is dropped.
  r.f->advance(6000);  // the settings save shortly after a change
  r.flash.device.flags |= DEVICE_FLAGS_RETIRED;
  auto g = boot(r.flash, T0 + 60);
  CHECK((g->device().flags & DEVICE_FLAGS_RETIRED) == 0, "2.2.0's switch is cleared on load");
  FeederMqtt m(*g);
  std::vector<MqttOut> out;
  CHECK(m.on_message("policy/daily_cap/set", "3", out) == Result::NOT_ALLOWED && g->cfg.daily_cap == 5,
        "and the limits stay read only");
}

static void test_not_commands() {
  current = "what is not a command";
  Rig r;
  CHECK(r.send("schedule/A/event/1/time", "07:30") == Result::NONE && r.out.empty(),
        "our own state topic, back from the broker: ignored");
  CHECK(r.send("policy/daily_cap", "1") == Result::NONE, "a policy state topic");
  CHECK(r.send("schedule/D/every_n_days/set", "2") == Result::NONE, "no profile D");
  CHECK(r.send("schedule/A/event/5/days/set", "all") == Result::NONE, "no event 5");
  CHECK(r.send("schedule/A/event/1/colour/set", "red") == Result::NONE, "no such field");
  CHECK(r.send("calibration/offset/set", "3") == Result::NONE, "components are never on MQTT (D26)");
  CHECK(r.send("result", "x") == Result::NONE, "nor our result topic");
}

static void test_changes_from_elsewhere() {
  current = "changes from the web app, BLE or the schedule";
  Rig r;
  std::vector<MqttOut> out;
  r.m->changed(out);
  CHECK(out.size() == 72, "the first call: everything");
  out.clear();
  r.m->changed(out);
  CHECK(out.empty(), "then nothing until something changes");
  r.f->set_daily_cap(2);  // the web app
  r.m->changed(out);
  CHECK(out.size() == 1 && sent(out, "policy/daily_cap") == "2", "only what changed");
  out.clear();
  r.m->forget();  // a reconnect
  r.m->changed(out);
  CHECK(out.size() == 72, "everything again after a reconnect");
}

int main() {
  test_states();
  test_events();
  test_profiles();
  test_oneshots();
  test_policy();
  test_not_commands();
  test_changes_from_elsewhere();
  if (failures == 0)
    std::printf("All MQTT topic tests passed\n");
  return failures == 0 ? 0 : 1;
}
