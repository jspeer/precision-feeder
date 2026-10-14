// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "precision_feeder.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "esphome/core/hal.h"
#include "esphome/core/helpers.h"
#include "esphome/core/log.h"

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder";

static constexpr uint32_t SETTINGS_VERSION = 2;  // 2: late-start grace removed
static constexpr uint32_t STATE_VERSION = 1;
static constexpr uint32_t HOLD_FOREVER = UINT32_MAX;  // hold_until marker: until the user cancels it
static constexpr float SERVO_PERIOD_US = 20000.0f;  // 50 Hz
static constexpr int HORIZON_DAYS = 30;
static const char *const DAY_NAMES[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
static const char *const SOURCE_NAMES[5] = {"schedule", "home assistant", "ble", "button", "one-shot"};

// ---------------------------------------------------------------------------------------------
// Setup, persistence
// ---------------------------------------------------------------------------------------------

void PrecisionFeeder::defaults_() {
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

  std::memset(&this->state_, 0, sizeof(this->state_));
  this->state_.version = STATE_VERSION;
  this->state_.moving_to = NO_MOVE;
}

void PrecisionFeeder::load_() {
  this->defaults_();
  Settings s;
  if (this->pref_settings_.load(&s) && s.version == SETTINGS_VERSION) {
    this->cfg = s;
  } else {
    ESP_LOGI(TAG, "No saved settings, using defaults");
  }
  State st;
  if (this->pref_state_.load(&st) && st.version == STATE_VERSION) {
    this->state_ = st;
  } else {
    ESP_LOGI(TAG, "No saved tray state, assuming a freshly loaded tray at position 0");
  }

  // Clamp everything that came from flash.
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

  // Feed history stamped with an unknown time cannot be aged after a reboot: drop it.
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] != 0 && this->state_.hist_ts[i] == 0)
      this->state_.hist_n[i] = 0;
  }
}

void PrecisionFeeder::save_settings_() {
  this->pref_settings_.save(&this->cfg);
}

void PrecisionFeeder::save_state_(bool flush) {
  this->pref_state_.save(&this->state_);
  if (flush)
    global_preferences->sync();
}

void PrecisionFeeder::cfg_changed() {
  this->project_dirty_ = true;
  this->set_timeout("save_cfg", 2000, [this]() { this->save_settings_(); });
}

void PrecisionFeeder::setup() {
  this->pref_settings_ = global_preferences->make_preference<Settings>(fnv1_hash("precision_feeder.settings"));
  this->pref_state_ = global_preferences->make_preference<State>(fnv1_hash("precision_feeder.state"));
  this->load_();

  if (this->state_.moving_to != NO_MOVE) {
    // Power was lost in the middle of a step: the tray may or may not have advanced.
    ESP_LOGW(TAG, "Unfinished move to position %u found: tray position is now UNKNOWN", this->state_.moving_to);
    this->state_.unknown = 1;
    this->state_.moving_to = NO_MOVE;
    this->save_state_(true);
  }

  if (!this->state_.unknown) {
    // Go to the saved position (never to 0), then let the servo go limp after a moment.
    this->write_angle_(this->angle_for_(this->state_.position));
    this->phase_ = Phase::SETTLE;
    this->phase_start_ = millis();
  }
}

void PrecisionFeeder::dump_config() {
  ESP_LOGCONFIG(TAG, "Precision Feeder:");
  ESP_LOGCONFIG(TAG, "  Sweep: %.1f deg, pitch %.1f deg, invert: %s", this->sweep_, this->sweep_ / NUM_POSITIONS,
                YESNO(this->invert_));
  ESP_LOGCONFIG(TAG, "  Pulse: %u-%u us, step time %u ms, wiggle: %s, detach delay %u ms",
                (unsigned) this->min_pulse_us_, (unsigned) this->max_pulse_us_, (unsigned) this->step_ms_,
                YESNO(this->wiggle_), (unsigned) this->detach_ms_);
  ESP_LOGCONFIG(TAG, "  Tray position: %u (%s), remaining: %u", this->state_.position,
                this->tray_state_text().c_str(), this->remaining());
}

// ---------------------------------------------------------------------------------------------
// Servo
// ---------------------------------------------------------------------------------------------

