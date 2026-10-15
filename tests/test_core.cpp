// Host-side tests for the feeder logic (components/precision_feeder/feeder_core.*), with a simulated clock.
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder test_core.cpp ../components/precision_feeder/feeder_core.cpp -o /tmp/test_core && /tmp/test_core
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "feeder_core.h"

using namespace esphome::precision_feeder;

static int failures = 0;
static const char *current = "";
#define CHECK(cond, msg)                                                  \
  do {                                                                    \
    if (!(cond)) {                                                        \
      std::printf("FAIL [%s]: %s (line %d)\n", current, msg, __LINE__);   \
      failures++;                                                         \
    }                                                                     \
  } while (0)

// ---- fake platform ---------------------------------------------------------------------------

/// Storage that survives a simulated reboot.
struct Flash {
  bool have_settings{false};
  bool have_state{false};
  Settings settings{};
  State state{};
};

static constexpr int64_t TZ = -5 * 3600;  // fixed UTC-5, no daylight saving

static int64_t local_epoch(int y, int mo, int d, int h, int mi, int s = 0) {
  return static_cast<int64_t>(days_from_civil(y, mo, d)) * 86400 + h * 3600 + mi * 60 + s - TZ;
}

class FakeFeeder : public FeederCore {
 public:
  FakeFeeder(Flash &flash, int64_t epoch_ms, bool clock_valid) : flash_(flash), epoch_ms_(epoch_ms), valid_(clock_valid) {}

  void start() {
    this->set_detach_delay(1000);
    this->core_start(flash_.have_settings ? &flash_.settings : nullptr, flash_.have_state ? &flash_.state : nullptr);
    this->set_feed_listener([this](const FeedRecord &r) { this->records.push_back(r); });
  }

  /// Advance simulated time, running the main loop every `step_ms`.
  void advance(int64_t ms, int64_t step_ms = 250) {
    for (int64_t t = 0; t < ms; t += step_ms) {
      this->uptime_ms_ += step_ms;
      this->epoch_ms_ += step_ms;
      this->core_loop();
    }
  }
  void set_clock_valid(bool v) { this->valid_ = v; }
  int64_t now() const { return this->epoch_ms_ / 1000; }
  int count(Source s, Result r) const {
    int n = 0;
    for (const auto &rec : this->records)
      if (rec.source == static_cast<uint8_t>(s) && rec.result == static_cast<uint8_t>(r))
        n++;
    return n;
  }
  int count_source(Source s) const {
    int n = 0;
    for (const auto &rec : this->records)
      if (rec.source == static_cast<uint8_t>(s))
        n++;
    return n;
  }

  std::vector<FeedRecord> records;
  std::vector<float> angles;
  int releases{0};
  std::vector<std::string> logs;

 protected:
  uint32_t hal_millis() override { return static_cast<uint32_t>(this->uptime_ms_); }
  int64_t hal_uptime_ms() override { return this->uptime_ms_; }
  bool hal_time_valid() override { return this->valid_; }
  int64_t hal_epoch() override { return this->epoch_ms_ / 1000; }
  void hal_set_epoch(int64_t epoch) override {
    this->epoch_ms_ = epoch * 1000;
    this->valid_ = true;
  }
  LocalTime hal_local(int64_t epoch) override {
    const int64_t local = epoch + TZ;
    int64_t days = local / 86400;
    int64_t rem = local % 86400;
    if (rem < 0) {
      rem += 86400;
      days--;
    }
    int y;
    unsigned m, d;
    civil_from_days(static_cast<int32_t>(days), y, m, d);
    return LocalTime{y, static_cast<int>(m), static_cast<int>(d), static_cast<int>(rem / 3600),
                     static_cast<int>(rem % 3600 / 60), static_cast<int>(rem % 60)};
  }
  int64_t hal_local_to_epoch(int y, int mo, int d, int h, int mi) override { return local_epoch(y, mo, d, h, mi); }
  void hal_servo_write(float angle) override { this->angles.push_back(angle); }
  void hal_servo_release() override { this->releases++; }
  void hal_save_settings(const Settings &s, bool) override {
    flash_.settings = s;
    flash_.have_settings = true;
  }
  void hal_save_state(const State &s, bool) override {
    flash_.state = s;
    flash_.have_state = true;
  }
  void hal_log(LogLevel, const char *message) override { this->logs.emplace_back(message); }

