// Precision Feeder: tray control, feed guard and on-board scheduler.
//
// Part of the Precision Feeder ESPHome firmware (MIT licence).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
//
// The component owns everything with logic or safety consequences: the tray
// state machine, the feed guard, the scheduler and persistence. The YAML
// packages only expose it to Home Assistant through template entities.
#pragma once

#include <string>
#include <vector>

#include "esphome/core/component.h"
#include "esphome/core/preferences.h"
#include "esphome/components/output/float_output.h"
#include "esphome/components/time/real_time_clock.h"

#include "calendar.h"
#include "solar.h"

namespace esphome {
namespace precision_feeder {

constexpr uint8_t NUM_POSITIONS = 7;                  // tray positions 0..6 (0 = closed slot)
constexpr uint8_t MAX_PORTIONS = NUM_POSITIONS - 1;   // 6 open slots per load
constexpr uint8_t NUM_PROFILES = 3;
constexpr uint8_t EVENTS_PER_PROFILE = 4;
constexpr uint8_t MAX_ONESHOTS = 8;
constexpr uint8_t HISTORY_SIZE = 8;
constexpr uint8_t NO_MOVE = 0xFF;
constexpr int16_t INVALID_OFFSET = INT16_MIN;

/// Where a feed request came from.
enum class Source : uint8_t { SCHEDULE = 0, HOME_ASSISTANT = 1, BLE = 2, BUTTON = 3, ONE_SHOT = 4 };

/// Result of a feed request (values match the protocol in DESIGN.md).
enum class Result : uint8_t {
  OK = 0,
  BUSY = 1,
  TOO_SOON = 2,
  DAILY_CAP = 3,
  EMPTY = 4,
  UNKNOWN_POSITION = 5,
  CALIBRATING = 6,
  HOLD = 7,
  INVALID_ARG = 8,
  DUPLICATE = 9,
  TIME_INVALID = 10,
  PORTION_LIMIT = 11,
  NONE = 255,
};

enum class TrayState : uint8_t { LOADED = 0, EMPTY = 1, UNKNOWN = 2, CALIBRATING = 3, MOVING = 4 };

enum class Anchor : uint8_t { CLOCK = 0, SUNRISE = 1, SUNSET = 2 };

/// One scheduled feed inside a profile.
struct Event {
  uint8_t enabled;
  uint8_t anchor;       // Anchor
  int16_t offset_min;   // clock: minutes after midnight; solar: signed minutes from the anchor
  uint8_t portions;     // 1..6
  uint8_t days;         // weekday mask, bit0 = Sunday ... bit6 = Saturday
};

/// A schedule profile: cadence, fast days and up to four events.
struct Profile {
  uint8_t every_n;      // feed every N days (1 = every day)
  uint8_t fast_days;    // weekday mask of no-feed days
  uint32_t anchor_day;  // day number that counts as cycle day 0 (0 = set on first valid time)
  Event ev[EVENTS_PER_PROFILE];
};

/// A single dated feed (the original firmware's "calendar" entries).
struct OneShot {
  uint32_t epoch;
  uint8_t portions;
  uint8_t done;
};

/// User configuration, persisted to flash (debounced).
struct Settings {
  uint32_t version;
  Profile profiles[NUM_PROFILES];
  OneShot oneshots[MAX_ONESHOTS];
  int16_t cal[NUM_POSITIONS];  // per-position calibration offsets, tenths of a degree
  float latitude;
  float longitude;
  uint16_t min_gap_s;          // minimum seconds between feed requests
  uint16_t portion_gap_s;      // seconds between portions of a multi-portion feed
  uint16_t reload_pause_min;   // scheduled feeding pause after "tray reload"
  uint16_t refill_interval_days;
  uint8_t daily_cap;           // max portions in any rolling 24 h
  uint8_t max_portions_cmd;    // max portions per manual/remote command
  uint8_t active_profile;
  uint8_t scheduler_enabled;
};

/// Runtime state that must survive a reboot (flushed to flash on every feed).
struct State {
  uint32_t version;
  uint32_t hold_until;                                    // epoch; 0 = no hold; UINT32_MAX = until cancelled
  uint32_t fired[NUM_PROFILES][EVENTS_PER_PROFILE];       // day number each event last fired for
  uint32_t last_feed_epoch;
  uint32_t hist_ts[HISTORY_SIZE];                         // feed history (epoch, 0 = time unknown)
  uint8_t hist_n[HISTORY_SIZE];                           // portions per history entry
  uint8_t hist_src[HISTORY_SIZE];
  uint8_t position;                                       // tray position 0..6
  uint8_t moving_to;                                      // NO_MOVE or the target of an unfinished move
  uint8_t unknown;                                        // 1 when the position cannot be trusted
  uint8_t last_src;
};

class PrecisionFeeder : public Component {
 public:
  // ---- configuration (set from Python) ----
  void set_output(output::FloatOutput *output) { this->output_ = output; }
  void set_time(time::RealTimeClock *time) { this->time_ = time; }
  void set_sweep(float degrees) { this->sweep_ = degrees; }
  void set_pulse_range(uint32_t min_us, uint32_t max_us) {
    this->min_pulse_us_ = min_us;
    this->max_pulse_us_ = max_us;
  }
  void set_invert(bool invert) { this->invert_ = invert; }
  void set_step_time(uint32_t ms) { this->step_ms_ = ms; }
  void set_wiggle(bool wiggle) { this->wiggle_ = wiggle; }
  void set_detach_delay(uint32_t ms) { this->detach_ms_ = ms; }

  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ---- commands ----
  /// Ask for a feed. Every source goes through the same guard.
  Result request_feed(uint8_t portions, Source source);
  /// The user has refilled the tray: return to position 0 and pause scheduled feeding briefly.
  void tray_reload();
  /// Abort any motion. An interrupted step leaves the position unknown.
  void stop();
  /// Tell the firmware where the tray really is (after UNKNOWN).
  void declare_position(uint8_t position);
  /// Hold scheduled feeding for N minutes (no upper limit). Needs a valid clock.
  Result set_hold_minutes(uint32_t minutes);
  /// Hold scheduled feeding until cancel_hold() is called. Needs no clock.
  void set_hold_indefinite();
  void cancel_hold();
  /// Treat today's already-passed events as done, and restart the every-N-days cycle today.
  void restart_cadence_today();

