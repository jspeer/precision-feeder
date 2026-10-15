// Precision Feeder core: start-up and persistence, servo motion, calibration, feed guard, clock.
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

  std::memset(&this->calib, 0, sizeof(this->calib));
  this->calib.version = CALIBRATION_VERSION;
  this->calib.min_pulse_us = this->default_min_pulse_us_;
  this->calib.max_pulse_us = this->default_max_pulse_us_;

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
  for (auto &c : this->calib.offsets)
    c = std::min<int16_t>(std::max<int16_t>(c, -150), 150);
  this->cfg.active_profile = std::min<uint8_t>(this->cfg.active_profile, NUM_PROFILES - 1);
  this->cfg.min_gap_s = std::min<uint16_t>(std::max<uint16_t>(this->cfg.min_gap_s, 10), 3600);
  this->cfg.portion_gap_s = std::min<uint16_t>(std::max<uint16_t>(this->cfg.portion_gap_s, 1), 60);
  this->cfg.reload_pause_min = std::min<uint16_t>(this->cfg.reload_pause_min, 60);
  this->cfg.refill_interval_days = std::min<uint16_t>(std::max<uint16_t>(this->cfg.refill_interval_days, 1), 30);
  this->cfg.scheduler_enabled = this->cfg.scheduler_enabled ? 1 : 0;
  if (!(this->cfg.latitude >= -90.0f && this->cfg.latitude <= 90.0f))  // also catches NaN
    this->cfg.latitude = 0.0f;
  if (!(this->cfg.longitude >= -180.0f && this->cfg.longitude <= 180.0f))
    this->cfg.longitude = 0.0f;
  this->cfg.daily_cap = std::min<uint8_t>(std::max<uint8_t>(this->cfg.daily_cap, 1), MAX_PORTIONS);
  this->cfg.max_portions_cmd = std::min<uint8_t>(std::max<uint8_t>(this->cfg.max_portions_cmd, 1), MAX_PORTIONS);
  this->state_.position = std::min<uint8_t>(this->state_.position, MAX_PORTIONS);
  if (!pulse_range_ok(this->calib.min_pulse_us, this->calib.max_pulse_us)) {
    this->calib.min_pulse_us = this->default_min_pulse_us_;
    this->calib.max_pulse_us = this->default_max_pulse_us_;
  }
}

void FeederCore::save_state_(bool flush) { this->hal_save_state(this->state_, flush); }

void FeederCore::cfg_changed() {
  this->project_dirty_ = true;
  this->cfg_dirty_ = true;
  this->cfg_dirty_since_ = this->hal_millis();
}

void FeederCore::save_calibration() {
  this->hal_save_calibration(this->calib, true);
  this->hal_save_settings(this->cfg, true);
  this->cfg_dirty_ = false;
}

void FeederCore::migrate_v3_(const legacy::SettingsV3 &v3, bool take_calibration) {
  std::memcpy(this->cfg.profiles, v3.profiles, sizeof(this->cfg.profiles));
  std::memcpy(this->cfg.oneshots, v3.oneshots, sizeof(this->cfg.oneshots));
  this->cfg.latitude = v3.latitude;
  this->cfg.longitude = v3.longitude;
  this->cfg.min_gap_s = v3.min_gap_s;
  this->cfg.portion_gap_s = v3.portion_gap_s;
  this->cfg.reload_pause_min = v3.reload_pause_min;
  this->cfg.refill_interval_days = v3.refill_interval_days;
  this->cfg.daily_cap = v3.daily_cap;
  this->cfg.max_portions_cmd = v3.max_portions_cmd;
  this->cfg.active_profile = v3.active_profile;
  this->cfg.scheduler_enabled = v3.scheduler_enabled;
  if (take_calibration) {
    std::memcpy(this->calib.offsets, v3.cal, sizeof(this->calib.offsets));
    this->calib.min_pulse_us = v3.min_pulse_us;
    this->calib.max_pulse_us = v3.max_pulse_us;
  }
}