  Flash &flash_;
  int64_t epoch_ms_;
  int64_t uptime_ms_{0};
  bool valid_;
};

// 2026-10-14 is a Wednesday.
static const int64_t T0 = local_epoch(2026, 10, 14, 7, 55);

static std::unique_ptr<FakeFeeder> boot(Flash &flash, int64_t epoch, bool clock_valid = true) {
  auto f = std::make_unique<FakeFeeder>(flash, epoch * 1000, clock_valid);
  f->start();
  f->advance(1500);  // let the boot settle (servo released)
  return f;
}

static void enable_event(FakeFeeder &f, int profile, int event, Anchor anchor, int offset_min, int portions = 1,
                         uint8_t days = 0x7F) {
  Event &e = f.cfg.profiles[profile].ev[event];
  e.enabled = 1;
  e.anchor = static_cast<uint8_t>(anchor);
  e.offset_min = static_cast<int16_t>(offset_min);
  e.portions = static_cast<uint8_t>(portions);
  e.days = days;
  f.cfg_changed();
}

// ---- tests -----------------------------------------------------------------------------------

static void test_feed_motion() {
  current = "feed motion";
  Flash flash;
  auto f = boot(flash, T0);
  CHECK(f->position() == 0 && f->remaining() == 6, "fresh tray");
  f->angles.clear();
  const int releases = f->releases;
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "feed accepted");
  CHECK(flash.state.moving_to == 1, "intent saved before the move");
  CHECK(f->tray_state() == TrayState::MOVING, "moving");
  f->advance(3500);
  CHECK(f->position() == 1 && f->remaining() == 5, "advanced one position");
  CHECK(flash.state.position == 1 && flash.state.moving_to == NO_MOVE, "position committed to storage");
  CHECK(f->angles.size() == 3, "three moves: forward, back, forward (wiggle)");
  if (f->angles.size() == 3)
    CHECK(f->angles[0] == f->angles[2] && f->angles[1] < f->angles[0], "wiggle pattern");
  f->advance(1500);
  CHECK(f->releases == releases + 1, "servo released after the move");
  CHECK(f->count(Source::HOME_ASSISTANT, Result::OK) == 1, "one accepted record");
}

static void test_guard() {
  current = "guard";
  Flash flash;
  auto f = boot(flash, T0);
  CHECK(f->request_feed(2, Source::HOME_ASSISTANT) == Result::PORTION_LIMIT, "per-command limit (default 1)");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "first feed");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::BUSY, "busy while moving");
  f->advance(5000);
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::TOO_SOON, "minimum gap");
  for (int i = 0; i < 3; i++) {
    f->advance(61000);
    CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "spaced feeds accepted");
  }
  f->advance(61000);
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::DAILY_CAP, "daily cap of 4 reached");
  f->set_calibration_mode(true);
  CHECK(f->request_feed(1, Source::BUTTON) == Result::CALIBRATING, "refused while calibrating");
  f->set_calibration_mode(false);
  CHECK(f->cfg.cal[0] == 0, "calibration untouched");

  // Empty tray.
  Flash flash2;
  auto g = boot(flash2, T0);
  g->cfg.daily_cap = 6;
  for (int i = 0; i < 6; i++) {
    CHECK(g->request_feed(1, Source::BLE) == Result::OK, "feed until empty");
    g->advance(61000);
  }
  CHECK(g->remaining() == 0 && g->tray_state() == TrayState::EMPTY, "empty");
  CHECK(g->request_feed(1, Source::BLE) == Result::EMPTY, "refused when empty");
}

