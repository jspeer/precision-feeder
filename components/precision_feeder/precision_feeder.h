// Precision Feeder: the ESPHome component.
//
// All the logic is in FeederCore (feeder_core.h), which is plain C++ and tested on a host. This class only
// connects it to the device: the clock, the servo's PWM output, flash storage and the log. YAML lambdas and
// the BLE component call the FeederCore methods through it (id(feeder).request_feed(...) and so on).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include "esphome/core/component.h"
#include "esphome/core/helpers.h"
#include "esphome/core/preferences.h"
#include "esphome/components/esp32/gpio.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/time/real_time_clock.h"
#ifdef USE_TIME_TIMEZONE
#include "esphome/components/time/posix_tz.h"
#endif

#include <cmath>
#include <string>
#include <vector>

#ifdef USE_WEBSERVER
#include "esphome/components/web_server/web_server.h"
#endif

#include "feeder_core.h"

namespace esphome {
namespace precision_feeder {

// ---- Publishing entity states only when they change ---------------------------------------------------
// The YAML packages refresh their entities from one interval each and call these, so Home Assistant, the web UI
// and the log only see real changes (template entities would otherwise republish the same value every poll).
// They take any ESPHome entity with the matching `state` / publish_state(), so this header needs no entity headers.

template<typename E> void publish_number(E *e, float v) {
  if (e->has_state() && (e->state == v || (std::isnan(e->state) && std::isnan(v))))
    return;
  e->publish_state(v);
}
template<typename E> void publish_text(E *e, const std::string &v) {
  if (e->has_state() && e->state == v)
    return;
  e->publish_state(v);
}
template<typename E> void publish_bool(E *e, bool v) {
  if (e->has_state() && e->state == v)
    return;
  e->publish_state(v);
}
template<typename E> void publish_option(E *e, const std::string &v) {
  if (e->has_state() && e->current_option() == v)
    return;
  e->publish_state(v);
}

class PrecisionFeeder : public Component, public FeederCore {
 public:
  // ---- configuration (set from Python) ----
  void set_output(output::FloatOutput *output) { this->output_ = output; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }

  // ---- runtime pins (DESIGN.md section 2.1; set from Python) ----
  // The drivers read their pin when they start, so PinLoader (below) rewrites them first. "None" for the button or
  // the LED keeps that driver from starting at all.
  void set_servo_pin(esp32::ESP32InternalGPIOPin *pin) { this->servo_pin_ = pin; }
  void set_button(Component *sensor, esp32::ESP32InternalGPIOPin *pin) {
    this->button_ = sensor;
    this->button_pin_ = pin;
  }
  /// The LED strip driver keeps its pin as a plain number, so the build's pin is passed in too.
  template<typename L> void set_status_led(Component *light, L *output, uint8_t build_pin) {
    this->led_light_ = light;
    this->led_output_ = output;
    this->led_build_pin_ = build_pin;
    this->led_set_pin_ = [](Component *o, uint8_t pin) { static_cast<L *>(o)->set_pin(pin); };
  }
  /// Load the saved pins and give them to the drivers. Runs before any driver's setup().
  void apply_pins();

  /// Wired to the sntp time platform's on_time_sync. ESPHome also fires that trigger once at boot when the clock
  /// is already valid (kept by the RTC across a soft reset); only a real SNTP synchronisation counts as NTP.
  void note_sntp_sync();

#ifdef USE_WEBSERVER
  /// The settings password guards writes to these entities ("number/Daily Portion Cap") through the web server.
  void set_web_server(web_server::WebServer *web_server) { this->web_server_ = web_server; }
  void add_protected(const char *entity) { this->protected_.emplace_back(entity); }
  /// Put the settings guard in front of the web server's own handler. Called before any component starts, so it is
  /// registered first and sees every request before the web server does.
  void register_settings_guard();
  bool is_protected(const std::string &entity) const;
  const std::vector<std::string> &protected_entities() const { return this->protected_; }
  web_server::WebServer *web_server() const { return this->web_server_; }
#endif
  /// The settings-password calls of FeederCore run on the web server's task too: they go through this lock.
  Mutex &password_lock() { return this->password_lock_; }
  /// Run `f` on the main loop (safe to call from the web server's task).
  void run_on_main_loop(std::function<void()> &&f) { this->defer(std::move(f)); }

  void setup() override;
  void loop() override {
    LockGuard guard(this->password_lock_);
    this->core_loop();
  }
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
  void hal_save_calibration(const Calibration &calibration, bool flush) override;
  void hal_save_state(const State &state, bool flush) override;
  void hal_save_device(const Device &device) override;
  void hal_apply_timezone(const TzRules *rules) override;
  void hal_log(LogLevel level, const char *message) override;
  void hal_random(uint8_t *out, size_t n) override;

  output::FloatOutput *output_{nullptr};
  esp32::ESP32InternalGPIOPin *servo_pin_{nullptr};
  Component *button_{nullptr};
  esp32::ESP32InternalGPIOPin *button_pin_{nullptr};
  Component *led_light_{nullptr};
  Component *led_output_{nullptr};
  uint8_t led_build_pin_{PIN_NONE};
  void (*led_set_pin_)(Component *output, uint8_t pin){nullptr};
  time::RealTimeClock *time_{nullptr};
  ESPPreferenceObject pref_settings_;
  ESPPreferenceObject pref_calibration_;
  ESPPreferenceObject pref_state_;
  ESPPreferenceObject pref_device_;
  Mutex password_lock_;
#ifdef USE_WEBSERVER
  web_server::WebServer *web_server_{nullptr};
  std::vector<std::string> protected_;
#endif
#ifdef USE_TIME_TIMEZONE
  time::ParsedTimezone build_tz_{};  // the timezone in use at boot, before any set on the device
#endif
};

/// Applies the saved pins before the drivers start: set up ahead of everything at HARDWARE priority (LEDC, the LED
/// strip, GPIO binary sensors). The preferences are ready by then (ESPHome opens them before any setup()).
class PinLoader : public Component {
 public:
  explicit PinLoader(PrecisionFeeder *feeder) : feeder_(feeder) {}
  void setup() override { this->feeder_->apply_pins(); }
  float get_setup_priority() const override { return setup_priority::BUS; }

 protected:
  PrecisionFeeder *feeder_;
};

}  // namespace precision_feeder
}  // namespace esphome
