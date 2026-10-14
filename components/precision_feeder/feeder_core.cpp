// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "feeder_core.h"

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

static constexpr int HORIZON_DAYS = 30;
static constexpr uint32_t CFG_SAVE_DELAY_MS = 2000;
static constexpr int64_t STALE_CLOCK_MIN = 7 * 24 * 60;
// Late feeds are skipped by design. An event only fires during the minute it is due; if the feeder was
// offline, on hold, reloading, or the guard refused it for that whole minute, the event is lost. The window
// exists only so a BUSY or TOO_SOON answer can be retried on the next tick.
static constexpr int64_t EVENT_WINDOW_S = 60;
static constexpr uint8_t NO_PENDING = static_cast<uint8_t>(Result::NONE);

static const char *const DAY_NAMES[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
static const char *const DAY_ABBR[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *const SOURCE_NAMES[6] = {"schedule", "home assistant", "ble", "button", "one-shot", "adjustment"};

static const char *source_name(Source s) { return SOURCE_NAMES[static_cast<uint8_t>(s) % 6]; }

// ---------------------------------------------------------------------------------------------
// Start-up, persistence, logging
// ---------------------------------------------------------------------------------------------

void FeederCore::log_(LogLevel level, const char *fmt, ...) {
  char buf[192];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  this->hal_log(level, buf);
}

void FeederCore::defaults_() {
  std::memset(&this->cfg, 0, sizeof(this->cfg));
  this->cfg.version = SETTINGS_VERSION;
  for (auto &p : this->cfg.profiles) {
    p.every_n = 1;
    // Sensible starting events, all disabled: nothing feeds until the user turns events on.
    const int16_t times[EVENTS_PER_PROFILE] = {8 * 60, 12 * 60, 18 * 60, 20 * 60};
    for (uint8_t i = 0; i < EVENTS_PER_PROFILE; i++) {
      p.ev[i].enabled = 0;
      p.ev[i].anchor = static_cast<uint8_t>(Anchor::CLOCK);
      p.ev[i].offset_min = times[i];
      p.ev[i].portions = 1;
      p.ev[i].days = 0x7F;
    }
  }
  this->cfg.min_gap_s = 60;
  this->cfg.portion_gap_s = 5;
  this->cfg.reload_pause_min = 10;
  this->cfg.refill_interval_days = 3;
  this->cfg.daily_cap = 4;
  this->cfg.max_portions_cmd = 1;
  this->cfg.active_profile = 0;
  this->cfg.scheduler_enabled = 1;
  this->cfg.min_pulse_us = this->default_min_pulse_us_;
  this->cfg.max_pulse_us = this->default_max_pulse_us_;

  std::memset(&this->state_, 0, sizeof(this->state_));
  this->state_.version = STATE_VERSION;
  this->state_.moving_to = NO_MOVE;
}

void FeederCore::clamp_loaded_() {
  for (auto &p : this->cfg.profiles) {
    p.every_n = std::min<uint8_t>(std::max<uint8_t>(p.every_n, 1), 14);
    p.fast_days &= 0x7F;
    for (auto &e : p.ev) {
      e.anchor = std::min<uint8_t>(e.anchor, static_cast<uint8_t>(Anchor::SUNSET));
      e.portions = std::min<uint8_t>(std::max<uint8_t>(e.portions, 1), MAX_PORTIONS);
      e.days &= 0x7F;
    }
  }
  for (auto &c : this->cfg.cal)
    c = std::min<int16_t>(std::max<int16_t>(c, -150), 150);
  this->cfg.active_profile = std::min<uint8_t>(this->cfg.active_profile, NUM_PROFILES - 1);
  this->cfg.daily_cap = std::min<uint8_t>(std::max<uint8_t>(this->cfg.daily_cap, 1), MAX_PORTIONS);
  this->cfg.max_portions_cmd = std::min<uint8_t>(std::max<uint8_t>(this->cfg.max_portions_cmd, 1), MAX_PORTIONS);
  this->state_.position = std::min<uint8_t>(this->state_.position, MAX_PORTIONS);
  if (!pulse_range_ok(this->cfg.min_pulse_us, this->cfg.max_pulse_us)) {
    this->cfg.min_pulse_us = this->default_min_pulse_us_;
    this->cfg.max_pulse_us = this->default_max_pulse_us_;
  }
}

void FeederCore::save_state_(bool flush) { this->hal_save_state(this->state_, flush); }

void FeederCore::cfg_changed() {
  this->project_dirty_ = true;
  this->cfg_dirty_ = true;
  this->cfg_dirty_since_ = this->hal_millis();
}

void FeederCore::save_calibration() {
  this->hal_save_settings(this->cfg, true);
  this->cfg_dirty_ = false;
}

void FeederCore::core_start(const Settings *settings, const State *state) {
  this->defaults_();
  if (settings != nullptr && settings->version == SETTINGS_VERSION) {
    this->cfg = *settings;
  } else {
    this->log_(LogLevel::kInfo, "No saved settings, using defaults");
  }
  if (state != nullptr && state->version == STATE_VERSION) {
    this->state_ = *state;
  } else {
    this->log_(LogLevel::kInfo, "No saved tray state, assuming a freshly loaded tray at position 0");
  }
  this->clamp_loaded_();
  this->clock_kept_ = this->hal_time_valid();
  if (this->clock_kept_)
    this->log_(LogLevel::kInfo, "Clock kept across a restart (valid at boot, not a fresh sync)");

  for (auto &row : this->pending_)
    for (auto &p : row)
      p = NO_PENDING;
  for (auto &row : this->pending_day_)
    for (auto &d : row)
      d = INT32_MIN;
  for (auto &p : this->oneshot_pending_)
    p = NO_PENDING;

  // Feed history stamped with an unknown time cannot be aged after a reboot: drop it.
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    this->hist_this_boot_[i] = false;
    if (this->state_.hist_n[i] != 0 && this->state_.hist_ts[i] == 0)
      this->state_.hist_n[i] = 0;
  }

  if (this->state_.moving_to != NO_MOVE) {
    // Power was lost in the middle of a step: the tray may or may not have advanced.
    this->log_(LogLevel::kWarn, "Unfinished move to position %u found: tray position is now UNKNOWN",
               this->state_.moving_to);
    this->state_.unknown = 1;
    this->state_.moving_to = NO_MOVE;
    this->save_state_(true);
  }

  if (!this->state_.unknown) {
    // Go to the saved position (never to 0), then let the servo go limp after a moment.
    this->move_to_(this->state_.position);
    this->phase_ = Phase::SETTLE;
    this->phase_start_ = this->hal_millis();
  }
}

void FeederCore::core_loop() {
  this->run_motion_();
  const uint32_t now = this->hal_millis();
  if (this->cfg_dirty_ && now - this->cfg_dirty_since_ >= CFG_SAVE_DELAY_MS) {
    this->hal_save_settings(this->cfg, false);
    this->cfg_dirty_ = false;
  }
  if (now - this->last_tick_ >= 1000) {
    this->last_tick_ = now;
    this->schedule_tick_();
  }
}

// ---------------------------------------------------------------------------------------------
// Servo and motion
// ---------------------------------------------------------------------------------------------

float FeederCore::angle_for_(uint8_t position) const {
  position = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  const float margin = (180.0f - this->sweep_) / 2.0f;
  const float pitch = this->sweep_ / NUM_POSITIONS;
  float angle = margin + position * pitch + this->cfg.cal[position] / 10.0f;
  angle = std::min(180.0f, std::max(0.0f, angle));
  return this->invert_ ? 180.0f - angle : angle;
}

bool FeederCore::moving_() const {
  return this->phase_ == Phase::STEP_A || this->phase_ == Phase::STEP_B || this->phase_ == Phase::STEP_C ||
         this->phase_ == Phase::GAP;
}

void FeederCore::start_next_portion_() {
  const uint8_t target = this->state_.position + 1;
  // Persist the intent before moving, so a power loss mid-step is detected at the next boot.
  this->state_.moving_to = target;
  this->save_state_(true);
  this->move_to_(target);
  this->phase_ = Phase::STEP_A;
  this->phase_start_ = this->hal_millis();
}

void FeederCore::commit_step_() {
  this->state_.position = this->state_.moving_to;
  this->state_.moving_to = NO_MOVE;
  this->save_state_(true);
  this->project_dirty_ = true;
  if (this->queue_ > 0)
    this->queue_--;
  this->phase_start_ = this->hal_millis();
  if (this->queue_ > 0 && this->state_.position < MAX_PORTIONS) {
    this->phase_ = Phase::GAP;
  } else {
    this->phase_ = Phase::SETTLE;
    this->queue_ = 0;
  }
}

void FeederCore::run_motion_() {
  const uint32_t now = this->hal_millis();
  switch (this->phase_) {
    case Phase::IDLE:
      return;
    case Phase::STEP_A:
      if (now - this->phase_start_ < this->step_ms_)
        return;
      if (this->wiggle_) {
        // Step back and forward again to shake the food loose.
        this->move_to_(this->state_.position);
        this->phase_ = Phase::STEP_B;
        this->phase_start_ = now;
      } else {
        this->commit_step_();
      }
      return;
    case Phase::STEP_B:
      if (now - this->phase_start_ < this->step_ms_)
        return;
      this->move_to_(this->state_.moving_to);
      this->phase_ = Phase::STEP_C;
      this->phase_start_ = now;
      return;
    case Phase::STEP_C:
      if (now - this->phase_start_ < this->step_ms_)
        return;
      this->commit_step_();
      return;
    case Phase::GAP:
      if (now - this->phase_start_ < static_cast<uint32_t>(this->cfg.portion_gap_s) * 1000UL)
        return;
      this->start_next_portion_();
      return;
    case Phase::SETTLE:
      if (now - this->phase_start_ < this->detach_ms_)
        return;
      if (!this->calibrating_)
        this->hal_servo_release();
      this->phase_ = Phase::IDLE;
      return;
  }
}

void FeederCore::abort_motion_() {
  switch (this->phase_) {
    case Phase::STEP_A:
    case Phase::STEP_B:
    case Phase::STEP_C:
      // Interrupted mid-step: nobody knows whether the tray advanced.
      this->log_(LogLevel::kWarn, "Motion aborted mid-step: tray position is now UNKNOWN");
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
      this->queue_ = 0;
      this->phase_ = Phase::IDLE;
      this->hal_servo_release();
      break;
    case Phase::GAP:
      this->queue_ = 0;
      this->phase_ = Phase::SETTLE;
      this->phase_start_ = this->hal_millis();
      break;
    default:
      this->queue_ = 0;
      break;
  }
}

void FeederCore::stop() {
  this->log_(LogLevel::kInfo, "Stop requested");
  this->abort_motion_();
}

bool FeederCore::reload_pause_active() {
  if (this->reload_pause_ms_active_ &&
      static_cast<int32_t>(this->hal_millis() - this->reload_pause_until_ms_) < 0)
    return true;
  // The pause also survives a reboot when the clock was known at the time of the reload.
  return this->state_.reload_until != 0 && this->time_valid() &&
         this->hal_epoch() < static_cast<int64_t>(this->state_.reload_until);
}

void FeederCore::tray_reload() {
  if (this->calibrating_) {
    this->log_(LogLevel::kWarn, "Ignoring 'tray reload' while calibrating");
    return;
  }
  this->abort_motion_();
  this->state_.position = 0;
  this->state_.moving_to = NO_MOVE;
  this->state_.unknown = 0;
  const uint32_t pause_ms = static_cast<uint32_t>(this->cfg.reload_pause_min) * 60000UL;
  this->reload_pause_ms_active_ = pause_ms > 0;
  this->reload_pause_until_ms_ = this->hal_millis() + pause_ms;
  this->state_.reload_until =
      (pause_ms > 0 && this->time_valid())
          ? static_cast<uint32_t>(this->hal_epoch() + static_cast<int64_t>(this->cfg.reload_pause_min) * 60)
          : 0;
  this->save_state_(true);
  this->move_to_(0);
  this->phase_ = Phase::SETTLE;
  this->phase_start_ = this->hal_millis();
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Tray reload: position 0, 6 portions, scheduled feeding paused for %u min",
             this->cfg.reload_pause_min);
}

