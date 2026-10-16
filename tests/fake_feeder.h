// Shared host-test fixture: FeederCore on a simulated clock (fixed UTC-5) with storage that survives a simulated
// reboot. Used by test_core.cpp and test_ble_engine.cpp.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "feeder_core.h"

using namespace esphome::precision_feeder;

// ---- fake platform ---------------------------------------------------------------------------

/// Storage that survives a simulated reboot.
struct Flash {
  bool have_settings{false};
  bool have_state{false};
  bool have_calibration{false};
  bool have_v3{false};
  bool have_device{false};
  Settings settings{};
  State state{};
  Calibration calibration{};
  legacy::SettingsV3 v3{};
  Device device{};
  int device_saves{0};
  int settings_saves{0};
  bool settings_flushed{false};
  bool state_flushed{false};  // whether the last state save asked for an immediate flush
};

static constexpr int64_t TZ = -5 * 3600;  // fixed UTC-5, no daylight saving

inline int64_t local_epoch(int y, int mo, int d, int h, int mi, int s = 0) {
  return static_cast<int64_t>(days_from_civil(y, mo, d)) * 86400 + h * 3600 + mi * 60 + s - TZ;
}

class FakeFeeder : public FeederCore {
 public:
  FakeFeeder(Flash &flash, int64_t epoch_ms, bool clock_valid) : flash_(flash), epoch_ms_(epoch_ms), valid_(clock_valid) {}

  void start() {
    this->set_detach_delay(1000);
    this->core_start(flash_.have_settings ? &flash_.settings : nullptr,
                     flash_.have_calibration ? &flash_.calibration : nullptr,
                     flash_.have_state ? &flash_.state : nullptr, flash_.have_v3 ? &flash_.v3 : nullptr,
                     flash_.have_device ? &flash_.device : nullptr);
    this->set_feed_listener([this](const FeedRecord &r) { this->records.push_back(r); });
  }

  /// Advance simulated time, running the main loop every `step_ms`.
  void advance(int64_t ms, int64_t step_ms = 250) {
    for (int64_t t = 0; t < ms; t += step_ms) {
      this->uptime_ms_ += step_ms;
      this->epoch_ms_ += step_ms;
      this->core_loop();
    }
  }
  void set_clock_valid(bool v) { this->valid_ = v; }
  int64_t now() const { return this->epoch_ms_ / 1000; }
  int count(Source s, Result r) const {
    int n = 0;
    for (const auto &rec : this->records)
      if (rec.source == static_cast<uint8_t>(s) && rec.result == static_cast<uint8_t>(r))
        n++;
    return n;
  }
  int count_source(Source s) const {
    int n = 0;
    for (const auto &rec : this->records)
      if (rec.source == static_cast<uint8_t>(s))
        n++;
    return n;
  }

  std::vector<FeedRecord> records;
  std::vector<float> angles;
  int releases{0};
  std::vector<std::string> logs;
  int tz_applies{0};         // calls to hal_apply_timezone
  bool tz_applied{false};    // the last call set a timezone (false: went back to the build's)
  TzRules tz_rules{};        // what the last call set

 protected:
  uint32_t hal_millis() override { return static_cast<uint32_t>(this->uptime_ms_); }
  int64_t hal_uptime_ms() override { return this->uptime_ms_; }
  bool hal_time_valid() override { return this->valid_; }
  int64_t hal_epoch() override { return this->epoch_ms_ / 1000; }
  void hal_set_epoch(int64_t epoch) override {
    this->epoch_ms_ = epoch * 1000;
    this->valid_ = true;
  }
  LocalTime hal_local(int64_t epoch) override {
    const int64_t local = epoch + TZ;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) {
      rem += 86400;
      days--;
    }
    int y;
    unsigned m, d;
    civil_from_days(static_cast<int32_t>(days), y, m, d);
    return LocalTime{y, static_cast<int>(m), static_cast<int>(d), static_cast<int>(rem / 3600),
                     static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60)};
  }
  int64_t hal_local_to_epoch(int y, int mo, int d, int h, int mi) override { return local_epoch(y, mo, d, h, mi); }
  void hal_servo_write(float angle) override { this->angles.push_back(angle); }
  void hal_servo_release() override { this->releases++; }
  void hal_save_settings(const Settings &s, bool flush) override {
    flash_.settings_saves++;
    flash_.settings_flushed = flush;
    flash_.settings = s;
    flash_.have_settings = true;
  }
  void hal_save_calibration(const Calibration &c, bool) override {
    flash_.calibration = c;
    flash_.have_calibration = true;
  }
  void hal_save_state(const State &s, bool flush) override {
    flash_.state_flushed = flush;
    flash_.state = s;
    flash_.have_state = true;
  }
  void hal_save_device(const Device &d) override {
    flash_.device_saves++;
    flash_.device = d;
    flash_.have_device = true;
  }
  void hal_apply_timezone(const TzRules *rules) override {
    this->tz_applies++;
    this->tz_applied = rules != nullptr;
    if (rules != nullptr)
      this->tz_rules = *rules;
  }
  void hal_log(LogLevel, const char *message) override { this->logs.emplace_back(message); }

  Flash &flash_;
  int64_t epoch_ms_;
  int64_t uptime_ms_{0};
  bool valid_;
};

// 2026-10-14 is a Wednesday.
static const int64_t T0 = local_epoch(2026, 10, 14, 7, 55);

inline std::unique_ptr<FakeFeeder> boot(Flash &flash, int64_t epoch, bool clock_valid = true) {
  auto f = std::make_unique<FakeFeeder>(flash, epoch * 1000, clock_valid);
  f->start();
  f->advance(1500);  // let the boot settle (servo released)
  return f;
}

inline void enable_event(FakeFeeder &f, int profile, int event, Anchor anchor, int offset_min, int portions = 1,
                         uint8_t days = 0x7F) {
  Event &e = f.cfg.profiles[profile].ev[event];
  e.enabled = 1;
  e.anchor = static_cast<uint8_t>(anchor);
  e.offset_min = static_cast<int16_t>(offset_min);
  e.portions = static_cast<uint8_t>(portions);
  e.days = days;
  f.cfg_changed();
}
