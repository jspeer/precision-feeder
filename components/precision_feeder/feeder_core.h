// Precision Feeder: tray state machine, feed guard, scheduler and status, as plain C++.
//
// No ESPHome dependencies: everything the logic needs from the platform (clock, servo, storage,
// logging) goes through the hal_* hooks below. The ESPHome component (precision_feeder.h) implements
// them for the real device; tests/test_core.cpp implements them with a simulated clock, so the logic
// can be tested on any computer.
//
// Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
// Designed and written with the assistance of an AI (Anthropic's Claude);
// reviewed by a human, but it may still contain mistakes.
#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "calendar.h"
#include "crypto.h"
#include "solar.h"
#include "tz_rules.h"

namespace esphome {
namespace precision_feeder {

constexpr uint8_t NUM_POSITIONS = 7;                 // tray positions 0..6 (0 = closed slot)
constexpr uint8_t MAX_PORTIONS = NUM_POSITIONS - 1;  // 6 open slots per load
constexpr uint8_t NUM_PROFILES = 3;
constexpr uint8_t EVENTS_PER_PROFILE = 4;
constexpr uint8_t MAX_ONESHOTS = 8;
constexpr uint8_t HISTORY_SIZE = 8;
constexpr uint8_t NO_MOVE = 0xFF;
constexpr uint32_t HOLD_FOREVER = UINT32_MAX;  // State::hold_until marker: until the user cancels it
// On a day with no sunrise or no sunset (polar night or midnight sun), sun events use these local times instead
// (before their offset), so the animals are still fed at a sensible hour (owner decision 2026-10-14).
constexpr int POLAR_SUNRISE_MIN = 6 * 60;
constexpr int POLAR_SUNSET_MIN = 18 * 60;
// Saved records. When a record's layout changes: bump its version, give it a new preference key in the adapter,
// keep the old layout below (namespace legacy) and migrate it in core_start(). Never just discard saved data.
constexpr uint32_t SETTINGS_VERSION = 4;       // 2: no late start; 3: servo pulse range; 4: calibration moved out
constexpr uint32_t CALIBRATION_VERSION = 1;
constexpr uint32_t STATE_VERSION = 2;          // 2: reload pause persisted
constexpr uint32_t DEVICE_VERSION = 1;
// Setting limits: the single source of truth. Every way of changing a setting (Home Assistant, BLE, loading from
// flash) goes through the setters below, which check against these. The YAML entities repeat them as min/max for
// their input boxes; tests/test_consistency.py checks that they agree.
constexpr int32_t MIN_GAP_S_LO = 10, MIN_GAP_S_HI = 3600;           // minimum seconds between feeds
constexpr int32_t PORTION_GAP_S_LO = 1, PORTION_GAP_S_HI = 60;      // seconds between portions of one feed
constexpr int32_t RELOAD_PAUSE_MIN_LO = 0, RELOAD_PAUSE_MIN_HI = 60;
constexpr int32_t REFILL_DAYS_LO = 1, REFILL_DAYS_HI = 30;
constexpr int32_t DAILY_CAP_LO = 1, DAILY_CAP_HI = 6;               // portions in any rolling 24 h
constexpr int32_t PORTIONS_CMD_LO = 1, PORTIONS_CMD_HI = 6;         // portions per manual / remote command
constexpr int16_t CAL_OFFSET_LO = -150, CAL_OFFSET_HI = 150;        // tenths of a degree
// Servo pulse range limits (microseconds for 0 and 180 degrees). Wide enough for servos that turn less than
// the nominal amount per microsecond; the firmware never commands more than about 155 degrees.
constexpr uint16_t PULSE_MIN_LO = 300;
constexpr uint16_t PULSE_MIN_HI = 1500;
constexpr uint16_t PULSE_MAX_LO = 1500;
constexpr uint16_t PULSE_MAX_HI = 3000;
constexpr uint16_t PULSE_SPAN_MIN = 500;  // max - min

// Device record (2.0): pins, names and credentials that a prebuilt binary sets at run time. Each field has a "use
// the build's value" setting, so a YAML build keeps following its YAML until something is changed on the device.
constexpr uint8_t PIN_BUILD = 0xFE;  // use the YAML substitution
constexpr uint8_t PIN_NONE = 0xFF;   // no button or no LED fitted
constexpr unsigned HOSTNAME_SIZE = 32;                 // ESPHome names are at most 31 characters
constexpr unsigned TIMEZONE_SIZE = TZ_TEXT_MAX + 1;    // POSIX TZ string and its terminator
constexpr unsigned AP_PASSWORD_SIZE = 64;              // WPA2: at most 63 characters
constexpr unsigned DEVICE_KEY_SIZE = 32;               // BLE key, as docs/PROTOCOL.md
constexpr unsigned PASSWORD_SALT_SIZE = 16;
constexpr unsigned PASSWORD_HASH_SIZE = 32;            // SHA-256
// Device::flags
constexpr uint8_t DEVICE_SETUP_DONE = 0x01;        // the setup wizard has finished
constexpr uint8_t DEVICE_BLE_GUARD = 0x02;         // BLE may change the guard limits (default on)
constexpr uint8_t DEVICE_BLE_CALIBRATION = 0x04;   // BLE may change the calibration (default on)
constexpr uint8_t DEVICE_FLAGS_KNOWN = DEVICE_SETUP_DONE | DEVICE_BLE_GUARD | DEVICE_BLE_CALIBRATION;

// Settings password (DESIGN.md section 10.1). The browser derives a key from the password with PBKDF2 and proves it
// knows the key with an HMAC over a one-time challenge, so the password never crosses plain HTTP.
constexpr uint8_t PASSWORD_SCHEME_NONE = 0;
constexpr uint8_t PASSWORD_SCHEME_PBKDF2 = 1;        // PBKDF2-HMAC-SHA256, PASSWORD_ITERATIONS rounds
constexpr uint32_t PASSWORD_ITERATIONS = 20000;
constexpr unsigned AUTH_NONCE_SIZE = 16;
constexpr unsigned AUTH_TAG_SIZE = 32;               // HMAC-SHA256
constexpr uint32_t AUTH_NONCE_TTL_MS = 60000;        // a challenge must be answered within a minute
constexpr uint32_t AUTH_WAIT_MIN_MS = 1000;          // wait after the first failure, doubling after each one
constexpr uint32_t AUTH_WAIT_MAX_MS = 60000;
constexpr uint32_t BOOT_HOLD_MS = 10000;             // hold BOOT this long to clear the settings password

/// The answer to a settings-password check.
enum class AuthResult : uint8_t {
  OK = 0,            // proven, or no password is set
  REFUSED = 1,       // wrong answer (a wait starts)
  WAIT = 2,          // too soon after a failure: not even checked
  NO_CHALLENGE = 3,  // no challenge outstanding, or it expired or was for another nonce
};

// Pins (firmware 2.0.0, DESIGN.md section 2.1): the ESP32-S3 GPIOs the servo, button and LED may use. Refused: not on
// the chip (22 to 25), the SPI flash and PSRAM (26 to 32), USB (19, 20) and the strapping pins (0, 3, 45, 46). Warned
// about but accepted: the octal flash or PSRAM of some modules (33 to 37; for example N8R8) and UART0 (43, 44: the boot
// ROM prints on TX at reset). packages/hardware.yaml offers exactly the pins not refused (tests/test_consistency.py).
constexpr uint8_t GPIO_MAX = 48;
constexpr uint8_t PINS_REFUSED[] = {0, 3, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 45, 46};
constexpr uint8_t PINS_WARNED[] = {33, 34, 35, 36, 37, 43, 44};

enum class PinCheck : uint8_t { OK = 0, WARN = 1, REFUSED = 2 };
enum class PinRole : uint8_t { SERVO = 0, BUTTON = 1, LED = 2 };

/// GPIO numbers, or PIN_NONE for no button or no LED.
struct Pins {
  uint8_t servo;
  uint8_t button;
  uint8_t led;
  bool operator==(const Pins &o) const { return servo == o.servo && button == o.button && led == o.led; }
  bool operator!=(const Pins &o) const { return !(*this == o); }
};

/// Board presets. BUILD: every pin as the build (YAML) sets it. CUSTOM: pins chosen one by one.
enum class Board : uint8_t { BUILD = 0, WAVESHARE_S3_ZERO = 1, DEVKITC_1 = 2, CUSTOM = 3 };
struct BoardPreset {
  Board board;
  const char *name;  // as the Board select shows it
  Pins pins;
};
// Waveshare ESP32-S3-Zero: the pins this firmware was built and bench-tested on (WS2812 on GPIO21). Espressif
// ESP32-S3-DevKitC-1: GPIO4 and GPIO5 are free on every module; the RGB LED is GPIO38 on v1.1 boards (v1.0: GPIO48,
// choose Custom).
constexpr BoardPreset BOARD_PRESETS[] = {
    {Board::WAVESHARE_S3_ZERO, "Waveshare ESP32-S3-Zero", {1, 2, 21}},
    {Board::DEVKITC_1, "ESP32-S3-DevKitC-1", {4, 5, 38}},
};

/// Whether the servo, button or LED may use this GPIO (see PINS_REFUSED and PINS_WARNED).
PinCheck check_pin(uint8_t gpio);
/// Why a pin is refused or warned about ("" for a pin that is fine).
const char *pin_note(uint8_t gpio);
/// "GPIO5", or "none".
std::string pin_text(uint8_t gpio);
/// Accepts "GPIO5", "5" and "none" (case does not matter). Does not check the pin.
bool parse_pin(const std::string &text, uint8_t &gpio);
const char *board_name(Board board);
bool parse_board(const std::string &text, Board &board);

/// Where a feed request came from.
// ESPHOME: a feed button of the device itself, pressed in Home Assistant (native API) or the web UI.
enum class Source : uint8_t { SCHEDULE = 0, ESPHOME = 1, BLE = 2, BUTTON = 3, ONE_SHOT = 4,
                              // A hand edit of the 24 h count (set_portions_24h). Only ever in the feed history:
                              // never a feed request, never sent over BLE.
                              ADJUSTMENT = 5 };

/// Result of a request (values match the BLE protocol, docs/PROTOCOL.md).
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
  IGNORED_NTP_ACTIVE = 12,  // a clock pushed over BLE was refused because network time is fresh
  BLE_TIME_DISABLED = 13,   // a clock pushed over BLE was refused by policy
  UNSUPPORTED = 14,         // unknown or reserved BLE opcode
  NOT_CALIBRATING = 15,     // the command needs calibration mode
  NONE = 255,
};

enum class TrayState : uint8_t { LOADED = 0, EMPTY = 1, UNKNOWN = 2, CALIBRATING = 3, MOVING = 4 };

enum class Anchor : uint8_t { CLOCK = 0, SUNRISE = 1, SUNSET = 2 };

/// Where the clock was last set from.
enum class TimeSource : uint8_t { NONE = 0, BLE = 1, NTP = 2, HOME_ASSISTANT = 3 };

/// One scheduled feed inside a profile.
struct Event {
  uint8_t enabled;
  uint8_t anchor;      // Anchor
  int16_t offset_min;  // clock: minutes after midnight; solar: signed minutes from the anchor
  uint8_t portions;    // 1..6
  uint8_t days;        // weekday mask, bit0 = Sunday ... bit6 = Saturday
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
  float latitude;
  float longitude;
  uint16_t min_gap_s;          // minimum seconds between feed requests
  uint16_t portion_gap_s;      // seconds between portions of a multi-portion feed
  uint16_t reload_pause_min;   // scheduled feeding pause after "tray reload"
  uint16_t refill_interval_days;
  uint8_t daily_cap;           // max portions in any rolling 24 h
  uint8_t max_portions_cmd;    // max portions per manual/remote command
  uint8_t active_profile;
  uint8_t scheduler_enabled;   // recurring profile events; one-shot feeds run regardless
};

/// The servo calibration, saved on its own so that a change to the other settings never loses it.
struct Calibration {
  uint32_t version;
  int16_t offsets[NUM_POSITIONS];  // per-position offsets, tenths of a degree
  uint16_t min_pulse_us;           // servo pulse for 0 degrees (tuned at run time; YAML gives the default)
  uint16_t max_pulse_us;           // servo pulse for 180 degrees
};

/// Device settings, saved on their own (DESIGN.md section 9.2). The layout holds everything 2.0 sets at run time, so
/// later milestones fill fields in rather than change it. Strings are NUL-terminated; empty means "the build's value".
struct Device {
  uint32_t version;
  uint8_t servo_pin;   // GPIO number or PIN_BUILD
  uint8_t button_pin;  // GPIO number, PIN_NONE or PIN_BUILD
  uint8_t led_pin;     // GPIO number, PIN_NONE or PIN_BUILD
  uint8_t flags;       // DEVICE_* bits
  char hostname[HOSTNAME_SIZE];
  char timezone[TIMEZONE_SIZE];  // POSIX TZ string; empty = the build's timezone, or the one Home Assistant sends
  char ap_password[AP_PASSWORD_SIZE];
  uint8_t ble_key[DEVICE_KEY_SIZE];  // all zero = no key set on the device
  uint8_t password_scheme;           // 0 = no settings password
  uint8_t reserved[3];
  uint8_t password_salt[PASSWORD_SALT_SIZE];
  uint8_t password_hash[PASSWORD_HASH_SIZE];
};

/// The pins a Device record asks for, given the build's (PIN_NONE where the build has no button or LED). A saved pin
/// that is refused falls back to the build's, and so do all three if they clash; `problem` (optional) then says why.
Pins resolve_pins(const Device &device, const Pins &build, std::string *problem);

namespace legacy {
/// Settings as saved by firmware 0.3.0 to 0.4.x (version 3, with the calibration inside). Migrated, not discarded.
struct SettingsV3 {
  uint32_t version;
  Profile profiles[NUM_PROFILES];
  OneShot oneshots[MAX_ONESHOTS];
  int16_t cal[NUM_POSITIONS];
  float latitude;
  float longitude;
  uint16_t min_gap_s;
  uint16_t portion_gap_s;
  uint16_t reload_pause_min;
  uint16_t refill_interval_days;
  uint8_t daily_cap;
  uint8_t max_portions_cmd;
  uint8_t active_profile;
  uint8_t scheduler_enabled;
  uint16_t min_pulse_us;
  uint16_t max_pulse_us;
};
}  // namespace legacy

/// Runtime state that must survive a reboot (flushed to flash on every feed).
struct State {
  uint32_t version;
  uint32_t hold_until;                               // epoch; 0 = no hold; HOLD_FOREVER = until cancelled
  uint32_t reload_until;                             // epoch the reload pause ends; 0 = none or clock unknown
  uint32_t fired[NUM_PROFILES][EVENTS_PER_PROFILE];  // day number each event last fired for
  uint32_t last_feed_epoch;
  uint32_t hist_ts[HISTORY_SIZE];  // feed history (epoch, 0 = clock unknown)
  uint8_t hist_n[HISTORY_SIZE];    // portions per history entry
  uint8_t hist_src[HISTORY_SIZE];
  uint8_t position;   // tray position 0..6
  uint8_t moving_to;  // NO_MOVE or the target of an unfinished move
  uint8_t unknown;    // 1 when the position cannot be trusted
  uint8_t last_src;
};

/// The outcome of one feed request, reported to the listener (the BLE event log).
struct FeedRecord {
  uint32_t epoch;    // 0 when the clock is not set
  uint8_t source;    // Source
  uint8_t portions;  // portions requested (or dispensed, when accepted)
  uint8_t result;    // Result
};

/// Broken-down local time, as the platform's timezone sees it.
struct LocalTime {
  int year;
  int month;   // 1..12
  int day;     // 1..31
  int hour;
  int minute;
  int second;
};

enum class LogLevel : uint8_t { kError = 0, kWarn = 1, kInfo = 2, kDebug = 3 };

class FeederCore {
 public:
  virtual ~FeederCore() = default;

