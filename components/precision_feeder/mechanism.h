// Precision Feeder: the mechanism interface. A mechanism is everything that turns "drop one portion" into motion:
// the servo fan tray (mechanism_servo.h) is the reference one; others follow AGENTS.md, "Adding a mechanism".
//
// The core (feeder_core.h) decides everything else: the feed guard, the schedule, the history, the tray position
// and the "unknown" state after an interrupted move. A mechanism only carries out one verb at a time, started by the
// core after the guard allowed it, and reports through poll() when it is done. It never starts a move by itself.
//
// No ESPHome dependencies: a mechanism reaches hardware only through hooks the adapter implements, so it can be
// tested on a computer.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>

namespace esphome {
namespace precision_feeder {

/// Progress of the move a mechanism was last asked for.
enum class MoveStatus : uint8_t {
  BUSY = 0,   // still moving
  DONE = 1,   // finished (also: nothing to do)
  FAULT = 2,  // could not finish; the motor is stopped and nobody knows where the tray is
};

/// One tuning value of a mechanism, as its table declares it. Values are integers in `scale` units (scale 10: tenths),
/// so the limits can be checked exactly. The core enforces min and max for every way of changing a value (Home
/// Assistant, the web app, loading from flash) and saves the values in the Tuning record.
struct TuningParam {
  const char *key;   // stable id, saved with the values' order (layout); never reuse one
  const char *name;  // as the entity shows it
  const char *unit;
  int32_t min;
  int32_t max;
  int32_t def;
  uint16_t scale;
};

class Mechanism {
 public:
  virtual ~Mechanism() = default;

  /// Short id: the Mechanism entity reports it and the web app picks its description by it ("servo").
  virtual const char *name() const = 0;
  /// Portions per load. The tray position runs from 0 (just loaded) to capacity() (empty); the core keeps it.
  virtual uint8_t capacity() const = 0;

  /// Start dropping one portion: from position `from` to from + 1, the wiggle (if any) included.
  virtual void feed_one(uint8_t from, uint32_t now_ms) = 0;
  /// Make the tray ready for loading; afterwards it is at position 0. A mechanism that needs no motion moves nothing.
  virtual void reload(uint32_t now_ms) = 0;
  /// Put or keep the tray at `position`: at boot, after a declared position, when calibration ends.
  virtual void restore(uint8_t position, uint32_t now_ms) = 0;
  /// The move started last: BUSY, DONE or FAULT. DONE when nothing was started.
  virtual MoveStatus poll(uint32_t now_ms) = 0;
  /// Abort any move now and let go of the motor.
  virtual void stop() = 0;
  /// Let go of the motor once the tray has been still for settle_ms() (servo: stop the PWM).
  virtual void release() = 0;
  virtual uint32_t settle_ms() const = 0;
  /// Why the last move failed, or "" if it did not.
  virtual const char *fault_text() const { return ""; }

  /// Calibration (calibration mode is on, so every feed is refused): the points a user picks and fine-tunes, for
  /// example the servo's seven slots. 0: this mechanism has none.
  virtual uint8_t calibration_points() const { return 0; }
  /// Move to a calibration point so it can be checked by eye.
  virtual void calibration_show(uint8_t point, uint32_t now_ms) {
    (void) point;
    (void) now_ms;
  }
  /// A step of the mechanism's own guided calibration (the direct-drive disc's, for example). The core calls it only
  /// in calibration mode; `action` and `arg` are the mechanism's own. False: not possible now (or no such action).
  virtual bool calibration_action(uint8_t action, int32_t arg, uint32_t now_ms) {
    (void) action;
    (void) arg;
    (void) now_ms;
    return false;
  }
  /// What the guided calibration expects next, for the user; "" without one.
  virtual const char *calibration_text() const { return ""; }
  /// A tuning value the calibration worked out (a measured count, a reversed direction): the core sets it through
  /// its tuning setter, which checks and saves it. False when there is none waiting.
  virtual bool take_tuning_update(uint8_t &index, int32_t &value) {
    (void) index;
    (void) value;
    return false;
  }
  /// True once the tray moved during calibration in a way restore() does not undo (a disc turned by hand or by the
  /// guided calibration): the core then marks the position unknown when calibration ends, so the tray is reloaded
  /// before anything is fed.
  virtual bool calibration_moved() const { return false; }

  /// The tuning table (TuningParam). `layout` numbers the table's order and meaning; change it whenever entries are
  /// added, removed or reordered, so values saved under the old layout are not read as the new one.
  virtual uint8_t tuning_count() const { return 0; }
  virtual uint8_t tuning_layout() const { return 0; }
  virtual const TuningParam *tuning_param(uint8_t index) const {
    (void) index;
    return nullptr;
  }
  /// Use a value. The core has already checked it against the table and only calls this while nothing moves.
  virtual void apply_tuning(uint8_t index, int32_t value) {
    (void) index;
    (void) value;
  }
};

}  // namespace precision_feeder
}  // namespace esphome
