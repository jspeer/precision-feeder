// Precision Feeder: MQTT's own topics (see feeder_mqtt.h and docs/MQTT.md).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_mqtt.h"

#include <cctype>
#include <cstdlib>

namespace esphome {
namespace precision_feeder {

static const char PROFILE_LETTERS[NUM_PROFILES + 1] = "ABC";
static const char *const ANCHOR_NAMES[3] = {"clock", "sunrise", "sunset"};
static const char *const EVENT_FIELDS[5] = {"enabled", "anchor", "time", "portions", "days"};
static const char *const POLICY_KEYS[4] = {"daily_cap", "min_gap_s", "portion_gap_s", "max_portions_cmd"};

static std::string lower(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

static std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && std::isspace(static_cast<unsigned char>(s[a])))
    a++;
  while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1])))
    b--;
  return s.substr(a, b - a);
}

/// A whole number, nothing else (no "3.5", no "3 portions").
static bool parse_int(const std::string &text, int32_t &out) {
  const std::string t = trim(text);
  if (t.empty() || t.size() > 9)
    return false;
  size_t i = (t[0] == '-' || t[0] == '+') ? 1 : 0;
  if (i == t.size())
    return false;
  for (size_t j = i; j < t.size(); j++) {
    if (!std::isdigit(static_cast<unsigned char>(t[j])))
      return false;
  }
  out = static_cast<int32_t>(std::strtol(t.c_str(), nullptr, 10));
  return true;
}

static bool parse_bool(const std::string &text, bool &out) {
  const std::string t = lower(trim(text));
  if (t == "on" || t == "true" || t == "1") {
    out = true;
  } else if (t == "off" || t == "false" || t == "0") {
    out = false;
  } else {
    return false;
  }
  return true;
}

static std::string profile_topic(uint8_t p) { return std::string("schedule/") + PROFILE_LETTERS[p]; }

static std::string event_topic(uint8_t p, uint8_t e, const char *field) {
  return profile_topic(p) + "/event/" + std::to_string(e + 1) + "/" + field;
}

/// "A" to "C" (or "a" to "c") at the start of `s`, followed by end or '/'.
static bool parse_profile(const std::string &s, uint8_t &p) {
  if (s.empty() || (s.size() > 1 && s[1] != '/'))
    return false;
  const char c = static_cast<char>(std::toupper(static_cast<unsigned char>(s[0])));
  for (uint8_t i = 0; i < NUM_PROFILES; i++) {
    if (PROFILE_LETTERS[i] == c) {
      p = i;
      return true;
    }
  }
  return false;
}

void FeederMqtt::states(std::vector<MqttOut> &out) {
  const Settings &c = this->f_.cfg;
  for (uint8_t p = 0; p < NUM_PROFILES; p++) {
    const Profile &prof = c.profiles[p];
    for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
      const Event &ev = prof.ev[e];
      out.push_back({event_topic(p, e, "enabled"), ev.enabled ? "ON" : "OFF", true});
      out.push_back({event_topic(p, e, "anchor"), ANCHOR_NAMES[ev.anchor % 3], true});
      out.push_back({event_topic(p, e, "time"), this->f_.event_time_text(p, e), true});
      out.push_back({event_topic(p, e, "portions"), std::to_string(ev.portions), true});
      out.push_back({event_topic(p, e, "days"), FeederCore::format_days(ev.days), true});
    }
    out.push_back({profile_topic(p) + "/every_n_days", std::to_string(prof.every_n), true});
    out.push_back({profile_topic(p) + "/fast_days", FeederCore::format_days(prof.fast_days), true});
  }
  out.push_back({"oneshot/list", this->f_.oneshots_text(), true});
  const int32_t policy[4] = {c.daily_cap, c.min_gap_s, c.portion_gap_s, c.max_portions_cmd};
  for (uint8_t i = 0; i < 4; i++)
    out.push_back({std::string("policy/") + POLICY_KEYS[i], std::to_string(policy[i]), true});
  out.push_back({"policy/writable", this->f_.mqtt_may_change_guard() ? "ON" : "OFF", true});
}

void FeederMqtt::changed(std::vector<MqttOut> &out) {
  std::vector<MqttOut> now;
  this->states(now);
  for (auto &m : now) {
    auto it = this->last_.find(m.topic);
    if (it != this->last_.end() && it->second == m.payload)
      continue;
    this->last_[m.topic] = m.payload;
    out.push_back(std::move(m));
  }
}

Result FeederMqtt::on_message(const std::string &topic, const std::string &payload, std::vector<MqttOut> &out) {
  std::string state_topic;
  const Result r = this->command_(topic, payload, state_topic);
  if (r == Result::NONE)
    return r;
  const size_t before = out.size();
  this->changed(out);
  // The command's own state topic, also when nothing changed: a refused value is answered with the one in force.
  bool sent = state_topic.empty();
  for (size_t i = before; i < out.size() && !sent; i++)
    sent = out[i].topic == state_topic;
  if (!sent) {
    std::vector<MqttOut> all;
    this->states(all);
    for (auto &m : all) {
      if (m.topic == state_topic)
        out.push_back(std::move(m));
    }
  }
  out.push_back({"result", topic + ": " + FeederCore::result_name(r), false});
  return r;
}