  // ---- configuration ----
  void set_sweep(float degrees) { this->sweep_ = degrees; }
  void set_invert(bool invert) { this->invert_ = invert; }
  void set_step_time(uint32_t ms) { this->step_ms_ = ms; }
  void set_wiggle(bool wiggle) { this->wiggle_ = wiggle; }
  void set_detach_delay(uint32_t ms) { this->detach_ms_ = ms; }
  /// The pulse range a fresh device starts with (the YAML values). Saved settings override it.
  void set_default_pulse_range(uint16_t min_us, uint16_t max_us) {
    this->default_min_pulse_us_ = min_us;
    this->default_max_pulse_us_ = max_us;
  }
  /// True if a pulse range is within the limits above.
  static bool pulse_range_ok(uint16_t min_us, uint16_t max_us) {
    return min_us >= PULSE_MIN_LO && min_us <= PULSE_MIN_HI && max_us >= PULSE_MAX_LO && max_us <= PULSE_MAX_HI &&
           max_us >= min_us + PULSE_SPAN_MIN;
  }

  /// Start up. Pass what was loaded from storage (nullptr if nothing was); wrong versions are ignored. `legacy_v3`
  /// is the old settings record, used only when there are no current settings: it is migrated.
  void core_start(const Settings *settings, const Calibration *calibration, const State *state,
                  const legacy::SettingsV3 *legacy_v3 = nullptr, const Device *device = nullptr);
  /// Call often (every main-loop pass). Runs motion, the scheduler and debounced saving.
  void core_loop();