void FeederCore::button_press() {
  if (this->calibrating_)
    return;
  if (this->remaining() == 0 || this->state_.unknown) {
    this->tray_reload();
  } else {
    this->request_feed(1, Source::BUTTON);
  }
}

void FeederCore::declare_position(uint8_t position) {
  if (position > MAX_PORTIONS || this->calibrating_)
    return;
  this->abort_motion_();
  this->state_.position = position;
  this->state_.moving_to = NO_MOVE;
  this->state_.unknown = 0;
  this->save_state_(true);
  this->move_to_(position);
  this->phase_ = Phase::SETTLE;
  this->phase_start_ = this->hal_millis();
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Tray position declared: %u", position);
}

// ---------------------------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------------------------

void FeederCore::set_calibration_mode(bool on) {
  if (on == this->calibrating_)
    return;
  if (on) {
    this->abort_motion_();
    this->calibrating_ = true;
    this->cal_pos_ = std::min<uint8_t>(this->state_.position, NUM_POSITIONS - 1);
    this->move_to_(this->cal_pos_);
    this->phase_ = Phase::IDLE;
    this->log_(LogLevel::kInfo, "Calibration mode on");
  } else {
    this->calibrating_ = false;
    this->save_calibration();
    if (!this->state_.unknown) {
      this->move_to_(this->state_.position);
      this->phase_ = Phase::SETTLE;
      this->phase_start_ = this->hal_millis();
    } else {
      this->hal_servo_release();
    }
    this->log_(LogLevel::kInfo, "Calibration mode off, offsets saved");
  }
}

