// Precision Feeder core: local time, the scheduler (profiles, sun events, one-shots) and the projection.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
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
// Local time helpers
// ---------------------------------------------------------------------------------------------

int64_t FeederCore::local_offset_s_(int64_t epoch) {
  const LocalTime l = this->hal_local(epoch);
  const int64_t local = static_cast<int64_t>(days_from_civil(l.year, l.month, l.day)) * 86400 + l.hour * 3600 +
                        l.minute * 60 + l.second;
  return local - epoch;
}

/// Epoch of a wall-clock moment: `minutes` after local midnight of day `day_no` (may be negative or past
/// 1440). Going through local fields keeps events right on daylight-saving change days.
int64_t FeederCore::wall_to_epoch_(int32_t day_no, int minutes) {
  const int32_t extra = minutes >= 0 ? minutes / 1440 : -((-minutes + 1439) / 1440);
  const int in_day = minutes - extra * 1440;
  int y;
  unsigned m, d;
  civil_from_days(day_no + extra, y, m, d);
  return this->hal_local_to_epoch(y, static_cast<int>(m), static_cast<int>(d), in_day / 60, in_day % 60);
}

int32_t FeederCore::today_() {
  const LocalTime l = this->hal_local(this->hal_epoch());
  return days_from_civil(l.year, l.month, l.day);
}

FeederCore::DayCtx FeederCore::make_day_(int32_t day_no) {
  const int64_t hour = this->hal_epoch() / 3600;
  if (hour != this->day_cache_hour_) {
    for (auto &used : this->day_cache_used_)
      used = false;
    this->day_cache_hour_ = hour;
  }
  for (uint8_t i = 0; i < DAY_CACHE_SIZE; i++) {
    if (this->day_cache_used_[i] && this->day_cache_[i].day_no == day_no)
      return this->day_cache_[i];
  }
  DayCtx d;
  d.day_no = day_no;
  d.wday = weekday_from_days(day_no);
  d.midnight = this->wall_to_epoch_(day_no, 0);
  d.tz_min = static_cast<int>(this->local_offset_s_(this->wall_to_epoch_(day_no, 12 * 60)) / 60);
  this->day_cache_[this->day_cache_next_] = d;
  this->day_cache_used_[this->day_cache_next_] = true;
  this->day_cache_next_ = (this->day_cache_next_ + 1) % DAY_CACHE_SIZE;
  return d;
}

std::string FeederCore::format_local_(int64_t epoch, bool with_date) {
  const LocalTime l = this->hal_local(epoch);
  char buf[40];
  if (with_date) {
    const int wday = weekday_from_days(days_from_civil(l.year, l.month, l.day));
    std::snprintf(buf, sizeof(buf), "%s %04d-%02d-%02d %02d:%02d", DAY_ABBR[wday], l.year, l.month, l.day, l.hour,
                  l.minute);
  } else {
    std::snprintf(buf, sizeof(buf), "%02d:%02d", l.hour, l.minute);
  }
  return buf;
}

// ---------------------------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------------------------

bool FeederCore::sun_cached_(const DayCtx &d, double &rise, double &set) {
  for (auto &c : this->sun_cache_) {
    if (c.day_no == d.day_no && c.tz_min == d.tz_min && c.lat == this->cfg.latitude && c.lon == this->cfg.longitude) {
      rise = c.rise;
      set = c.set;
      return c.ok;
    }
  }
  SunCache &c = this->sun_cache_[this->sun_cache_next_];
  this->sun_cache_next_ = (this->sun_cache_next_ + 1) % DAY_CACHE_SIZE;
  c.day_no = d.day_no;
  c.tz_min = d.tz_min;
  c.lat = this->cfg.latitude;
  c.lon = this->cfg.longitude;
  c.ok = sun_times(d.day_no, c.lat, c.lon, d.tz_min, c.rise, c.set);
  rise = c.rise;
  set = c.set;
  return c.ok;
}