static void test_power_loss_mid_step_and_button() {
  current = "power loss and button";
  Flash flash;
  {
    auto f = boot(flash, T0);
    CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "feed starts");
    f->advance(500);  // power lost in the middle of the step
  }
  auto f = boot(flash, T0 + 10);
  CHECK(f->position_unknown() && f->tray_state() == TrayState::UNKNOWN, "position unknown after reboot");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::UNKNOWN_POSITION, "feeds refused");
  f->button_press();  // bug 1: the button must be able to recover a button-only build
  CHECK(!f->position_unknown() && f->position() == 0, "button reloads when the position is unknown");

  // Button when empty reloads, otherwise feeds. The interrupted feed above still counts toward the daily
  // cap (it may have dispensed food), so let it age out first.
  f->advance(24 * 3600 * 1000LL, 1000);
  f->cfg.daily_cap = 6;
  f->cfg.reload_pause_min = 0;
  for (int i = 0; i < 6; i++) {
    f->advance(61000);
    f->button_press();
  }
  f->advance(5000);
  CHECK(f->remaining() == 0, "six button feeds empty the tray");
  CHECK(f->count(Source::BUTTON, Result::OK) == 6, "six button feeds recorded");
  f->button_press();
  CHECK(f->remaining() == 6, "button reloads an empty tray");
}

static void test_schedule_fires_once_and_not_late() {
  current = "schedule once, no late feeds";
  Flash flash;
  {
    auto f = boot(flash, T0);
    enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
    f->advance((5 * 60 + 20) * 1000LL);  // to 08:00:20
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "fired at 08:00");
    f->advance(5000);
  }
  // Reboot inside the event's window: must not feed again.
  auto f = boot(flash, local_epoch(2026, 10, 14, 8, 0, 40));
  f->advance(60000);
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 0, "not repeated after a reboot inside the window");
  CHECK(f->position() == 1, "still one portion used");

  // Device off at the event time: nothing is made up afterwards, and nothing is reported.
  Flash flash2;
  auto g = boot(flash2, T0);
  enable_event(*g, 0, 0, Anchor::CLOCK, 8 * 60);
  g->advance(2000);
  Flash flash3 = flash2;
  auto h = boot(flash3, local_epoch(2026, 10, 14, 8, 2));
  h->advance(10 * 60 * 1000);
  CHECK(h->records.empty(), "late event skipped silently");
  CHECK(h->position() == 0, "no portion used");
}

static void test_retry_is_silent() {
  current = "retry silence (bug 2)";
  Flash flash;
  auto f = boot(flash, local_epoch(2026, 10, 14, 7, 59, 0));
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
  f->advance(30000);  // 07:59:30
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "manual feed at 07:59:30");
  f->advance(90000);  // to 08:01:00
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "scheduled feed happened once the gap allowed it");
  CHECK(f->count_source(Source::SCHEDULE) == 1, "no refusals reported while retrying");
  CHECK(f->position() == 2, "two portions used");
}

static void test_refusal_reported_once() {
  current = "refusal reported once";
  Flash flash;
  auto f = boot(flash, T0);
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
  f->set_hold_indefinite();
  f->advance(10 * 60 * 1000);
  CHECK(f->count_source(Source::SCHEDULE) == 1, "exactly one record for the held event");
  CHECK(f->count(Source::SCHEDULE, Result::HOLD) == 1, "reported as HOLD");
  CHECK(f->position() == 0, "not fed during the hold");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "manual feeds still work during a hold");

  // A definitive refusal (daily cap) is reported once, not every second.
  Flash flash2;
  auto g = boot(flash2, T0);
  g->cfg.daily_cap = 1;
  enable_event(*g, 0, 0, Anchor::CLOCK, 7 * 60 + 56);
  CHECK(g->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "use up the cap");
  g->advance(5 * 60 * 1000);
  CHECK(g->count(Source::SCHEDULE, Result::DAILY_CAP) == 1, "daily cap reported once");
}