void FeederCore::calibration_select(uint8_t position) {
  this->cal_pos_ = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  if (this->calibrating_)
    this->move_to_(this->cal_pos_);
}

void FeederCore::set_calibration_offset(int16_t tenths) { this->set_calibration_offset_at(this->cal_pos_, tenths); }

void FeederCore::set_calibration_offset_at(uint8_t position, int16_t tenths) {
  position = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  this->cfg.cal[position] = std::min<int16_t>(std::max<int16_t>(tenths, -150), 150);
  this->cfg_changed();
  if (this->calibrating_ && position == this->cal_pos_)
    this->move_to_(position);
}

Result FeederCore::set_pulse_range(uint16_t min_us, uint16_t max_us) {
  if (!pulse_range_ok(min_us, max_us)) {
    this->log_(LogLevel::kWarn, "Servo pulse range %u-%u us refused (limits: min %u-%u, max %u-%u, at least %u apart)",
               min_us, max_us, PULSE_MIN_LO, PULSE_MIN_HI, PULSE_MAX_LO, PULSE_MAX_HI, PULSE_SPAN_MIN);
    return Result::INVALID_ARG;
  }
  this->cfg.min_pulse_us = min_us;
  this->cfg.max_pulse_us = max_us;
  this->cfg_changed();
  this->log_(LogLevel::kInfo, "Servo pulse range %u-%u us", min_us, max_us);
  if (this->calibrating_)
    this->move_to_(this->cal_pos_);
  return Result::OK;
}