  // ---- commands ----
  /// Ask for a feed. Every source goes through the same guard. If `accepted` is given it receives the
  /// number of portions that will really be dispensed (a request for more than remain is clamped).
  Result request_feed(uint8_t portions, Source source, uint8_t *accepted = nullptr);
  /// The optional push button, as in the original firmware: feed one portion, or reload the tray when it is
  /// empty or its position is unknown (reloading moves to the closed slot, so it is always safe).
  void button_press();
  /// Before loading the tray: return to position 0 (closed) and pause scheduled feeding briefly.
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
  /// Restart a profile's every-N-days cycle today. -1 means the profile selected in the editor.
  void restart_cadence_today(int profile = -1);

  // ---- calibration ----
  void set_calibration_mode(bool on);
  bool calibration_mode() const { return this->calibrating_; }
  void calibration_select(uint8_t position);
  uint8_t calibration_position() const { return this->cal_pos_; }
  void set_calibration_offset(int16_t tenths);
  int16_t calibration_offset() const { return this->calib.offsets[this->cal_pos_]; }
  /// Set the offset (tenths of a degree) of a specific position without changing the selected one.
  void set_calibration_offset_at(uint8_t position, int16_t tenths);
  /// Nudge a position's offset and move the servo there. Needs calibration mode.
  Result jog_calibration(uint8_t position, int16_t delta_tenths);
  /// Write the offsets (all settings) to storage now.
  void save_calibration();
  /// Change the servo pulse range at run time (saved like other settings). In calibration mode the servo
  /// moves to the selected position again so the effect can be seen. INVALID_ARG if out of range.
  Result set_pulse_range(uint16_t min_us, uint16_t max_us);
  uint16_t min_pulse_us() const { return this->calib.min_pulse_us; }
  uint16_t max_pulse_us() const { return this->calib.max_pulse_us; }
  Calibration calib{};