static void test_oneshot_and_scheduler_switch() {
  current = "one-shots and the scheduler switch (bugs 5, 6)";
  Flash flash;
  auto f = boot(flash, T0);
  enable_event(*f, 0, 0, Anchor::CLOCK, 9 * 60);
  f->cfg.scheduler_enabled = 0;
  f->cfg_changed();
  CHECK(f->add_oneshot("2026-10-14 08:30", 1), "one-shot added");
  CHECK(!f->add_oneshot("2026-02-31 08:30", 1), "impossible date rejected");
  CHECK(!f->add_oneshot("2026-10-13 08:30", 1), "past date rejected");
  f->advance(2000);
  CHECK(f->next_event_epoch() == static_cast<uint32_t>(local_epoch(2026, 10, 14, 8, 30)),
        "next event is the one-shot, not the disabled recurring event");
  f->advance((36 * 60) * 1000LL);  // past 08:30
  CHECK(f->count(Source::ONE_SHOT, Result::OK) == 1, "one-shot fed with the scheduler off");
  f->advance(40 * 60 * 1000);  // past 09:00
  CHECK(f->count_source(Source::SCHEDULE) == 0, "recurring event not fed with the scheduler off");

  // Solar fault follows the scheduler switch.
  enable_event(*f, 0, 1, Anchor::SUNSET, 0);  // no location set
  f->advance(2000);
  CHECK(!f->solar_fault(), "no solar fault while the scheduler is off");
  f->cfg.scheduler_enabled = 1;
  f->cfg_changed();
  f->advance(2000);
  CHECK(f->solar_fault(), "solar fault with the scheduler on and no location");
  f->cfg.latitude = 39.7684f;
  f->cfg.longitude = -86.1581f;
  f->cfg_changed();
  f->advance(2000);
  CHECK(!f->solar_fault(), "fault clears once the location is set");
}

static void test_cap_after_reboot_without_clock() {
  current = "daily cap after a reboot without a clock (bug 4)";
  Flash flash;
  {
    auto f = boot(flash, T0);
    for (int i = 0; i < 4; i++) {
      CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "four feeds");
      f->advance(61000);
    }
    CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::DAILY_CAP, "cap reached");
  }
  // Power cut, comes back with no network: the old feeds cannot be aged, so they do not block feeding.
  auto f = boot(flash, T0 + 3600, false);
  CHECK(f->request_feed(1, Source::BUTTON) == Result::OK, "manual feed allowed without a clock");
  f->advance(5000);
  // The clock comes back: the old feeds were less than 24 h ago, so they count again.
  f->set_clock_valid(true);
  f->advance(61000);
  CHECK(f->request_feed(1, Source::BUTTON) == Result::DAILY_CAP, "cap applies again once the clock is known");
}

static void test_reload_pause_survives_reboot() {
  current = "reload pause survives a reboot (bug 7)";
  Flash flash;
  {
    auto f = boot(flash, T0);  // 07:55
    enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
    f->advance(2000);
    f->tray_reload();  // pause until 08:05
    f->advance(60000);
  }
  auto f = boot(flash, T0 + 120);  // 07:57, rebooted
  CHECK(f->reload_pause_active(), "pause still active after the reboot");
  f->advance(10 * 60 * 1000);
  CHECK(f->count(Source::SCHEDULE, Result::HOLD) == 1, "08:00 event refused by the pause");
  CHECK(f->position() == 0, "not fed during the pause");
  CHECK(!f->reload_pause_active(), "pause over");
}

static void test_sunset_after_midnight() {
  current = "sunset + 6 h lands after midnight";
  Flash flash;
  auto f = boot(flash, local_epoch(2026, 10, 14, 12, 0));
  f->cfg.latitude = 39.7684f;
  f->cfg.longitude = -86.1581f;
  enable_event(*f, 0, 0, Anchor::SUNSET, 6 * 60);
  f->advance(2000);
  // Sunset at UTC-5 is about 18:09, so the event is about 00:09 on the 15th.
  const int64_t next = f->next_event_epoch();
  CHECK(next > local_epoch(2026, 10, 15, 0, 0) && next < local_epoch(2026, 10, 15, 0, 20), "next event just after midnight");
  f->advance(13 * 3600 * 1000LL, 1000);  // to 01:00 on the 15th
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "fed once after midnight");
  f->advance(24 * 3600 * 1000LL, 1000);
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 2, "and once the next night");
}