float PrecisionFeeder::angle_for_(uint8_t position) const {
  position = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  const float margin = (180.0f - this->sweep_) / 2.0f;
  const float pitch = this->sweep_ / NUM_POSITIONS;
  float angle = margin + position * pitch + this->cfg.cal[position] / 10.0f;
  angle = std::min(180.0f, std::max(0.0f, angle));
  return this->invert_ ? 180.0f - angle : angle;
}

void PrecisionFeeder::write_angle_(float angle) {
  const float span = static_cast<float>(this->max_pulse_us_) - static_cast<float>(this->min_pulse_us_);
  const float pulse = static_cast<float>(this->min_pulse_us_) + (angle / 180.0f) * span;
  this->output_->set_level(pulse / SERVO_PERIOD_US);
}

void PrecisionFeeder::detach_() {
  this->output_->set_level(0.0f);
}

// ---------------------------------------------------------------------------------------------
// Motion
// ---------------------------------------------------------------------------------------------

void PrecisionFeeder::start_next_portion_() {
  const uint8_t target = this->state_.position + 1;
  // Persist the intent before moving, so a power loss mid-step is detected at the next boot.
  this->state_.moving_to = target;
  this->save_state_(true);
  this->write_angle_(this->angle_for_(target));
  this->phase_ = Phase::STEP_A;
  this->phase_start_ = millis();
}

void PrecisionFeeder::commit_step_() {
  this->state_.position = this->state_.moving_to;
  this->state_.moving_to = NO_MOVE;
  this->save_state_(true);
  this->project_dirty_ = true;
  if (this->queue_ > 0)
    this->queue_--;
  this->phase_start_ = millis();
  this->phase_ = (this->queue_ > 0 && this->state_.position < MAX_PORTIONS) ? Phase::GAP : Phase::SETTLE;
  if (this->phase_ == Phase::SETTLE)
    this->queue_ = 0;
}

void PrecisionFeeder::run_motion_() {
  const uint32_t now = millis();
  switch (this->phase_) {
    case Phase::IDLE:
      return;
    case Phase::STEP_A:
      if (now - this->phase_start_ < this->step_ms_)
        return;
      if (this->wiggle_) {
        // Step back and forward again to shake the food loose.
        this->write_angle_(this->angle_for_(this->state_.position));
        this->phase_ = Phase::STEP_B;
        this->phase_start_ = now;
      } else {
        this->commit_step_();
      }
      return;
    case Phase::STEP_B:
      if (now - this->phase_start_ < this->step_ms_)
        return;
      this->write_angle_(this->angle_for_(this->state_.moving_to));
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
        this->detach_();
      this->phase_ = Phase::IDLE;
      return;
  }
}

void PrecisionFeeder::abort_motion_() {
  switch (this->phase_) {
    case Phase::STEP_A:
    case Phase::STEP_B:
    case Phase::STEP_C:
      // Interrupted mid-step: nobody knows whether the tray advanced.
      ESP_LOGW(TAG, "Motion aborted mid-step: tray position is now UNKNOWN");
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
      this->queue_ = 0;
      this->phase_ = Phase::IDLE;
      this->detach_();
      break;
    case Phase::GAP:
      this->queue_ = 0;
      this->phase_ = Phase::SETTLE;
      this->phase_start_ = millis();
      break;
    default:
      this->queue_ = 0;
      break;
  }
}

void PrecisionFeeder::stop() {
  ESP_LOGI(TAG, "Stop requested");
  this->abort_motion_();
}

bool PrecisionFeeder::reload_pause_now_() const {
  return this->reload_pause_active_ && static_cast<int32_t>(millis() - this->reload_pause_until_ms_) < 0;
}

void PrecisionFeeder::tray_reload() {
  if (this->calibrating_) {
    ESP_LOGW(TAG, "Ignoring 'tray reload' while calibrating");
    return;
  }
  this->abort_motion_();
  this->state_.position = 0;
  this->state_.moving_to = NO_MOVE;
  this->state_.unknown = 0;
  this->save_state_(true);
  this->write_angle_(this->angle_for_(0));
  this->phase_ = Phase::SETTLE;
  this->phase_start_ = millis();
  this->reload_pause_active_ = this->cfg.reload_pause_min > 0;
  this->reload_pause_until_ms_ = millis() + static_cast<uint32_t>(this->cfg.reload_pause_min) * 60000UL;
  this->project_dirty_ = true;
  ESP_LOGI(TAG, "Tray reload: position 0, 6 portions, scheduled feeding paused for %u min",
           this->cfg.reload_pause_min);
}