bool FeederCore::resolve_minutes_(const Event &e, const DayCtx &d, int &minutes) {
  if (e.anchor == static_cast<uint8_t>(Anchor::CLOCK)) {
    minutes = e.offset_min;
    return true;
  }
  if (this->cfg.latitude == 0.0f && this->cfg.longitude == 0.0f)
    return false;  // location not set: skipped (a setup mistake, reported in Faults)
  const bool sunrise = e.anchor == static_cast<uint8_t>(Anchor::SUNRISE);
  double rise, set;
  double base;
  if (this->sun_cached_(d, rise, set))
    base = sunrise ? rise : set;
  else
    base = sunrise ? POLAR_SUNRISE_MIN : POLAR_SUNSET_MIN;  // no sunrise or sunset that day: fixed fallback
  minutes = static_cast<int>(std::lround(base)) + e.offset_min;
  return true;
}

bool FeederCore::day_active_(const Profile &p, const DayCtx &d) const {
  if (p.fast_days & (1 << d.wday))
    return false;
  const int32_t n = std::max<int32_t>(1, p.every_n);
  const int32_t anchor = p.anchor_day != 0 ? static_cast<int32_t>(p.anchor_day) : d.day_no;
  const int32_t diff = d.day_no - anchor;
  return ((diff % n) + n) % n == 0;
}

void FeederCore::on_time_valid_() {
  const int32_t today = this->today_();
  bool anchored = false;
  for (auto &p : this->cfg.profiles) {
    if (p.anchor_day == 0) {
      p.anchor_day = static_cast<uint32_t>(today);
      anchored = true;
    }
  }
  // Feeds made before the clock was set count as happening now: the conservative choice for the daily cap.
  const uint32_t now = static_cast<uint32_t>(this->hal_epoch());
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    HistoryEntry e = this->history_(i);
    if (e.portions != 0 && e.epoch == 0) {
      e.epoch = now;
      this->set_history_(i, e);
    }
  }
  if (anchored)
    this->cfg_changed();  // only then is there anything new to save
  this->project_dirty_ = true;
  this->save_state_(false);
  this->log_(LogLevel::kInfo, "Clock is valid, scheduler running");
}

void FeederCore::schedule_tick_() {
  const bool valid = this->time_valid();
  if (valid && !this->was_time_valid_)
    this->on_time_valid_();
  this->was_time_valid_ = valid;
  if (!valid) {
    this->solar_fault_ = false;
    this->polar_fallback_ = false;
    return;
  }

  const int64_t now = this->hal_epoch();
  if (this->state_.hold_until != 0 && this->state_.hold_until != HOLD_FOREVER &&
      now >= static_cast<int64_t>(this->state_.hold_until)) {
    this->state_.hold_until = 0;
    this->save_state_(false);
    this->project_dirty_ = true;
    this->log_(LogLevel::kInfo, "Hold ended");
  }
  if (this->state_.reload_until != 0 && now >= static_cast<int64_t>(this->state_.reload_until)) {
    this->state_.reload_until = 0;
    this->save_state_(false);
  }
  this->update_solar_fault_();
  if (this->project_dirty_ || this->hal_millis() - this->last_project_ > 60000)
    this->project_();
  this->fire_due_();
}

void FeederCore::update_solar_fault_() {
  // Only meaningful while recurring events can actually run.
  this->solar_fault_ = false;
  this->polar_fallback_ = false;
  if (!this->cfg.scheduler_enabled)
    return;
  const Profile &p = this->cfg.profiles[this->cfg.active_profile];
  const DayCtx d = this->make_day_(this->today_());
  if (!this->day_active_(p, d))
    return;
  for (const Event &ev : p.ev) {
    if (!ev.enabled || ev.anchor == static_cast<uint8_t>(Anchor::CLOCK) || !(ev.days & (1 << d.wday)))
      continue;
    if (this->cfg.latitude == 0.0f && this->cfg.longitude == 0.0f) {
      this->solar_fault_ = true;
      return;
    }
    double rise, set;
    if (!this->sun_cached_(d, rise, set))
      this->polar_fallback_ = true;
  }
}

static bool deferrable(Result r) { return r == Result::BUSY || r == Result::TOO_SOON; }

void FeederCore::mark_fired_(uint8_t profile, uint8_t event, int32_t day) {
  // Only forward, except past a last day that came from a wrong clock (far ahead).
  uint32_t &last = this->state_.fired[profile][event];
  const int64_t ahead = static_cast<int64_t>(last) - day;
  if (ahead <= 0 || ahead > FIRED_AHEAD_MAX_DAYS)
    last = static_cast<uint32_t>(day);
}