static void test_cadence_and_fast_days() {
  current = "every N days and fast days";
  Flash flash;
  auto f = boot(flash, local_epoch(2026, 10, 14, 7, 0));  // Wednesday
  f->cfg.daily_cap = 6;
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
  f->cfg.profiles[0].every_n = 2;
  f->restart_cadence_today(0);
  f->advance(3 * 24 * 3600 * 1000LL, 1000);  // Wed, Thu, Fri
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 2, "every 2 days: Wednesday and Friday");

  Flash flash2;
  auto g = boot(flash2, local_epoch(2026, 10, 14, 7, 0));
  g->cfg.daily_cap = 6;
  enable_event(*g, 0, 0, Anchor::CLOCK, 8 * 60);
  g->cfg.profiles[0].fast_days = 1 << 4;  // Thursday
  g->cfg_changed();
  g->advance(3 * 24 * 3600 * 1000LL, 1000);
  CHECK(g->count(Source::SCHEDULE, Result::OK) == 2, "Thursday fast: fed Wednesday and Friday");
}

static void test_projection() {
  current = "runway and over-capacity";
  Flash flash;
  auto f = boot(flash, T0);
  enable_event(*f, 0, 0, Anchor::CLOCK, 9 * 60);
  enable_event(*f, 0, 1, Anchor::CLOCK, 18 * 60);
  f->cfg.refill_interval_days = 4;
  f->cfg_changed();
  f->advance(2000);
  // 6 portions, 2 per day from today 09:00: the 6th is on day 3 at 18:00.
  CHECK(f->runway_days() == 2, "runway two whole days");
  CHECK(f->schedule_oversubscribed(), "8 portions needed in 4 days, 6 available");
  f->set_hold_indefinite();
  f->advance(2000);
  CHECK(f->next_event_epoch() == 0 && f->runway_days() == 255, "nothing projected during an indefinite hold");
}

static void test_clock_tracking() {
  current = "clock source tracking";
  Flash flash;
  auto f = boot(flash, T0, false);
  CHECK(!f->time_ever_synced() && f->time_source() == TimeSource::NONE, "never synced");
  CHECK(f->network_time_age_ms() < 0, "no network time yet");
  f->set_system_time(static_cast<uint32_t>(T0));
  CHECK(f->time_valid() && f->time_source() == TimeSource::BLE, "clock from BLE");
  CHECK(f->network_time_age_ms() < 0, "BLE does not count as network time");
  f->note_time_sync(TimeSource::NTP);
  f->advance(3 * 60 * 1000);
  CHECK(f->time_source() == TimeSource::NTP && f->time_age_min() == 3, "NTP, three minutes old");
  CHECK(f->network_time_age_ms() >= 180000, "network time age");
}

static void test_pulse_range() {
  current = "pulse range";
  Flash flash;
  {
    auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
    f->set_default_pulse_range(600, 2600);
    f->start();
    CHECK(f->min_pulse_us() == 600 && f->max_pulse_us() == 2600, "fresh device uses the YAML defaults");
    CHECK(f->set_pulse_range(654, 2770) == Result::OK, "valid range accepted");
    CHECK(f->set_pulse_range(1500, 1900) == Result::INVALID_ARG, "less than 500 us apart refused");
    CHECK(f->set_pulse_range(299, 2400) == Result::INVALID_ARG, "min below its limit refused");
    CHECK(f->set_pulse_range(544, 3001) == Result::INVALID_ARG, "max above its limit refused");
    CHECK(f->min_pulse_us() == 654 && f->max_pulse_us() == 2770, "refused changes leave the range alone");
    f->angles.clear();
    f->set_pulse_range(660, 2770);
    CHECK(f->angles.empty(), "no move outside calibration mode");
    f->set_calibration_mode(true);
    f->calibration_select(3);
    f->angles.clear();
    f->set_pulse_range(654, 2770);
    CHECK(f->angles.size() == 1, "calibration mode: servo moves to the selected position again");
    f->set_calibration_mode(false);
    CHECK(flash.have_settings && flash.settings.min_pulse_us == 654 && flash.settings.max_pulse_us == 2770,
          "saved with the settings");
  }
  {
    auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
    f->set_default_pulse_range(544, 2400);
    f->start();
    CHECK(f->min_pulse_us() == 654 && f->max_pulse_us() == 2770, "saved range wins over the YAML defaults");
  }
  {
    flash.settings.min_pulse_us = 0;  // corrupt
    auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
    f->set_default_pulse_range(544, 2400);
    f->start();
    CHECK(f->min_pulse_us() == 544 && f->max_pulse_us() == 2400, "an invalid saved range falls back to the defaults");
  }
}