void PrecisionFeeder::declare_position(uint8_t position) {
  if (position > MAX_PORTIONS || this->calibrating_)
    return;
  this->abort_motion_();
  this->state_.position = position;
  this->state_.moving_to = NO_MOVE;
  this->state_.unknown = 0;
  this->save_state_(true);
  this->write_angle_(this->angle_for_(position));
  this->phase_ = Phase::SETTLE;
  this->phase_start_ = millis();
  this->project_dirty_ = true;
  ESP_LOGI(TAG, "Tray position declared: %u", position);
}

// ---------------------------------------------------------------------------------------------
// Calibration
// ---------------------------------------------------------------------------------------------

void PrecisionFeeder::set_calibration_mode(bool on) {
  if (on == this->calibrating_)
    return;
  if (on) {
    this->abort_motion_();
    this->calibrating_ = true;
    this->cal_pos_ = std::min<uint8_t>(this->state_.position, NUM_POSITIONS - 1);
    this->write_angle_(this->angle_for_(this->cal_pos_));
    this->phase_ = Phase::IDLE;
    ESP_LOGI(TAG, "Calibration mode on");
  } else {
    this->calibrating_ = false;
    this->cancel_timeout("save_cfg");
    this->save_settings_();
    global_preferences->sync();
    if (!this->state_.unknown) {
      this->write_angle_(this->angle_for_(this->state_.position));
      this->phase_ = Phase::SETTLE;
      this->phase_start_ = millis();
    } else {
      this->detach_();
    }
    ESP_LOGI(TAG, "Calibration mode off, offsets saved");
  }
}

void PrecisionFeeder::calibration_select(uint8_t position) {
  this->cal_pos_ = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  if (this->calibrating_)
    this->write_angle_(this->angle_for_(this->cal_pos_));
}

void PrecisionFeeder::set_calibration_offset(int16_t tenths) {
  this->cfg.cal[this->cal_pos_] = std::min<int16_t>(std::max<int16_t>(tenths, -150), 150);
  this->cfg_changed();
  if (this->calibrating_)
    this->write_angle_(this->angle_for_(this->cal_pos_));
}

// ---------------------------------------------------------------------------------------------
// Feed guard
// ---------------------------------------------------------------------------------------------

bool PrecisionFeeder::time_valid() {
  return this->time_ != nullptr && this->time_->now().is_valid();
}

bool PrecisionFeeder::hold_active() {
  if (this->state_.hold_until == 0)
    return false;
  if (this->state_.hold_until == HOLD_FOREVER)
    return true;
  return this->time_valid() && static_cast<uint32_t>(this->time_->timestamp_now()) < this->state_.hold_until;
}

uint16_t PrecisionFeeder::portions_24h_(time_t now_epoch, bool time_ok) const {
  uint16_t total = 0;
  const uint32_t ms = millis();
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] == 0)
      continue;
    bool recent;
    if (this->state_.hist_ts[i] != 0 && time_ok)
      recent = (now_epoch - static_cast<time_t>(this->state_.hist_ts[i])) < 86400;
    else
      recent = (ms - this->hist_ms_[i]) < 86400000UL;
    if (recent)
      total += this->state_.hist_n[i];
  }
  return total;
}

void PrecisionFeeder::record_history_(uint8_t portions, Source source) {
  for (uint8_t i = HISTORY_SIZE - 1; i > 0; i--) {
    this->state_.hist_ts[i] = this->state_.hist_ts[i - 1];
    this->state_.hist_n[i] = this->state_.hist_n[i - 1];
    this->state_.hist_src[i] = this->state_.hist_src[i - 1];
    this->hist_ms_[i] = this->hist_ms_[i - 1];
  }
  const bool ok = this->time_valid();
  this->state_.hist_ts[0] = ok ? static_cast<uint32_t>(this->time_->timestamp_now()) : 0;
  this->state_.hist_n[0] = portions;
  this->state_.hist_src[0] = static_cast<uint8_t>(source);
  this->hist_ms_[0] = millis();
}

