// Precision Feeder: the direct-drive disc on the device (see __init__.py). The mechanism itself is N20Mechanism
// (components/precision_feeder/mechanism_n20.h, host-tested); this class gives it a bridge, an encoder and a clock.
//
// - Bridge: IN1 and IN2 on two LEDC channels (8-bit, 20 kHz by default), STBY on a GPIO. STBY is driven low first
//   thing at setup, and on any shutdown.
// - Encoder: PCNT in quadrature (x4: both edges of both channels), with internal pull-ups, as the bench sketch's
//   interrupts counted. The direction does not matter: the calibration's direction test sets Reverse Encoder.
// - Control: an esp_timer every millisecond calls N20Mechanism::control() with the count and sets the bridge.
//   The main loop watches it: if it stops running while the motor is driven, STBY goes low (the mechanism's own
//   watchdog then faults the move at its next step).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <atomic>
#include <cstdint>

#include "esphome/core/component.h"
#include "esphome/components/precision_feeder/precision_feeder.h"
#include "esphome/components/precision_feeder/mechanism_n20.h"

#include <driver/pulse_cnt.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>

namespace esphome {
namespace precision_feeder {

class N20Driver : public Component, protected N20Lock, public MotorPinUser {
 public:
  explicit N20Driver(uint8_t slots) : mech_(slots) { this->mech_.set_lock(this); }

  void set_pins(uint8_t in1, uint8_t in2, uint8_t standby, uint8_t enc_a, uint8_t enc_b) {
    this->in1_ = in1;
    this->in2_ = in2;
    this->stby_ = standby;
    this->enc_a_ = enc_a;
    this->enc_b_ = enc_b;
  }
  void set_pwm_frequency(uint32_t hz) { this->pwm_hz_ = hz; }
  /// The pins to use, from the feeder at boot (the saved ones, or the build's), before setup (D19).
  void use_pins(const uint8_t *pins, uint8_t n) override {
    if (n >= 5)
      this->set_pins(pins[0], pins[1], pins[2], pins[3], pins[4]);
  }
  /// One of N20Timing's fields by name (from the YAML).
  void set_timing_ms(const char *field, uint32_t value);
  /// Register with the feeder: called before the feeder's setup, so core_start() already sees this mechanism.
  /// `with_servo`: the build also has the servo (a single binary: the drive is chosen on the device).
  void attach(PrecisionFeeder *feeder, bool with_servo, bool build_default);

  void setup() override;
  void loop() override;
  void dump_config() override;
  void on_shutdown() override { this->standby_now_(); }
  void on_safe_shutdown() override { this->standby_now_(); }
  // Before the feeder (HARDWARE priority, like the LEDC outputs): the bridge is off before anything can feed.
  float get_setup_priority() const override { return setup_priority::HARDWARE + 1.0f; }

  N20Mechanism &mechanism() { return this->mech_; }
  /// The encoder count (after Reverse Encoder), for the Encoder Ticks diagnostic.
  int32_t encoder_ticks() const { return this->mech_.ticks(); }
  /// The longest gap between two control steps since boot, in microseconds (the S2.0 timing check).
  uint32_t max_gap_us() const { return this->max_gap_us_.load(); }
  /// False if the hardware could not be set up (a refused pin, LEDC or PCNT); the feeder then never feeds.
  bool ready() const { return this->ready_; }

 protected:
  void lock() override { portENTER_CRITICAL(&this->mux_); }
  void unlock() override { portEXIT_CRITICAL(&this->mux_); }
  static void tick_(void *arg) { static_cast<N20Driver *>(arg)->control_(); }
  void control_();
  void apply_(const Bridge &b);
  void standby_now_();
  bool setup_pwm_();
  bool setup_encoder_();

  N20Mechanism mech_;
  N20Timing timing_{};
  PrecisionFeeder *feeder_{nullptr};
  uint8_t in1_{1}, in2_{2}, stby_{6}, enc_a_{4}, enc_b_{5};
  uint32_t pwm_hz_{20000};
  portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
  pcnt_unit_handle_t pcnt_{nullptr};
  esp_timer_handle_t timer_{nullptr};
  Bridge applied_{};
  bool ready_{false};
  bool stby_set_up_{false};  // setup() ran and owns STBY (not in a single binary running the servo)
  std::atomic<int64_t> last_step_us_{0};
  std::atomic<uint32_t> max_gap_us_{0};
  std::atomic<bool> driving_{false};
  bool watchdog_tripped_{false};
};

}  // namespace precision_feeder
}  // namespace esphome