static void test_next_feed_skips_fired_event() {
  current = "next feed skips an event that already fed today";
  Flash flash;
  auto f = boot(flash, T0);
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
  f->advance((5 * 60 + 20) * 1000LL);  // to 08:00:20
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "fired at 08:00");
  // Move the same event later today: it has fed today, so it next feeds tomorrow at the new time.
  f->cfg.profiles[0].ev[0].offset_min = 9 * 60;
  f->cfg_changed();
  f->advance(3000);
  CHECK(f->next_event_epoch() == static_cast<uint32_t>(local_epoch(2026, 10, 15, 9, 0)),
        "Next Feed is tomorrow 09:00, not today");
  f->advance(61 * 60 * 1000LL);  // past 09:00 today
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "not fed again today");
}

static void test_clock_kept_across_restart() {
  current = "clock kept across a restart";
  Flash flash;
  auto f = boot(flash, T0, true);
  CHECK(f->clock_kept() && f->time_source() == TimeSource::NONE, "valid at boot: kept, no source");
  CHECK(f->time_source_text() == "kept across restart", "shown as kept");
  CHECK(f->network_time_age_ms() < 0, "no network sync yet, so a BLE clock push is not blocked");
  f->note_time_sync(TimeSource::NTP);
  CHECK(f->time_source_text() == "NTP" && f->network_time_age_ms() >= 0, "a real sync takes over");
  Flash flash2;
  auto g = boot(flash2, T0, false);
  CHECK(!g->clock_kept() && g->time_source_text() == "none", "power-on without a clock: not kept");
}

static void test_edit_portions_24h() {
  current = "edit the 24 h count";
  Flash flash;
  auto f = boot(flash, T0);
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "feed 1");
  f->advance(61000);
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "feed 2");
  f->advance(61000);
  CHECK(f->portions_24h() == 2, "two portions counted");
  f->set_portions_24h(0);
  CHECK(f->portions_24h() == 0, "lowered to 0");
  CHECK(f->position() == 2, "the tray did not move");
  f->set_portions_24h(3);
  CHECK(f->portions_24h() == 3, "raised to 3");
  CHECK(f->recent_feeds_text().find("adjustment") != std::string::npos, "adjustment shown in the history");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "3 + 1 is within the cap of 4");
  f->advance(61000);
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::DAILY_CAP, "4 counted: refused");
  f->set_portions_24h(2);  // takes 2 of the 3 adjustment portions (oldest first)
  CHECK(f->portions_24h() == 2, "lowered to 2 (partial entry)");
  CHECK(flash.have_state, "saved");
  int adjustments = 0;
  for (const auto &r : f->records)
    if (r.source == static_cast<uint8_t>(Source::ADJUSTMENT))
      adjustments++;
  CHECK(adjustments == 0, "an adjustment is never reported as a feed");
}

