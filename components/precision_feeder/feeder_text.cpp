// Precision Feeder core: schedule editor parsing and the status texts shown in Home Assistant.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_internal.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace esphome {
namespace precision_feeder {

// ---------------------------------------------------------------------------------------------
// Editor helpers
// ---------------------------------------------------------------------------------------------

void FeederCore::restart_cadence_today(int profile) {
  if (!this->time_valid())
    return;
  Profile &p = profile < 0 ? this->edit_profile() : this->cfg.profiles[std::min<int>(profile, NUM_PROFILES - 1)];
  p.anchor_day = static_cast<uint32_t>(this->today_());
  this->cfg_changed();
}

void FeederCore::set_event_anchor(uint8_t anchor) {
  Event &e = this->edit_event();
  anchor = std::min<uint8_t>(anchor, static_cast<uint8_t>(Anchor::SUNSET));
  const bool was_clock = e.anchor == static_cast<uint8_t>(Anchor::CLOCK);
  const bool now_clock = anchor == static_cast<uint8_t>(Anchor::CLOCK);
  if (was_clock != now_clock)
    e.offset_min = now_clock ? 12 * 60 : 0;
  e.anchor = anchor;
  this->cfg_changed();
}

std::string FeederCore::format_days(uint8_t mask) {
  mask &= 0x7F;
  if (mask == 0x7F)
    return "all";
  if (mask == 0)
    return "none";
  std::string out;
  for (int i = 0; i < 7; i++) {
    if (mask & (1 << i)) {
      if (!out.empty())
        out += ",";
      out += DAY_NAMES[i];
    }
  }
  return out;
}

bool FeederCore::parse_days(const std::string &text, uint8_t &mask) {
  uint8_t result = 0;
  std::string token;
  auto flush = [&](const std::string &t) -> bool {
    if (t.empty())
      return true;
    if (t == "all" || t == "daily" || t == "everyday") {
      result |= 0x7F;
    } else if (t == "none" || t == "off") {
      // adds nothing
    } else if (t == "weekdays") {
      result |= 0x3E;
    } else if (t == "weekend" || t == "weekends") {
      result |= 0x41;
    } else {
      static const char *const FULL[7] = {"sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday"};
      bool found = false;
      for (int i = 0; i < 7; i++) {
        const std::string full = FULL[i];
        if (t == DAY_NAMES[i] || t == full || t == full + "s") {
          result |= (1 << i);
          found = true;
          break;
        }
      }
      if (!found)
        return false;
    }
    return true;
  };
  for (char c : text) {
    if (c == ',' || c == ' ' || c == ';' || c == '/') {
      if (!flush(token))
        return false;
      token.clear();
    } else {
      token += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
  }
  if (!flush(token))
    return false;
  mask = result;
  return true;
}

std::string FeederCore::event_time_text() {
  const Event &e = this->edit_event();
  char buf[16];
  if (e.anchor == static_cast<uint8_t>(Anchor::CLOCK)) {
    std::snprintf(buf, sizeof(buf), "%02d:%02d", e.offset_min / 60, e.offset_min % 60);
  } else {
    const int a = std::abs(static_cast<int>(e.offset_min));
    std::snprintf(buf, sizeof(buf), "%c%02d:%02d", e.offset_min < 0 ? '-' : '+', a / 60, a % 60);
  }
  return buf;
}

bool FeederCore::set_event_time_text(const std::string &text) {
  Event &e = this->edit_event();
  const char *s = text.c_str();
  while (*s == ' ')
    s++;
  int sign = 1;
  bool has_sign = false;
  if (*s == '+' || *s == '-') {
    has_sign = true;
    sign = *s == '-' ? -1 : 1;
    s++;
  }
  int h = 0, m = 0;
  int minutes;
  if (std::sscanf(s, "%d:%d", &h, &m) == 2) {
    if (h < 0 || m < 0 || m > 59)
      return false;
    minutes = h * 60 + m;
  } else if (std::sscanf(s, "%d", &h) == 1 && h >= 0 && has_sign) {
    minutes = h;  // "+90" means plus 90 minutes (solar anchors only)
  } else {
    return false;
  }
  if (e.anchor == static_cast<uint8_t>(Anchor::CLOCK)) {
    if (has_sign || minutes > 24 * 60 - 1)
      return false;
    e.offset_min = static_cast<int16_t>(minutes);
  } else {
    if (minutes > 720)
      return false;
    e.offset_min = static_cast<int16_t>(sign * minutes);
  }
  this->cfg_changed();
  return true;
}

bool FeederCore::add_oneshot(const std::string &local_datetime, uint8_t portions) {
  int y, mo, d, h, mi;
  if (std::sscanf(local_datetime.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5)
    return false;
  if (y < 2019 || y > 2105 || mo < 1 || mo > 12 || d < 1 || h < 0 || h > 23 || mi < 0 || mi > 59)
    return false;
  // Reject impossible dates (31 February) instead of letting them roll over.
  int cy;
  unsigned cm, cd;
  civil_from_days(days_from_civil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)), cy, cm, cd);
  if (cy != y || static_cast<int>(cm) != mo || static_cast<int>(cd) != d)
    return false;
  if (portions < 1 || portions > this->capacity())
    return false;
  const int64_t at = this->hal_local_to_epoch(y, mo, d, h, mi);
  if (at <= 0)
    return false;
  if (this->time_valid() && at <= this->hal_epoch())
    return false;  // in the past
  for (uint8_t i = 0; i < MAX_ONESHOTS; i++) {
    OneShot &os = this->cfg.oneshots[i];
    if (os.epoch == 0 || os.done) {
      os.epoch = static_cast<uint32_t>(at);
      os.portions = portions;
      os.done = 0;
      this->oneshot_pending_[i] = NO_PENDING;
      this->cfg_changed();
      return true;
    }
  }
  return false;  // list full
}

void FeederCore::clear_oneshots() {
  std::memset(this->cfg.oneshots, 0, sizeof(this->cfg.oneshots));
  for (auto &p : this->oneshot_pending_)
    p = NO_PENDING;
  this->cfg_changed();
}

// ---------------------------------------------------------------------------------------------
// Status text
// ---------------------------------------------------------------------------------------------

const char *FeederCore::result_name(Result r) {
  switch (r) {
    case Result::OK:
      return "ok";
    case Result::BUSY:
      return "busy";
    case Result::TOO_SOON:
      return "too soon";
    case Result::DAILY_CAP:
      return "daily cap reached";
    case Result::EMPTY:
      return "tray empty";
    case Result::UNKNOWN_POSITION:
      return "position unknown";
    case Result::CALIBRATING:
      return "calibrating";
    case Result::HOLD:
      return "hold or reload pause";
    case Result::INVALID_ARG:
      return "invalid argument";
    case Result::DUPLICATE:
      return "duplicate";
    case Result::TIME_INVALID:
      return "clock not set";
    case Result::PORTION_LIMIT:
      return "too many portions for one command";
    case Result::IGNORED_NTP_ACTIVE:
      return "clock from BLE ignored (network time is fresh)";
    case Result::BLE_TIME_DISABLED:
      return "clock from BLE disabled";
    case Result::UNSUPPORTED:
      return "unsupported";
    case Result::NOT_CALIBRATING:
      return "calibration mode is off";
    case Result::NONE:
      return "none yet";
  }
  return "?";
}

std::string FeederCore::last_result_text() const { return result_name(this->last_result_); }

TrayState FeederCore::tray_state() const {
  if (this->calibrating_)
    return TrayState::CALIBRATING;
  if (this->moving_())
    return TrayState::MOVING;
  if (this->state_.unknown)
    return TrayState::UNKNOWN;
  if (this->state_.position >= this->capacity())
    return TrayState::EMPTY;
  return TrayState::LOADED;
}

std::string FeederCore::tray_state_text() const {
  switch (this->tray_state()) {
    case TrayState::LOADED:
      return "loaded";
    case TrayState::EMPTY:
      return "empty";
    case TrayState::UNKNOWN:
      return "unknown";
    case TrayState::CALIBRATING:
      return "calibrating";
    case TrayState::MOVING:
      return "moving";
  }
  return "?";
}

uint16_t FeederCore::feeds_today() {
  uint16_t total = 0;
  const bool time_ok = this->time_valid();
  const int64_t midnight = time_ok ? this->wall_to_epoch_(this->today_(), 0) : 0;
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    const HistoryEntry e = this->history_(i);
    // Without a clock, "today" is whatever was fed since the clock was lost (stamped 0).
    if (e.portions != 0 && (time_ok ? static_cast<int64_t>(e.epoch) >= midnight : e.epoch == 0))
      total += e.portions;
  }
  return total;
}