Result FeederCore::jog_calibration(uint8_t position, int16_t delta_tenths) {
  if (!this->calibrating_)
    return Result::NOT_CALIBRATING;
  if (position >= NUM_POSITIONS)
    return Result::INVALID_ARG;
  this->cal_pos_ = position;
  const int32_t next = static_cast<int32_t>(this->cfg.cal[position]) + delta_tenths;
  this->cfg.cal[position] = static_cast<int16_t>(std::min<int32_t>(std::max<int32_t>(next, -150), 150));
  this->cfg_changed();
  this->move_to_(position);
  return Result::OK;
}

// ---------------------------------------------------------------------------------------------
// Feed guard
// ---------------------------------------------------------------------------------------------

bool FeederCore::hold_active() {
  if (this->state_.hold_until == 0)
    return false;
  if (this->state_.hold_until == HOLD_FOREVER)
    return true;
  return this->time_valid() && this->hal_epoch() < static_cast<int64_t>(this->state_.hold_until);
}

uint16_t FeederCore::portions_24h_(int64_t now_epoch, bool time_ok) {
  uint16_t total = 0;
  const uint32_t ms = this->hal_millis();
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] == 0)
      continue;
    bool recent;
    if (this->state_.hist_ts[i] != 0 && time_ok) {
      recent = now_epoch - static_cast<int64_t>(this->state_.hist_ts[i]) < 86400;
    } else if (this->hist_this_boot_[i]) {
      recent = ms - this->hist_ms_[i] < 86400000UL;
    } else {
      // Saved before this boot, and the clock is not set again yet: its age is unknown, so it is not counted.
      // (Counting it would refuse feeds for a day after a power cut with no network; decision 2026-10-14.)
      recent = false;
    }
    if (recent)
      total += this->state_.hist_n[i];
  }
  return total;
}

uint16_t FeederCore::portions_24h() {
  const bool time_ok = this->time_valid();
  return this->portions_24h_(time_ok ? this->hal_epoch() : 0, time_ok);
}