Result PrecisionFeeder::check_guard_(uint8_t &portions, Source source) {
  const bool scheduled = source == Source::SCHEDULE || source == Source::ONE_SHOT;
  if (portions == 0 || portions > MAX_PORTIONS)
    return Result::INVALID_ARG;
  if (!scheduled && portions > this->cfg.max_portions_cmd)
    return Result::PORTION_LIMIT;
  if (this->calibrating_)
    return Result::CALIBRATING;
  if (this->phase_ == Phase::STEP_A || this->phase_ == Phase::STEP_B || this->phase_ == Phase::STEP_C ||
      this->phase_ == Phase::GAP)
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
    if (this->hold_active() || this->reload_pause_now_())
      return Result::HOLD;
  }

  const time_t now_epoch = time_ok ? this->time_->timestamp_now() : 0;
  uint32_t since_ms = UINT32_MAX;
  if (this->have_last_feed_ms_) {
    since_ms = millis() - this->last_feed_ms_;
  } else if (time_ok && this->state_.last_feed_epoch != 0) {
    const int64_t diff = static_cast<int64_t>(now_epoch) - static_cast<int64_t>(this->state_.last_feed_epoch);
    since_ms = diff < 0 ? 0 : static_cast<uint32_t>(std::min<int64_t>(diff, 4000000) * 1000);
  }
  if (since_ms < static_cast<uint32_t>(this->cfg.min_gap_s) * 1000UL)
    return Result::TOO_SOON;

  if (this->portions_24h_(now_epoch, time_ok) + portions > this->cfg.daily_cap)
    return Result::DAILY_CAP;
  return Result::OK;
}

Result PrecisionFeeder::request_feed(uint8_t portions, Source source) {
  const Result r = this->check_guard_(portions, source);
  this->last_result_ = r;
  if (r != Result::OK) {
    ESP_LOGW(TAG, "Feed rejected (%s) from %s", result_name(r), SOURCE_NAMES[static_cast<uint8_t>(source)]);
    return r;
  }
  ESP_LOGI(TAG, "Feeding %u portion(s) from %s, tray position %u", portions,
           SOURCE_NAMES[static_cast<uint8_t>(source)], this->state_.position);
  this->record_history_(portions, source);
  this->state_.last_feed_epoch = this->state_.hist_ts[0];
  this->state_.last_src = static_cast<uint8_t>(source);
  this->last_feed_ms_ = millis();
  this->have_last_feed_ms_ = true;
  this->queue_ = portions;
  this->project_dirty_ = true;
  this->start_next_portion_();  // persists the intent and the history before the tray moves
  return Result::OK;
}

Result PrecisionFeeder::set_hold_minutes(uint32_t minutes) {
  if (minutes == 0) {
    this->cancel_hold();
    return Result::OK;
  }
  if (!this->time_valid())
    return Result::TIME_INVALID;
  // No upper limit on the duration; only the 32-bit epoch (year 2106) bounds it, so clamp just below
  // the "until cancelled" marker.
  const uint64_t until = static_cast<uint64_t>(this->time_->timestamp_now()) + static_cast<uint64_t>(minutes) * 60ULL;
  this->state_.hold_until = until >= HOLD_FOREVER ? HOLD_FOREVER - 1 : static_cast<uint32_t>(until);
  this->save_state_(false);
  this->project_dirty_ = true;
  ESP_LOGI(TAG, "Hold set for %u min", (unsigned) minutes);
  return Result::OK;
}

void PrecisionFeeder::set_hold_indefinite() {
  // Needs no clock: scheduled feeding stays off until the hold is cancelled.
  this->state_.hold_until = HOLD_FOREVER;
  this->save_state_(false);
  this->project_dirty_ = true;
  ESP_LOGI(TAG, "Hold set until cancelled");
}

std::string PrecisionFeeder::hold_text() {
  if (this->state_.hold_until == 0)
    return "none";
  if (this->state_.hold_until == HOLD_FOREVER)
    return "until cancelled";
  if (!this->time_valid())
    return "active (clock not set)";
  return "until " + this->format_local_(static_cast<time_t>(this->state_.hold_until));
}

