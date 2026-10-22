// Precision Feeder: the direct-drive disc on the device (n20_driver.h).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "n20_driver.h"

#include <cstring>

#include <driver/gpio.h>
#include <driver/ledc.h>

#include "esphome/core/log.h"

namespace esphome {
namespace precision_feeder {

static const char *const TAG = "precision_feeder.n20";

// LEDC: the last timer and the last two channels, away from the ones ESPHome's ledc outputs take first.
static constexpr ledc_mode_t PWM_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_timer_t PWM_TIMER = LEDC_TIMER_3;
static constexpr ledc_channel_t CH_IN1 = LEDC_CHANNEL_6;
static constexpr ledc_channel_t CH_IN2 = LEDC_CHANNEL_7;
static constexpr int PCNT_LIMIT = 30000;          // the unit's range; it accumulates beyond it
static constexpr int64_t WATCHDOG_US = 30000;      // the control timer silent this long while driving: STBY low

void N20Driver::set_timing_ms(const char *field, uint32_t value) {
  N20Timing &t = this->timing_;
  struct Field {
    const char *name;
    uint32_t *u32;
    uint8_t *u8;
    int32_t *i32;
  };
  const Field fields[] = {
      {"start_ms", &t.start_ms, nullptr, nullptr},
      {"first_brake_ms", &t.first_brake_ms, nullptr, nullptr},
      {"creep_brake_ms", &t.creep_brake_ms, nullptr, nullptr},
      {"final_brake_ms", &t.final_brake_ms, nullptr, nullptr},
      {"settle_ms", &t.settle_ms, nullptr, nullptr},
      {"wiggle_first_brake_ms", &t.wiggle_first_brake_ms, nullptr, nullptr},
      {"wiggle_creep_brake_ms", &t.wiggle_creep_brake_ms, nullptr, nullptr},
      {"wiggle_turn_brake_ms", &t.wiggle_turn_brake_ms, nullptr, nullptr},
      {"stall_ms", &t.stall_ms, nullptr, nullptr},
      {"drive_stall_ms", &t.drive_stall_ms, nullptr, nullptr},
      {"timeout_base_ms", &t.timeout_base_ms, nullptr, nullptr},
      {"watchdog_ms", &t.watchdog_ms, nullptr, nullptr},
      {"direction_test_ms", &t.direction_test_ms, nullptr, nullptr},
      {"pulse_start_ms", nullptr, &t.pulse_start_ms, nullptr},
      {"pulse_min_ms", nullptr, &t.pulse_min_ms, nullptr},
      {"pulse_max_ms", nullptr, &t.pulse_max_ms, nullptr},
      {"wiggle_pulse_floor_ms", nullptr, &t.wiggle_pulse_floor_ms, nullptr},
      {"pulse_far_ticks", nullptr, nullptr, &t.pulse_far_ticks},
      {"wiggle_far_ticks", nullptr, nullptr, &t.wiggle_far_ticks},
      {"reverse_fault_ticks", nullptr, nullptr, &t.reverse_fault_ticks},
  };
  for (const Field &f : fields) {
    if (std::strcmp(f.name, field) != 0)
      continue;
    if (f.u32)
      *f.u32 = value;
    else if (f.u8)
      *f.u8 = static_cast<uint8_t>(value);
    else
      *f.i32 = static_cast<int32_t>(value);
    this->mech_.set_timing(t);
    return;
  }
}

// The names the motor pins go by (Pins In Use, the log); their order is set_pins()'s.
static const char *const PIN_NAMES[5] = {"IN1", "IN2", "STBY", "encoder A", "encoder B"};

void N20Driver::attach(PrecisionFeeder *feeder, bool with_servo, bool build_default) {
  this->feeder_ = feeder;
  // The YAML's pins are the build's; ones saved on the device win (D19). With the servo in the build too, this is a
  // second drive, chosen on the device; its pins are reserved only while it is the drive.
  const uint8_t pins[] = {this->in1_, this->in2_, this->stby_, this->enc_a_, this->enc_b_};
  feeder->offer_drive(&this->mech_, this, pins, sizeof(pins), with_servo, build_default);
  feeder->offer_motor_pins(this, pins, sizeof(pins), PIN_NAMES);
}

void N20Driver::standby_now_() {
  // A driver that was never set up (a single binary running the servo) owns no pins: touch none.
  if (!this->stby_set_up_)
    return;
  // Bridge off first, whatever else happens: STBY low, both inputs low.
  gpio_set_level(static_cast<gpio_num_t>(this->stby_), 0);
  if (this->ready_) {
    ledc_set_duty(PWM_MODE, CH_IN1, 0);
    ledc_update_duty(PWM_MODE, CH_IN1);
    ledc_set_duty(PWM_MODE, CH_IN2, 0);
    ledc_update_duty(PWM_MODE, CH_IN2);
  }
}

bool N20Driver::setup_pwm_() {
  ledc_timer_config_t timer;
  std::memset(&timer, 0, sizeof(timer));
  timer.speed_mode = PWM_MODE;
  timer.duty_resolution = LEDC_TIMER_8_BIT;
  timer.timer_num = PWM_TIMER;
  timer.freq_hz = this->pwm_hz_;
  timer.clk_cfg = LEDC_AUTO_CLK;
  if (ledc_timer_config(&timer) != ESP_OK)
    return false;
  for (const auto &io : {std::make_pair(CH_IN1, this->in1_), std::make_pair(CH_IN2, this->in2_)}) {
    ledc_channel_config_t ch;
    std::memset(&ch, 0, sizeof(ch));
    ch.gpio_num = io.second;
    ch.speed_mode = PWM_MODE;
    ch.channel = io.first;
    ch.intr_type = LEDC_INTR_DISABLE;
    ch.timer_sel = PWM_TIMER;
    ch.duty = 0;
    if (ledc_channel_config(&ch) != ESP_OK)
      return false;
  }
  return true;
}

bool N20Driver::setup_encoder_() {
  pcnt_unit_config_t unit;
  std::memset(&unit, 0, sizeof(unit));
  unit.high_limit = PCNT_LIMIT;
  unit.low_limit = -PCNT_LIMIT;
  unit.flags.accum_count = 1;  // keep counting past the limits
  if (pcnt_new_unit(&unit, &this->pcnt_) != ESP_OK)
    return false;
  pcnt_glitch_filter_config_t filter;
  std::memset(&filter, 0, sizeof(filter));
  filter.max_glitch_ns = 1000;  // about 5 k edges a second at 35 rpm: 200 us apart
  pcnt_unit_set_glitch_filter(this->pcnt_, &filter);
  // Quadrature x4: each channel counts both edges of one signal, its direction set by the other's level.
  pcnt_channel_handle_t a = nullptr, b = nullptr;
  pcnt_chan_config_t ca;
  std::memset(&ca, 0, sizeof(ca));
  ca.edge_gpio_num = this->enc_a_;
  ca.level_gpio_num = this->enc_b_;
  pcnt_chan_config_t cb;
  std::memset(&cb, 0, sizeof(cb));
  cb.edge_gpio_num = this->enc_b_;
  cb.level_gpio_num = this->enc_a_;
  if (pcnt_new_channel(this->pcnt_, &ca, &a) != ESP_OK || pcnt_new_channel(this->pcnt_, &cb, &b) != ESP_OK)
    return false;
  pcnt_channel_set_edge_action(a, PCNT_CHANNEL_EDGE_ACTION_DECREASE, PCNT_CHANNEL_EDGE_ACTION_INCREASE);
  pcnt_channel_set_level_action(a, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  pcnt_channel_set_edge_action(b, PCNT_CHANNEL_EDGE_ACTION_INCREASE, PCNT_CHANNEL_EDGE_ACTION_DECREASE);
  pcnt_channel_set_level_action(b, PCNT_CHANNEL_LEVEL_ACTION_KEEP, PCNT_CHANNEL_LEVEL_ACTION_INVERSE);
  pcnt_unit_add_watch_point(this->pcnt_, PCNT_LIMIT);
  pcnt_unit_add_watch_point(this->pcnt_, -PCNT_LIMIT);
  // The encoder is powered from 3V3 with open-collector style outputs: pull them up, as the bench sketch did.
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->enc_a_), GPIO_PULLUP_ONLY);
  gpio_set_pull_mode(static_cast<gpio_num_t>(this->enc_b_), GPIO_PULLUP_ONLY);
  return pcnt_unit_enable(this->pcnt_) == ESP_OK && pcnt_unit_clear_count(this->pcnt_) == ESP_OK &&
         pcnt_unit_start(this->pcnt_) == ESP_OK;
}

void N20Driver::setup() {
  // STBY low before anything else: the bridge stays off however the rest goes.
  const gpio_num_t stby = static_cast<gpio_num_t>(this->stby_);
  gpio_reset_pin(stby);
  gpio_set_direction(stby, GPIO_MODE_OUTPUT);
  gpio_set_level(stby, 0);
  this->stby_set_up_ = true;
  for (uint8_t pin : {this->in1_, this->in2_, this->stby_, this->enc_a_, this->enc_b_}) {
    if (check_pin(pin) == PinCheck::REFUSED) {
      ESP_LOGE(TAG, "GPIO%u cannot be used (%s): the motor stays off and nothing feeds", pin, pin_note(pin));
      return;
    }
  }
  if (!this->setup_pwm_()) {
    ESP_LOGE(TAG, "Bridge PWM (LEDC) could not be set up: the motor stays off and nothing feeds");
    return;
  }
  if (!this->setup_encoder_()) {
    ESP_LOGE(TAG, "Encoder (PCNT) could not be set up: the motor stays off and nothing feeds");
    return;
  }
  this->ready_ = true;
  esp_timer_create_args_t args;
  std::memset(&args, 0, sizeof(args));
  args.callback = &N20Driver::tick_;
  args.arg = this;
  args.dispatch_method = ESP_TIMER_TASK;
  args.name = "n20";
  if (esp_timer_create(&args, &this->timer_) != ESP_OK || esp_timer_start_periodic(this->timer_, 1000) != ESP_OK) {
    ESP_LOGE(TAG, "Motor control timer could not start: nothing feeds");
    this->ready_ = false;
    this->standby_now_();
  }
}

void N20Driver::control_() {
  const int64_t now_us = esp_timer_get_time();
  const int64_t last = this->last_step_us_.load();
  if (last != 0 && now_us - last > static_cast<int64_t>(this->max_gap_us_.load()))
    this->max_gap_us_.store(static_cast<uint32_t>(now_us - last));
  this->last_step_us_.store(now_us);
  int count = 0;
  pcnt_unit_get_count(this->pcnt_, &count);  // outside the lock: the mechanism takes it in control()
  const Bridge b = this->mech_.control(static_cast<uint32_t>(now_us / 1000), count);
  this->apply_(b);
}

void N20Driver::apply_(const Bridge &b) {
  if (b == this->applied_)
    return;
  uint32_t d1 = 0, d2 = 0;
  bool enable = true;
  switch (b.mode) {
    case BridgeMode::STANDBY:
      enable = false;
      break;
    case BridgeMode::COAST:
      break;
    case BridgeMode::FORWARD:
      d1 = b.duty;
      break;
    case BridgeMode::REVERSE:
      d2 = b.duty;
      break;
    case BridgeMode::BRAKE:
      d1 = d2 = 255;  // both inputs high: as the bench sketch's ledcWrite(255)
      break;
  }
  if (!enable)
    gpio_set_level(static_cast<gpio_num_t>(this->stby_), 0);
  ledc_set_duty(PWM_MODE, CH_IN1, d1);
  ledc_update_duty(PWM_MODE, CH_IN1);
  ledc_set_duty(PWM_MODE, CH_IN2, d2);
  ledc_update_duty(PWM_MODE, CH_IN2);
  if (enable)
    gpio_set_level(static_cast<gpio_num_t>(this->stby_), 1);
  this->applied_ = b;
  this->driving_.store(b.mode == BridgeMode::FORWARD || b.mode == BridgeMode::REVERSE);
}

void N20Driver::loop() {
  // The control timer must run while the motor is driven. If it has stopped, cut the bridge from here.
  if (this->ready_ && this->driving_.load() && esp_timer_get_time() - this->last_step_us_.load() > WATCHDOG_US) {
    gpio_set_level(static_cast<gpio_num_t>(this->stby_), 0);
    if (!this->watchdog_tripped_)
      ESP_LOGE(TAG, "Motor control timer stopped while driving: bridge off");
    this->watchdog_tripped_ = true;
  }
}

void N20Driver::dump_config() {
  ESP_LOGCONFIG(TAG, "Direct-drive disc (DRV8833 + N20):");
  ESP_LOGCONFIG(TAG, "  Slots: %u (%u portions per load)", this->mech_.slots(), this->mech_.capacity());
  ESP_LOGCONFIG(TAG, "  IN1 GPIO%u, IN2 GPIO%u, STBY GPIO%u, encoder A GPIO%u, B GPIO%u", this->in1_, this->in2_,
                this->stby_, this->enc_a_, this->enc_b_);
  ESP_LOGCONFIG(TAG, "  PWM %u Hz; control every 1 ms; ready: %s", (unsigned) this->pwm_hz_, YESNO(this->ready_));
}

}  // namespace precision_feeder
}  // namespace esphome