std::string FeederCore::next_event_text() {
  if (!this->time_valid())
    return "clock not set";
  if (this->next_event_epoch_ == 0)
    return "none";
  return this->format_local_(this->next_event_epoch_);
}

std::string FeederCore::empty_on_text() {
  if (!this->time_valid())
    return "clock not set";
  if (this->remaining() == 0)
    return "empty now";
  if (this->empty_on_epoch_ == 0)
    return "not within 30 days";
  return this->format_local_(this->empty_on_epoch_);
}

std::string FeederCore::last_feed_text() {
  if (this->state_.last_feed_epoch == 0)
    return this->history_(0).portions != 0 ? "clock was not set" : "never";
  return this->format_local_(this->state_.last_feed_epoch) + " (" + source_name(static_cast<Source>(this->state_.last_src)) + ")";
}

std::string FeederCore::recent_feeds_text() {
  std::string out;
  for (uint8_t i = 0; i < HISTORY_SIZE && i < 6; i++) {
    const HistoryEntry e = this->history_(i);
    if (e.portions == 0)
      continue;
    if (!out.empty())
      out += "; ";
    if (e.epoch == 0) {
      out += "unknown time";
    } else {
      const LocalTime l = this->hal_local(e.epoch);
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", l.month, l.day, l.hour, l.minute);
      out += buf;
    }
    out += " x" + std::to_string(e.portions) + " " + source_name(static_cast<Source>(e.source));
  }
  return out.empty() ? "none" : out;
}

