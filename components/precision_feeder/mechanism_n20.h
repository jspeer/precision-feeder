// Precision Feeder: the direct-drive disc mechanism. A GA12-N20 gear motor with a quadrature encoder, behind one
// channel of a DRV8833, turns a disc of open slots directly (15 on the bench print, 24 degrees apart). One slot is
// always over the chute, so a load holds slots - 1 portions. It only ever turns forward, except inside a wiggle.
// Reload moves nothing. DESIGN.md section 3.3; the plan is docs/plans/drive-decoupling.md, stage 2.
//
// Two parts, both plain C++ (no ESPHome), so the host tests can run them:
// - N20Motion: closed-loop moves to an encoder count and the wiggle about it, written from the owner's bench sketch
//   (tests/reference/motor-test.cpp, v7) and checked against it on a simulated motor (tests/test_n20.cpp). Full
//   power until a braking distance before the target, then brake and creep with short adaptive pulses, a final brake,
//   release. Faults: encoder counting the wrong way, no encoder edge (stall), timeout; and, beyond the sketch, a
//   travel limit, a shorter stall window while driving at full power, and a watchdog on its own step rate.
// - N20Mechanism: the slot grid (slot k is at origin + k * ticks per turn / slots, so rounding never adds up), a
//   feed as one slot forward and the wiggle, the tuning table and the guided calibration (docs/CALIBRATION.md).
//
// The adapter calls control() about every millisecond from its own timer task, with the encoder count, and drives
// the bridge as it answers. Everything else (feed_one, poll, the calibration actions) comes from the main loop; the
// adapter's lock (N20Lock) keeps the two apart.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>

#include "mechanism.h"

namespace esphome {
namespace precision_feeder {

/// What the bridge should do. STANDBY: STBY low (the DRV8833 is off, the motor coasts). COAST: enabled, both inputs
/// low. FORWARD and REVERSE: one input at `duty`, the other low. BRAKE: both inputs high (dynamic brake).
enum class BridgeMode : uint8_t { STANDBY = 0, COAST = 1, FORWARD = 2, REVERSE = 3, BRAKE = 4 };
struct Bridge {
  BridgeMode mode{BridgeMode::STANDBY};
  uint8_t duty{0};  // 0 to 255, for FORWARD and REVERSE
  bool operator==(const Bridge &o) const { return this->mode == o.mode && this->duty == o.duty; }
  bool operator!=(const Bridge &o) const { return !(*this == o); }
};

/// The motion's timings and limits. YAML-only (D10); the defaults are the bench sketch's v7 values.
struct N20Timing {
  uint32_t start_ms{2};               // bridge enabled, coasting, before a move drives
  uint32_t first_brake_ms{100};       // brake at the braking distance, before the first creep pulse
  uint32_t creep_brake_ms{15};        // brake between creep pulses
  uint32_t final_brake_ms{100};       // brake at the target, then release
  uint32_t settle_ms{100};            // released, still, before the move counts as done
  uint32_t wiggle_first_brake_ms{10};  // the same three inside a wiggle (its last leg uses the ones above)
  uint32_t wiggle_creep_brake_ms{8};
  uint32_t wiggle_turn_brake_ms{20};  // brake at each wiggle extreme before turning back
  uint8_t pulse_start_ms{3};          // creep pulse length, adapted between these
  uint8_t pulse_min_ms{2};
  uint8_t pulse_max_ms{12};
  int32_t pulse_far_ticks{6};         // a pulse that moved more than this is shortened
  uint8_t wiggle_pulse_floor_ms{5};   // inside a wiggle, pulses at least this long while far from the target
  int32_t wiggle_far_ticks{20};
  int32_t reverse_fault_ticks{32};    // counted this far the wrong way: fault
  uint32_t stall_ms{500};             // no encoder edge this long: fault
  uint32_t drive_stall_ms{50};        // the same while driving at full power (an encoder that stops counting must
                                      // not let the motor run for half a second)
  uint32_t timeout_base_ms{3000};     // a move's time limit: this, plus three times its expected time at full speed
  uint32_t watchdog_ms{25};           // control() not called for this long while driving: fault
  uint32_t direction_test_ms{120};    // the calibration's direction test drives this long, open loop
};

/// Closed-loop moves and the wiggle (the bench sketch's motion, as a state machine with no I/O of its own).
class N20Motion {
 public:
  enum class Status : uint8_t { IDLE, BUSY, DONE, FAULT };