template<typename Fn> void FeederCore::for_each_occurrence_(int32_t first_day, int32_t last_day, Fn &&fn) {
  if (!this->cfg.scheduler_enabled)
    return;  // one-shot feeds are separate and run regardless
  const uint8_t ap = this->cfg.active_profile;
  const Profile &p = this->cfg.profiles[ap];
  for (int32_t day = first_day; day <= last_day; day++) {
    const DayCtx d = this->make_day_(day);
    if (!this->day_active_(p, d))
      continue;
    for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
      const Event &ev = p.ev[e];
      if (!ev.enabled || !(ev.days & (1 << d.wday)))
        continue;
      int minutes;
      if (!this->resolve_minutes_(ev, d, minutes))
        continue;  // a sun event with no location: skipped, and reported in Faults
      const int64_t epoch = this->wall_to_epoch_(d.day_no, minutes);
      if (epoch < 0)
        continue;
      // Each event feeds at most once per (anchor) day, also when it is moved later after it fed, and never again for
      // a day on or before the last one it fired for: a clock set back across a date must not repeat it (Codex review,
      // 2.1.1). A last day far ahead (more than FIRED_AHEAD_MAX_DAYS) came from a wrong clock: then only that day.
      const int64_t last = static_cast<int64_t>(this->state_.fired[ap][e]);
      const bool fed = last == d.day_no || (last > d.day_no && last - d.day_no <= FIRED_AHEAD_MAX_DAYS);
      fn(Occurrence{d.day_no, e, epoch, ev.portions, fed});
    }
  }
}

void FeederCore::fire_due_() {
  const int64_t now = this->hal_epoch();
  const int32_t today = this->today_();
  const bool held = this->hold_active() || this->reload_pause_active();
  bool state_dirty = false;

  // Recurring events. Yesterday is included because an event such as "sunset + 3 h" lands after midnight, and
  // tomorrow because one such as "sunrise - 10 h" lands the evening before its day.
  const uint8_t ap = this->cfg.active_profile;
  this->for_each_occurrence_(today - 1, today + 1, [&](const Occurrence &o) {
    if (o.fed || now < o.epoch)
      return;
    uint8_t &pending = this->pending_[ap][o.event];
    const bool pending_here = pending != NO_PENDING && this->pending_day_[ap][o.event] == o.day_no;
    if (now >= o.epoch + EVENT_WINDOW_S) {
      // Window closed. If it was refused while open, report that once; otherwise it was simply missed.
      if (pending_here) {
        this->report_(Source::SCHEDULE, o.portions, static_cast<Result>(pending));
        this->mark_fired_(ap, o.event, o.day_no);
        pending = NO_PENDING;
        state_dirty = true;
      }
      return;
    }
    if (held) {
      pending = static_cast<uint8_t>(Result::HOLD);
      this->pending_day_[ap][o.event] = o.day_no;
      return;
    }
    uint8_t portions = o.portions;
    const Result check = this->check_guard_(portions, Source::SCHEDULE);
    if (deferrable(check)) {
      // Try again next tick while the window is open, without reporting a refusal yet.
      pending = static_cast<uint8_t>(check);
      this->pending_day_[ap][o.event] = o.day_no;
      return;
    }
    // Marked before it feeds, so the feed's own save (its intent) carries it: a power cut cannot repeat it.
    this->mark_fired_(ap, o.event, o.day_no);
    this->request_feed(o.portions, Source::SCHEDULE);  // reports the outcome, accepted or refused
    pending = NO_PENDING;
    state_dirty = true;
  });

  // One-shot (calendar-style) feeds: independent of the scheduler switch.
  bool cfg_dirty = false;
  for (uint8_t i = 0; i < MAX_ONESHOTS; i++) {
    OneShot &os = this->cfg.oneshots[i];
    if (os.epoch == 0 || os.done)
      continue;
    const int64_t at = static_cast<int64_t>(os.epoch);
    if (now < at)
      continue;
    uint8_t &pending = this->oneshot_pending_[i];
    if (now >= at + EVENT_WINDOW_S) {
      if (pending != NO_PENDING) {
        this->report_(Source::ONE_SHOT, os.portions, static_cast<Result>(pending));
        pending = NO_PENDING;
      } else {
        this->log_(LogLevel::kWarn, "One-shot feed at %s missed", this->format_local_(at).c_str());
      }
      os.done = 1;
      cfg_dirty = true;
      continue;
    }
    if (held) {
      pending = static_cast<uint8_t>(Result::HOLD);
      continue;
    }
    uint8_t portions = os.portions;
    const Result check = this->check_guard_(portions, Source::ONE_SHOT);
    if (deferrable(check)) {
      pending = static_cast<uint8_t>(check);
      continue;
    }
    // Done first, and on flash, before it feeds: otherwise a power cut after the feed and before the settings' delayed
    // save would feed it again in the same minute (Codex review, 2.1.1). A missed one-shot beats a repeated one.
    pending = NO_PENDING;
    os.done = 1;
    if (!this->hal_save_settings(this->cfg, true)) {
      this->log_(LogLevel::kError, "Saving the one-shot feed as done failed: not feeding it");
      this->storage_fault_ = true;
      this->report_(Source::ONE_SHOT, os.portions, Result::STORAGE_FAILED);
      cfg_dirty = true;
      continue;
    }
    this->request_feed(os.portions, Source::ONE_SHOT);
  }
  if (state_dirty)
    this->save_state_(false);
  if (cfg_dirty)
    this->cfg_changed();
}