void FeederCore::core_start(const Settings *settings, const Calibration *calibration, const State *state,
                            const legacy::SettingsV3 *legacy_v3) {
  this->defaults_();
  const bool have_calibration = calibration != nullptr && calibration->version == CALIBRATION_VERSION;
  if (have_calibration)
    this->calib = *calibration;
  if (settings != nullptr && settings->version == SETTINGS_VERSION) {
    this->cfg = *settings;
  } else if (legacy_v3 != nullptr && legacy_v3->version == 3) {
    this->migrate_v3_(*legacy_v3, !have_calibration);
    this->cfg_changed();  // save them in the current format shortly
    this->log_(LogLevel::kInfo, "Settings migrated from version 3 (schedule, guard settings and calibration kept)");
  } else {
    this->log_(LogLevel::kInfo, "No saved settings, using defaults");
  }
  if (!have_calibration && !(legacy_v3 != nullptr && legacy_v3->version == 3))
    this->log_(LogLevel::kInfo, "No saved calibration, using the YAML pulse range and zero offsets");
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
    // Flushed: otherwise ESPHome holds the write for up to its flash_write_interval (60 s by default), and a power
    // cut in that time would lose the edit. Records that did not change are not rewritten.
    this->hal_save_settings(this->cfg, true);
    this->hal_save_calibration(this->calib, true);
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
  float angle = margin + position * pitch + this->calib.offsets[position] / 10.0f;
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
      // Interrupted mid-step: nobody knows whether the tray advanced. The step in progress still counts as fed
      // (the safe side for the daily cap); later portions of the same feed never started.
      this->log_(LogLevel::kWarn, "Motion aborted mid-step: tray position is now UNKNOWN");
      if (this->queue_ > 1)
        this->uncount_portions_(this->queue_ - 1);
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
      this->queue_ = 0;
      this->phase_ = Phase::IDLE;
      this->hal_servo_release();
      break;
    case Phase::GAP:
      // Between portions of one feed: the remaining ones never dropped.
      this->uncount_portions_(this->queue_);
      this->queue_ = 0;
      this->save_state_(true);
      this->phase_ = Phase::SETTLE;
      this->phase_start_ = this->hal_millis();
      break;
    default:
      this->queue_ = 0;
      break;
  }
}

void FeederCore::uncount_portions_(uint8_t portions) {
  // The newest history entry is the feed being cut short (it was recorded, with all its portions, before the tray
  // moved). Take off the portions that never dropped.
  if (portions == 0 || this->state_.hist_n[0] == 0)
    return;
  this->state_.hist_n[0] = static_cast<uint8_t>(this->state_.hist_n[0] > portions ? this->state_.hist_n[0] - portions : 0);
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Feed cut short: %u portion(s) did not drop", portions);
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
  this->calib.offsets[position] = std::min<int16_t>(std::max<int16_t>(tenths, -150), 150);
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
  this->calib.min_pulse_us = min_us;
  this->calib.max_pulse_us = max_us;
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
  const int32_t next = static_cast<int32_t>(this->calib.offsets[position]) + delta_tenths;
  this->calib.offsets[position] = static_cast<int16_t>(std::min<int32_t>(std::max<int32_t>(next, -150), 150));
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
  this->save_state_(true);  // flushed: a hold lost to a power cut would let the schedule run while you are away
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Hold set for %u min", static_cast<unsigned>(minutes));
  return Result::OK;
}

void FeederCore::set_hold_indefinite() {
  // Needs no clock: scheduled feeding stays off until the hold is cancelled.
  this->state_.hold_until = HOLD_FOREVER;
  this->save_state_(true);
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Hold set until cancelled");
}

void FeederCore::cancel_hold() {
  this->state_.hold_until = 0;
  this->save_state_(true);
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
// Clock
// ---------------------------------------------------------------------------------------------

void FeederCore::note_time_sync(TimeSource source) {
  this->last_sync_ms_[static_cast<uint8_t>(source)] = this->hal_uptime_ms();
  this->last_time_source_ = source;
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Clock set by %s", this->time_source_text().c_str());
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

}  // namespace precision_feeder
}  // namespace esphome