void FeederCore::set_portions_24h(uint16_t portions) {
  const bool time_ok = this->time_valid();
  const int64_t now = time_ok ? this->hal_epoch() : 0;
  const uint16_t before = this->portions_24h_(now, time_ok);
  if (portions == before)
    return;
  if (portions > before) {
    this->record_history_(static_cast<uint8_t>(std::min<uint16_t>(portions - before, 255)), Source::ADJUSTMENT);
  } else {
    // Remove counted portions, oldest first, until the count is right.
    uint16_t excess = before - portions;
    for (int i = HISTORY_SIZE - 1; i >= 0 && excess > 0; i--) {
      if (this->state_.hist_n[i] == 0)
        continue;
      const uint16_t total = this->portions_24h_(now, time_ok);
      const uint8_t keep = this->state_.hist_n[i];
      this->state_.hist_n[i] = 0;
      const uint16_t removed = total - this->portions_24h_(now, time_ok);  // 0 if this entry was not counted
      if (removed == 0) {
        this->state_.hist_n[i] = keep;
        continue;
      }
      const uint16_t take = std::min<uint16_t>(removed, excess);
      this->state_.hist_n[i] = static_cast<uint8_t>(keep - take);
      excess -= take;
    }
  }
  this->save_state_(true);
  this->project_dirty_ = true;
  this->log_(LogLevel::kWarn, "Portions counted in the last 24 h set by hand: %u -> %u", before,
             this->portions_24h_(now, time_ok));
}

void FeederCore::record_history_(uint8_t portions, Source source) {
  for (uint8_t i = HISTORY_SIZE - 1; i > 0; i--) {
    this->state_.hist_ts[i] = this->state_.hist_ts[i - 1];
    this->state_.hist_n[i] = this->state_.hist_n[i - 1];
    this->state_.hist_src[i] = this->state_.hist_src[i - 1];
    this->hist_ms_[i] = this->hist_ms_[i - 1];
    this->hist_this_boot_[i] = this->hist_this_boot_[i - 1];
  }
  this->state_.hist_ts[0] = this->time_valid() ? static_cast<uint32_t>(this->hal_epoch()) : 0;
  this->state_.hist_n[0] = portions;
  this->state_.hist_src[0] = static_cast<uint8_t>(source);
  this->hist_ms_[0] = this->hal_millis();
  this->hist_this_boot_[0] = true;
}

Result FeederCore::check_guard_(uint8_t &portions, Source source) {
  const bool scheduled = source == Source::SCHEDULE || source == Source::ONE_SHOT;
  if (portions == 0 || portions > MAX_PORTIONS)
    return Result::INVALID_ARG;
  if (!scheduled && portions > this->cfg.max_portions_cmd)
    return Result::PORTION_LIMIT;
  if (this->calibrating_)
    return Result::CALIBRATING;
  if (this->moving_())
    return Result::BUSY;
  if (this->state_.unknown)
    return Result::UNKNOWN_POSITION;
  if (this->remaining() == 0)
    return Result::EMPTY;
  portions = std::min<uint8_t>(portions, this->remaining());

  const bool time_ok = this->time_valid();
  if (scheduled) {
    if (!time_ok)
      return Result::TIME_INVALID;
    if (this->hold_active() || this->reload_pause_active())
      return Result::HOLD;
  }

  const int64_t now_epoch = time_ok ? this->hal_epoch() : 0;
  uint32_t since_ms = UINT32_MAX;
  if (this->have_last_feed_ms_) {
    since_ms = this->hal_millis() - this->last_feed_ms_;
  } else if (time_ok && this->state_.last_feed_epoch != 0) {
    const int64_t diff = now_epoch - static_cast<int64_t>(this->state_.last_feed_epoch);
    since_ms = diff < 0 ? 0 : static_cast<uint32_t>(std::min<int64_t>(diff, 4000000) * 1000);
  }
  if (since_ms < static_cast<uint32_t>(this->cfg.min_gap_s) * 1000UL)
    return Result::TOO_SOON;

  if (this->portions_24h_(now_epoch, time_ok) + portions > this->cfg.daily_cap)
    return Result::DAILY_CAP;
  return Result::OK;
}

void FeederCore::report_(Source source, uint8_t portions, Result result) {
  this->last_result_ = result;
  if (result != Result::OK)
    this->log_(LogLevel::kWarn, "Feed refused (%s) from %s", result_name(result), source_name(source));
  if (this->feed_listener_) {
    FeedRecord rec;
    rec.epoch = this->time_valid() ? static_cast<uint32_t>(this->hal_epoch()) : 0;
    rec.source = static_cast<uint8_t>(source);
    rec.portions = portions;
    rec.result = static_cast<uint8_t>(result);
    this->feed_listener_(rec);
  }
}