static void test_cap_warning_and_edited_summary() {
  current = "over-cap warning and edited-profile summary";
  Flash flash;
  auto f = boot(flash, T0);
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60, 2);
  enable_event(*f, 0, 1, Anchor::CLOCK, 18 * 60, 2);
  CHECK(f->max_portions_per_day() == 4 && !f->schedule_over_cap(), "2 + 2 fits the default cap of 4");
  enable_event(*f, 0, 2, Anchor::CLOCK, 20 * 60, 1, 0x02);  // Mondays only
  CHECK(f->max_portions_per_day() == 5 && f->schedule_over_cap(), "5 on Mondays exceeds the cap");
  CHECK(f->faults_text().find("daily cap") != std::string::npos, "shown in Faults");
  f->cfg.profiles[0].fast_days = 0x02;  // fast on Mondays
  f->cfg_changed();
  CHECK(f->max_portions_per_day() == 4 && !f->schedule_over_cap(), "a fast day does not count");
  f->cfg.profiles[0].fast_days = 0;
  f->cfg.scheduler_enabled = 0;
  CHECK(!f->schedule_over_cap(), "no warning while the scheduler is off");
  f->cfg.scheduler_enabled = 1;

  f->editor_profile = 0;
  std::string s = f->edited_profile_summary();
  CHECK(s.rfind("A: 08:00 x2", 0) == 0, "edited summary lists profile A");
  CHECK(s.find("[active]") != std::string::npos && s.find("5 portions/day > cap 4") != std::string::npos,
        "marked active, with the cap warning");
  f->editor_profile = 1;
  s = f->edited_profile_summary();
  CHECK(s.find("B: no events") == 0 && s.find("[not active]") != std::string::npos, "profile B: empty, not active");
  CHECK(f->schedule_summary().rfind("A:", 0) == 0, "Active Schedule still shows the active profile");
}

static void test_needs_food() {
  current = "needs food";
  Flash flash;
  auto f = boot(flash, T0);  // 07:55, tray full (6)
  CHECK(!f->needs_food(), "full tray, nothing scheduled");
  enable_event(*f, 0, 0, Anchor::CLOCK, 18 * 60, 2);
  f->advance(3000);
  CHECK(f->next_event_portions() == 2 && !f->needs_food(), "next feed 2, 6 remain");
  f->declare_position(5);  // 1 portion left
  f->advance(3000);
  CHECK(f->needs_food(), "next feed 2, only 1 remains");
  CHECK(f->faults_text().find("more portions than remain") != std::string::npos, "shown in Faults");
  f->cfg.profiles[0].ev[0].portions = 1;
  f->cfg_changed();
  f->advance(3000);
  CHECK(!f->needs_food(), "next feed 1, 1 remains");
  f->declare_position(6);
  f->advance(3000);
  CHECK(f->needs_food(), "empty");
  f->cfg.profiles[0].ev[0].enabled = 0;
  f->cfg_changed();
  f->advance(3000);
  CHECK(f->next_event_portions() == 0 && f->needs_food(), "empty, even with nothing scheduled");
  f->declare_position(3);
  f->advance(3000);
  CHECK(!f->needs_food(), "nothing scheduled, 3 remain");
  CHECK(f->request_feed(1, Source::HOME_ASSISTANT) == Result::OK, "feed");
  f->advance(300);
  f->stop();  // interrupted: position unknown
  CHECK(f->position_unknown() && !f->needs_food(), "position unknown: not 'needs food'");
}

int main() {
  test_feed_motion();
  test_guard();
  test_power_loss_mid_step_and_button();
  test_schedule_fires_once_and_not_late();
  test_retry_is_silent();
  test_refusal_reported_once();
  test_oneshot_and_scheduler_switch();
  test_cap_after_reboot_without_clock();
  test_reload_pause_survives_reboot();
  test_sunset_after_midnight();
  test_cadence_and_fast_days();
  test_projection();
  test_clock_tracking();
  test_pulse_range();
  test_next_feed_skips_fired_event();
  test_clock_kept_across_restart();
  test_edit_portions_24h();
  test_cap_warning_and_edited_summary();
  test_needs_food();
  if (failures == 0)
    std::printf("All core tests passed\n");
  return failures == 0 ? 0 : 1;
}
