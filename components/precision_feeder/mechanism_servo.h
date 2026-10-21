// Precision Feeder: the servo fan tray, the reference mechanism (DESIGN.md section 3).
//
// A hobby servo swings a 7-slot tray: slot 0 is closed (the idle and reload position), slots 1 to 6 are open, so a
// load holds 6 portions. Open loop: each move is given a fixed time. A feed steps to the next slot, back, and forward
// again (the wiggle). Its calibration (per-slot offsets and the pulse range) is in the Calibration record, which the
// core keeps, as it has since firmware 0.5.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>

#include "mechanism.h"

namespace esphome {
namespace precision_feeder {

constexpr uint8_t SERVO_POSITIONS = 7;                   // slots 0..6 (0 = closed slot)
constexpr uint8_t SERVO_CAPACITY = 6;   // open slots per load (packages/mechanism_servo.yaml repeats it)
static_assert(SERVO_CAPACITY == SERVO_POSITIONS - 1, "one closed slot, the rest open");

/// Where the servo's commands go; FeederCore implements it with its hal_servo_* hooks.
class ServoOutput {
 public:
  virtual void servo_write(float angle_degrees) = 0;
  virtual void servo_release() = 0;

 protected:
  ~ServoOutput() = default;
};

class ServoMechanism : public Mechanism {
 public:
  /// `offsets`: the per-slot calibration offsets (SERVO_POSITIONS values, tenths of a degree), owned by the core.
  ServoMechanism(ServoOutput &out, const int16_t *offsets) : out_(out), offsets_(offsets) {}

  void set_sweep(float degrees) { this->sweep_ = degrees; }
  void set_invert(bool invert) { this->invert_ = invert; }
  void set_step_time(uint32_t ms) { this->step_ms_ = ms; }
  void set_wiggle(bool wiggle) { this->wiggle_ = wiggle; }
  void set_detach_delay(uint32_t ms) { this->detach_ms_ = ms; }
  float sweep() const { return this->sweep_; }
  bool invert() const { return this->invert_; }
  uint32_t step_time() const { return this->step_ms_; }
  bool wiggle() const { return this->wiggle_; }

  /// The servo angle for a slot: margin + slot * pitch + its offset, within 0 to 180 (mirrored when inverted).
  float angle_for(uint8_t position) const;

  const char *name() const override { return "servo"; }
  uint8_t capacity() const override { return SERVO_CAPACITY; }
  void feed_one(uint8_t from, uint32_t now_ms) override;
  void reload(uint32_t now_ms) override;
  void restore(uint8_t position, uint32_t now_ms) override;
  MoveStatus poll(uint32_t now_ms) override;
  void stop() override;
  void release() override { this->out_.servo_release(); }
  uint32_t settle_ms() const override { return this->detach_ms_; }
  uint8_t calibration_points() const override { return SERVO_POSITIONS; }
  void calibration_show(uint8_t point, uint32_t now_ms) override;

 protected:
  // A feed: TO_NEXT (step to the next slot), then with the wiggle BACK (to the slot it came from) and FORWARD (to the
  // next slot again). Each step is given step_ms_.
  enum class Step : uint8_t { IDLE, TO_NEXT, BACK, FORWARD };
  void go_(uint8_t position) { this->out_.servo_write(this->angle_for(position)); }

  ServoOutput &out_;
  const int16_t *offsets_;
  float sweep_{175.0f};
  bool invert_{false};
  uint32_t step_ms_{1000};
  bool wiggle_{true};
  uint32_t detach_ms_{1000};
  Step step_{Step::IDLE};
  uint8_t from_{0};
  uint32_t step_start_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
