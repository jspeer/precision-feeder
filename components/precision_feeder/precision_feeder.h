// Precision Feeder: the ESPHome component.
//
// All the logic is in FeederCore (feeder_core.h), which is plain C++ and tested on a host. This class only
// connects it to the device: the clock, the servo's PWM output, flash storage and the log. YAML lambdas and
// the BLE component call the FeederCore methods through it (id(feeder).request_feed(...) and so on).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/time/real_time_clock.h"

#include "feeder_core.h"

namespace esphome {
namespace precision_feeder {

class PrecisionFeeder : public Component, public FeederCore {
 public:
  // ---- configuration (set from Python) ----
  void set_output(output::FloatOutput *output) { this->output_ = output; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }

  /// Wired to the sntp time platform's on_time_sync. ESPHome also fires that trigger once at boot when the clock
  /// is already valid (kept by the RTC across a soft reset); only a real SNTP synchronisation counts as NTP.
  void note_sntp_sync();

  void setup() override;
  void loop() override { this->core_loop(); }
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

 protected:
  uint32_t hal_millis() override;
  int64_t hal_uptime_ms() override;
  bool hal_time_valid() override;
  int64_t hal_epoch() override;
  void hal_set_epoch(int64_t epoch) override;
  LocalTime hal_local(int64_t epoch) override;
  int64_t hal_local_to_epoch(int year, int month, int day, int hour, int minute) override;
  void hal_servo_write(float angle_degrees) override;
  void hal_servo_release() override;
  void hal_save_settings(const Settings &settings, bool flush) override;
  void hal_save_state(const State &state, bool flush) override;
  void hal_log(LogLevel level, const char *message) override;

  output::FloatOutput *output_{nullptr};
  time::RealTimeClock *time_{nullptr};
  ESPPreferenceObject pref_settings_;
  ESPPreferenceObject pref_state_;
};

}  // namespace precision_feeder
}  // namespace esphome