  void set_timing(const N20Timing &timing) { this->t_ = timing; }
  const N20Timing &timing() const { return this->t_; }
  void set_power(uint8_t percent) { this->power_ = percent; }
  void set_approach_ticks(int32_t ticks) { this->approach_ = ticks; }
  /// Ticks a move may end past its target before it is a fault (0: no limit).
  void set_travel_limit(int32_t ticks) { this->travel_limit_ = ticks; }
  /// Encoder ticks per millisecond at full power: a move's time limit is timeout_base_ms plus three times its expected
  /// time at this speed (0: timeout_base_ms alone).
  void set_full_speed(double ticks_per_ms) { this->full_speed_ = ticks_per_ms; }
  /// The most the disc may turn with the bridge driving and no encoder edge, in ticks (a share of a slot): the time it
  /// takes at full speed, between FEEDBACK_MIN_MS and drive_stall_ms, bounds the powered time without feedback in
  /// every closed-loop phase, creep and wiggle included (0: drive_stall_ms).
  void set_feedback_ticks(int32_t ticks) { this->feedback_ticks_ = ticks; }
  static constexpr uint32_t FEEDBACK_MIN_MS = 25;

  /// Move to `target`. False if busy, or already there.
  bool start(int32_t target, int32_t ticks, uint32_t now);
  /// Wiggle about where it is: to center - amplitude, + amplitude, ... `cycles` times, then back to the center.
  bool start_wiggle(int32_t amplitude, uint8_t cycles, int32_t ticks, uint32_t now);
  /// Drive open loop for `ms`, then brake and settle (the calibration's direction test). No direction checks.
  bool start_open(int direction, uint32_t ms, int32_t ticks, uint32_t now);
  /// Call about every millisecond with the encoder count; returns what the bridge should do.
  Bridge step(uint32_t now, int32_t ticks);
  /// Brake now (an abort): the motion ends as a fault, "stopped".
  void stop(uint32_t now);

  Status status() const { return this->status_; }
  /// How long the last move took, from start() to done (ms): a bench diagnostic.
  uint32_t last_move_ms() const { return this->last_move_ms_; }
  bool busy() const { return this->status_ == Status::BUSY; }
  const char *fault() const { return this->fault_; }
  Bridge bridge() const { return this->bridge_; }
  int32_t target() const { return this->target_; }

 protected:
  enum class Phase : uint8_t { IDLE, STARTING, MOVING, CREEP_WAIT, CREEP_DRIVE, OPEN_DRIVE, BRAKING, SETTLING };
  bool begin_(int32_t target, int32_t ticks, uint32_t now);
  void brake_(uint32_t now, bool reached, const char *fault);
  void finish_(uint32_t now, int32_t ticks);
  bool advance_wiggle_(uint32_t now, int32_t ticks);
  bool fast_wiggle_() const { return this->wiggle_ && this->leg_ < 2 * this->cycles_; }
  uint32_t feedback_window_ms_() const;
  Bridge drive_() const {
    return Bridge{this->dir_ > 0 ? BridgeMode::FORWARD : BridgeMode::REVERSE, this->duty_};
  }

  N20Timing t_{};
  uint8_t power_{100};
  int32_t approach_{160};
  int32_t travel_limit_{0};
  double full_speed_{0};
  int32_t feedback_ticks_{0};
  uint32_t driven_since_edge_{0};  // ms the bridge drove since the last encoder edge (this move or wiggle)

  Status status_{Status::IDLE};
  const char *fault_{""};
  Phase phase_{Phase::IDLE};
  Bridge bridge_{};
  bool creep_from_start_{false};
  int32_t start_ticks_{0}, target_{0}, last_ticks_{0};
  int dir_{1};
  uint8_t duty_{255};
  uint32_t began_{0}, last_edge_{0}, phase_since_{0}, timeout_ms_{0}, last_step_{0};
  uint32_t move_started_{0}, last_move_ms_{0};
  bool stepped_{false};
  uint32_t pulse_ms_{3}, drive_pulse_ms_{3};
  int32_t pulse_start_{0};
  bool pulse_done_{false};
  bool reached_{false};
  bool open_{false};  // an open-loop push (start_open): no target to reach or pass
  uint32_t open_ms_{0};
  // The wiggle in progress.
  bool wiggle_{false};
  int32_t center_{0}, amplitude_{0};
  uint8_t cycles_{0}, leg_{0};
};

/// Mutual exclusion between the adapter's timer task (control()) and the main loop (everything else).
class N20Lock {
 public:
  virtual void lock() {}
  virtual void unlock() {}