  // ---- calibration ----
  void set_calibration_mode(bool on);
  bool calibration_mode() const { return this->calibrating_; }
  void calibration_select(uint8_t position);
  uint8_t calibration_position() const { return this->cal_pos_; }
  void set_calibration_offset(int16_t tenths);
  int16_t calibration_offset() const { return this->cfg.cal[this->cal_pos_]; }

  // ---- schedule editing (the "editor" selects a profile and event) ----
  Settings cfg;
  uint8_t editor_profile{0};
  uint8_t editor_event{0};
  Profile &edit_profile() { return this->cfg.profiles[this->editor_profile]; }
  Event &edit_event() { return this->cfg.profiles[this->editor_profile].ev[this->editor_event]; }
  void cfg_changed();  // call after changing cfg; persists after a short delay and refreshes projections

  /// Change the editor event's anchor; the offset resets when switching between clock and solar.
  void set_event_anchor(uint8_t anchor);
  static std::string format_days(uint8_t mask);
  static bool parse_days(const std::string &text, uint8_t &mask);
  std::string event_time_text();
  bool set_event_time_text(const std::string &text);
  bool add_oneshot(const std::string &local_datetime, uint8_t portions);
  void clear_oneshots();

  // ---- status ----
  uint8_t position() const { return this->state_.position; }
  uint8_t remaining() const { return this->state_.position >= MAX_PORTIONS ? 0 : MAX_PORTIONS - this->state_.position; }
  TrayState tray_state() const;
  std::string tray_state_text() const;
  bool time_valid();
  uint16_t feeds_today();
  /// Whole days of feeding left in the tray under the active schedule; 255 = more than the planning horizon.
  uint8_t runway_days() const { return this->runway_days_; }
  bool schedule_oversubscribed() const { return this->oversubscribed_; }
  std::string next_event_text();
  std::string empty_on_text();
  std::string last_feed_text();
  std::string last_result_text() const;
  std::string faults_text();
  std::string schedule_summary();
  std::string recent_feeds_text();
  std::string oneshots_text();
  /// 0 ok, 1 time not valid, 2 tray empty, 3 position unknown, 4 moving, 5 calibrating, 6 hold or reload pause
  uint8_t led_code();
  bool hold_active();
  std::string hold_text();
  static const char *result_name(Result r);
  Result last_result() const { return this->last_result_; }