  // ---- schedule editing (the "editor" selects a profile and event) ----
  Settings cfg{};
  uint8_t editor_profile{0};
  uint8_t editor_event{0};
  Profile &edit_profile() { return this->cfg.profiles[this->editor_profile]; }
  Event &edit_event() { return this->cfg.profiles[this->editor_profile].ev[this->editor_event]; }
  /// Call after changing cfg; saves after a short delay and refreshes projections.
  void cfg_changed();

  // ---- settings, validated (INVALID_ARG and no change when out of range) ----
  Result set_min_gap_s(int32_t seconds);
  Result set_portion_gap_s(int32_t seconds);
  Result set_reload_pause_min(int32_t minutes);
  Result set_refill_interval_days(int32_t days);
  Result set_daily_cap(int32_t portions);
  Result set_max_portions_cmd(int32_t portions);
  Result set_scheduler_enabled(bool on);
  Result set_active_profile(int32_t profile);
  /// Degrees, north and east positive. Both 0 means "not set" (sun events are then skipped).
  Result set_location(float latitude, float longitude);
  /// The same in units of 1e-5 degree (BLE). Checked as integers, before the conversion to float can round a value
  /// just outside the limits (180.00001 degrees is 180.0 as a float) back inside them.
  Result set_location_e5(int32_t latitude_e5, int32_t longitude_e5);
  static bool location_ok(float latitude, float longitude) {
    return latitude >= -90.0f && latitude <= 90.0f && longitude >= -180.0f && longitude <= 180.0f;  // false for NaN
  }
  /// Change the editor event's anchor; the offset resets when switching between clock and solar.
  void set_event_anchor(uint8_t anchor);
  static std::string format_days(uint8_t mask);
  static bool parse_days(const std::string &text, uint8_t &mask);
  std::string event_time_text();
  bool set_event_time_text(const std::string &text);
  bool add_oneshot(const std::string &local_datetime, uint8_t portions);
  void clear_oneshots();