Result FeederCore::request_feed(uint8_t portions, Source source, uint8_t *accepted) {
  const uint8_t requested = portions;
  const Result r = this->check_guard_(portions, source);
  if (r != Result::OK) {
    this->report_(source, requested, r);
    return r;
  }
  if (accepted != nullptr)
    *accepted = portions;
  this->log_(LogLevel::kInfo, "Feeding %u portion(s) from %s, tray position %u", portions, source_name(source),
             this->state_.position);
  this->record_history_(portions, source);
  this->state_.last_feed_epoch = this->state_.hist_ts[0];
  this->state_.last_src = static_cast<uint8_t>(source);
  this->last_feed_ms_ = this->hal_millis();
  this->have_last_feed_ms_ = true;
  this->queue_ = portions;
  this->project_dirty_ = true;
  this->start_next_portion_();  // persists the intent and the history before the tray moves
  this->report_(source, portions, Result::OK);
  return Result::OK;
}

Result FeederCore::set_hold_minutes(uint32_t minutes) {
  if (minutes == 0) {
    this->cancel_hold();
    return Result::OK;
  }
  if (!this->time_valid())
    return Result::TIME_INVALID;
  // No upper limit on the duration; only the 32-bit epoch (year 2106) bounds it, so clamp just below
  // the "until cancelled" marker.
  const int64_t until = this->hal_epoch() + static_cast<int64_t>(minutes) * 60;
  this->state_.hold_until = until >= static_cast<int64_t>(HOLD_FOREVER) ? HOLD_FOREVER - 1 : static_cast<uint32_t>(until);
  this->save_state_(false);
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Hold set for %u min", static_cast<unsigned>(minutes));
  return Result::OK;
}

void FeederCore::set_hold_indefinite() {
  // Needs no clock: scheduled feeding stays off until the hold is cancelled.
  this->state_.hold_until = HOLD_FOREVER;
  this->save_state_(false);
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Hold set until cancelled");
}

void FeederCore::cancel_hold() {
  this->state_.hold_until = 0;
  this->save_state_(false);
  this->project_dirty_ = true;
}

std::string FeederCore::hold_text() {
  if (this->state_.hold_until == 0)
    return "none";
  if (this->state_.hold_until == HOLD_FOREVER)
    return "until cancelled";
  if (!this->time_valid())
    return "active (clock not set)";
  return "until " + this->format_local_(static_cast<int64_t>(this->state_.hold_until));
}

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
  DayCtx d;
  d.day_no = day_no;
  d.wday = weekday_from_days(day_no);
  d.midnight = this->wall_to_epoch_(day_no, 0);
  d.tz_min = static_cast<int>(this->local_offset_s_(this->wall_to_epoch_(day_no, 12 * 60)) / 60);
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
  this->sun_cache_next_ = (this->sun_cache_next_ + 1) % 3;
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
    return false;  // location not set
  double rise, set;
  if (!this->sun_cached_(d, rise, set))
    return false;
  const double base = e.anchor == static_cast<uint8_t>(Anchor::SUNRISE) ? rise : set;
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
  for (auto &p : this->cfg.profiles) {
    if (p.anchor_day == 0)
      p.anchor_day = static_cast<uint32_t>(today);
  }
  // Feeds made before the clock was set count as happening now: the conservative choice for the daily cap.
  const uint32_t now = static_cast<uint32_t>(this->hal_epoch());
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] != 0 && this->state_.hist_ts[i] == 0)
      this->state_.hist_ts[i] = now;
  }
  this->cfg_changed();
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
  if (this->project_dirty_ || this->hal_millis() - this->last_project_ > 30000)
    this->project_();
  this->fire_due_();
}

void FeederCore::update_solar_fault_() {
  // Only meaningful while recurring events can actually run.
  this->solar_fault_ = false;
  if (!this->cfg.scheduler_enabled)
    return;
  const Profile &p = this->cfg.profiles[this->cfg.active_profile];
  const DayCtx d = this->make_day_(this->today_());
  if (!this->day_active_(p, d))
    return;
  for (const Event &ev : p.ev) {
    if (!ev.enabled || ev.anchor == static_cast<uint8_t>(Anchor::CLOCK) || !(ev.days & (1 << d.wday)))
      continue;
    int minutes;
    if (!this->resolve_minutes_(ev, d, minutes)) {
      this->solar_fault_ = true;
      return;
    }
  }
}

static bool deferrable(Result r) { return r == Result::BUSY || r == Result::TOO_SOON; }