std::string FeederCore::oneshots_text() {
  std::string out;
  for (const OneShot &os : this->cfg.oneshots) {
    if (os.epoch == 0 || os.done)
      continue;
    if (!out.empty())
      out += "; ";
    out += this->format_local_(os.epoch) + " x" + std::to_string(os.portions);
  }
  return out.empty() ? "none" : out;
}

std::string FeederCore::faults_text() {
  std::string out;
  auto add = [&](const char *s) {
    if (!out.empty())
      out += "; ";
    out += s;
  };
  if (!this->time_valid())
    add("clock not set");
  if (this->time_stale())
    add("clock not re-synchronised for over a week");
  if (this->state_.unknown)
    add("tray position unknown");
  if (this->state_.position >= this->capacity())
    add("tray empty");
  else if (this->needs_food())
    add("next feed needs more portions than remain");
  if (this->solar_fault_)
    add("sunrise/sunset unavailable (set location)");
  if (this->polar_fallback_)
    add("no sunrise or sunset today: sun events use 06:00 / 18:00");
  if (this->oversubscribed_)
    add("schedule needs more portions than the tray holds before the next refill");
  if (this->schedule_over_cap())
    add("schedule feeds more per day than the daily cap allows (some events will be refused)");
  return out.empty() ? "none" : out;
}

uint16_t FeederCore::max_portions_per_day(int profile) const {
  const Profile &p = this->cfg.profiles[profile < 0 ? this->cfg.active_profile : std::min<int>(profile, NUM_PROFILES - 1)];
  uint16_t most = 0;
  for (int wday = 0; wday < 7; wday++) {
    if (p.fast_days & (1 << wday))
      continue;
    uint16_t total = 0;
    for (const Event &e : p.ev)
      if (e.enabled && (e.days & (1 << wday)))
        total += e.portions;
    most = std::max(most, total);
  }
  return most;
}

bool FeederCore::schedule_over_cap() const {
  return this->cfg.scheduler_enabled && this->max_portions_per_day() > this->cfg.daily_cap;
}

std::string FeederCore::edited_profile_summary() {
  const uint8_t ep = this->editor_profile;
  std::string out = this->profile_summary_(ep);
  out += ep == this->cfg.active_profile ? " [active]" : " [not active]";
  const uint16_t most = this->max_portions_per_day(ep);
  if (most > this->cfg.daily_cap)
    out += " ! " + std::to_string(most) + " portions/day > cap " + std::to_string(this->cfg.daily_cap);
  return out;
}

std::string FeederCore::schedule_summary() { return this->profile_summary_(this->cfg.active_profile); }