int64_t FeederCore::blocked_until_(int64_t now) {
  int64_t until = 0;
  if (this->state_.hold_until == HOLD_FOREVER)
    return INT64_MAX;
  if (this->state_.hold_until != 0)
    until = static_cast<int64_t>(this->state_.hold_until);
  if (this->reload_pause_active()) {
    int64_t pause_end = this->state_.reload_until != 0 ? static_cast<int64_t>(this->state_.reload_until) : 0;
    if (this->reload_pause_ms_active_) {
      const int32_t left_ms = static_cast<int32_t>(this->reload_pause_until_ms_ - this->hal_millis());
      if (left_ms > 0)
        pause_end = std::max<int64_t>(pause_end, now + (left_ms + 999) / 1000);
    }
    until = std::max(until, pause_end);
  }
  return until;
}

void FeederCore::project_() {
  this->project_dirty_ = false;
  this->last_project_ = this->hal_millis();
  this->next_event_epoch_ = 0;
  this->next_event_portions_ = 0;
  this->empty_on_epoch_ = 0;
  this->runway_days_ = 255;
  this->oversubscribed_ = false;
  if (!this->time_valid())
    return;

  const int64_t now = this->hal_epoch();
  const int32_t today = this->today_();
  // Events inside a hold or the reload pause are skipped by fire_due_, so they are not "next" either.
  const int64_t blocked_until = this->blocked_until_(now);

  struct Item {
    int64_t epoch;
    uint8_t portions;
  };
  std::vector<Item> items;
  items.reserve(64);
  // The same occurrences the scheduler fires (for_each_occurrence_), minus those already fed, past, or inside a
  // hold or the reload pause: exactly the ones that will run, if nothing else changes.
  this->for_each_occurrence_(today - 1, today + HORIZON_DAYS, [&](const Occurrence &o) {
    if (o.fed || o.epoch <= now || o.epoch < blocked_until)
      return;
    items.push_back({o.epoch, o.portions});
  });
  for (const OneShot &os : this->cfg.oneshots) {
    if (os.epoch == 0 || os.done || static_cast<int64_t>(os.epoch) <= now)
      continue;
    if (static_cast<int64_t>(os.epoch) < blocked_until)
      continue;
    items.push_back({static_cast<int64_t>(os.epoch), os.portions});
  }
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.epoch < b.epoch; });

  if (!items.empty()) {
    this->next_event_epoch_ = items.front().epoch;
    this->next_event_portions_ = items.front().portions;
  }

  const uint32_t remaining = this->remaining();
  if (remaining == 0) {
    this->runway_days_ = 0;
    return;
  }
  uint32_t cumulative = 0;
  uint32_t within_refill = 0;
  const int64_t refill_end = now + static_cast<int64_t>(this->cfg.refill_interval_days) * 86400;
  bool empty_found = false;
  for (const Item &it : items) {
    cumulative += it.portions;
    if (it.epoch < refill_end)
      within_refill += it.portions;
    if (!empty_found && cumulative >= remaining) {
      empty_found = true;
      this->empty_on_epoch_ = it.epoch;
      this->runway_days_ = static_cast<uint8_t>(std::min<int64_t>((it.epoch - now) / 86400, 254));
    }
  }
  this->oversubscribed_ = within_refill > remaining;
}

