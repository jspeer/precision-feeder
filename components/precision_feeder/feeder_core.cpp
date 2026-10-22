// Precision Feeder core: start-up and persistence, motion (through the mechanism), calibration, feed guard, clock.
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

  std::memset(&this->dev_, 0, sizeof(this->dev_));
  this->dev_.version = DEVICE_VERSION;
  this->dev_.servo_pin = PIN_BUILD;
  this->dev_.button_pin = PIN_BUILD;
  this->dev_.led_pin = PIN_BUILD;
  this->dev_.flags = DEVICE_BLE_GUARD | DEVICE_BLE_CALIBRATION;
}

void FeederCore::clamp_device_() {
  Device &d = this->dev_;
  d.hostname[HOSTNAME_SIZE - 1] = '\0';
  d.timezone[TIMEZONE_SIZE - 1] = '\0';
  d.ap_password[AP_PASSWORD_SIZE - 1] = '\0';
  d.ntp_servers[NTP_SERVERS_SIZE - 1] = '\0';
  d.flags &= DEVICE_FLAGS_KNOWN;
  std::vector<std::string> servers;
  if (d.ntp_servers[0] != '\0' && (!parse_ntp_servers(d.ntp_servers, servers) || servers.empty())) {
    this->log_(LogLevel::kWarn, "Saved NTP servers not usable, using the build's");
    std::memset(d.ntp_servers, 0, NTP_SERVERS_SIZE);
  }
  if (d.hostname[0] != '\0' && !check_hostname(d.hostname)) {
    this->log_(LogLevel::kWarn, "Saved name not usable, using the build's");
    std::memset(d.hostname, 0, HOSTNAME_SIZE);
  }
  if (d.ap_password[0] != '\0' && !check_wifi_password(d.ap_password, false)) {
    this->log_(LogLevel::kWarn, "Saved access-point password not usable, using the build's");
    std::memset(d.ap_password, 0, AP_PASSWORD_SIZE);
  }
  TzRules tz;
  if (d.timezone[0] != '\0' && !parse_tz(d.timezone, tz)) {
    this->log_(LogLevel::kWarn, "Saved timezone not understood, using the build's");
    d.timezone[0] = '\0';
  }
}

void FeederCore::clamp_loaded_() {
  for (auto &p : this->cfg.profiles) {
    p.every_n = std::min<uint8_t>(std::max<uint8_t>(p.every_n, 1), 14);
    p.fast_days &= 0x7F;
    for (auto &e : p.ev) {
      e.anchor = std::min<uint8_t>(e.anchor, static_cast<uint8_t>(Anchor::SUNSET));
      // Up to the most any mechanism holds, not this one's: a drive or slot count with a smaller load must not
      // rewrite what the user set (it is held to the capacity when it feeds, check_guard_).
      e.portions = std::min<uint8_t>(std::max<uint8_t>(e.portions, EVENT_PORTIONS_LO), CAPACITY_MAX);
      e.days &= 0x7F;
    }
  }
  // The same limits as the setters (feeder_core.h), so nothing out of range survives a reboot.
  auto clamp = [](int32_t v, int32_t lo, int32_t hi) { return std::min(std::max(v, lo), hi); };
  for (auto &c : this->calib.offsets)
    c = static_cast<int16_t>(clamp(c, CAL_OFFSET_LO, CAL_OFFSET_HI));
  this->cfg.active_profile = std::min<uint8_t>(this->cfg.active_profile, NUM_PROFILES - 1);
  this->cfg.min_gap_s = static_cast<uint16_t>(clamp(this->cfg.min_gap_s, MIN_GAP_S_LO, MIN_GAP_S_HI));
  this->cfg.portion_gap_s = static_cast<uint16_t>(clamp(this->cfg.portion_gap_s, PORTION_GAP_S_LO, PORTION_GAP_S_HI));
  this->cfg.reload_pause_min =
      static_cast<uint16_t>(clamp(this->cfg.reload_pause_min, RELOAD_PAUSE_MIN_LO, RELOAD_PAUSE_MIN_HI));
  this->cfg.refill_interval_days =
      static_cast<uint16_t>(clamp(this->cfg.refill_interval_days, REFILL_DAYS_LO, REFILL_DAYS_HI));
  this->cfg.daily_cap = static_cast<uint8_t>(clamp(this->cfg.daily_cap, DAILY_CAP_LO, CAPACITY_MAX));
  this->cfg.max_portions_cmd = static_cast<uint8_t>(clamp(this->cfg.max_portions_cmd, PORTIONS_CMD_LO, CAPACITY_MAX));
  this->cfg.scheduler_enabled = this->cfg.scheduler_enabled ? 1 : 0;
  if (!location_ok(this->cfg.latitude, this->cfg.longitude)) {  // also catches NaN
    this->cfg.latitude = 0.0f;
    this->cfg.longitude = 0.0f;
  }
  this->state_.position = std::min<uint8_t>(this->state_.position, this->capacity());
  if (!pulse_range_ok(this->calib.min_pulse_us, this->calib.max_pulse_us)) {
    this->calib.min_pulse_us = this->default_min_pulse_us_;
    this->calib.max_pulse_us = this->default_max_pulse_us_;
  }
}

