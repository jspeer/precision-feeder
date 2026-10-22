// Precision Feeder: the direct-drive disc mechanism (mechanism_n20.h).
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#include "mechanism_n20.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace esphome {
namespace precision_feeder {

// ---------------------------------------------------------------------------------------------
// N20Motion: the bench sketch's motion (tests/reference/motor-test.cpp, v7)
// ---------------------------------------------------------------------------------------------

bool N20Motion::begin_(int32_t target, int32_t ticks, uint32_t now) {
  if (this->phase_ != Phase::IDLE)
    return false;
  const int32_t delta = target - ticks;
  if (delta == 0)
    return false;
  this->start_ticks_ = ticks;
  this->target_ = target;
  this->dir_ = delta > 0 ? 1 : -1;
  this->reached_ = false;
  this->open_ = false;
  // Enabled and coasting for start_ms, then drive (or brake and creep, for a move shorter than the braking distance).
  this->bridge_ = Bridge{BridgeMode::COAST, 0};
  this->duty_ = static_cast<uint8_t>(std::lround(this->power_ * 255.0 / 100.0));
  this->pulse_ms_ = this->t_.pulse_start_ms;
  this->pulse_done_ = false;
  this->creep_from_start_ = std::abs(delta) <= this->approach_;
  this->last_ticks_ = ticks;
  this->phase_ = Phase::STARTING;
  this->phase_since_ = now;
  this->move_started_ = now;
  const double expected_ms = this->full_speed_ > 0 ? std::abs(delta) / this->full_speed_ : 0;
  this->timeout_ms_ = this->t_.timeout_base_ms + static_cast<uint32_t>(3.0 * expected_ms);
  this->status_ = Status::BUSY;
  this->fault_ = "";
  return true;
}

bool N20Motion::start(int32_t target, int32_t ticks, uint32_t now) {
  if (this->wiggle_)
    return false;
  this->driven_since_edge_ = 0;
  return this->begin_(target, ticks, now);
}

bool N20Motion::start_wiggle(int32_t amplitude, uint8_t cycles, int32_t ticks, uint32_t now) {
  if (this->phase_ != Phase::IDLE || this->wiggle_ || amplitude <= 0 || cycles == 0)
    return false;
  this->wiggle_ = true;
  this->driven_since_edge_ = 0;  // and not again between its legs: a lost encoder gets one budget for the wiggle
  this->center_ = ticks;
  this->amplitude_ = amplitude;
  this->cycles_ = cycles;
  this->leg_ = 0;
  if (!this->begin_(ticks - amplitude, ticks, now)) {
    this->wiggle_ = false;
    return false;
  }
  return true;
}

bool N20Motion::start_open(int direction, uint32_t ms, int32_t ticks, uint32_t now) {
  if (this->phase_ != Phase::IDLE || this->wiggle_)
    return false;
  this->start_ticks_ = this->target_ = this->last_ticks_ = ticks;
  this->dir_ = direction >= 0 ? 1 : -1;
  this->duty_ = static_cast<uint8_t>(std::lround(this->power_ * 255.0 / 100.0));
  this->reached_ = true;  // nothing to reach: it ends as done unless stopped
  this->open_ = true;
  this->open_ms_ = ms;
  this->bridge_ = this->drive_();
  this->phase_ = Phase::OPEN_DRIVE;
  this->phase_since_ = now;
  this->status_ = Status::BUSY;
  this->fault_ = "";
  return true;
}

void N20Motion::brake_(uint32_t now, bool reached, const char *fault) {
  // Both inputs high: dynamic brake; released after a bounded time.
  this->reached_ = reached;
  if (fault != nullptr && fault[0] != '\0')
    this->fault_ = fault;
  this->bridge_ = Bridge{BridgeMode::BRAKE, 0};
  this->phase_ = Phase::BRAKING;
  this->phase_since_ = now;
}

void N20Motion::stop(uint32_t now) {
  if (this->phase_ == Phase::IDLE) {
    this->bridge_ = Bridge{BridgeMode::STANDBY, 0};
    return;
  }
  // As the sketch's stop: brake now, whatever the phase (a stop while braking starts the brake time again).
  this->fault_ = "";
  this->brake_(now, false, "stopped");
}

bool N20Motion::advance_wiggle_(uint32_t now, int32_t ticks) {
  // The extremes alternate (- first), then the last leg returns to the center.
  while (++this->leg_ <= 2 * this->cycles_) {
    const int32_t offset = this->leg_ == 2 * this->cycles_ ? 0 : ((this->leg_ & 1) ? this->amplitude_ : -this->amplitude_);
    if (this->begin_(this->center_ + offset, ticks, now))
      return true;
  }
  this->wiggle_ = false;
  return false;
}

void N20Motion::finish_(uint32_t now, int32_t ticks) {
  this->phase_ = Phase::IDLE;
  this->last_move_ms_ = now - this->move_started_;
  if (!this->reached_) {
    // Stopped or faulted: the rest of a wiggle is cancelled; the caller decides what the position is.
    this->wiggle_ = false;
    this->status_ = Status::FAULT;
    if (this->fault_[0] == '\0')
      this->fault_ = "stopped";
    return;
  }
  if (this->wiggle_ && this->advance_wiggle_(now, ticks))
    return;
  this->status_ = Status::DONE;
}

uint32_t N20Motion::feedback_window_ms_() const {
  if (this->feedback_ticks_ <= 0 || this->full_speed_ <= 0)
    return this->t_.drive_stall_ms;
  const uint32_t ms = static_cast<uint32_t>(std::lround(this->feedback_ticks_ / this->full_speed_));
  return std::min<uint32_t>(std::max<uint32_t>(ms, FEEDBACK_MIN_MS), this->t_.drive_stall_ms);
}

Bridge N20Motion::step(uint32_t now, int32_t ticks) {
  const bool driving = this->bridge_.mode == BridgeMode::FORWARD || this->bridge_.mode == BridgeMode::REVERSE;
  const bool late = this->stepped_ && driving && now - this->last_step_ > this->t_.watchdog_ms;
  // Powered time since the last encoder edge, in every closed-loop phase (an open-loop push is bounded by its time).
  if (this->stepped_ && driving && this->phase_ != Phase::OPEN_DRIVE)
    this->driven_since_edge_ += now - this->last_step_;
  this->last_step_ = now;
  this->stepped_ = true;
  if (late) {
    // Driving with nobody watching the encoder: stop before anything else.
    this->brake_(now, false, "the control loop ran late");
    return this->bridge_;
  }
  switch (this->phase_) {
    case Phase::IDLE:
      break;
    case Phase::STARTING:
      if (now - this->phase_since_ < this->t_.start_ms)
        break;
      this->began_ = this->last_edge_ = this->phase_since_ = now;
      if (this->creep_from_start_) {
        this->bridge_ = Bridge{BridgeMode::BRAKE, 0};
        this->phase_ = Phase::CREEP_WAIT;
      } else {
        this->bridge_ = this->drive_();
        this->phase_ = Phase::MOVING;
      }
      break;
    case Phase::OPEN_DRIVE:
      if (now - this->phase_since_ >= this->open_ms_)
        this->brake_(now, true, "");
      break;
    case Phase::MOVING:
    case Phase::CREEP_WAIT:
    case Phase::CREEP_DRIVE: {
      if (ticks != this->last_ticks_) {
        this->last_ticks_ = ticks;
        this->last_edge_ = now;
        this->driven_since_edge_ = 0;
      }
      const int64_t remaining = static_cast<int64_t>(this->target_ - ticks) * this->dir_;
      const uint32_t stall = this->phase_ == Phase::MOVING ? this->t_.drive_stall_ms : this->t_.stall_ms;
      if (static_cast<int64_t>(ticks - this->start_ticks_) * this->dir_ < -this->t_.reverse_fault_ticks) {
        this->brake_(now, false, "the encoder counted the wrong way");
      } else if (remaining <= 0) {
        this->brake_(now, true, "");
      } else if (now - this->began_ >= this->timeout_ms_) {
        this->brake_(now, false, "the move took too long");
      } else if (now - this->last_edge_ >= stall || this->driven_since_edge_ >= this->feedback_window_ms_()) {
        // No edge for the stall window, or driven for longer than a share of a slot takes at full speed without one
        // (creep pulses grow while nothing is counted: an unplugged encoder must not let them carry the disc on).
        this->brake_(now, false, "no encoder motion (stalled, or the encoder is not counting)");
      } else if (this->phase_ == Phase::MOVING) {
        if (remaining <= this->approach_) {
          this->bridge_ = Bridge{BridgeMode::BRAKE, 0};
          this->phase_ = Phase::CREEP_WAIT;
          this->phase_since_ = now;
          this->pulse_done_ = false;
        }
      } else if (this->phase_ == Phase::CREEP_WAIT) {
        const bool fast = this->fast_wiggle_();
        const uint32_t wait = fast ? (this->pulse_done_ ? this->t_.wiggle_creep_brake_ms : this->t_.wiggle_first_brake_ms)
                                   : (this->pulse_done_ ? this->t_.creep_brake_ms : this->t_.first_brake_ms);
        if (now - this->phase_since_ >= wait) {
          // A pulse that moved nothing was too short for the static friction; one that moved far, too long.
          if (this->pulse_done_) {
            if (ticks == this->pulse_start_)
              this->pulse_ms_ = std::min<uint32_t>(this->pulse_ms_ + 1, this->t_.pulse_max_ms);
            else if (std::abs(ticks - this->pulse_start_) > this->t_.pulse_far_ticks)
              this->pulse_ms_ = std::max<uint32_t>(this->t_.pulse_min_ms, this->pulse_ms_ - 1);
          }
          this->drive_pulse_ms_ = fast && remaining > this->t_.wiggle_far_ticks
                                      ? std::max<uint32_t>(this->pulse_ms_, this->t_.wiggle_pulse_floor_ms)
                                      : this->pulse_ms_;
          this->pulse_start_ = ticks;
          this->pulse_done_ = false;
          this->bridge_ = this->drive_();
          this->phase_ = Phase::CREEP_DRIVE;
          this->phase_since_ = now;
        }
      } else if (now - this->phase_since_ >= this->drive_pulse_ms_) {
        this->bridge_ = Bridge{BridgeMode::BRAKE, 0};
        this->phase_ = Phase::CREEP_WAIT;
        this->phase_since_ = now;
        this->pulse_done_ = true;
      }
      break;
    }
    case Phase::BRAKING:
    case Phase::SETTLING: {
      // Beyond the sketch: a move that ends far past its target (a brake that did not hold) is a fault.
      if (this->reached_ && !this->open_ && this->travel_limit_ > 0 &&
          static_cast<int64_t>(ticks - this->target_) * this->dir_ > this->travel_limit_) {
        this->reached_ = false;
        this->fault_ = "it ran past its target";
      }
      if (this->phase_ == Phase::SETTLING) {
        if (now - this->phase_since_ >= this->t_.settle_ms)
          this->finish_(now, ticks);
        break;
      }
      const bool intermediate = this->wiggle_ && this->reached_ && this->leg_ < 2 * this->cycles_;
      if (intermediate && now - this->phase_since_ >= this->t_.wiggle_turn_brake_ms) {
        this->finish_(now, ticks);
      } else if (!intermediate && now - this->phase_since_ >= this->t_.final_brake_ms) {
        this->bridge_ = Bridge{BridgeMode::STANDBY, 0};
        this->phase_ = Phase::SETTLING;
        this->phase_since_ = now;
      }
      break;
    }
  }
  return this->bridge_;
}

// ---------------------------------------------------------------------------------------------
// N20Mechanism
// ---------------------------------------------------------------------------------------------

static const TuningParam N20_PARAMS[N20_TUNING_COUNT] = {
    // Measured by the calibration (one or more full turns); the bench sketch's estimate is the default.
    {"ticks_per_turn", "Ticks Per Turn", "ticks", 1000, 100000, 8344, 1},
    {"drive_power", "Drive Power", "%", 20, 100, 100, 1},
    // Where it stops driving and starts braking and creeping, before the target.
    {"braking_distance", "Braking Distance", "ticks", 10, 2000, 160, 1},
    {"wiggle_amplitude", "Wiggle Amplitude", "°", 1, 50, 20, 10},
    {"wiggle_cycles", "Wiggle Cycles", "", 1, 10, 4, 1},
    // Shifts every slot together (the calibration's trim): at most half a 24-degree slot either way.
    {"trim", "Slot Trim", "°", -120, 120, 0, 10},
    {"calibration_turns", "Calibration Turns", "", 1, 10, 1, 1},
    {"reverse_motor", "Reverse Motor", "", 0, 1, 0, 1},
    {"reverse_encoder", "Reverse Encoder", "", 0, 1, 0, 1},
    // Slots on the disc, all open, one over the chute (portions per load: slots - 1). The default is the build's.
    {"disc_slots", "Disc Slots", "", N20Mechanism::SLOTS_MIN, N20Mechanism::SLOTS_MAX, 15, 1},
};

N20Mechanism::N20Mechanism(uint8_t slots) : slots_(std::min<uint8_t>(std::max<uint8_t>(slots, SLOTS_MIN), SLOTS_MAX)) {
  for (uint8_t i = 0; i < N20_TUNING_COUNT; i++) {
    this->params_[i] = N20_PARAMS[i];
    this->value_[i] = N20_PARAMS[i].def;
  }
  this->params_[N20_SLOTS].def = this->value_[N20_SLOTS] = this->slots_;
  this->configure_motion_();
}

const TuningParam *N20Mechanism::tuning_param(uint8_t index) const {
  return index < N20_TUNING_COUNT ? &this->params_[index] : nullptr;
}

void N20Mechanism::apply_tuning(uint8_t index, int32_t value) {
  Guard g(this->lock_);
  if (index >= N20_TUNING_COUNT)
    return;
  this->value_[index] = value;
  if (index == N20_SLOTS && value != this->slots_) {
    // Another slot count: the grid starts again from where the disc is, and which slots hold food is no longer known
    // (calibration mode is on: the core marks the position unknown when it ends).
    this->slots_ = static_cast<uint8_t>(value);
    this->origin_ = this->ticks_ - this->degrees_to_ticks_(this->value_[N20_TRIM]);
    this->k_ = 0;
    this->cal_moved_ = true;
  }
  this->configure_motion_();
}

void N20Mechanism::configure_motion_() {
  const int32_t turn = this->value_[N20_TICKS_PER_TURN];
  this->motion_.set_power(static_cast<uint8_t>(this->value_[N20_POWER]));
  this->motion_.set_approach_ticks(this->value_[N20_BRAKING_DISTANCE]);
  this->motion_.set_travel_limit(turn / this->slots_ / 2);  // half a slot
  this->motion_.set_full_speed(turn * RATED_RPM / 60000.0);
  this->motion_.set_feedback_ticks(turn * 3 / (4 * this->slots_));  // three quarters of a slot
}

int32_t N20Mechanism::degrees_to_ticks_(int32_t tenths) const {
  return static_cast<int32_t>(std::lround(tenths * static_cast<double>(this->value_[N20_TICKS_PER_TURN]) / 3600.0));
}

int32_t N20Mechanism::slot_target_(int32_t k) const {
  // From the origin each time, never from where the last move stopped, so rounding and stopping errors never add up.
  const int64_t n = static_cast<int64_t>(k) * this->value_[N20_TICKS_PER_TURN];
  const int64_t step = (2 * n + this->slots_) / (2 * this->slots_);  // rounded (k >= 0)
  return static_cast<int32_t>(this->origin_ + step) + this->degrees_to_ticks_(this->value_[N20_TRIM]);
}

bool N20Mechanism::start_move_(int32_t target, uint32_t now) { return this->motion_.start(target, this->ticks_, now); }

Bridge N20Mechanism::control(uint32_t now_ms, int32_t raw_ticks) {
  Guard g(this->lock_);
  this->ticks_ = this->value_[N20_REVERSE_ENCODER] ? -raw_ticks : raw_ticks;
  this->now_ = now_ms;
  this->have_ticks_ = true;
  this->motion_.step(now_ms, this->ticks_);
  if (this->job_ != Job::NONE && !this->motion_.busy())
    this->job_done_(now_ms);  // may start the next move of the job
  Bridge b = this->motion_.bridge();
  if (this->value_[N20_REVERSE_MOTOR]) {
    if (b.mode == BridgeMode::FORWARD)
      b.mode = BridgeMode::REVERSE;
    else if (b.mode == BridgeMode::REVERSE)
      b.mode = BridgeMode::FORWARD;
  }
  return b;
}

void N20Mechanism::job_fault_() {
  this->fault_ = this->motion_.fault();
  this->result_ = MoveStatus::FAULT;
  if (this->cal_ != Cal::OFF || this->job_ >= Job::CAL_MOVE)
    this->cal_fault_ = this->fault_;
  this->job_ = Job::NONE;
}

void N20Mechanism::job_done_(uint32_t now) {
  if (this->motion_.status() == N20Motion::Status::FAULT) {
    this->job_fault_();
    return;
  }
  const int32_t amplitude = this->degrees_to_ticks_(this->value_[N20_WIGGLE_AMPLITUDE]);
  const uint8_t cycles = static_cast<uint8_t>(this->value_[N20_WIGGLE_CYCLES]);
  switch (this->job_) {
    case Job::FEED_MOVE:
    case Job::CAL_SLOT_MOVE:
      if (this->job_ == Job::FEED_MOVE)
        this->last_feed_move_ms_ = this->motion_.last_move_ms();
      // The wiggle is part of positioning (D13): its last leg returns to the slot's centre.
      if (this->motion_.start_wiggle(amplitude, cycles, this->ticks_, now)) {
        this->job_ = this->job_ == Job::FEED_MOVE ? Job::FEED_WIGGLE : Job::CAL_SLOT_WIGGLE;
        return;
      }
      break;
    case Job::CAL_DIRECTION:
      this->direction_delta_ = this->ticks_ - this->direction_from_;
      this->direction_done_ = true;
      break;
    case Job::CAL_TURNS:
      this->cal_ = Cal::MEASURE;
      break;
    case Job::CAL_OVERSHOT_BACK:
      // Come forward again, so every stop is approached from the same side (gearbox backlash).
      if (this->start_move_(this->ticks_ + this->degrees_to_ticks_(40), now)) {
        this->job_ = Job::CAL_OVERSHOT_FORWARD;
        return;
      }
      break;
    default:
      break;
  }
  if (this->job_ == Job::FEED_MOVE || this->job_ == Job::FEED_WIGGLE) {
    this->k_ = this->next_k_;
    this->result_ = MoveStatus::DONE;
    this->last_feed_ms_ = now - this->feed_started_;
  } else if (this->job_ == Job::CAL_SLOT_MOVE || this->job_ == Job::CAL_SLOT_WIGGLE) {
    this->k_ = this->next_k_;
    this->checked_++;
    if (this->checked_ >= this->slots_)
      this->cal_ = Cal::DONE;
  }
  this->job_ = Job::NONE;
}

void N20Mechanism::feed_one(uint8_t from, uint32_t now_ms) {
  (void) from;  // the core keeps the portion count; the disc keeps its own slot count
  Guard g(this->lock_);
  this->fault_ = "";
  if (!this->have_ticks_) {
    this->result_ = MoveStatus::FAULT;
    this->fault_ = "the motor control is not running";
    return;
  }
  this->next_k_ = this->k_ + 1;
  if (!this->start_move_(this->slot_target_(this->next_k_), now_ms)) {
    this->result_ = MoveStatus::FAULT;
    this->fault_ = "the motor is busy";
    return;
  }
  this->result_ = MoveStatus::BUSY;
  this->job_ = Job::FEED_MOVE;
  this->feed_started_ = now_ms;
}

void N20Mechanism::restore(uint8_t position, uint32_t now_ms) {
  (void) position;
  (void) now_ms;
  // Nothing moves. Where the disc is now is a slot: the grid starts again from here (at boot, a declared position,
  // the end of calibration).
  Guard g(this->lock_);
  this->origin_ = this->ticks_ - this->degrees_to_ticks_(this->value_[N20_TRIM]);
  this->k_ = 0;
  this->cal_moved_ = false;
  this->cal_ = Cal::OFF;
  this->cal_fault_ = "";
}

MoveStatus N20Mechanism::poll(uint32_t now_ms) {
  (void) now_ms;
  Guard g(this->lock_);
  return this->result_;
}

void N20Mechanism::stop() {
  Guard g(this->lock_);
  this->motion_.stop(this->now_);
}

void N20Mechanism::queue_tuning_(uint8_t index, int32_t value) {
  if (this->pending_n_ < sizeof(this->pending_index_)) {
    this->pending_index_[this->pending_n_] = index;
    this->pending_value_[this->pending_n_] = value;
    this->pending_n_++;
  }
}

bool N20Mechanism::take_tuning_update(uint8_t &index, int32_t &value) {
  Guard g(this->lock_);
  if (this->pending_n_ == 0)
    return false;
  index = this->pending_index_[0];
  value = this->pending_value_[0];
  for (uint8_t i = 1; i < this->pending_n_; i++) {
    this->pending_index_[i - 1] = this->pending_index_[i];
    this->pending_value_[i - 1] = this->pending_value_[i];
  }
  this->pending_n_--;
  return true;
}

bool N20Mechanism::calibration_action(uint8_t action, int32_t arg, uint32_t now_ms) {
  Guard g(this->lock_);
  if (!this->have_ticks_ || this->job_ != Job::NONE || this->motion_.busy())
    return false;
  this->cal_fault_ = "";
  switch (action) {
    case N20_CAL_START:
      // The direction test: a short open-loop push forward, then the user says which way it went.
      this->cal_ = Cal::DIRECTION;
      this->direction_done_ = false;
      this->direction_flipped_ = false;
      this->checked_ = 0;
      this->measure_rejected_ = 0;
      this->direction_from_ = this->ticks_;
      this->trim_was_set_ = false;
      if (!this->motion_.start_open(1, this->motion_.timing().direction_test_ms, this->ticks_, now_ms))
        return false;
      this->cal_moved_ = true;
      this->job_ = Job::CAL_DIRECTION;
      return true;
    case N20_CAL_YES:
    case N20_CAL_NO: {
      if (this->cal_ == Cal::CHECK && action == N20_CAL_YES)
        return this->calibration_action_next_(now_ms);
      if (this->cal_ != Cal::DIRECTION || !this->direction_done_ || this->direction_flipped_ ||
          std::abs(this->direction_delta_) < DIRECTION_MIN_TICKS)
        return false;
      const bool feeding_way = action == N20_CAL_YES;
      // Forward must turn the feeding way, and the encoder must count up while it does. Reversing the motor also
      // reverses what the encoder sees.
      bool flip_motor = !feeding_way;
      bool flip_encoder = feeding_way ? this->direction_delta_ < 0 : this->direction_delta_ > 0;
      if (!flip_motor && !flip_encoder) {
        this->cal_ = Cal::ZERO;
        return true;
      }
      if (flip_motor)
        this->queue_tuning_(N20_REVERSE_MOTOR, this->value_[N20_REVERSE_MOTOR] ? 0 : 1);
      if (flip_encoder)
        this->queue_tuning_(N20_REVERSE_ENCODER, this->value_[N20_REVERSE_ENCODER] ? 0 : 1);
      this->direction_flipped_ = true;  // test again (Start) once the core has applied them
      return true;
    }
    case N20_CAL_NUDGE:
      // Forward only, so gearbox backlash never enters. Also for recovery, outside the guided steps.
      if (arg < 1 || arg > 100)
        return false;
      if (!this->start_move_(this->ticks_ + std::max<int32_t>(1, this->degrees_to_ticks_(arg)), now_ms))
        return false;
      this->cal_moved_ = true;
      this->job_ = Job::CAL_MOVE;
      return true;
    case N20_CAL_OVERSHOT:
      if (!this->start_move_(this->ticks_ - this->degrees_to_ticks_(50), now_ms))
        return false;
      this->cal_moved_ = true;
      this->job_ = Job::CAL_OVERSHOT_BACK;
      return true;
    case N20_CAL_ZERO:
      if (this->cal_ == Cal::OFF || this->cal_ == Cal::DIRECTION)
        return false;
      this->origin_ = this->ticks_ - this->degrees_to_ticks_(this->value_[N20_TRIM]);
      this->k_ = 0;
      this->cal_ = Cal::TURNS;
      return true;
    case N20_CAL_TURNS: {
      if (this->cal_ != Cal::TURNS)
        return false;
      const int32_t turns = this->value_[N20_CALIBRATION_TURNS];
      this->measure_turns_ = turns;
      if (!this->start_move_(this->slot_target_(turns * this->slots_), now_ms))
        return false;
      this->cal_moved_ = true;
      this->job_ = Job::CAL_TURNS;
      return true;
    }
    case N20_CAL_MEASURED: {
      if (this->cal_ != Cal::MEASURE || this->measure_turns_ < 1)
        return false;
      const int32_t start = this->origin_ + this->degrees_to_ticks_(this->value_[N20_TRIM]);
      const int32_t measured =
          static_cast<int32_t>(std::lround(static_cast<double>(this->ticks_ - start) / this->measure_turns_));
      const int32_t expected = this->value_[N20_TICKS_PER_TURN];
      const TuningParam &p = N20_PARAMS[N20_TICKS_PER_TURN];
      if (measured < p.min || measured > p.max || std::abs(measured - expected) * 5 > expected) {
        // More than 20% from the value it turned by: a missed slot or a slipping disc, not a calibration.
        this->measure_rejected_ = measured;
        this->cal_ = Cal::ZERO;
        return true;
      }
      this->value_[N20_TICKS_PER_TURN] = measured;  // used at once; the core saves it (take_tuning_update)
      this->configure_motion_();
      this->queue_tuning_(N20_TICKS_PER_TURN, measured);
      this->origin_ = this->ticks_ - this->degrees_to_ticks_(this->value_[N20_TRIM]);
      this->k_ = 0;
      this->checked_ = 0;
      this->cal_ = Cal::CHECK;
      return true;
    }
    case N20_CAL_NEXT:
      this->trim_was_set_ = false;
      return this->calibration_action_next_(now_ms);
    case N20_CAL_TRIM_HERE: {
      // The user nudged this slot until it was centred: the difference from where the grid put it is the trim, for
      // every slot alike. Forward nudges make it larger; Overshot (back, then forward) smaller.
      if (this->cal_ != Cal::CHECK || this->checked_ == 0)
        return false;
      const int32_t off_ticks = this->ticks_ - this->slot_target_(this->k_);
      const double tenths = off_ticks * 3600.0 / this->value_[N20_TICKS_PER_TURN];
      const TuningParam &p = N20_PARAMS[N20_TRIM];
      const int32_t trim = std::min(p.max, std::max(p.min, this->value_[N20_TRIM] + static_cast<int32_t>(std::lround(tenths))));
      this->value_[N20_TRIM] = trim;  // used at once; the core saves it (take_tuning_update)
      this->configure_motion_();
      this->queue_tuning_(N20_TRIM, trim);
      this->trimmed_ = trim;
      this->trim_was_set_ = true;
      return true;
    }
    default:
      return false;
  }
}

bool N20Mechanism::calibration_action_next_(uint32_t now_ms) {
  if (this->cal_ != Cal::CHECK)
    return false;
  this->next_k_ = this->k_ + 1;
  if (!this->start_move_(this->slot_target_(this->next_k_), now_ms))
    return false;
  this->cal_moved_ = true;
  this->job_ = Job::CAL_SLOT_MOVE;
  return true;
}

const char *N20Mechanism::calibration_text() const {
  Guard g(this->lock_);
  char *b = this->text_;
  const size_t n = sizeof(this->text_);
  if (this->job_ != Job::NONE || this->motion_.busy()) {
    std::snprintf(b, n, "Moving...");
  } else if (this->cal_fault_[0] != '\0') {
    std::snprintf(b, n, "Motor fault: %s. Press Start to begin again.", this->cal_fault_);
  } else {
    // Each under 255 characters (Home Assistant's limit), and each says which step, what to look at, which button.
    switch (this->cal_) {
      case Cal::OFF:
        std::snprintf(b, n, "Step 1 of 5: check Disc slots (%u) matches your disc, empty every slot, then press Start: "
                      "the disc gives a short push forward. (After a power cut: Nudge, then Declare.)", this->slots_);
        break;
      case Cal::DIRECTION:
        if (!this->direction_done_)
          std::snprintf(b, n, "Step 1 of 5: testing the direction...");
        else if (this->direction_flipped_)
          std::snprintf(b, n, "Step 1 of 5: direction reversed (Reverse Motor / Reverse Encoder). Press Start to "
                        "test once more.");
        else if (std::abs(this->direction_delta_) < DIRECTION_MIN_TICKS)
          std::snprintf(b, n, "Step 1 of 5: the encoder did not count (%ld ticks). Check its C1, C2 and 3V3 wires, "
                        "then press Start.", static_cast<long>(this->direction_delta_));
        else
          std::snprintf(b, n, "Step 1 of 5: did the disc just turn the way it should when it feeds? Press Yes or No.");
        break;
      case Cal::ZERO:
        if (this->measure_rejected_ != 0)
          std::snprintf(b, n, "Step 2 of 5: that turn measured %ld ticks, too far from %ld (a slipped disc?). Centre "
                        "a slot over the hole again, then press Zero.", static_cast<long>(this->measure_rejected_),
                        static_cast<long>(this->value_[N20_TICKS_PER_TURN]));
        else
          std::snprintf(b, n, "Step 2 of 5: centre one empty slot exactly over the hole with Nudge (forward only; "
                        "too far? Overshot backs up), then press Zero.");
        break;
      case Cal::TURNS:
        std::snprintf(b, n, "Step 3 of 5: press Run turns. The disc makes %ld full turn(s) and stops close to the same "
                      "slot.", static_cast<long>(this->value_[N20_CALIBRATION_TURNS]));
        break;
      case Cal::MEASURE:
        std::snprintf(b, n, "Step 3 of 5: nudge until that same slot is centred over the hole again (too far? "
                      "Overshot), then press Measured.");
        break;
      case Cal::CHECK:
        if (this->checked_ == 0)
          std::snprintf(b, n, "Step 4 of 5: %ld ticks per turn. Press Next slot: the disc moves one slot exactly as a "
                        "feed does, then you check it.", static_cast<long>(this->value_[N20_TICKS_PER_TURN]));
        else if (this->trim_was_set_)
          std::snprintf(b, n, "Step 4 of 5: trim set to %+.1f degrees; every slot moves with it. Press Next slot to "
                        "check the next one.", this->trimmed_ / 10.0);
        else
          std::snprintf(b, n, "Step 4 of 5, slot %u of %u: centred over the hole? Yes: Next slot. No: nudge it to the "
                        "centre (too far? Overshot), then press Trim here.", this->checked_, this->slots_);
        break;
      case Cal::DONE:
        std::snprintf(b, n, "Step 5 of 5: all %u slots checked. Turn Calibration mode off, fill every slot except the "
                      "one over the hole, then press Tray reload.", this->slots_);
        break;
    }
  }
  return b;
}

}  // namespace precision_feeder
}  // namespace esphome