  // ---- clock ----
  /// Record that the clock was just set by `source` (wired to the time platforms' on_time_sync).
  void note_time_sync(TimeSource source);
  /// Set the system clock from a Unix epoch (UTC), for example one pushed over BLE.
  void set_system_time(uint32_t epoch);
  TimeSource time_source() const { return this->last_time_source_; }
  std::string time_source_text() const;
  bool time_ever_synced() const { return this->last_time_source_ != TimeSource::NONE; }
  /// True if the clock was already valid at boot: the RTC kept it across a soft reset (OTA, crash, flashing).
  /// The time is good, but no source has set it since this boot, so the source stays NONE.
  bool clock_kept() const { return this->clock_kept_; }
  /// Minutes since the clock was last set by any source (saturating); 0 if never.
  uint16_t time_age_min();
  /// Milliseconds since NTP or Home Assistant last set the clock; negative if neither ever did.
  int64_t network_time_age_ms();
  /// True if the clock has not been re-synchronised for a week.
  bool time_stale();
  bool time_valid() { return this->hal_time_valid(); }
  /// Milliseconds since boot, without the 49-day wrap of a 32-bit millisecond counter.
  int64_t uptime() { return this->hal_uptime_ms(); }

  // ---- device settings ----
  const Device &device() const { return this->dev_; }
  /// Set the timezone from a POSIX TZ string ("CET-1CEST,M3.5.0,M10.5.0/3"); empty goes back to the build's
  /// timezone (or Home Assistant's). Saved at once and applied without a reboot. INVALID_ARG if it does not parse.
  /// While one is set, it wins over the timezone Home Assistant sends.
  Result set_timezone(const std::string &posix);
  /// The timezone set on the device, or "" when the build's (or Home Assistant's) is in use.
  std::string timezone_text() const { return this->dev_.timezone; }

  // ---- pins (DESIGN.md section 2.1) ----
  // The adapter resolves the saved pins at boot, before the drivers start, and reports what it applied. A change
  // here is saved at once and applies at the next restart.
  void set_build_pins(const Pins &pins) { this->build_pins_ = pins; }
  /// What the adapter applied. A build without the button or LED package has none, whatever is saved.
  void set_pins_in_use(const Pins &pins, bool has_button = true, bool has_led = true) {
    this->pins_in_use_ = pins;
    this->pins_runtime_ = true;
    this->has_button_ = has_button;
    this->has_led_ = has_led;
  }
  /// The pins the drivers were started with.
  const Pins &pins_in_use() const { return this->pins_in_use_; }
  /// The pins the next restart will use.
  Pins pins_saved() const;
  bool restart_needed() const { return this->pins_runtime_ && this->pins_saved() != this->pins_in_use_; }
  /// Choose one pin: a GPIO, PIN_NONE (button and LED only) or PIN_BUILD. INVALID_ARG for a refused pin or one
  /// another role already uses; UNSUPPORTED when the build cannot change pins.
  Result set_pin(PinRole role, uint8_t gpio);
  /// Choose a board preset (all three pins), or BUILD for the build's pins. CUSTOM changes nothing.
  Result set_board(Board board);
  /// BUILD while all three pins follow the build, else the preset the saved pins match, else CUSTOM.
  Board board() const;
  /// "servo GPIO1, button none, LED GPIO21": the pins in use.
  std::string pins_text() const;
  bool status_led_fitted() const { return this->pins_in_use_.led != PIN_NONE; }