bool FeederCore::save_state_(bool flush) {
  const bool ok = this->hal_save_state(this->state_, flush);
  if (flush) {
    if (!ok && !this->storage_fault_)
      this->log_(LogLevel::kError, "Saving the tray state to flash failed");
    this->storage_fault_ = !ok;
  }
  return ok;
}

namespace legacy {
State migrate_state_v2(const StateV2 &v2) {
  State s;
  std::memset(&s, 0, sizeof(s));
  s.version = STATE_VERSION;
  s.hold_until = v2.hold_until;
  s.reload_until = v2.reload_until;
  std::memcpy(s.fired, v2.fired, sizeof(s.fired));
  s.last_feed_epoch = v2.last_feed_epoch;
  for (uint8_t i = 0; i < HISTORY_SIZE_V2; i++) {
    s.hist_ts[i] = v2.hist_ts[i];
    s.hist_n[i] = v2.hist_n[i];
    s.hist_src[i] = v2.hist_src[i];
  }
  s.position = v2.position;
  s.moving_to = v2.moving_to;
  s.unknown = v2.unknown;
  s.last_src = v2.last_src;
  return s;
}
}  // namespace legacy

void FeederCore::cfg_changed() {
  this->project_dirty_ = true;
  this->cfg_dirty_ = true;
  this->cfg_dirty_since_ = this->hal_millis();
}

static bool in_range(int32_t v, int32_t lo, int32_t hi) { return v >= lo && v <= hi; }