void FeederCore::fire_due_() {
  const int64_t now = this->hal_epoch();
  const int32_t today = this->today_();
  const bool held = this->hold_active() || this->reload_pause_active();
  bool state_dirty = false;

  // Recurring events: only while the scheduler is enabled.
  if (this->cfg.scheduler_enabled) {
    const uint8_t ap = this->cfg.active_profile;
    Profile &p = this->cfg.profiles[ap];
    // Yesterday is included because an event such as "sunset + 3 h" can land after midnight.
    for (int back = 1; back >= 0; back--) {
      const DayCtx d = this->make_day_(today - back);
      if (!this->day_active_(p, d))
        continue;
      for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
        const Event &ev = p.ev[e];
        if (!ev.enabled || !(ev.days & (1 << d.wday)))
          continue;
        if (this->state_.fired[ap][e] == static_cast<uint32_t>(d.day_no))
          continue;
        int minutes;
        if (!this->resolve_minutes_(ev, d, minutes))
          continue;
        const int64_t fire = this->wall_to_epoch_(d.day_no, minutes);
        if (fire < 0 || now < fire)
          continue;
        uint8_t &pending = this->pending_[ap][e];
        const bool pending_here = pending != NO_PENDING && this->pending_day_[ap][e] == d.day_no;
        if (now >= fire + EVENT_WINDOW_S) {
          // Window closed. If it was refused while open, report that once; otherwise it was simply missed.
          if (pending_here) {
            this->report_(Source::SCHEDULE, ev.portions, static_cast<Result>(pending));
            this->state_.fired[ap][e] = static_cast<uint32_t>(d.day_no);
            pending = NO_PENDING;
            state_dirty = true;
          }
          continue;
        }
        if (held) {
          pending = static_cast<uint8_t>(Result::HOLD);
          this->pending_day_[ap][e] = d.day_no;
          continue;
        }
        uint8_t portions = ev.portions;
        const Result check = this->check_guard_(portions, Source::SCHEDULE);
        if (deferrable(check)) {
          // Try again next tick while the window is open, without reporting a refusal yet.
          pending = static_cast<uint8_t>(check);
          this->pending_day_[ap][e] = d.day_no;
          continue;
        }
        this->request_feed(ev.portions, Source::SCHEDULE);  // reports the outcome, accepted or refused
        this->state_.fired[ap][e] = static_cast<uint32_t>(d.day_no);
        pending = NO_PENDING;
        state_dirty = true;
      }
    }
  }

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
    this->request_feed(os.portions, Source::ONE_SHOT);
    pending = NO_PENDING;
    os.done = 1;
    cfg_dirty = true;
  }
  if (state_dirty)
    this->save_state_(false);
  if (cfg_dirty)
    this->cfg_changed();
}

void FeederCore::project_() {
  this->project_dirty_ = false;
  this->last_project_ = this->hal_millis();
  this->next_event_epoch_ = 0;
  this->empty_on_epoch_ = 0;
  this->runway_days_ = 255;
  this->oversubscribed_ = false;
  if (!this->time_valid())
    return;

  const int64_t now = this->hal_epoch();
  const int32_t today = this->today_();

  struct Item {
    int64_t epoch;
    uint8_t portions;
  };
  std::vector<Item> items;
  items.reserve(64);
  if (this->cfg.scheduler_enabled) {
    const uint8_t ap = this->cfg.active_profile;
    const Profile &p = this->cfg.profiles[ap];
    for (int off = -1; off <= HORIZON_DAYS; off++) {
      const DayCtx d = this->make_day_(today + off);
      if (!this->day_active_(p, d))
        continue;
      for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
        const Event &ev = p.ev[e];
        if (!ev.enabled || !(ev.days & (1 << d.wday)))
          continue;
        // Each event feeds at most once per (anchor) day. One that already fed today and was then moved later
        // in the day does not feed again until tomorrow, so it is not "next" either.
        if (this->state_.fired[ap][e] == static_cast<uint32_t>(d.day_no))
          continue;
        int minutes;
        if (!this->resolve_minutes_(ev, d, minutes))
          continue;
        const int64_t fire = this->wall_to_epoch_(d.day_no, minutes);
        if (fire <= now)
          continue;
        if (this->state_.hold_until != 0 && fire < static_cast<int64_t>(this->state_.hold_until))
          continue;
        items.push_back({fire, ev.portions});
      }
    }
  }
  for (const OneShot &os : this->cfg.oneshots) {
    if (os.epoch == 0 || os.done || static_cast<int64_t>(os.epoch) <= now)
      continue;
    if (this->state_.hold_until != 0 && os.epoch < this->state_.hold_until)
      continue;
    items.push_back({static_cast<int64_t>(os.epoch), os.portions});
  }
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.epoch < b.epoch; });

  if (!items.empty())
    this->next_event_epoch_ = items.front().epoch;

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