  // ---- settings password (DESIGN.md section 10.1) ----
  // These are called from the web server's task as well as the main loop: the adapter serialises them with a lock,
  // and writes to flash (set_password, clear_password) happen on the main loop.
  bool password_set() const { return this->dev_.password_scheme != PASSWORD_SCHEME_NONE; }
  /// A fresh one-time challenge. It replaces any outstanding one and expires after AUTH_NONCE_TTL_MS.
  void password_challenge(uint8_t nonce[AUTH_NONCE_SIZE]);
  /// Check tag = HMAC-SHA256(key, nonce || message). Consumes the challenge whatever the outcome. OK without a
  /// password. After a refusal, every check waits (1 s, doubling to 60 s); `wait_ms` says how long is left.
  AuthResult password_check(const uint8_t nonce[AUTH_NONCE_SIZE], const uint8_t tag[AUTH_TAG_SIZE],
                            const std::string &message, uint32_t &wait_ms);
  /// Milliseconds before the next check is allowed (0 = now).
  uint32_t password_wait_ms();
  /// A new key travels masked: key XOR HMAC-SHA256(current key, nonce || "new key"); without a password the mask
  /// is zero. Call after password_check() passed for that nonce.
  void password_unmask(const uint8_t nonce[AUTH_NONCE_SIZE], const uint8_t masked[PASSWORD_HASH_SIZE],
                       uint8_t key[PASSWORD_HASH_SIZE]) const;
  /// Store a new password: its salt and the key the browser derived from it. Saved at once.
  void set_password(const uint8_t salt[PASSWORD_SALT_SIZE], const uint8_t key[PASSWORD_HASH_SIZE]);
  /// Remove the settings password. Saved at once.
  void clear_password(const char *why);
  /// The BOOT button (GPIO0), pressed or released. Held for BOOT_HOLD_MS, it clears the settings password only.
  void boot_button(bool pressed);

  // ---- feed events ----
  /// Called once for every feed request with its final outcome, accepted or refused.
  void set_feed_listener(std::function<void(const FeedRecord &)> &&listener) { this->feed_listener_ = std::move(listener); }

  // ---- status ----
  uint8_t position() const { return this->state_.position; }
  uint8_t remaining() const { return this->state_.position >= MAX_PORTIONS ? 0 : MAX_PORTIONS - this->state_.position; }
  bool position_unknown() const { return this->state_.unknown != 0; }
  TrayState tray_state() const;
  std::string tray_state_text() const;
  uint16_t feeds_today();
  /// Portions the daily cap currently counts (the rolling 24 h window).
  uint16_t portions_24h();
  /// Debugging and recovery (owner decision 2026-10-14): set the number of portions the daily cap counts.
  /// Lowering it drops the oldest counted portions from the feed history; raising it adds an "adjustment"
  /// entry stamped now. Logged as a warning. Never feeds and never moves the tray.
  void set_portions_24h(uint16_t portions);
  /// Whole days of feeding left in the tray under the active schedule; 255 = more than the planning horizon.
  uint8_t runway_days() const { return this->runway_days_; }
  bool schedule_oversubscribed() const { return this->oversubscribed_; }
  /// Portions a profile feeds on its busiest weekday (enabled events, fast days excluded). -1 = active profile.
  uint16_t max_portions_per_day(int profile = -1) const;
  /// True if the active profile's busiest day needs more portions than the daily cap allows, so some of its
  /// events would be refused every such day. Only while the scheduler is on.
  bool schedule_over_cap() const;
  uint32_t last_feed_epoch() const { return this->state_.last_feed_epoch; }
  uint8_t last_feed_source() const { return this->state_.last_src; }
  uint32_t next_event_epoch() const { return static_cast<uint32_t>(this->next_event_epoch_); }
  /// Portions the next scheduled or one-shot feed asks for (0 = none due, or the clock is not set).
  uint8_t next_event_portions() const { return this->next_event_portions_; }
  /// True when the tray is empty, or the next due feed needs more portions than remain (it would be cut
  /// short). False while the position is unknown: that needs checking, not food, and Faults reports it.
  bool needs_food() const {
    if (this->state_.unknown)
      return false;
    return this->remaining() == 0 || (this->next_event_portions_ != 0 && this->next_event_portions_ > this->remaining());
  }
  /// A sun event of the active profile cannot be placed today: no location set (the event is skipped), or no
  /// sunrise/sunset today (the event uses the polar fallback time; see polar_fallback()).
  bool solar_fault() const { return this->solar_fault_ || this->polar_fallback_; }
  /// Today has no sunrise or sunset at the set location, so sun events use 06:00 / 18:00 plus their offset.
  bool polar_fallback() const { return this->polar_fallback_; }
  bool reload_pause_active();
  bool hold_active();
  std::string hold_text();
  std::string next_event_text();
  std::string empty_on_text();
  std::string last_feed_text();
  std::string last_result_text() const;
  std::string faults_text();
  std::string schedule_summary();
  /// The same summary for the profile selected in the editor, with a warning if it exceeds the daily cap.
  std::string edited_profile_summary();
  std::string recent_feeds_text();
  std::string oneshots_text();
  /// 0 ok, 1 time not valid, 2 tray empty, 3 position unknown, 4 moving, 5 calibrating, 6 hold or reload pause
  uint8_t led_code();

