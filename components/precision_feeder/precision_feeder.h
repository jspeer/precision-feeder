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
#if defined(USE_OTA_PASSWORD) && defined(USE_OTA_ENCRYPTION_PROVISIONED)
#include "esphome/components/esphome/ota/ota_esphome.h"
#endif
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

#include <atomic>

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

/// A driver that claims its own pins at setup (the direct-drive disc): it is told which ones to use first (D19).
class MotorPinUser {
 public:
  virtual void use_pins(const uint8_t *pins, uint8_t n) = 0;

 protected:
  ~MotorPinUser() = default;
};

class PrecisionFeeder : public Component, public FeederCore {
 public:
  // ---- configuration (set from Python) ----
  void set_output(output::FloatOutput *output) { this->output_ = output; }
  /// The servo output's component (its LEDC), so a build that also carries another drive can leave it unset up.
  void set_servo_driver(Component *driver) { this->servo_driver_ = driver; }
  /// A mechanism component offers itself (before setup). `with_servo` false: it is this build's mechanism. True (the
  /// build has the servo's output too): a second drive, chosen on the device (experiment/single-binary), and this
  /// build's default. `pins`: the GPIOs it drives.
  void offer_drive(Mechanism *mechanism, Component *driver, const uint8_t *pins, uint8_t n, bool with_servo);
  /// At boot, before the drivers start (PinLoader): use the saved drive; the others are never set up.
  void apply_drive();
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
  /// A driver offers its pins as the build sets them (before setup); the ones saved on the device win (D19).
  void offer_motor_pins(MotorPinUser *user, const uint8_t *pins, uint8_t n, const char *const *names) {
    this->motor_user_ = user;
    this->set_motor_pins_build(pins, n, names);
  }
  /// At boot, before the drivers start (PinLoader): the motor pins from the saved record, told to their driver.
  void apply_motor_pins_early();
  // The drives a single binary carries, besides the servo (index = drive number - 2).
  struct DriveOffer {
    Component *driver{nullptr};  // left unset up while another drive is in use
  };
  DriveOffer offers_[DRIVES_MAX]{};
  Component *servo_driver_{nullptr};
  /// Apply the saved name and access-point password (DESIGN.md section 9.3). Runs before Wi-Fi, mDNS, the API and
  /// BLE start, as they read the name when they do.
  void apply_network();
  /// The name the YAML gave (with its MAC suffix, if the build adds one), before any saved on the device.
  const std::string &hostname_build() const { return this->hostname_build_; }
  /// The build's access point has no password (the prebuilt binaries, on a clean flash). Set before Wi-Fi starts.
  bool ap_build_open() const { return this->ap_build_open_; }
  /// True if this build has Wi-Fi, so a network can be saved from the web app.
  static bool wifi_settable();
  /// Save a Wi-Fi network in ESPHome's own slot and join it now (the one the captive portal and Improv use). Main
  /// loop only. False in a build without Wi-Fi.
  bool set_wifi(const std::string &ssid, const std::string &password);

#if defined(USE_OTA_PASSWORD) && defined(USE_OTA_ENCRYPTION_PROVISIONED)
  /// A build whose API key is provisioned at run time (packages/api_key_on_device.yaml): ESPHome's OTA can then only
  /// offer encryption, so a password nobody knows, made at every boot, keeps plaintext OTA out. Encrypted (Noise)
  /// OTA clients use the API key and never see it.
  void set_ota(ESPHomeOTAComponent *ota) { this->ota_ = ota; }
#endif

  /// The API (Home Assistant) key: where it comes from, as /feeder/auth reports it. OFF: this build cannot have
  /// one set at run time and has none from the YAML (no API, or no encryption). Updated on the main loop.
  enum class ApiKey : uint8_t { NONE = 0, BUILD = 1, DEVICE = 2, OFF = 3 };
  ApiKey api_key_state() const { return this->api_key_state_.load(); }
  /// True if the API key can be set and cleared at run time (`api: encryption: {}` with no `key:`).
  static bool api_key_settable();
  /// Save an API key (activated after about 100 ms; Home Assistant reconnects with it), or clear it with nullptr.
  /// Main loop only. False if the build cannot or the save failed.
  bool set_api_key(const uint8_t *key);

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
  /// A firmware image was written and checked (POST /feeder/firmware): restart into it half a second from now (so the
  /// answer gets out), or once the tray stops if it is moving. Safe to call from the web server's task.
  void restart_after_upload() { this->restart_at_.store((millis() + 500) | 1); }
  /// Run `f` on the main loop (safe to call from the web server's task).
  void run_on_main_loop(std::function<void()> &&f) { this->defer(std::move(f)); }

  void setup() override;
  void loop() override {
    LockGuard guard(this->password_lock_);
    this->core_loop();
    this->update_api_key_state_();
    this->restart_if_due_();
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
  void hal_save_tuning(const Tuning &tuning, bool flush) override;
  void hal_save_motor_pins(const MotorPins &pins) override;
  void hal_apply_timezone(const TzRules *rules) override;
  void hal_apply_ntp(const std::vector<std::string> *servers) override;
  void hal_log(LogLevel level, const char *message) override;
  void hal_random(uint8_t *out, size_t n) override;

  void update_api_key_state_();
  void restart_if_due_();
  /// The Device record as saved, read before setup() (for the pin and network loaders); defaults if none.
  static Device load_device_early_();

  output::FloatOutput *output_{nullptr};
  std::atomic<ApiKey> api_key_state_{ApiKey::OFF};  // read by the web server's task
  std::atomic<uint32_t> restart_at_{0};               // millis() of a restart after a firmware upload; 0: none
#if defined(USE_OTA_PASSWORD) && defined(USE_OTA_ENCRYPTION_PROVISIONED)
  ESPHomeOTAComponent *ota_{nullptr};
#endif
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
  ESPPreferenceObject pref_tuning_;
  ESPPreferenceObject pref_motor_pins_;
  MotorPinUser *motor_user_{nullptr};
  std::string hostname_build_;
  bool ap_build_open_{false};
  const char *ntp_build_[NTP_SERVERS_MAX]{};  // the build's NTP servers, as the sntp component gave them
  Mutex password_lock_;
#ifdef USE_WEBSERVER
  web_server::WebServer *web_server_{nullptr};
  std::vector<std::string> protected_;
#endif
#ifdef USE_TIME_TIMEZONE
  time::ParsedTimezone build_tz_{};  // the timezone in use at boot, before any set on the device
#endif
};

/// Applies the saved pins before the drivers start (LEDC, the LED strip, GPIO binary sensors: HARDWARE priority), and
/// the saved name and access-point password before Wi-Fi starts: set up ahead of everything, at BUS priority. The
/// preferences are ready by then (ESPHome opens them before any setup()).
class PinLoader : public Component {
 public:
  explicit PinLoader(PrecisionFeeder *feeder) : feeder_(feeder) {}
  void setup() override {
    this->feeder_->apply_drive();            // first: which drive this boot uses, and its drivers
    this->feeder_->apply_motor_pins_early();  // then that drive's pins (none for the servo)
    this->feeder_->apply_pins();
    this->feeder_->apply_network();
  }
  float get_setup_priority() const override { return setup_priority::BUS; }

 protected:
  PrecisionFeeder *feeder_;
};

}  // namespace precision_feeder
}  // namespace esphome