// ---------------------------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------------------------

void FeederCore::note_time_sync(TimeSource source) {
  this->last_sync_ms_[static_cast<uint8_t>(source)] = this->hal_uptime_ms();
  this->last_time_source_ = source;
  this->project_dirty_ = true;
}

void FeederCore::set_system_time(uint32_t epoch) {
  this->hal_set_epoch(static_cast<int64_t>(epoch));
  this->note_time_sync(TimeSource::BLE);
  this->log_(LogLevel::kInfo, "Clock set over BLE to epoch %u", static_cast<unsigned>(epoch));
}

std::string FeederCore::time_source_text() const {
  if (this->last_time_source_ == TimeSource::NONE && this->clock_kept_)
    return "kept across restart";
  switch (this->last_time_source_) {
    case TimeSource::BLE:
      return "BLE";
    case TimeSource::NTP:
      return "NTP";
    case TimeSource::HOME_ASSISTANT:
      return "Home Assistant";
    default:
      return "none";
  }
}

uint16_t FeederCore::time_age_min() {
  if (this->last_time_source_ == TimeSource::NONE)
    return 0;
  const int64_t since = this->last_sync_ms_[static_cast<uint8_t>(this->last_time_source_)];
  if (since < 0)
    return 0;
  const int64_t minutes = (this->hal_uptime_ms() - since) / 60000;
  return static_cast<uint16_t>(std::min<int64_t>(std::max<int64_t>(minutes, 0), 65535));
}

int64_t FeederCore::network_time_age_ms() {
  const int64_t ntp = this->last_sync_ms_[static_cast<uint8_t>(TimeSource::NTP)];
  const int64_t ha = this->last_sync_ms_[static_cast<uint8_t>(TimeSource::HOME_ASSISTANT)];
  const int64_t newest = std::max(ntp, ha);
  if (newest < 0)
    return -1;
  return this->hal_uptime_ms() - newest;
}

bool FeederCore::time_stale() { return this->time_ever_synced() && this->time_age_min() > STALE_CLOCK_MIN; }

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
      bool found = false;
      for (int i = 0; i < 7; i++) {
        if (t.compare(0, 3, DAY_NAMES[i]) == 0) {
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
  if (portions < 1 || portions > MAX_PORTIONS)
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
  if (this->state_.position >= MAX_PORTIONS)
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
  if (!this->time_valid()) {
    for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
      if (this->state_.hist_ts[i] == 0 && this->state_.hist_n[i] != 0)
        total += this->state_.hist_n[i];
    }
    return total;
  }
  const int64_t midnight = this->wall_to_epoch_(this->today_(), 0);
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] != 0 && static_cast<int64_t>(this->state_.hist_ts[i]) >= midnight)
      total += this->state_.hist_n[i];
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
    return this->state_.hist_n[0] != 0 ? "clock was not set" : "never";
  return this->format_local_(this->state_.last_feed_epoch) + " (" + source_name(static_cast<Source>(this->state_.last_src)) + ")";
}

std::string FeederCore::recent_feeds_text() {
  std::string out;
  for (uint8_t i = 0; i < HISTORY_SIZE && i < 6; i++) {
    if (this->state_.hist_n[i] == 0)
      continue;
    if (!out.empty())
      out += "; ";
    if (this->state_.hist_ts[i] == 0) {
      out += "unknown time";
    } else {
      const LocalTime l = this->hal_local(this->state_.hist_ts[i]);
      char buf[24];
      std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", l.month, l.day, l.hour, l.minute);
      out += buf;
    }
    out += " x" + std::to_string(this->state_.hist_n[i]) + " " +
           source_name(static_cast<Source>(this->state_.hist_src[i]));
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
  if (this->state_.position >= MAX_PORTIONS)
    add("tray empty");
  if (this->solar_fault_)
    add("sunrise/sunset unavailable (set location)");
  if (this->oversubscribed_)
    add("schedule needs more portions than the tray holds before the next refill");
  return out.empty() ? "none" : out;
}

std::string FeederCore::schedule_summary() {
  const Profile &p = this->cfg.profiles[this->cfg.active_profile];
  std::string out(1, static_cast<char>('A' + this->cfg.active_profile));
  out += ": ";
  bool any = false;
  for (const Event &e : p.ev) {
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