  // ---- read-only data for the web app (DESIGN.md section 7) ----
  /// Every profile in one compact line, so a client can show the whole schedule without stepping the editor:
  /// "1|A|B|C" with each profile "every_n,fast,ev1,ev2,ev3,ev4" (fast: weekday mask, 2 hex digits) and each event
  /// "<enabled 0|1><anchor c|r|s><offset>/<portions>/<days>" (offset in minutes as stored; days: 2 hex digits,
  /// bit 0 = Sunday). The leading 1 is the format version.
  std::string schedule_data_text() const;
  /// The device's local time with its UTC offset, "Thu 2026-10-15 17:30 -05:00"; "clock not set" without a clock.
  std::string device_time_text();
  /// Today's feeds (active profile and one-shots) in time order, "08:30 x1 fed; 17:30 x1 next; 20:00 x1 later
  /// one-shot", each one fed, skipped (missed or refused), due (in its minute now), next, later, or held (inside a
  /// hold or the reload pause, so it will be skipped). With none: "none", "none (fast day)", "none (not a feeding
  /// day)" or "none (scheduler off)". "clock not set" without a clock.
  std::string todays_feeds_text();
  /// What the guard would answer to a one-portion feed from ESPHome right now, without feeding: OK or the refusal.
  Result feed_guard_preview();
  /// feed_guard_preview() as text: "ready", or the refusal as Last Result words it ("too soon", ...).
  std::string feed_guard_text();
  static const char *result_name(Result r);
  Result last_result() const { return this->last_result_; }

 protected:
  // ---- platform hooks ----
  virtual uint32_t hal_millis() = 0;      // wrapping 32-bit millisecond counter
  virtual int64_t hal_uptime_ms() = 0;    // non-wrapping milliseconds since boot
  virtual bool hal_time_valid() = 0;      // the clock has been set
  virtual int64_t hal_epoch() = 0;        // UTC seconds
  virtual void hal_set_epoch(int64_t epoch) = 0;
  virtual LocalTime hal_local(int64_t epoch) = 0;
  /// Epoch of a local wall-clock time (the platform resolves daylight saving); negative if impossible.
  virtual int64_t hal_local_to_epoch(int year, int month, int day, int hour, int minute) = 0;
  virtual void hal_servo_write(float angle_degrees) = 0;
  virtual void hal_servo_release() = 0;
  virtual void hal_save_settings(const Settings &settings, bool flush) = 0;
  virtual void hal_save_calibration(const Calibration &calibration, bool flush) = 0;
  virtual void hal_save_state(const State &state, bool flush) = 0;
  virtual void hal_save_device(const Device &device) = 0;  // always written at once
  /// Use this timezone for local time from now on; nullptr goes back to the one in use before any was applied.
  virtual void hal_apply_timezone(const TzRules *rules) = 0;
  virtual void hal_log(LogLevel /*level*/, const char * /*message*/) {}
  virtual void hal_random(uint8_t *out, size_t n) = 0;  // cryptographically strong

  enum class Phase : uint8_t { IDLE, STEP_A, STEP_B, STEP_C, GAP, SETTLE };

  struct DayCtx {
    int32_t day_no;
    int wday;
    int64_t midnight;
    int tz_min;
  };