void PrecisionFeeder::cancel_hold() {
  this->state_.hold_until = 0;
  this->save_state_(false);
  this->project_dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// Scheduler
// ---------------------------------------------------------------------------------------------

void PrecisionFeeder::loop() {
  this->run_motion_();
  const uint32_t now = millis();
  if (now - this->last_tick_ >= 1000) {
    this->last_tick_ = now;
    this->schedule_tick_();
  }
}

int64_t PrecisionFeeder::local_offset_s_(time_t epoch) const {
  auto secs = [](const ESPTime &t) -> int64_t {
    return static_cast<int64_t>(days_from_civil(t.year, t.month, t.day_of_month)) * 86400 + t.hour * 3600 +
           t.minute * 60 + t.second;
  };
  return secs(ESPTime::from_epoch_local(epoch)) - secs(ESPTime::from_epoch_utc(epoch));
}

/// Epoch of a wall-clock moment: `minutes` after local midnight of day `day_no` (may be negative or
/// past 1440). Going through local fields keeps events right on daylight-saving change days.
time_t PrecisionFeeder::wall_to_epoch_(int32_t day_no, int minutes) const {
  const int32_t extra = minutes >= 0 ? minutes / 1440 : -((-minutes + 1439) / 1440);
  const int in_day = minutes - extra * 1440;
  int y;
  unsigned m, dd;
  civil_from_days(day_no + extra, y, m, dd);
  ESPTime t{};
  t.year = static_cast<uint16_t>(y);
  t.month = static_cast<uint8_t>(m);
  t.day_of_month = static_cast<uint8_t>(dd);
  t.hour = static_cast<uint8_t>(in_day / 60);
  t.minute = static_cast<uint8_t>(in_day % 60);
  t.second = 0;
  t.recalc_timestamp_local();
  return t.timestamp;
}

PrecisionFeeder::DayCtx PrecisionFeeder::make_day_(int32_t day_no) const {
  DayCtx d;
  d.day_no = day_no;
  d.wday = weekday_from_days(day_no);
  d.midnight = this->wall_to_epoch_(day_no, 0);
  d.tz_min = static_cast<int>(this->local_offset_s_(this->wall_to_epoch_(day_no, 12 * 60)) / 60);
  return d;
}

bool PrecisionFeeder::sun_cached_(const DayCtx &d, double &rise, double &set) {
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

bool PrecisionFeeder::resolve_minutes_(const Event &e, const DayCtx &d, int &minutes) {
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

bool PrecisionFeeder::day_active_(const Profile &p, const DayCtx &d) const {
  if (p.fast_days & (1 << d.wday))
    return false;
  const int32_t n = std::max<int32_t>(1, p.every_n);
  const int32_t anchor = p.anchor_day != 0 ? static_cast<int32_t>(p.anchor_day) : d.day_no;
  const int32_t diff = d.day_no - anchor;
  return ((diff % n) + n) % n == 0;
}

void PrecisionFeeder::on_time_valid_() {
  const ESPTime now = this->time_->now();
  const uint32_t today = days_from_civil(now.year, now.month, now.day_of_month);
  for (auto &p : this->cfg.profiles) {
    if (p.anchor_day == 0)
      p.anchor_day = today;
  }
  // Feeds made before the clock was set count as happening now: the conservative choice for the daily cap.
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] != 0 && this->state_.hist_ts[i] == 0)
      this->state_.hist_ts[i] = static_cast<uint32_t>(now.timestamp);
  }
  this->cfg_changed();
  this->save_state_(false);
  ESP_LOGI(TAG, "Clock is valid, scheduler running");
}

void PrecisionFeeder::schedule_tick_() {
  const bool valid = this->time_valid();
  if (valid && !this->was_time_valid_)
    this->on_time_valid_();
  this->was_time_valid_ = valid;
  if (!valid)
    return;

  const time_t now = this->time_->timestamp_now();
  if (this->state_.hold_until != 0 && this->state_.hold_until != HOLD_FOREVER &&
      static_cast<uint32_t>(now) >= this->state_.hold_until) {
    this->state_.hold_until = 0;
    this->save_state_(false);
    this->project_dirty_ = true;
    ESP_LOGI(TAG, "Hold ended");
  }
  if (this->project_dirty_ || millis() - this->last_project_ > 30000)
    this->project_();
  if (this->cfg.scheduler_enabled)
    this->fire_due_();
}