std::string FeederCore::profile_summary_(uint8_t profile) {
  const Profile &p = this->cfg.profiles[profile];
  std::string out(1, static_cast<char>('A' + profile));
  out += ": ";
  // List the events in the order they happen in a day: sun events by today's sun times when the clock and
  // location are known, otherwise as if sunrise were 06:00 and sunset 18:00.
  int order[EVENTS_PER_PROFILE];
  int key[EVENTS_PER_PROFILE];
  const bool time_ok = this->time_valid();
  for (uint8_t i = 0; i < EVENTS_PER_PROFILE; i++) {
    const Event &e = p.ev[i];
    order[i] = i;
    int minutes = e.offset_min;
    if (e.anchor != static_cast<uint8_t>(Anchor::CLOCK)) {
      if (!time_ok || !this->resolve_minutes_(e, this->make_day_(this->today_()), minutes))
        minutes = (e.anchor == static_cast<uint8_t>(Anchor::SUNRISE) ? POLAR_SUNRISE_MIN : POLAR_SUNSET_MIN) + e.offset_min;
    }
    key[i] = minutes;
  }
  std::stable_sort(order, order + EVENTS_PER_PROFILE, [&](int a, int b) { return key[a] < key[b]; });
  bool any = false;
  for (int idx : order) {
    const Event &e = p.ev[idx];
    if (!e.enabled)
      continue;
    if (any)
      out += ", ";
    any = true;
    char buf[40];
    if (e.anchor == static_cast<uint8_t>(Anchor::CLOCK)) {
      std::snprintf(buf, sizeof(buf), "%02d:%02d", e.offset_min / 60, e.offset_min % 60);
    } else {
      const int a = std::abs(static_cast<int>(e.offset_min));
      std::snprintf(buf, sizeof(buf), "%s%c%02d:%02d",
                    e.anchor == static_cast<uint8_t>(Anchor::SUNRISE) ? "sunrise" : "sunset",
                    e.offset_min < 0 ? '-' : '+', a / 60, a % 60);
    }
    out += buf;
    out += " x" + std::to_string(e.portions);
    if (e.days != 0x7F)
      out += " (" + format_days(e.days) + ")";
  }
  if (!any)
    out += "no events";
  if (p.every_n > 1)
    out += "; every " + std::to_string(p.every_n) + " days";
  if (p.fast_days != 0)
    out += "; fast: " + format_days(p.fast_days);
  if (!this->cfg.scheduler_enabled)
    out += " (scheduler off)";
  return out;
}

std::string FeederCore::schedule_data_text() const {
  static const char ANCHORS[3] = {'c', 'r', 's'};
  std::string out = "1";
  char buf[24];
  for (const Profile &p : this->cfg.profiles) {
    std::snprintf(buf, sizeof(buf), "|%u,%02x", static_cast<unsigned>(p.every_n), static_cast<unsigned>(p.fast_days & 0x7F));
    out += buf;
    for (const Event &e : p.ev) {
      std::snprintf(buf, sizeof(buf), ",%u%c%d/%u/%02x", e.enabled ? 1u : 0u, ANCHORS[e.anchor % 3],
                    static_cast<int>(e.offset_min), static_cast<unsigned>(e.portions),
                    static_cast<unsigned>(e.days & 0x7F));
      out += buf;
    }
  }
  return out;
}

std::string FeederCore::device_time_text() {
  if (!this->time_valid())
    return "clock not set";
  const int64_t now = this->hal_epoch();
  const int offset_min = static_cast<int>(this->local_offset_s_(now) / 60);
  const int a = std::min(std::abs(offset_min), 24 * 60 - 1);  // real offsets are within +-14 h
  char buf[16];
  std::snprintf(buf, sizeof(buf), " %c%02d:%02d", offset_min < 0 ? '-' : '+', a / 60, a % 60);
  return this->format_local_(now) + buf;
}

uint8_t FeederCore::led_code() {
  switch (this->tray_state()) {
    case TrayState::CALIBRATING:
      return 5;
    case TrayState::MOVING:
      return 4;
    case TrayState::UNKNOWN:
      return 3;
    case TrayState::EMPTY:
      return 2;
    default:
      break;
  }
  if (!this->time_valid())
    return 1;
  if (this->hold_active() || this->reload_pause_active())
    return 6;
  return 0;
}

}  // namespace precision_feeder
}  // namespace esphome