  void log_(LogLevel level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
  void defaults_();
  void clamp_loaded_();
  void clamp_device_();
  void apply_timezone_();
  void timezone_changed_();
  void migrate_v3_(const legacy::SettingsV3 &v3, bool take_calibration);
  void save_state_(bool flush);

  // servo
  float angle_for_(uint8_t position) const;
  void move_to_(uint8_t position) { this->hal_servo_write(this->angle_for_(position)); }

  // motion
  void start_next_portion_();
  void commit_step_();
  void run_motion_();
  void abort_motion_();
  void uncount_portions_(uint8_t portions);
  bool moving_() const;

  // feed history: newest first, HISTORY_SIZE entries. Saved as the parallel hist_* arrays of State (that layout is
  // kept so saved data stays loadable); everything else uses these records.
  struct HistoryEntry {
    uint32_t epoch{0};      // when it was fed; 0 = the clock was not set
    uint8_t portions{0};    // 0 = an empty slot
    uint8_t source{0};      // Source
    uint32_t at_ms{0};      // hal_millis() when recorded (only meaningful when this_boot)
    bool this_boot{false};  // recorded since this boot
  };
  HistoryEntry history_(uint8_t i) const;
  void set_history_(uint8_t i, const HistoryEntry &e);
  void push_history_(const HistoryEntry &e);
  /// Whether an entry is inside the rolling 24 h the daily cap looks at.
  bool counts_today_(const HistoryEntry &e, int64_t now_epoch, bool time_ok);

  // guard
  Result check_guard_(uint8_t &portions, Source source);
  uint16_t portions_24h_(int64_t now_epoch, bool time_ok);
  void record_history_(uint8_t portions, Source source);
  void report_(Source source, uint8_t portions, Result result);

  // scheduler
  void schedule_tick_();
  void fire_due_();
  void update_solar_fault_();
  DayCtx make_day_(int32_t day_no);
  int64_t wall_to_epoch_(int32_t day_no, int minutes);
  bool resolve_minutes_(const Event &e, const DayCtx &d, int &minutes);
  bool day_active_(const Profile &p, const DayCtx &d) const;
  int64_t local_offset_s_(int64_t epoch);
  bool sun_cached_(const DayCtx &d, double &rise, double &set);
  void on_time_valid_();
  /// One occurrence of a recurring event: event `event` of the active profile, for anchor day `day_no`.
  struct Occurrence {
    int32_t day_no;
    uint8_t event;
    int64_t epoch;     // when it is due
    uint8_t portions;
    bool fed;          // it already fed (or was finally refused) for this day: it will not run again
  };
  /// Calls fn(const Occurrence &) for every occurrence of the active profile's events whose anchor day is in
  /// [first_day, last_day] and that would run: scheduler on, a feeding day (cadence, fast days), the event enabled
  /// on that weekday, and its time resolvable. The one place that decides what the schedule contains; firing and
  /// the projection (Next Feed, runway, Needs Food) both use it, so they cannot disagree.
  template<typename Fn> void for_each_occurrence_(int32_t first_day, int32_t last_day, Fn &&fn);
  void project_();
  /// Scheduled feeds before this epoch are skipped (hold or reload pause); 0 = nothing blocks them.
  int64_t blocked_until_(int64_t now);
  int32_t today_();
  std::string format_local_(int64_t epoch, bool with_date = true);
  std::string profile_summary_(uint8_t profile);

  float sweep_{175.0f};
  bool invert_{false};
  uint32_t step_ms_{1000};
  bool wiggle_{true};
  uint32_t detach_ms_{1000};
  uint16_t default_min_pulse_us_{544};
  uint16_t default_max_pulse_us_{2400};

  struct SunCache {
    int32_t day_no{INT32_MIN};
    float lat{0}, lon{0};
    int tz_min{0};
    bool ok{false};
    double rise{0}, set{0};
  };

  State state_{};
  uint32_t hist_ms_[HISTORY_SIZE]{};    // hal_millis() stamp per history entry
  bool hist_this_boot_[HISTORY_SIZE]{};  // entry was recorded since this boot (so hist_ms_ is meaningful)
  // Enough for the whole projection (yesterday + 30 days + tomorrow's anchor), so it does not thrash.
  static constexpr uint8_t DAY_CACHE_SIZE = 34;
  SunCache sun_cache_[DAY_CACHE_SIZE];
  uint8_t sun_cache_next_{0};
  // Day contexts (midnight, weekday, UTC offset) are recomputed at most once an hour, so a timezone pushed by
  // Home Assistant or a daylight-saving change is picked up within the hour.
  DayCtx day_cache_[DAY_CACHE_SIZE]{};
  bool day_cache_used_[DAY_CACHE_SIZE]{};
  uint8_t day_cache_next_{0};
  int64_t day_cache_hour_{-1};

  Phase phase_{Phase::IDLE};
  uint32_t phase_start_{0};
  uint8_t queue_{0};
  bool calibrating_{false};
  uint8_t cal_pos_{0};

  std::function<void(const FeedRecord &)> feed_listener_;
  TimeSource last_time_source_{TimeSource::NONE};
  bool clock_kept_{false};
  int64_t last_sync_ms_[4]{-1, -1, -1, -1};  // uptime of the last sync per TimeSource, -1 = never

  Result last_result_{Result::NONE};
  uint32_t last_feed_ms_{0};
  bool have_last_feed_ms_{false};
  uint32_t reload_pause_until_ms_{0};
  bool reload_pause_ms_active_{false};
  bool was_time_valid_{false};
  bool cfg_dirty_{false};
  Device dev_{};
  Pins build_pins_{PIN_NONE, PIN_NONE, PIN_NONE};
  Pins pins_in_use_{PIN_NONE, PIN_NONE, PIN_NONE};
  bool pins_runtime_{false};  // the adapter applied the saved pins at boot, so changing them means something
  bool has_button_{true};     // the build has the button package
  bool has_led_{true};        // the build has the status LED package
  bool tz_reapply_{false};
  uint8_t auth_nonce_[AUTH_NONCE_SIZE]{};
  bool auth_nonce_live_{false};
  uint32_t auth_nonce_ms_{0};
  uint8_t auth_failures_{0};
  uint32_t auth_wait_from_ms_{0};
  uint32_t auth_wait_ms_{0};
  bool boot_pressed_{false};
  bool boot_handled_{false};
  uint32_t boot_pressed_ms_{0};  // Home Assistant pushed its timezone over ours: put ours back on the next loop
  uint32_t cfg_dirty_since_{0};

  // A scheduled event or one-shot refused with BUSY / TOO_SOON / HOLD inside its window is retried
  // silently; the last refusal is reported once, when the window closes without it being fed.
  uint8_t pending_[NUM_PROFILES][EVENTS_PER_PROFILE];
  int32_t pending_day_[NUM_PROFILES][EVENTS_PER_PROFILE];
  uint8_t oneshot_pending_[MAX_ONESHOTS];

  uint32_t last_tick_{0};
  uint32_t last_project_{0};
  bool project_dirty_{true};
  bool solar_fault_{false};    // location not set: sun events skipped
  bool polar_fallback_{false};  // no sunrise/sunset today: sun events use the fallback times
  uint8_t runway_days_{255};
  bool oversubscribed_{false};
  int64_t next_event_epoch_{0};
  uint8_t next_event_portions_{0};
  int64_t empty_on_epoch_{0};
};

}  // namespace precision_feeder
}  // namespace esphome