// Late feeds are skipped by design. An event only fires during the minute it is due; if the feeder was
// offline, on hold, reloading, or the guard refused it for that whole minute, the event is simply lost.
// The short window exists only so a BUSY or TOO_SOON answer can be retried on the next tick.
static constexpr time_t EVENT_WINDOW_S = 60;

void PrecisionFeeder::fire_due_() {
  const ESPTime now_l = this->time_->now();
  const time_t now = now_l.timestamp;
  const int32_t today = days_from_civil(now_l.year, now_l.month, now_l.day_of_month);
  const uint8_t ap = this->cfg.active_profile;
  Profile &p = this->cfg.profiles[ap];
  const bool held = this->hold_active() || this->reload_pause_now_();
  const time_t window = EVENT_WINDOW_S;
  bool solar_problem = false;
  bool state_dirty = false;

  // Yesterday is included because an event such as "sunset + 3 h" can land after midnight.
  for (int back = 1; back >= 0; back--) {
    const DayCtx d = this->make_day_(today - back);
    if (!this->day_active_(p, d))
      continue;
    for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
      const Event &ev = p.ev[e];
      if (!ev.enabled || !(ev.days & (1 << d.wday)))
        continue;
      int minutes;
      if (!this->resolve_minutes_(ev, d, minutes)) {
        if (back == 0)
          solar_problem = true;
        continue;
      }
      if (this->state_.fired[ap][e] == static_cast<uint32_t>(d.day_no))
        continue;
      const time_t fire = this->wall_to_epoch_(d.day_no, minutes);
      if (now < fire || now >= fire + window || held)
        continue;
      const Result r = this->request_feed(ev.portions, Source::SCHEDULE);
      if (r == Result::BUSY || r == Result::TOO_SOON)
        continue;  // try again on the next tick while the window is open
      this->state_.fired[ap][e] = d.day_no;
      state_dirty = true;
    }
  }
  this->solar_fault_ = solar_problem;

  // One-shot (calendar-style) feeds.
  bool cfg_dirty = false;
  for (auto &os : this->cfg.oneshots) {
    if (os.epoch == 0 || os.done)
      continue;
    const time_t at = static_cast<time_t>(os.epoch);
    if (now < at)
      continue;
    if (now >= at + window) {
      ESP_LOGW(TAG, "One-shot feed at %s missed", this->format_local_(at).c_str());
      os.done = 1;
      cfg_dirty = true;
      continue;
    }
    if (held)
      continue;
    const Result r = this->request_feed(os.portions, Source::ONE_SHOT);
    if (r == Result::BUSY || r == Result::TOO_SOON)
      continue;
    os.done = 1;
    cfg_dirty = true;
  }
  if (state_dirty)
    this->save_state_(false);
  if (cfg_dirty)
    this->cfg_changed();
}