Result FeederMqtt::command_(const std::string &topic, const std::string &payload, std::string &state_topic) {
  if (topic == "oneshot/add") {
    // "YYYY-MM-DD HH:MM" and optionally the portions (1 by default).
    const std::string t = trim(payload);
    std::string when = t;
    int32_t portions = 1;
    const size_t sp = t.rfind(' ');
    if (sp != std::string::npos && t.find(' ') != sp) {
      if (!parse_int(t.substr(sp + 1), portions))
        return Result::INVALID_ARG;
      when = t.substr(0, sp);
    }
    state_topic = "oneshot/list";
    if (portions < 1 || portions > 255)
      return Result::INVALID_ARG;
    return this->f_.add_oneshot(when, static_cast<uint8_t>(portions)) ? Result::OK : Result::INVALID_ARG;
  }
  if (topic == "oneshot/clear") {
    state_topic = "oneshot/list";
    this->f_.clear_oneshots();
    return Result::OK;
  }
  const std::string set_suffix = "/set";
  const bool is_set = topic.size() > set_suffix.size() &&
                      topic.compare(topic.size() - set_suffix.size(), set_suffix.size(), set_suffix) == 0;
  if (topic.rfind("schedule/", 0) == 0) {
    const std::string rest = topic.substr(9);
    uint8_t p;
    if (!parse_profile(rest, p))
      return Result::NONE;
    const std::string sub = rest.size() > 2 ? rest.substr(2) : "";  // after "A/"
    if (sub == "restart_cycle") {
      if (!this->f_.time_valid())
        return Result::TIME_INVALID;
      this->f_.restart_cadence_today(p);
      return Result::OK;
    }
    if (!is_set)
      return Result::NONE;
    const std::string what = sub.substr(0, sub.size() - set_suffix.size());
    state_topic = profile_topic(p) + "/" + what;
    if (what == "every_n_days") {
      int32_t n;
      return parse_int(payload, n) ? this->f_.set_profile_every_n(p, n) : Result::INVALID_ARG;
    }
    if (what == "fast_days") {
      uint8_t mask;
      return FeederCore::parse_days(lower(trim(payload)), mask) ? this->f_.set_profile_fast_days(p, mask)
                                                                : Result::INVALID_ARG;
    }
    // event/<1-4>/<field>
    if (what.rfind("event/", 0) != 0 || what.size() < 9 || what[7] != '/')
      return Result::NONE;
    const int n = what[6] - '1';
    if (n < 0 || n >= EVENTS_PER_PROFILE)
      return Result::NONE;
    const uint8_t e = static_cast<uint8_t>(n);
    const std::string field = what.substr(8);
    if (field == EVENT_FIELDS[0]) {
      bool on;
      return parse_bool(payload, on) ? this->f_.set_event_enabled(p, e, on) : Result::INVALID_ARG;
    }
    if (field == EVENT_FIELDS[1]) {
      const std::string a = lower(trim(payload));
      for (uint8_t i = 0; i < 3; i++) {
        if (a == ANCHOR_NAMES[i])
          return this->f_.set_event_anchor(p, e, i);
      }
      return Result::INVALID_ARG;
    }
    if (field == EVENT_FIELDS[2])
      return this->f_.set_event_time(p, e, trim(payload));
    if (field == EVENT_FIELDS[3]) {
      int32_t portions;
      return parse_int(payload, portions) ? this->f_.set_event_portions(p, e, portions) : Result::INVALID_ARG;
    }
    if (field == EVENT_FIELDS[4]) {
      uint8_t mask;
      return FeederCore::parse_days(lower(trim(payload)), mask) ? this->f_.set_event_days(p, e, mask)
                                                                : Result::INVALID_ARG;
    }
    state_topic.clear();
    return Result::NONE;
  }
  if (topic.rfind("policy/", 0) == 0 && is_set) {
    const std::string key = topic.substr(7, topic.size() - 7 - set_suffix.size());
    for (uint8_t i = 0; i < 4; i++) {
      if (key != POLICY_KEYS[i])
        continue;
      state_topic = std::string("policy/") + key;
      // The feed guard's limits: only with MQTT Can Change Guard Limits on (D24; web app only, off by default).
      if (!this->f_.mqtt_may_change_guard())
        return Result::NOT_ALLOWED;
      int32_t v;
      if (!parse_int(payload, v))
        return Result::INVALID_ARG;
      switch (i) {
        case 0:
          return this->f_.set_daily_cap(v);
        case 1:
          return this->f_.set_min_gap_s(v);
        case 2:
          return this->f_.set_portion_gap_s(v);
        default:
          return this->f_.set_max_portions_cmd(v);
      }
    }
  }
  return Result::NONE;
}

}  // namespace precision_feeder
}  // namespace esphome