std::string FeederCore::todays_feeds_text() {
  if (!this->time_valid())
    return "clock not set";
  const int64_t now = this->hal_epoch();
  const int32_t today = this->today_();
  const int64_t day_start = this->wall_to_epoch_(today, 0);
  const int64_t day_end = this->wall_to_epoch_(today + 1, 0);
  const int64_t blocked_until = this->blocked_until_(now);

  // Whether a feed from `source` was recorded in the minute an item was due (the history keeps the last 8 feeds).
  auto fed_at = [this](int64_t due, Source source) {
    for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
      const HistoryEntry h = this->history_(i);
      if (h.portions != 0 && h.source == static_cast<uint8_t>(source) && static_cast<int64_t>(h.epoch) >= due &&
          static_cast<int64_t>(h.epoch) < due + EVENT_WINDOW_S)
        return true;
    }
    return false;
  };
  // An item not yet run: due in its minute now, held (a hold or the reload pause will skip it), or later.
  auto pending_status = [&](int64_t due) -> const char * {
    if (now >= due + EVENT_WINDOW_S)
      return "skipped";
    if (due < blocked_until)
      return "held";
    return now >= due ? "due" : "later";
  };

  struct Item {
    int64_t epoch;
    uint8_t portions;
    bool oneshot;
    const char *status;
  };
  std::vector<Item> items;
  // Yesterday's and tomorrow's anchor days too: "sunset + 3 h" can land after midnight, "sunrise - 10 h" before it.
  this->for_each_occurrence_(today - 1, today + 1, [&](const Occurrence &o) {
    if (o.epoch < day_start || o.epoch >= day_end)
      return;
    const char *status = o.fed ? (fed_at(o.epoch, Source::SCHEDULE) ? "fed" : "skipped") : pending_status(o.epoch);
    items.push_back({o.epoch, o.portions, false, status});
  });
  for (const OneShot &os : this->cfg.oneshots) {
    const int64_t at = static_cast<int64_t>(os.epoch);
    if (os.epoch == 0 || at < day_start || at >= day_end)
      continue;
    const char *status = os.done ? (fed_at(at, Source::ONE_SHOT) ? "fed" : "skipped") : pending_status(at);
    items.push_back({at, os.portions, true, status});
  }
  std::stable_sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.epoch < b.epoch; });
  for (Item &it : items) {
    if (std::strcmp(it.status, "later") == 0) {
      it.status = "next";  // the first one that will run
      break;
    }
  }

  if (items.empty()) {
    if (!this->cfg.scheduler_enabled)
      return "none (scheduler off)";
    const Profile &p = this->cfg.profiles[this->cfg.active_profile];
    const DayCtx d = this->make_day_(today);
    if (p.fast_days & (1 << d.wday))
      return "none (fast day)";
    if (!this->day_active_(p, d))
      return "none (not a feeding day)";
    return "none";
  }
  // Home Assistant keeps at most 255 characters of state; a busy day with many one-shots is cut short.
  constexpr size_t MAX_LEN = 240;
  std::string out;
  for (const Item &it : items) {
    std::string entry = this->format_local_(it.epoch, false) + " x" + std::to_string(it.portions) + " " + it.status;
    if (it.oneshot)
      entry += " one-shot";
    if (!out.empty() && out.size() + 2 + entry.size() > MAX_LEN) {
      out += "; ...";
      break;
    }
    if (!out.empty())
      out += "; ";
    out += entry;
  }
  return out;
}

}  // namespace precision_feeder
}  // namespace esphome