Result FeederCore::set_min_gap_s(int32_t s) {
  if (!in_range(s, MIN_GAP_S_LO, MIN_GAP_S_HI))
    return Result::INVALID_ARG;
  this->cfg.min_gap_s = static_cast<uint16_t>(s);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_portion_gap_s(int32_t s) {
  if (!in_range(s, PORTION_GAP_S_LO, PORTION_GAP_S_HI))
    return Result::INVALID_ARG;
  this->cfg.portion_gap_s = static_cast<uint16_t>(s);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_reload_pause_min(int32_t m) {
  if (!in_range(m, RELOAD_PAUSE_MIN_LO, RELOAD_PAUSE_MIN_HI))
    return Result::INVALID_ARG;
  this->cfg.reload_pause_min = static_cast<uint16_t>(m);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_refill_interval_days(int32_t d) {
  if (!in_range(d, REFILL_DAYS_LO, REFILL_DAYS_HI))
    return Result::INVALID_ARG;
  this->cfg.refill_interval_days = static_cast<uint16_t>(d);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_daily_cap(int32_t n) {
  if (!in_range(n, DAILY_CAP_LO, this->capacity()))
    return Result::INVALID_ARG;
  this->cfg.daily_cap = static_cast<uint8_t>(n);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_max_portions_cmd(int32_t n) {
  if (!in_range(n, PORTIONS_CMD_LO, this->capacity()))
    return Result::INVALID_ARG;
  this->cfg.max_portions_cmd = static_cast<uint8_t>(n);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_scheduler_enabled(bool on) {
  this->cfg.scheduler_enabled = on ? 1 : 0;
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_active_profile(int32_t profile) {
  if (!in_range(profile, 0, NUM_PROFILES - 1))
    return Result::INVALID_ARG;
  this->cfg.active_profile = static_cast<uint8_t>(profile);
  this->cfg_changed();
  return Result::OK;
}
Result FeederCore::set_location(float latitude, float longitude) {
  if (!location_ok(latitude, longitude))
    return Result::INVALID_ARG;
  this->cfg.latitude = latitude;
  this->cfg.longitude = longitude;
  this->cfg_changed();
  return Result::OK;
}

Result FeederCore::set_location_e5(int32_t lat, int32_t lon) {
  if (!in_range(lat, -9000000, 9000000) || !in_range(lon, -18000000, 18000000))
    return Result::INVALID_ARG;
  return this->set_location(static_cast<float>(lat) / 100000.0f, static_cast<float>(lon) / 100000.0f);
}

void FeederCore::save_calibration() {
  this->hal_save_calibration(this->calib, true);
  this->hal_save_settings(this->cfg, true);
  if (this->tuning_dirty_)
    this->hal_save_tuning(this->tuning_, true);
  this->cfg_dirty_ = false;
  this->tuning_dirty_ = false;
}

Result FeederCore::calibration_action(uint8_t action, int32_t arg) {
  if (!this->calibrating_)
    return Result::NOT_CALIBRATING;
  // The mechanism may move: a power cut from here on must not leave a trusted position behind (Codex review, 2.1.1).
  if (!this->invalidate_position_("the tray may move in calibration"))
    return Result::STORAGE_FAILED;
  return this->mech_->calibration_action(action, arg, this->hal_millis()) ? Result::OK : Result::INVALID_ARG;
}

uint8_t FeederCore::drive_of(const Mechanism *mechanism) const {
  for (uint8_t i = 0; i < this->drive_count_; i++) {
    if (this->drives_[i] == mechanism)
      return static_cast<uint8_t>(i + 1);
  }
  return DRIVE_BUILD;
}

uint8_t FeederCore::resolve_drive(uint8_t saved) const {
  if (saved >= DRIVE_SERVO && saved <= this->drive_count_)
    return saved;
  return this->drive_of(this->mech_build_ != nullptr ? this->mech_build_ : this->mech_);
}

uint8_t FeederCore::choose_drive(const Device &saved) {
  if (this->mech_build_ == nullptr)
    this->mech_build_ = this->mech_;  // the build's, as set_mechanism() left it
  const uint8_t use = this->resolve_drive(saved.version == DEVICE_VERSION ? saved.reserved[0] : DRIVE_BUILD);
  this->set_mechanism(this->drives_[use - 1]);
  this->drive_in_use_ = use;
  // A different drive from the one the last boot used, or none recorded yet (the first boot after a firmware with one
  // drive, which never recorded it: found on the bench). core_start() marks a saved tray position unknown then.
  this->drive_changed_ = saved.version != DEVICE_VERSION || saved.reserved[1] != use;
  return use;
}

Result FeederCore::set_drive(uint8_t drive) {
  if (drive != DRIVE_BUILD && (drive < DRIVE_SERVO || drive > this->drive_count_))
    return Result::INVALID_ARG;
  if (this->drive_count_ < 2)
    return Result::UNSUPPORTED;
  if (this->dev_.reserved[0] == drive)
    return Result::OK;
  this->dev_.reserved[0] = drive;
  this->hal_save_device(this->dev_);
  this->log_(LogLevel::kInfo, "Drive set to %s; restart to apply",
             drive == DRIVE_BUILD ? "the build's" : this->drives_[drive - 1]->name());
  return Result::OK;
}

int FeederCore::tuning_index(const char *key) const {
  for (uint8_t i = 0; i < this->tuning_count(); i++) {
    const TuningParam *p = this->tuning_param(i);
    if (p != nullptr && std::strcmp(p->key, key) == 0)
      return i;
  }
  return -1;
}

Result FeederCore::set_tuning(uint8_t index, int32_t value) {
  const TuningParam *p = this->tuning_param(index);
  if (p == nullptr || value < p->min || value > p->max) {
    if (p != nullptr)
      this->log_(LogLevel::kWarn, "%s %ld refused (limits %ld to %ld)", p->name, static_cast<long>(value),
                 static_cast<long>(p->min), static_cast<long>(p->max));
    return Result::INVALID_ARG;
  }
  if (this->moving_())
    return Result::BUSY;
  if (this->mech_->tuning_needs_calibration(index) && !this->calibrating_) {
    this->log_(LogLevel::kWarn, "%s can only be changed in calibration mode", p->name);
    return Result::NOT_CALIBRATING;
  }
  if (this->tuning_.values[index] == value)
    return Result::OK;
  // A value that moves where the slots are: which slots hold food is no longer known, also after a power cut.
  if (this->mech_->tuning_needs_calibration(index) && !this->invalidate_position_("the slots moved"))
    return Result::STORAGE_FAILED;
  // From the first change on, the record is this mechanism's (one saved for another mechanism is replaced only now).
  std::strncpy(this->tuning_.mechanism, this->mech_->name(), MECHANISM_NAME_SIZE - 1);
  this->tuning_.layout = this->mech_->tuning_layout();
  this->tuning_.count = this->tuning_count();
  this->tuning_.values[index] = value;
  this->mech_->apply_tuning(index, value);
  this->tuning_dirty_ = true;
  this->cfg_changed();
  this->log_(LogLevel::kInfo, "%s set to %ld", p->name, static_cast<long>(value));
  return Result::OK;
}

void FeederCore::load_tuning_(const Tuning *saved) {
  // Every value starts at its default; saved ones replace them only if they are this mechanism's, in this layout,
  // and within the limits.
  std::memset(&this->tuning_, 0, sizeof(this->tuning_));
  this->tuning_.version = TUNING_VERSION;
  std::strncpy(this->tuning_.mechanism, this->mech_->name(), MECHANISM_NAME_SIZE - 1);
  this->tuning_.layout = this->mech_->tuning_layout();
  this->tuning_.count = this->tuning_count();
  bool usable = false;
  if (saved != nullptr && saved->version == TUNING_VERSION) {
    char name[MECHANISM_NAME_SIZE];
    std::memcpy(name, saved->mechanism, MECHANISM_NAME_SIZE);
    name[MECHANISM_NAME_SIZE - 1] = '\0';
    usable = std::strcmp(name, this->tuning_.mechanism) == 0 &&
             (saved->layout == this->tuning_.layout ||
              (saved->layout < this->tuning_.layout && this->mech_->tuning_layout_extends(saved->layout)));
    if (usable && saved->layout != this->tuning_.layout)
      this->log_(LogLevel::kInfo, "Tuning from layout %u kept (layout %u only adds values)", saved->layout,
                 this->tuning_.layout);
    if (!usable)
      this->log_(LogLevel::kWarn, "Saved tuning is for %s (layout %u), not %s (layout %u): using the defaults", name,
                 saved->layout, this->tuning_.mechanism, this->tuning_.layout);
  }
  for (uint8_t i = 0; i < this->tuning_count(); i++) {
    const TuningParam *p = this->tuning_param(i);
    if (p == nullptr)
      continue;
    int32_t v = p->def;
    if (usable && i < saved->count) {
      if (saved->values[i] >= p->min && saved->values[i] <= p->max)
        v = saved->values[i];
      else
        this->log_(LogLevel::kWarn, "Saved %s %ld out of range, using %ld", p->name, static_cast<long>(saved->values[i]),
                   static_cast<long>(p->def));
    }
    this->tuning_.values[i] = v;
    this->mech_->apply_tuning(i, v);
  }
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
                            const legacy::SettingsV3 *legacy_v3, const Device *device,
                            const legacy::DeviceV1 *device_v1, const Tuning *tuning) {
  this->defaults_();
  this->load_tuning_(tuning);
  // A version 1 record (firmware 2.0.0 and 2.0.1) with no current one: migrate it and save it in the new layout.
  Device migrated;
  const bool migrate = device == nullptr && device_v1 != nullptr && device_v1->version == 1;
  if (migrate) {
    migrated = legacy::migrate_device_v1(*device_v1);
    device = &migrated;
  }
  if (device != nullptr && device->version == DEVICE_VERSION) {
    this->dev_ = *device;
    this->clamp_device_();
    if (migrate) {
      this->hal_save_device(this->dev_);
      this->log_(LogLevel::kInfo, "Device settings migrated from version 1 (all kept)");
    }
  } else if (device == nullptr) {
    // The first boot with a Device record to make. A feeder that was in use before (an update from 1.x: settings or a
    // calibration saved) is not a new one, so no setup wizard. Decided once and saved now: a new feeder saves its
    // settings and tray state soon after its first boot, so the next boot could no longer tell. Every other field
    // still says "the build's", so a YAML build keeps following its YAML.
    if ((settings != nullptr && settings->version == SETTINGS_VERSION) ||
        (calibration != nullptr && calibration->version == CALIBRATION_VERSION) ||
        (legacy_v3 != nullptr && legacy_v3->version == 3))
      this->dev_.flags |= DEVICE_SETUP_DONE;
    this->hal_save_device(this->dev_);
  }
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
  if (this->dev_.timezone[0] != '\0')
    this->apply_timezone_();
  if (this->dev_.ntp_servers[0] != '\0') {
    std::vector<std::string> servers;
    parse_ntp_servers(this->dev_.ntp_servers, servers);  // clamp_device_() checked it
    this->hal_apply_ntp(&servers);
  }
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
    HistoryEntry e = this->history_(i);
    e.this_boot = false;
    if (e.epoch == 0)
      e.portions = 0;
    this->set_history_(i, e);
  }

  if (this->drive_in_use_ != DRIVE_BUILD && this->dev_.reserved[1] != this->drive_in_use_) {
    // Remember the drive this boot uses. If it is not the one the last boot used, which slots hold food is not known.
    if (this->drive_changed_ && state != nullptr && state->version == STATE_VERSION && !this->state_.unknown) {
      this->log_(LogLevel::kWarn, "The drive changed to %s: reload the tray before feeding", this->mech_->name());
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
    }
    this->dev_.reserved[1] = this->drive_in_use_;
    this->hal_save_device(this->dev_);
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
    // Go to the saved position (never to 0), then let the motor go after a moment.
    this->mech_->restore(this->state_.position, this->hal_millis());
    this->positioned_();
  }
}

void FeederCore::core_loop() {
  this->run_motion_();
  // Values the mechanism's calibration worked out go through the same setter as every other change.
  uint8_t tuning_index;
  int32_t tuning_value;
  while (this->mech_->take_tuning_update(tuning_index, tuning_value))
    this->set_tuning(tuning_index, tuning_value);
  const uint32_t now = this->hal_millis();
  if (this->boot_pressed_ && !this->boot_handled_ && now - this->boot_pressed_ms_ >= BOOT_HOLD_MS) {
    this->boot_handled_ = true;
    if (this->password_set())
      this->clear_password("BOOT held for 10 s");
    else
      this->log_(LogLevel::kInfo, "BOOT held for 10 s: no settings password to clear");
  }
  if (this->cfg_dirty_ && now - this->cfg_dirty_since_ >= CFG_SAVE_DELAY_MS) {
    // Flushed: otherwise ESPHome holds the write for up to its flash_write_interval (60 s by default), and a power
    // cut in that time would lose the edit. Records that did not change are not rewritten.
    this->hal_save_settings(this->cfg, true);
    this->hal_save_calibration(this->calib, true);
    if (this->tuning_dirty_)
      this->hal_save_tuning(this->tuning_, true);
    this->cfg_dirty_ = false;
    this->tuning_dirty_ = false;
  }
  if (this->tz_reapply_) {
    this->tz_reapply_ = false;
    if (this->dev_.timezone[0] != '\0')
      this->apply_timezone_();
  }
  if (now - this->last_tick_ >= 1000) {
    this->last_tick_ = now;
    this->schedule_tick_();
  }
}

// ---------------------------------------------------------------------------------------------
// Motion: the mechanism moves; the core keeps the position, the intent and the history
// ---------------------------------------------------------------------------------------------

bool FeederCore::moving_() const {
  // A positioning move counts until it has settled: the servo reports it done at once, while it is still turning.
  return this->phase_ == Phase::MOVE || this->phase_ == Phase::GAP || this->phase_ == Phase::POSITION ||
         (this->phase_ == Phase::SETTLE && this->positioning_);
}

bool FeederCore::busy() {
  return this->moving_() || this->mech_->moving();
}

bool FeederCore::begin_positioning_(uint8_t target) {
  this->state_.position = target;
  this->state_.moving_to = target;
  if (!this->save_state_(true)) {
    this->log_(LogLevel::kError, "The move's intent could not be saved: not moving, tray position is now UNKNOWN");
    this->state_.unknown = 1;
    this->state_.moving_to = NO_MOVE;
    this->positioning_ = false;
    return false;
  }
  this->positioning_ = true;
  return true;
}

bool FeederCore::invalidate_position_(const char *why) {
  if (this->state_.unknown)
    return true;
  this->state_.unknown = 1;
  this->state_.moving_to = NO_MOVE;
  if (!this->save_state_(true)) {
    this->log_(LogLevel::kError, "Could not save the tray position as unknown: refused");
    this->state_.unknown = 0;
    return false;
  }
  this->project_dirty_ = true;
  this->log_(LogLevel::kWarn, "Tray position is now UNKNOWN (%s): reload the tray before feeding", why);
  return true;
}

void FeederCore::positioned_() {
  const uint32_t now = this->hal_millis();
  if (this->mech_->poll(now) == MoveStatus::BUSY) {
    this->phase_ = Phase::POSITION;
  } else {
    this->phase_ = Phase::SETTLE;
  }
  this->phase_start_ = now;
}

bool FeederCore::start_next_portion_() {
  const uint8_t target = this->state_.position + 1;
  // Persist the intent before moving, so a power loss mid-step is detected at the next boot. If that save failed, a
  // power loss mid-step would not be detected: do not move.
  this->state_.moving_to = target;
  if (!this->save_state_(true)) {
    this->state_.moving_to = NO_MOVE;
    this->log_(LogLevel::kError, "The feed's intent could not be saved: not moving");
    return false;
  }
  this->mech_->feed_one(this->state_.position, this->hal_millis());
  this->phase_ = Phase::MOVE;
  this->phase_start_ = this->hal_millis();
  return true;
}

void FeederCore::commit_step_() {
  this->state_.position = this->state_.moving_to;
  this->state_.moving_to = NO_MOVE;
  this->save_state_(true);
  this->project_dirty_ = true;
  if (this->queue_ > 0)
    this->queue_--;
  this->phase_start_ = this->hal_millis();
  if (this->queue_ > 0 && this->state_.position < this->capacity()) {
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
    case Phase::MOVE:
    case Phase::POSITION: {
      const MoveStatus status = this->mech_->poll(now);
      if (status == MoveStatus::BUSY)
        return;
      if (status == MoveStatus::FAULT) {
        // The mechanism could not finish (stalled, ran too far, timed out): treated as an interrupted move.
        this->log_(LogLevel::kError, "Motor fault: %s", this->mech_->fault_text());
        this->abort_motion_();
      } else if (this->phase_ == Phase::MOVE) {
        this->commit_step_();
      } else {
        this->phase_ = Phase::SETTLE;
        this->phase_start_ = now;
      }
      return;
    }
    case Phase::GAP:
      if (now - this->phase_start_ < static_cast<uint32_t>(this->cfg.portion_gap_s) * 1000UL)
        return;
      if (!this->start_next_portion_()) {
        this->uncount_portions_(this->queue_);  // the rest never dropped
        this->queue_ = 0;
        this->phase_ = Phase::SETTLE;
        this->phase_start_ = now;
      }
      return;
    case Phase::SETTLE:
      if (now - this->phase_start_ < this->mech_->settle_ms())
        return;
      if (this->positioning_) {
        // The positioning move is over: the intent goes.
        this->positioning_ = false;
        this->state_.moving_to = NO_MOVE;
        this->save_state_(true);
      }
      if (!this->calibrating_)
        this->mech_->release();
      this->phase_ = Phase::IDLE;
      return;
  }
}

void FeederCore::abort_motion_() {
  switch (this->phase_) {
    case Phase::MOVE:
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
      this->mech_->stop();
      break;
    case Phase::POSITION:
      // Interrupted while moving for a reload or a restore: the tray may be anywhere.
      this->log_(LogLevel::kWarn, "Motion aborted while positioning: tray position is now UNKNOWN");
      this->positioning_ = false;
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
      this->queue_ = 0;
      this->phase_ = Phase::IDLE;
      this->mech_->stop();
      break;
    case Phase::SETTLE:
      if (this->positioning_) {
        // A positioning move that had not settled: the tray may be anywhere.
        this->log_(LogLevel::kWarn, "Motion aborted while positioning: tray position is now UNKNOWN");
        this->positioning_ = false;
        this->state_.unknown = 1;
        this->state_.moving_to = NO_MOVE;
        this->save_state_(true);
        this->phase_ = Phase::IDLE;
        this->mech_->stop();
      }
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
  HistoryEntry newest = this->history_(0);
  if (portions == 0 || newest.portions == 0)
    return;
  newest.portions = static_cast<uint8_t>(newest.portions > portions ? newest.portions - portions : 0);
  this->set_history_(0, newest);
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Feed cut short: %u portion(s) did not drop", portions);
}

void FeederCore::stop() {
  this->log_(LogLevel::kInfo, "Stop requested");
  this->abort_motion_();
  if (this->mech_->moving()) {
    // A calibration move runs in the mechanism, outside the core's phases (Codex review, 2.1.1): stop it too. The
    // position was saved as unknown before it could move.
    this->mech_->stop();
  }
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
  const uint32_t pause_ms = static_cast<uint32_t>(this->cfg.reload_pause_min) * 60000UL;
  this->reload_pause_ms_active_ = pause_ms > 0;
  this->reload_pause_until_ms_ = this->hal_millis() + pause_ms;
  this->state_.reload_until =
      (pause_ms > 0 && this->time_valid())
          ? static_cast<uint32_t>(this->hal_epoch() + static_cast<int64_t>(this->cfg.reload_pause_min) * 60)
          : 0;
  // Back to the closed slot, saved as an intent until it has settled (a power cut on the way leaves it unknown).
  this->state_.unknown = 0;
  if (!this->begin_positioning_(0))
    return;
  this->mech_->reload(this->hal_millis());
  this->positioned_();
  this->project_dirty_ = true;
  this->log_(LogLevel::kInfo, "Tray reload: position 0, %u portions, scheduled feeding paused for %u min",
             this->capacity(), this->cfg.reload_pause_min);
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
  if (position > this->capacity() || this->calibrating_)
    return;
  this->abort_motion_();
  this->state_.unknown = 0;
  if (!this->begin_positioning_(position))
    return;
  this->mech_->restore(position, this->hal_millis());
  this->positioned_();
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
    this->cal_pos_ = 0;
    if (this->mech_->calibration_points() > 0) {
      this->cal_pos_ = std::min<uint8_t>(this->state_.position, this->mech_->calibration_points() - 1);
      this->mech_->calibration_show(this->cal_pos_, this->hal_millis());
    }
    this->phase_ = Phase::IDLE;
    this->log_(LogLevel::kInfo, "Calibration mode on");
  } else {
    this->calibrating_ = false;
    if (this->mech_->calibration_moved() && !this->state_.unknown) {
      // The tray turned during calibration: which slots still hold food is not known. Reload before feeding.
      this->log_(LogLevel::kWarn, "The tray moved during calibration: reload it before feeding");
      this->state_.unknown = 1;
      this->state_.moving_to = NO_MOVE;
      this->save_state_(true);
    }
    this->save_calibration();
    if (!this->state_.unknown && this->begin_positioning_(this->state_.position)) {
      this->mech_->restore(this->state_.position, this->hal_millis());
      this->positioned_();
    } else {
      this->mech_->release();
    }
    this->log_(LogLevel::kInfo, "Calibration mode off, offsets saved");
  }
}

void FeederCore::calibration_select(uint8_t position) {
  const uint8_t points = this->mech_->calibration_points();
  if (points == 0)
    return;
  this->cal_pos_ = std::min<uint8_t>(position, points - 1);
  if (this->calibrating_)
    this->mech_->calibration_show(this->cal_pos_, this->hal_millis());
}

void FeederCore::set_calibration_offset(int16_t tenths) { this->set_calibration_offset_at(this->cal_pos_, tenths); }

void FeederCore::set_calibration_offset_at(uint8_t position, int16_t tenths) {
  position = std::min<uint8_t>(position, NUM_POSITIONS - 1);
  this->calib.offsets[position] = std::min<int16_t>(std::max<int16_t>(tenths, CAL_OFFSET_LO), CAL_OFFSET_HI);
  this->cfg_changed();
  if (this->calibrating_ && position == this->cal_pos_ && position < this->mech_->calibration_points())
    this->mech_->calibration_show(position, this->hal_millis());
}

Result FeederCore::set_pulse_range(uint16_t min_us, uint16_t max_us) {
  if (!this->is_servo())
    return Result::UNSUPPORTED;
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
    this->mech_->calibration_show(this->cal_pos_, this->hal_millis());
  return Result::OK;
}

Result FeederCore::jog_calibration(uint8_t position, int16_t delta_tenths) {
  if (!this->calibrating_)
    return Result::NOT_CALIBRATING;
  if (position >= this->mech_->calibration_points())
    return Result::INVALID_ARG;
  this->cal_pos_ = position;
  const int32_t next = static_cast<int32_t>(this->calib.offsets[position]) + delta_tenths;
  this->calib.offsets[position] =
      static_cast<int16_t>(std::min<int32_t>(std::max<int32_t>(next, CAL_OFFSET_LO), CAL_OFFSET_HI));
  this->cfg_changed();
  this->mech_->calibration_show(position, this->hal_millis());
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

FeederCore::HistoryEntry FeederCore::history_(uint8_t i) const {
  HistoryEntry e;
  e.epoch = this->state_.hist_ts[i];
  e.portions = this->state_.hist_n[i];
  e.source = this->state_.hist_src[i];
  e.at_ms = this->hist_ms_[i];
  e.this_boot = this->hist_this_boot_[i];
  return e;
}

void FeederCore::set_history_(uint8_t i, const HistoryEntry &e) {
  this->state_.hist_ts[i] = e.epoch;
  this->state_.hist_n[i] = e.portions;
  this->state_.hist_src[i] = e.source;
  this->hist_ms_[i] = e.at_ms;
  this->hist_this_boot_[i] = e.this_boot;
}

void FeederCore::push_history_(const HistoryEntry &e) {
  for (uint8_t i = HISTORY_SIZE - 1; i > 0; i--)
    this->set_history_(i, this->history_(i - 1));
  this->set_history_(0, e);
}

bool FeederCore::counts_today_(const HistoryEntry &e, int64_t now_epoch, bool time_ok) {
  if (e.portions == 0)
    return false;
  if (e.epoch != 0 && time_ok)
    return now_epoch - static_cast<int64_t>(e.epoch) < 86400;
  if (e.this_boot)
    return this->hal_millis() - e.at_ms < 86400000UL;
  // Saved before this boot, and the clock is not set again yet: its age is unknown, so it is not counted.
  // (Counting it would refuse feeds for a day after a power cut with no network; decision 2026-10-14.)
  return false;
}

uint16_t FeederCore::portions_24h_(int64_t now_epoch, bool time_ok) {
  uint16_t total = 0;
  for (uint8_t i = 0; i < HISTORY_SIZE; i++) {
    const HistoryEntry e = this->history_(i);
    if (this->counts_today_(e, now_epoch, time_ok))
      total += e.portions;
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
      HistoryEntry e = this->history_(static_cast<uint8_t>(i));
      if (!this->counts_today_(e, now, time_ok))
        continue;
      const uint16_t take = std::min<uint16_t>(e.portions, excess);
      e.portions = static_cast<uint8_t>(e.portions - take);
      this->set_history_(static_cast<uint8_t>(i), e);
      excess -= take;
    }
  }
  this->save_state_(true);
  this->project_dirty_ = true;
  this->log_(LogLevel::kWarn, "Portions counted in the last 24 h set by hand: %u -> %u", before,
             this->portions_24h_(now, time_ok));
}

void FeederCore::record_history_(uint8_t portions, Source source) {
  HistoryEntry e;
  e.epoch = this->time_valid() ? static_cast<uint32_t>(this->hal_epoch()) : 0;
  e.portions = portions;
  e.source = static_cast<uint8_t>(source);
  e.at_ms = this->hal_millis();
  e.this_boot = true;
  this->push_history_(e);
}

Result FeederCore::check_guard_(uint8_t &portions, Source source) {
  const bool scheduled = source == Source::SCHEDULE || source == Source::ONE_SHOT;
  if (portions == 0)
    return Result::INVALID_ARG;
  if (portions > this->capacity()) {
    // A request for more than a load holds. Manual: refused, as typed. Scheduled: an event saved for a drive or slot
    // count with a bigger load feeds what this one holds (and then what remains), rather than nothing.
    if (!scheduled)
      return Result::INVALID_ARG;
    portions = this->capacity();
  }
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

Result FeederCore::feed_guard_preview() {
  uint8_t portions = 1;
  return this->check_guard_(portions, Source::ESPHOME);  // only reads: nothing is fed, recorded or reported
}

std::string FeederCore::feed_guard_text() {
  const Result r = this->feed_guard_preview();
  return r == Result::OK ? "ready" : result_name(r);
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
  this->state_.last_feed_epoch = this->history_(0).epoch;
  this->state_.last_src = static_cast<uint8_t>(source);
  this->last_feed_ms_ = this->hal_millis();
  this->have_last_feed_ms_ = true;
  this->queue_ = portions;
  this->project_dirty_ = true;
  if (!this->start_next_portion_()) {  // persists the intent and the history before the tray moves
    this->uncount_portions_(portions);  // nothing dropped
    this->queue_ = 0;
    this->report_(source, requested, Result::STORAGE_FAILED);
    return Result::STORAGE_FAILED;
  }
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
  // Home Assistant sends its timezone with the time, right after this call; a timezone set on the device wins.
  if (source == TimeSource::HOME_ASSISTANT && this->dev_.timezone[0] != '\0')
    this->tz_reapply_ = true;
  // Debug only: NTP and Home Assistant each re-sync every 15 minutes, so at INFO this would fill the log.
  this->log_(LogLevel::kDebug, "Clock set by %s", this->time_source_text().c_str());
}

Result FeederCore::set_timezone(const std::string &posix) {
  TzRules tz;
  if (!posix.empty() && !parse_tz(posix.c_str(), tz)) {
    this->log_(LogLevel::kWarn, "Timezone '%.63s' not understood (expected a POSIX TZ string)", posix.c_str());
    return Result::INVALID_ARG;
  }
  if (posix == this->dev_.timezone)
    return Result::OK;
  std::memset(this->dev_.timezone, 0, sizeof(this->dev_.timezone));
  std::memcpy(this->dev_.timezone, posix.data(), posix.size());  // parse_tz() checked the length
  this->hal_save_device(this->dev_);
  if (posix.empty()) {
    this->hal_apply_timezone(nullptr);
    this->log_(LogLevel::kInfo, "Timezone: back to the build's");
  } else {
    this->hal_apply_timezone(&tz);
    this->log_(LogLevel::kInfo, "Timezone set to %s", this->dev_.timezone);
  }
  this->timezone_changed_();
  return Result::OK;
}

void FeederCore::apply_timezone_() {
  TzRules tz;
  if (parse_tz(this->dev_.timezone, tz))
    this->hal_apply_timezone(&tz);
  this->timezone_changed_();
}

void FeederCore::timezone_changed_() {
  // Days, midnights and sun times all depend on the timezone: work them out again. Events already fired today stay
  // fired (State::fired holds day numbers), so a change never repeats a feed.
  this->day_cache_hour_ = -1;
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

}  // namespace precision_feeder
}  // namespace esphome