void PrecisionFeeder::project_() {
  this->project_dirty_ = false;
  this->last_project_ = millis();
  this->next_event_epoch_ = 0;
  this->empty_on_epoch_ = 0;
  this->runway_days_ = 255;
  this->oversubscribed_ = false;
  if (!this->time_valid())
    return;

  const ESPTime now_l = this->time_->now();
  const time_t now = now_l.timestamp;
  const int32_t today = days_from_civil(now_l.year, now_l.month, now_l.day_of_month);
  const Profile &p = this->cfg.profiles[this->cfg.active_profile];

  struct Item {
    time_t epoch;
    uint8_t portions;
  };
  std::vector<Item> items;
  items.reserve(64);
  for (int off = -1; off <= HORIZON_DAYS; off++) {
    const DayCtx d = this->make_day_(today + off);
    if (!this->day_active_(p, d))
      continue;
    for (const Event &ev : p.ev) {
      if (!ev.enabled || !(ev.days & (1 << d.wday)))
        continue;
      int minutes;
      if (!this->resolve_minutes_(ev, d, minutes))
        continue;
      const time_t fire = this->wall_to_epoch_(d.day_no, minutes);
      if (fire <= now)
        continue;
      if (this->state_.hold_until != 0 && static_cast<uint32_t>(fire) < this->state_.hold_until)
        continue;
      items.push_back({fire, ev.portions});
    }
  }
  for (const OneShot &os : this->cfg.oneshots) {
    if (os.epoch != 0 && !os.done && static_cast<time_t>(os.epoch) > now)
      items.push_back({static_cast<time_t>(os.epoch), os.portions});
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
  const time_t refill_end = now + static_cast<time_t>(this->cfg.refill_interval_days) * 86400;
  bool empty_found = false;
  for (const Item &it : items) {
    cumulative += it.portions;
    if (it.epoch < refill_end)
      within_refill += it.portions;
    if (!empty_found && cumulative >= remaining) {
      empty_found = true;
      this->empty_on_epoch_ = it.epoch;
      const time_t days = (it.epoch - now) / 86400;
      this->runway_days_ = static_cast<uint8_t>(std::min<time_t>(days, 254));
    }
  }
  this->oversubscribed_ = within_refill > remaining;
}

// ---------------------------------------------------------------------------------------------
// Editor helpers
// ---------------------------------------------------------------------------------------------

void PrecisionFeeder::restart_cadence_today() {
  if (!this->time_valid())
    return;
  const ESPTime now = this->time_->now();
  this->edit_profile().anchor_day = days_from_civil(now.year, now.month, now.day_of_month);
  this->cfg_changed();
}

void PrecisionFeeder::set_event_anchor(uint8_t anchor) {
  Event &e = this->edit_event();
  anchor = std::min<uint8_t>(anchor, static_cast<uint8_t>(Anchor::SUNSET));
  const bool was_clock = e.anchor == static_cast<uint8_t>(Anchor::CLOCK);
  const bool now_clock = anchor == static_cast<uint8_t>(Anchor::CLOCK);
  if (was_clock != now_clock)
    e.offset_min = now_clock ? 12 * 60 : 0;
  e.anchor = anchor;
  this->cfg_changed();
}

std::string PrecisionFeeder::format_days(uint8_t mask) {
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

bool PrecisionFeeder::parse_days(const std::string &text, uint8_t &mask) {
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

std::string PrecisionFeeder::event_time_text() {
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

bool PrecisionFeeder::set_event_time_text(const std::string &text) {
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

bool PrecisionFeeder::add_oneshot(const std::string &local_datetime, uint8_t portions) {
  int y, mo, d, h, mi;
  if (std::sscanf(local_datetime.c_str(), "%d-%d-%d %d:%d", &y, &mo, &d, &h, &mi) != 5)
    return false;
  if (y < 2019 || mo < 1 || mo > 12 || d < 1 || d > 31 || h < 0 || h > 23 || mi < 0 || mi > 59)
    return false;
  if (portions < 1 || portions > MAX_PORTIONS)
    return false;
  ESPTime t{};
  t.year = static_cast<uint16_t>(y);
  t.month = static_cast<uint8_t>(mo);
  t.day_of_month = static_cast<uint8_t>(d);
  t.hour = static_cast<uint8_t>(h);
  t.minute = static_cast<uint8_t>(mi);
  t.second = 0;
  t.recalc_timestamp_local();
  if (t.timestamp <= 0)
    return false;
  if (this->time_valid() && t.timestamp <= this->time_->timestamp_now())
    return false;  // in the past
  for (auto &os : this->cfg.oneshots) {
    if (os.epoch == 0 || os.done) {
      os.epoch = static_cast<uint32_t>(t.timestamp);
      os.portions = portions;
      os.done = 0;
      this->cfg_changed();
      return true;
    }
  }
  return false;  // list full
}

void PrecisionFeeder::clear_oneshots() {
  std::memset(this->cfg.oneshots, 0, sizeof(this->cfg.oneshots));
  this->cfg_changed();
}

// ---------------------------------------------------------------------------------------------
// Status text
// ---------------------------------------------------------------------------------------------

const char *PrecisionFeeder::result_name(Result r) {
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
    case Result::NONE:
      return "none yet";
  }
  return "?";
}

std::string PrecisionFeeder::last_result_text() const { return result_name(this->last_result_); }

TrayState PrecisionFeeder::tray_state() const {
  if (this->calibrating_)
    return TrayState::CALIBRATING;
  if (this->phase_ == Phase::STEP_A || this->phase_ == Phase::STEP_B || this->phase_ == Phase::STEP_C ||
      this->phase_ == Phase::GAP)
    return TrayState::MOVING;
  if (this->state_.unknown)
    return TrayState::UNKNOWN;
  if (this->state_.position >= MAX_PORTIONS)
    return TrayState::EMPTY;
  return TrayState::LOADED;
}

std::string PrecisionFeeder::tray_state_text() const {
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

std::string PrecisionFeeder::format_local_(time_t epoch, bool with_date) {
  char buf[ESPTime::STRFTIME_BUFFER_SIZE];
  ESPTime::from_epoch_local(epoch).strftime_to(buf, with_date ? "%a %Y-%m-%d %H:%M" : "%H:%M");
  return buf;
}

uint16_t PrecisionFeeder::feeds_today() {
  uint16_t total = 0;
  if (!this->time_valid()) {
    for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
      if (this->state_.hist_ts[i] == 0 && this->state_.hist_n[i] != 0)
        total += this->state_.hist_n[i];
    }
    return total;
  }
  const ESPTime now = this->time_->now();
  const DayCtx d = this->make_day_(days_from_civil(now.year, now.month, now.day_of_month));
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    if (this->state_.hist_n[i] != 0 && static_cast<time_t>(this->state_.hist_ts[i]) >= d.midnight)
      total += this->state_.hist_n[i];
  }
  return total;
}

std::string PrecisionFeeder::next_event_text() {
  if (!this->time_valid())
    return "clock not set";
  if (this->next_event_epoch_ == 0)
    return "none";
  return this->format_local_(this->next_event_epoch_);
}

std::string PrecisionFeeder::empty_on_text() {
  if (!this->time_valid())
    return "clock not set";
  if (this->remaining() == 0)
    return "empty now";
  if (this->empty_on_epoch_ == 0)
    return "not within 30 days";
  return this->format_local_(this->empty_on_epoch_);
}

std::string PrecisionFeeder::last_feed_text() {
  if (this->state_.last_feed_epoch == 0)
    return this->state_.hist_n[0] != 0 ? "clock was not set" : "never";
  return this->format_local_(this->state_.last_feed_epoch) + " (" + SOURCE_NAMES[this->state_.last_src % 5] + ")";
}

std::string PrecisionFeeder::recent_feeds_text() {
  std::string out;
  for (uint8_t i = 0; i < HISTORY_SIZE && i < 6; i++) {
    if (this->state_.hist_n[i] == 0)
      continue;
    if (!out.empty())
      out += "; ";
    if (this->state_.hist_ts[i] == 0) {
      out += "unknown time";
    } else {
      char buf[ESPTime::STRFTIME_BUFFER_SIZE];
      ESPTime::from_epoch_local(this->state_.hist_ts[i]).strftime_to(buf, "%m-%d %H:%M");
      out += buf;
    }
    out += " x" + std::to_string(this->state_.hist_n[i]) + " " + SOURCE_NAMES[this->state_.hist_src[i] % 5];
  }
  return out.empty() ? "none" : out;
}

std::string PrecisionFeeder::oneshots_text() {
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

std::string PrecisionFeeder::faults_text() {
  std::string out;
  auto add = [&](const char *s) {
    if (!out.empty())
      out += "; ";
    out += s;
  };
  if (!this->time_valid())
    add("clock not set");
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

std::string PrecisionFeeder::schedule_summary() {
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
      std::snprintf(buf, sizeof(buf), "%s%c%02d:%02d", e.anchor == static_cast<uint8_t>(Anchor::SUNRISE) ? "sunrise" : "sunset",
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
  return out;
}

uint8_t PrecisionFeeder::led_code() {
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
  if (this->hold_active() || this->reload_pause_now_())
    return 6;
  return 0;
}

}  // namespace precision_feeder
}  // namespace esphome