 protected:
  ~N20Lock() = default;
};

// The tuning table's order (TuningParam): saved values are kept by this order, so it never changes without a new
// N20_TUNING_LAYOUT (mechanism.h, Mechanism::tuning_layout).
enum N20Tuning : uint8_t {
  N20_TICKS_PER_TURN = 0,
  N20_POWER,
  N20_BRAKING_DISTANCE,
  N20_WIGGLE_AMPLITUDE,
  N20_WIGGLE_CYCLES,
  N20_TRIM,
  N20_CALIBRATION_TURNS,
  N20_REVERSE_MOTOR,
  N20_REVERSE_ENCODER,
  N20_SLOTS,  // layout 2: added at the end, so layout 1's values are kept (tuning_layout_extends)
  N20_TUNING_COUNT,
};
constexpr uint8_t N20_TUNING_LAYOUT = 2;

/// The guided calibration's actions (Mechanism::calibration_action; DESIGN.md section 3.3).
enum N20Action : uint8_t {
  N20_CAL_START = 0,     // begin (or begin again): the direction test
  N20_CAL_YES = 1,       // the direction test turned the feeding way / this slot is centred
  N20_CAL_NO = 2,        // the direction test turned the wrong way
  N20_CAL_NUDGE = 3,     // forward by arg tenths of a degree (1 to 100); also for recovery
  N20_CAL_OVERSHOT = 4,  // back off about 5 degrees and come forward again
  N20_CAL_ZERO = 5,      // the slot over the hole now is the grid's origin
  N20_CAL_TURNS = 6,     // turn the calibration turns, to the same slot
  N20_CAL_MEASURED = 7,  // centred again after the turns: work out ticks per turn
  N20_CAL_NEXT = 8,      // move to the next slot exactly as a feed does
  N20_CAL_TRIM_HERE = 9, // in the check: the slot was nudged until centred; trim every slot by that much
};

class N20Mechanism : public Mechanism {
 public:
  static constexpr uint8_t SLOTS_MIN = 3, SLOTS_MAX = 60;
  static constexpr double RATED_RPM = 35.0;  // GA12-N20, 6 V, 35 rpm: only for the time limits
  static constexpr int32_t DIRECTION_MIN_TICKS = 20;  // the direction test must count at least this many

  /// `slots`: slots on the disc (all open; one over the chute) for a new device: the default of the Disc Slots tuning
  /// value, which the calibration can change. Clamped to SLOTS_MIN to SLOTS_MAX.
  explicit N20Mechanism(uint8_t slots = 15);
  void set_lock(N20Lock *lock) { this->lock_ = lock; }
  void set_timing(const N20Timing &timing) { this->motion_.set_timing(timing); }
  uint8_t slots() const { return this->slots_; }

  /// The adapter's timer task: the raw encoder count in, the bridge out (already reversed if the motor is).
  Bridge control(uint32_t now_ms, int32_t raw_ticks);
  /// The encoder count after Reverse Encoder (for the Encoder Ticks diagnostic).
  int32_t ticks() const { return this->ticks_; }
  /// Bench diagnostics: the last feed's slot move, and the whole feed (move and wiggle), in ms.
  uint32_t last_feed_move_ms() const { return this->last_feed_move_ms_; }
  uint32_t last_feed_ms() const { return this->last_feed_ms_; }

  const char *name() const override { return "n20-direct"; }
  uint8_t capacity() const override { return static_cast<uint8_t>(this->slots_ - 1); }
  void feed_one(uint8_t from, uint32_t now_ms) override;
  void reload(uint32_t now_ms) override { (void) now_ms; }  // the disc never turns back: nothing to do
  void restore(uint8_t position, uint32_t now_ms) override;
  MoveStatus poll(uint32_t now_ms) override;
  void stop() override;
  void release() override {}  // the motion releases the bridge itself after every move
  uint32_t settle_ms() const override { return 0; }
  bool moving() override;
  const char *fault_text() const override { return this->fault_; }

