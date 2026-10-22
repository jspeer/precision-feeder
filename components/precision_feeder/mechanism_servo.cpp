// Precision Feeder: the servo fan tray mechanism (mechanism_servo.h).
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "mechanism_servo.h"

#include <algorithm>

namespace esphome {
namespace precision_feeder {

float ServoMechanism::angle_for(uint8_t position) const {
  position = std::min<uint8_t>(position, SERVO_POSITIONS - 1);
  const float margin = (180.0f - this->sweep_) / 2.0f;
  const float pitch = this->sweep_ / SERVO_POSITIONS;
  float angle = margin + position * pitch + this->offsets_[position] / 10.0f;
  angle = std::min(180.0f, std::max(0.0f, angle));
  return this->invert_ ? 180.0f - angle : angle;
}

void ServoMechanism::feed_one(uint8_t from, uint32_t now_ms) {
  this->from_ = from;
  this->go_(from + 1);
  this->step_ = Step::TO_NEXT;
  this->step_start_ = now_ms;
}

void ServoMechanism::reload(uint32_t now_ms) {
  // Back to the closed slot, so nothing falls out while the tray is filled. The servo gets there on its own.
  this->restore(0, now_ms);
}

void ServoMechanism::restore(uint8_t position, uint32_t now_ms) {
  (void) now_ms;
  this->step_ = Step::IDLE;
  this->go_(position);
}

void ServoMechanism::calibration_show(uint8_t point, uint32_t now_ms) { this->restore(point, now_ms); }

MoveStatus ServoMechanism::poll(uint32_t now_ms) {
  if (this->step_ == Step::IDLE)
    return MoveStatus::DONE;
  if (now_ms - this->step_start_ < this->step_ms_)
    return MoveStatus::BUSY;
  switch (this->step_) {
    case Step::TO_NEXT:
      if (!this->wiggle_)
        break;
      // Step back and forward again to shake the food loose.
      this->go_(this->from_);
      this->step_ = Step::BACK;
      this->step_start_ = now_ms;
      return MoveStatus::BUSY;
    case Step::BACK:
      this->go_(this->from_ + 1);
      this->step_ = Step::FORWARD;
      this->step_start_ = now_ms;
      return MoveStatus::BUSY;
    default:
      break;
  }
  this->step_ = Step::IDLE;
  return MoveStatus::DONE;
}

void ServoMechanism::stop() {
  this->step_ = Step::IDLE;
  this->out_.servo_release();
}

}  // namespace precision_feeder
}  // namespace esphome