 protected:
  enum class Phase : uint8_t { IDLE, STEP_A, STEP_B, STEP_C, GAP, SETTLE };

  struct DayCtx {
    int32_t day_no;
    int wday;
    time_t midnight;
    int tz_min;
  };

  // persistence
  void load_();
  void save_settings_();
  void save_state_(bool flush);
  void defaults_();

  // servo
  float angle_for_(uint8_t position) const;
  void write_angle_(float angle);
  void detach_();

  // motion
  void start_next_portion_();
  void commit_step_();
  void run_motion_();
  void abort_motion_();
  bool reload_pause_now_() const;

  // guard
  Result check_guard_(uint8_t &portions, Source source);
  uint16_t portions_24h_(time_t now_epoch, bool time_ok) const;
  void record_history_(uint8_t portions, Source source);

  // scheduler
  void schedule_tick_();
  void fire_due_();
  DayCtx make_day_(int32_t day_no) const;
  time_t wall_to_epoch_(int32_t day_no, int minutes) const;
  bool resolve_minutes_(const Event &e, const DayCtx &d, int &minutes);
  bool day_active_(const Profile &p, const DayCtx &d) const;
  int64_t local_offset_s_(time_t epoch) const;
  bool sun_cached_(const DayCtx &d, double &rise, double &set);
  void on_time_valid_();
  void project_();
  std::string format_local_(time_t epoch, bool with_date = true);

  output::FloatOutput *output_{nullptr};
  time::RealTimeClock *time_{nullptr};
  float sweep_{175.0f};
  uint32_t min_pulse_us_{500};
  uint32_t max_pulse_us_{2400};
  bool invert_{false};
  uint32_t step_ms_{1000};
  bool wiggle_{true};
  uint32_t detach_ms_{1000};

  struct SunCache {
    int32_t day_no{INT32_MIN};
    float lat{0}, lon{0};
    int tz_min{0};
    bool ok{false};
    double rise{0}, set{0};
  };

  ESPPreferenceObject pref_settings_;
  ESPPreferenceObject pref_state_;
  State state_{};
  uint32_t hist_ms_[HISTORY_SIZE]{};  // millis() stamp per history entry (aging while the clock is unset)
  SunCache sun_cache_[3];
  uint8_t sun_cache_next_{0};

  Phase phase_{Phase::IDLE};
  uint32_t phase_start_{0};
  uint8_t queue_{0};
  bool calibrating_{false};
  uint8_t cal_pos_{0};

  Result last_result_{Result::NONE};
  uint32_t last_feed_ms_{0};
  bool have_last_feed_ms_{false};
  uint32_t reload_pause_until_ms_{0};
  bool reload_pause_active_{false};
  bool was_time_valid_{false};

  uint32_t last_tick_{0};
  uint32_t last_project_{0};
  bool project_dirty_{true};
  bool solar_fault_{false};
  uint8_t runway_days_{255};
  bool oversubscribed_{false};
  time_t next_event_epoch_{0};
  time_t empty_on_epoch_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