  bool calibration_action(uint8_t action, int32_t arg, uint32_t now_ms) override;
  const char *calibration_text() const override;
  bool take_tuning_update(uint8_t &index, int32_t &value) override;
  bool calibration_moved() const override { return this->cal_moved_; }

  uint8_t tuning_count() const override { return N20_TUNING_COUNT; }
  uint8_t tuning_layout() const override { return N20_TUNING_LAYOUT; }
  bool tuning_layout_extends(uint8_t older) const override { return older == 1; }
  /// Where the slots are (count, ticks per turn, trim) and which way is forward: never with food loaded.
  bool tuning_needs_calibration(uint8_t index) const override {
    return index == N20_TICKS_PER_TURN || index == N20_TRIM || index == N20_REVERSE_MOTOR ||
           index == N20_REVERSE_ENCODER || index == N20_SLOTS;
  }
  const TuningParam *tuning_param(uint8_t index) const override;
  void apply_tuning(uint8_t index, int32_t value) override;

 protected:
  // What the mechanism is doing (the motion does one move at a time; a feed and the calibration steps are several).
  enum class Job : uint8_t { NONE, FEED_MOVE, FEED_WIGGLE, CAL_MOVE, CAL_TURNS, CAL_SLOT_MOVE, CAL_SLOT_WIGGLE,
                             CAL_DIRECTION, CAL_OVERSHOT_BACK, CAL_OVERSHOT_FORWARD };
  // Where the guided calibration is.
  enum class Cal : uint8_t { OFF, DIRECTION, ZERO, TURNS, MEASURE, CHECK, DONE };
  struct Guard {
    explicit Guard(N20Lock *l) : l_(l) {
      if (l_)
        l_->lock();
    }
    ~Guard() {
      if (l_)
        l_->unlock();
    }
    N20Lock *l_;
  };

  int32_t slot_target_(int32_t k) const;
  int32_t degrees_to_ticks_(int32_t tenths) const;
  void configure_motion_();
  bool start_move_(int32_t target, uint32_t now);
  void job_done_(uint32_t now);
  void job_fault_();
  void queue_tuning_(uint8_t index, int32_t value);
  bool calibration_action_next_(uint32_t now_ms);

  N20Lock *lock_{nullptr};
  N20Motion motion_{};
  TuningParam params_[N20_TUNING_COUNT];  // the table, with Disc Slots' default from the build
  uint8_t slots_;
  int32_t value_[N20_TUNING_COUNT]{};
  int32_t ticks_{0};       // encoder count, after Reverse Encoder
  uint32_t now_{0};        // the last control() time
  bool have_ticks_{false};
  int32_t origin_{0};      // the count of slot 0
  int32_t k_{0};           // slots moved since the origin (it only grows)
  int32_t next_k_{0};      // the slot a feed or a check is moving to
  uint32_t feed_started_{0}, last_feed_move_ms_{0}, last_feed_ms_{0};
  Job job_{Job::NONE};
  MoveStatus result_{MoveStatus::DONE};
  const char *fault_{""};
  // The guided calibration.
  Cal cal_{Cal::OFF};
  const char *cal_fault_{""};      // a calibration move that failed, until the next action
  bool cal_moved_{false};
  int32_t direction_from_{0};
  int32_t direction_delta_{0};
  bool direction_done_{false};
  bool direction_flipped_{false};  // a reversal was queued: test again
  int32_t measure_turns_{0};
  int32_t measure_rejected_{0};    // a measurement too far from the expected one, or 0
  uint8_t checked_{0};             // slots stepped through in the check pass
  int32_t trimmed_{0};             // the trim Trim here just set (tenths of a degree), shown until the next action
  bool trim_was_set_{false};
  mutable char text_[256]{};       // calibration_text(), built from the state above when asked (under 255 characters:
                                   // Home Assistant's limit for a state)
  // Tuning values the calibration worked out, for the core to set (take_tuning_update).
  uint8_t pending_index_[3]{};
  int32_t pending_value_[3]{};
  uint8_t pending_n_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
