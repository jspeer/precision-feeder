// Host-side tests for the feeder logic (components/precision_feeder/feeder_core.*), with a simulated clock.
// Build and run:
//   c++ -std=c++17 -Wall -Wextra -I../components/precision_feeder test_core.cpp ../components/precision_feeder/feeder_*.cpp -o /tmp/test_core && /tmp/test_core
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

#include "feeder_core.h"
#include "firmware_upload.h"

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

#include "fake_feeder.h"
#include "tz_golden.h"

// ---- tests -----------------------------------------------------------------------------------

static void test_feed_motion() {
  current = "feed motion";
  Flash flash;
  auto f = boot(flash, T0);
  CHECK(f->position() == 0 && f->remaining() == 6, "fresh tray");
  f->angles.clear();
  const int releases = f->releases;
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed accepted");
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
  CHECK(f->count(Source::ESPHOME, Result::OK) == 1, "one accepted record");
}

static void test_guard() {
  current = "guard";
  Flash flash;
  auto f = boot(flash, T0);
  CHECK(f->request_feed(2, Source::ESPHOME) == Result::PORTION_LIMIT, "per-command limit (default 1)");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "first feed");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::BUSY, "busy while moving");
  f->advance(5000);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::TOO_SOON, "minimum gap");
  for (int i = 0; i < 3; i++) {
    f->advance(61000);
    CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "spaced feeds accepted");
  }
  f->advance(61000);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::DAILY_CAP, "daily cap of 4 reached");
  f->set_calibration_mode(true);
  CHECK(f->request_feed(1, Source::BUTTON) == Result::CALIBRATING, "refused while calibrating");
  f->set_calibration_mode(false);
  CHECK(f->calib.offsets[0] == 0, "calibration untouched");

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
    CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed starts");
    f->advance(500);  // power lost in the middle of the step
  }
  auto f = boot(flash, T0 + 10);
  CHECK(f->position_unknown() && f->tray_state() == TrayState::UNKNOWN, "position unknown after reboot");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::UNKNOWN_POSITION, "feeds refused");
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
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "manual feed at 07:59:30");
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
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "manual feeds still work during a hold");

  // A definitive refusal (daily cap) is reported once, not every second.
  Flash flash2;
  auto g = boot(flash2, T0);
  g->cfg.daily_cap = 1;
  enable_event(*g, 0, 0, Anchor::CLOCK, 7 * 60 + 56);
  CHECK(g->request_feed(1, Source::ESPHOME) == Result::OK, "use up the cap");
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
      CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "four feeds");
      f->advance(61000);
    }
    CHECK(f->request_feed(1, Source::ESPHOME) == Result::DAILY_CAP, "cap reached");
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
    CHECK(flash.have_calibration && flash.calibration.min_pulse_us == 654 && flash.calibration.max_pulse_us == 2770,
          "saved with the settings");
  }
  {
    auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
    f->set_default_pulse_range(544, 2400);
    f->start();
    CHECK(f->min_pulse_us() == 654 && f->max_pulse_us() == 2770, "saved range wins over the YAML defaults");
  }
  {
    flash.calibration.min_pulse_us = 0;  // corrupt
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
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed 1");
  f->advance(61000);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed 2");
  f->advance(61000);
  CHECK(f->portions_24h() == 2, "two portions counted");
  f->set_portions_24h(0);
  CHECK(f->portions_24h() == 0, "lowered to 0");
  CHECK(f->position() == 2, "the tray did not move");
  f->set_portions_24h(3);
  CHECK(f->portions_24h() == 3, "raised to 3");
  CHECK(f->recent_feeds_text().find("adjustment") != std::string::npos, "adjustment shown in the history");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "3 + 1 is within the cap of 4");
  f->advance(61000);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::DAILY_CAP, "4 counted: refused");
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
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
  f->advance(300);
  f->stop();  // interrupted: position unknown
  CHECK(f->position_unknown() && !f->needs_food(), "position unknown: not 'needs food'");
}

static void test_polar_fallback() {
  current = "polar day fallback";
  Flash flash;
  auto f = boot(flash, T0);  // 2026-10-14 07:55 local
  enable_event(*f, 0, 0, Anchor::SUNSET, -60);  // sunset - 1 h
  f->advance(2000);
  CHECK(f->solar_fault() && !f->polar_fallback(), "no location: sun event skipped, fault");
  CHECK(f->next_event_epoch() == 0, "no location: nothing scheduled");
  // 89 degrees north in mid October: the sun does not rise (polar night).
  f->cfg.latitude = 89.0f;
  f->cfg.longitude = 0.0f;
  f->cfg_changed();
  f->advance(2000);
  CHECK(f->polar_fallback() && f->solar_fault(), "polar night: fallback in use, still flagged");
  CHECK(f->faults_text().find("06:00 / 18:00") != std::string::npos, "Faults explains the fallback");
  CHECK(f->next_event_epoch() == static_cast<uint32_t>(local_epoch(2026, 10, 14, 17, 0)),
        "sunset - 1 h resolves to 17:00 (18:00 fallback)");
  f->advance((9 * 3600 + 5 * 60 + 20) * 1000LL);  // to 17:00:20
  CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "fed at 17:00");
  // A normal latitude has a real sunset: no fallback.
  f->cfg.latitude = 51.5f;
  f->cfg_changed();
  f->advance(2000);
  CHECK(!f->polar_fallback() && !f->solar_fault(), "normal latitude: no fallback, no fault");
}

static void test_review_fixes() {
  current = "review fixes";
  {
    // A sun event that resolves to the evening before its day still feeds.
    Flash flash;
    auto f = boot(flash, local_epoch(2026, 10, 14, 12, 0));
    f->cfg.latitude = 51.5f;
    f->cfg.longitude = -0.13f;
    f->cfg_changed();
    enable_event(*f, 0, 0, Anchor::SUNRISE, -600);  // sunrise - 10 h
    f->advance(3000);
    const uint32_t due = f->next_event_epoch();
    CHECK(due > static_cast<uint32_t>(f->now()), "next feed is ahead");
    f->advance((static_cast<int64_t>(due) - f->now() + 30) * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "pre-midnight sun event fed in its minute");
    f->advance(3 * 3600 * 1000LL);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "and only once");
  }
  {
    // Holds are written to flash straight away.
    Flash flash;
    auto f = boot(flash, T0);
    f->set_hold_minutes(60);
    CHECK(flash.state_flushed && flash.state.hold_until != 0, "timed hold flushed");
    f->set_hold_indefinite();
    CHECK(flash.state_flushed && flash.state.hold_until == HOLD_FOREVER, "indefinite hold flushed");
    f->cancel_hold();
    CHECK(flash.state_flushed && flash.state.hold_until == 0, "cancel flushed");
  }
  {
    // A boot with a valid clock and nothing new to anchor does not rewrite the settings.
    Flash flash;
    { auto f = boot(flash, T0); f->advance(5000); }
    const int saves = flash.settings_saves;
    { auto f = boot(flash, T0 + 3600); f->advance(5000); }
    CHECK(flash.settings_saves == saves, "no settings write on an ordinary boot");
  }
  {
    // Day names: three letters or the full name, nothing else.
    uint8_t m = 0;
    CHECK(FeederCore::parse_days("monday, wed fridays", m) && m == 0x2A, "full, short and plural names");
    CHECK(!FeederCore::parse_days("sunshine", m), "a word that only starts with a day name is refused");
    CHECK(!FeederCore::parse_days("mo", m), "too short is refused");
  }
  {
    // Summaries list events in time order, not slot order.
    Flash flash;
    auto f = boot(flash, T0);
    enable_event(*f, 0, 0, Anchor::CLOCK, 20 * 60);
    enable_event(*f, 0, 1, Anchor::CLOCK, 8 * 60);
    CHECK(f->schedule_summary().rfind("A: 08:00 x1, 20:00 x1", 0) == 0, "time order");
  }
}

static void test_settings_migration() {
  current = "settings migration";
  {
    // Firmware 0.3.0 to 0.4.x saved one record (version 3) with the calibration inside: keep all of it.
    Flash flash;
    flash.have_v3 = true;
    flash.v3.version = 3;
    for (auto &p : flash.v3.profiles)
      p.every_n = 1;
    flash.v3.profiles[1].ev[2] = Event{1, static_cast<uint8_t>(Anchor::SUNSET), -30, 2, 0x3E};
    flash.v3.profiles[1].every_n = 2;
    flash.v3.cal[3] = 28;
    flash.v3.cal[5] = -10;
    flash.v3.latitude = 51.5f;
    flash.v3.longitude = -0.13f;
    flash.v3.min_gap_s = 90;
    flash.v3.portion_gap_s = 5;
    flash.v3.reload_pause_min = 0;
    flash.v3.refill_interval_days = 3;
    flash.v3.daily_cap = 3;
    flash.v3.max_portions_cmd = 1;
    flash.v3.active_profile = 1;
    flash.v3.scheduler_enabled = 1;
    flash.v3.min_pulse_us = 637;
    flash.v3.max_pulse_us = 2675;
    auto f = boot(flash, T0);
    const Event &e = f->cfg.profiles[1].ev[2];
    CHECK(e.enabled && e.anchor == static_cast<uint8_t>(Anchor::SUNSET) && e.offset_min == -30 && e.portions == 2 &&
              e.days == 0x3E,
          "event migrated");
    CHECK(f->cfg.profiles[1].every_n == 2 && f->cfg.active_profile == 1 && f->cfg.daily_cap == 3 &&
              f->cfg.min_gap_s == 90 && f->cfg.latitude == 51.5f,
          "profile and guard settings migrated");
    CHECK(f->calib.offsets[3] == 28 && f->calib.offsets[5] == -10 && f->min_pulse_us() == 637 &&
              f->max_pulse_us() == 2675,
          "calibration migrated");
    f->advance(3000);
    CHECK(flash.have_settings && flash.settings.version == SETTINGS_VERSION && flash.have_calibration &&
              flash.calibration.min_pulse_us == 637,
          "saved in the new format");
    flash.have_v3 = false;  // (the old record would still be there; it is ignored once current settings exist)
    auto g = boot(flash, T0 + 60);
    CHECK(g->cfg.profiles[1].ev[2].offset_min == -30 && g->calib.offsets[3] == 28, "and loads from it next boot");
  }
  {
    // A future settings change: settings unreadable, calibration record intact -> calibration survives.
    Flash flash;
    {
      auto f = boot(flash, T0);
      f->set_calibration_offset_at(2, 33);
      f->set_pulse_range(640, 2600);
      f->save_calibration();
    }
    flash.settings.version = 999;  // pretend the layout changed
    auto g = boot(flash, T0 + 60);
    CHECK(g->calib.offsets[2] == 33 && g->min_pulse_us() == 640, "calibration kept when settings are reset");
  }
}

static void test_bug_batch_b() {
  current = "bug batch B2-B6";
  {
    // B2: an event inside the reload pause is not shown as Next Feed (it will be skipped).
    Flash flash;
    auto f = boot(flash, T0);  // 07:55
    enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
    enable_event(*f, 0, 1, Anchor::CLOCK, 9 * 60);
    f->tray_reload();  // pause until 08:05
    f->advance(3000);
    CHECK(f->next_event_epoch() == static_cast<uint32_t>(local_epoch(2026, 10, 14, 9, 0)),
          "Next Feed skips 08:00 (inside the pause) and shows 09:00");
    f->advance(70 * 60 * 1000LL);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "08:00 skipped, 09:00 fed");
  }
  {
    // B3: settings edits are flushed, not left for ESPHome's write interval.
    Flash flash;
    auto f = boot(flash, T0);
    f->cfg.daily_cap = 3;
    f->cfg_changed();
    f->advance(3000);
    CHECK(flash.settings_flushed && flash.settings.daily_cap == 3, "settings save is flushed");
  }
  {
    // B5: a 2-portion feed stopped after the first portion counts 1, and the history says x1.
    Flash flash;
    auto f = boot(flash, T0);
    f->cfg.max_portions_cmd = 2;
    CHECK(f->request_feed(2, Source::ESPHOME) == Result::OK, "2-portion feed");
    f->advance(3500);  // first portion done, in the gap
    f->stop();
    CHECK(f->position() == 1 && f->portions_24h() == 1, "counts the one portion that dropped");
    CHECK(f->recent_feeds_text().find("x1 ESPHome") != std::string::npos, "history says x1");
    CHECK(flash.state.hist_n[0] == 1, "saved");
    // Stopped in the middle of the second step: that step counts, nothing after it.
    Flash flash2;
    auto g = boot(flash2, T0);
    g->cfg.max_portions_cmd = 3;
    g->cfg.portion_gap_s = 1;
    CHECK(g->request_feed(3, Source::ESPHOME) == Result::OK, "3-portion feed");
    g->advance(3000 + 1000 + 500);  // first portion, the gap, half a second into the second step
    g->stop();
    CHECK(g->position_unknown() && g->portions_24h() == 2, "mid-step stop: 2 counted (1 done, 1 maybe), 1 never started");
  }
  {
    // B6: out-of-range guard settings are clamped on load.
    Flash flash;
    {
      auto f = boot(flash, T0);
      f->cfg.min_gap_s = 0;
      f->cfg.portion_gap_s = 0;
      f->cfg.refill_interval_days = 99;
      f->cfg.latitude = NAN;
      f->cfg_changed();
      f->advance(3000);
    }
    auto g = boot(flash, T0 + 60);
    CHECK(g->cfg.min_gap_s == 10 && g->cfg.portion_gap_s == 1 && g->cfg.refill_interval_days == 30 &&
              g->cfg.latitude == 0.0f,
          "clamped on load");
  }
}

// R1 guard: the projection (Next Feed) and the scheduler must agree. For random schedules, every scheduled feed
// must have been predicted (same minute), and every prediction must end in a feed or a reported refusal.
static void test_projection_matches_firing() {
  current = "projection matches firing";
  uint32_t seed = 12345;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1103515245u + 12345u;
    return (seed >> 8) % n;
  };
  int predictions_checked = 0;
  for (int run = 0; run < 25; run++) {
    Flash flash;
    auto f = boot(flash, T0);
    f->cfg.latitude = 40.0f + static_cast<float>(rnd(20));
    f->cfg.longitude = -80.0f;
    f->cfg.daily_cap = 6;
    f->cfg.min_gap_s = 10;
    f->cfg.reload_pause_min = 0;
    Profile &p = f->cfg.profiles[0];
    p.every_n = static_cast<uint8_t>(1 + rnd(2));
    p.fast_days = rnd(3) == 0 ? static_cast<uint8_t>(1 << rnd(7)) : 0;
    for (uint8_t e = 0; e < EVENTS_PER_PROFILE; e++) {
      Event &ev = p.ev[e];
      ev.enabled = rnd(4) != 0;
      ev.anchor = static_cast<uint8_t>(rnd(3));
      // Spread events out (different hours) so two never compete for the same minute.
      ev.offset_min = ev.anchor == 0 ? static_cast<int16_t>(e * 300 + 60 + rnd(200))
                                     : static_cast<int16_t>(static_cast<int>(rnd(240)) - 120 + (e - 2) * 60);
      ev.portions = 1;
      ev.days = rnd(4) == 0 ? static_cast<uint8_t>(0x7F & ~(1 << rnd(7))) : 0x7F;
    }
    f->cfg_changed();
    f->advance(2000);
    int64_t predicted = 0;
    size_t seen = f->records.size();
    for (int s = 0; s < 3 * 86400; s++) {
      if (f->remaining() < 2) {
        f->tray_reload();
        f->advance(2000, 1000);
        s += 2;
      }
      const int64_t next = f->next_event_epoch();
      if (next != 0)
        predicted = next;
      f->advance(1000, 1000);
      for (; seen < f->records.size(); seen++) {
        const FeedRecord &r = f->records[seen];
        if (r.source != static_cast<uint8_t>(Source::SCHEDULE))
          continue;
        predictions_checked++;
        CHECK(predicted != 0 && r.epoch >= predicted && r.epoch < predicted + 60,
              "a scheduled feed (or refusal) happened in the predicted minute");
      }
      if (predicted != 0 && f->now() > predicted + 120) {
        // The predicted minute has passed: something must have been reported for it.
        bool reported = false;
        for (const FeedRecord &r : f->records)
          if (r.source == static_cast<uint8_t>(Source::SCHEDULE) && r.epoch >= predicted && r.epoch < predicted + 60)
            reported = true;
        CHECK(reported, "a predicted feed did not silently vanish");
        predicted = 0;
      }
    }
  }
  CHECK(predictions_checked > 50, "enough scheduled feeds were checked");
}

// ---- daylight saving -----------------------------------------------------------------------------------
// A second fake with US Eastern rules (EST -5, EDT -4; second Sunday of March 02:00 -> 03:00, first Sunday of
// November 02:00 -> 01:00), converting local times the way mktime does: a time in the skipped hour reads as standard
// time (so 02:30 becomes 03:30 EDT), and a repeated hour resolves to its first (daylight-time) occurrence.

static int64_t eastern_utc(int y, int mo, int d, int h, int mi, int off_h) {
  return static_cast<int64_t>(days_from_civil(y, mo, d)) * 86400 + h * 3600 + mi * 60 - off_h * 3600;
}
static int nth_sunday(int y, int mo, int n) {
  for (int d = 1, c = 0; d <= 31; d++)
    if (weekday_from_days(days_from_civil(y, mo, d)) == 0 && ++c == n)
      return d;
  return 0;
}
static int eastern_offset_h(int64_t utc) {
  int y;
  unsigned m, d;
  civil_from_days(static_cast<int32_t>(utc / 86400), y, m, d);
  const int64_t start = eastern_utc(y, 3, nth_sunday(y, 3, 2), 2, 0, -5);
  const int64_t end = eastern_utc(y, 11, nth_sunday(y, 11, 1), 2, 0, -4);
  return (utc >= start && utc < end) ? -4 : -5;
}

class DstFeeder : public FeederCore {
 public:
  explicit DstFeeder(int64_t epoch) : ms_(epoch * 1000) {
    this->set_feed_listener([this](const FeedRecord &r) {
      if (r.source == static_cast<uint8_t>(Source::SCHEDULE) && r.result == 0)
        this->fed_local.push_back(this->local_hhmm(r.epoch));
    });
    this->core_start(nullptr, nullptr, nullptr);
    this->cfg.min_gap_s = 10;
  }
  void advance_s(int64_t s) {
    for (int64_t t = 0; t < s; t++) {
      up_ += 1000;
      ms_ += 1000;
      this->core_loop();
    }
  }
  int local_hhmm(int64_t e) {
    const LocalTime l = this->hal_local(e);
    return l.hour * 100 + l.minute;
  }
  std::vector<int> fed_local;

 protected:
  uint32_t hal_millis() override { return static_cast<uint32_t>(up_); }
  int64_t hal_uptime_ms() override { return up_; }
  bool hal_time_valid() override { return true; }
  int64_t hal_epoch() override { return ms_ / 1000; }
  void hal_set_epoch(int64_t e) override { ms_ = e * 1000; }
  LocalTime hal_local(int64_t e) override {
    const int64_t l = e + eastern_offset_h(e) * 3600;
    int y;
    unsigned m, d;
    civil_from_days(static_cast<int32_t>(l / 86400), y, m, d);
    const int64_t r = ((l % 86400) + 86400) % 86400;
    return LocalTime{y, static_cast<int>(m), static_cast<int>(d), static_cast<int>(r / 3600),
                     static_cast<int>(r % 3600 / 60), static_cast<int>(r % 60)};
  }
  int64_t hal_local_to_epoch(int y, int mo, int d, int h, int mi) override {
    for (int off : {-4, -5}) {
      const int64_t u = eastern_utc(y, mo, d, h, mi, off);
      if (eastern_offset_h(u) == off)
        return u;
    }
    return eastern_utc(y, mo, d, h, mi, -5);  // in the skipped hour: read as standard time
  }
  void hal_servo_write(float) override {}
  void hal_servo_release() override {}
  bool hal_save_settings(const Settings &, bool) override { return true; }
  void hal_save_calibration(const Calibration &, bool) override {}
  bool hal_save_state(const State &, bool) override { return true; }
  void hal_save_device(const Device &) override {}
  void hal_save_tuning(const Tuning &, bool) override {}
  void hal_save_motor_pins(const MotorPins &) override {}
  void hal_apply_timezone(const TzRules *) override {}
  void hal_random(uint8_t *out, size_t n) override { std::memset(out, 0, n); }

  int64_t up_{0};
  int64_t ms_;
};

/// Feeds an event at `minutes` past midnight from noon the day before `y-mo-d` for three days; returns local times.
static std::vector<int> dst_run(int y, int mo, int d, int16_t minutes, Anchor anchor = Anchor::CLOCK) {
  const int64_t start = eastern_utc(y, mo, d - 1, 12, 0, eastern_offset_h(eastern_utc(y, mo, d - 1, 12, 0, -5)));
  DstFeeder f(start);
  f.cfg.latitude = 40.7f;
  f.cfg.longitude = -74.0f;
  f.cfg.profiles[0].ev[0] = Event{1, static_cast<uint8_t>(anchor), minutes, 1, 0x7F};
  f.cfg_changed();
  f.advance_s(3 * 86400);
  return f.fed_local;
}

static void test_daylight_saving() {
  current = "daylight saving";
  using V = std::vector<int>;
  CHECK(dst_run(2027, 3, 14, 2 * 60 + 30) == (V{330, 230, 230}), "spring forward: 02:30 does not exist, feeds at 03:30, once");
  CHECK(dst_run(2027, 3, 14, 8 * 60) == (V{800, 800, 800}), "spring forward: 08:00 stays 08:00");
  CHECK(dst_run(2026, 11, 1, 1 * 60 + 30) == (V{130, 130, 130}), "fall back: 01:30 happens twice, feeds once");
  CHECK(dst_run(2026, 11, 1, 8 * 60) == (V{800, 800, 800}), "fall back: 08:00 stays 08:00");
  CHECK(dst_run(2026, 11, 1, 23 * 60 + 30) == (V{2330, 2330, 2330}), "fall back: 23:30 stays 23:30");
  // Sunset moves by about an hour on the clock across the change; it must still feed once a day.
  const V sunset = dst_run(2026, 11, 1, 0, Anchor::SUNSET);
  CHECK(sunset.size() == 3, "sunset event feeds once a day across the change");
  if (sunset.size() == 3)
    CHECK(sunset[0] >= 1730 && sunset[0] <= 1800 && sunset[1] >= 1630 && sunset[1] <= 1700,
          "and follows the clock change (about 17:45 EDT, then about 16:45 EST)");
}

static void test_web_app_data() {
  current = "web app data (1.1.0)";
  // Schedule Data: every profile and event, compact.
  Flash flash;
  auto f = boot(flash, T0);  // Wed 2026-10-14 07:55, UTC-5
  CHECK(f->schedule_data_text() ==
            "1|1,00,0c480/1/7f,0c720/1/7f,0c1080/1/7f,0c1200/1/7f"
            "|1,00,0c480/1/7f,0c720/1/7f,0c1080/1/7f,0c1200/1/7f"
            "|1,00,0c480/1/7f,0c720/1/7f,0c1080/1/7f,0c1200/1/7f",
        "defaults");
  f->cfg.profiles[1].every_n = 14;
  f->cfg.profiles[1].fast_days = 0x41;
  enable_event(*f, 1, 2, Anchor::SUNSET, -720, 6, 0x3E);
  const std::string data = f->schedule_data_text();
  CHECK(data.find("|14,41,0c480/1/7f,0c720/1/7f,1s-720/6/3e,0c1200/1/7f|") != std::string::npos, "every field");
  CHECK(data.size() < 255, "fits Home Assistant's state");
  enable_event(*f, 1, 2, Anchor::CLOCK, 18 * 60);  // back as it was, for the rest of the test
  f->cfg.profiles[1].ev[2].enabled = 0;

  // Device Time.
  CHECK(f->device_time_text() == "Wed 2026-10-14 07:55 -05:00", "local time and offset");

  // Today's Feeds.
  CHECK(f->todays_feeds_text() == "none", "no events");
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
  enable_event(*f, 0, 1, Anchor::CLOCK, 20 * 60, 2);
  enable_event(*f, 0, 2, Anchor::CLOCK, 9 * 60, 1, 0x01);  // Sundays only: not today
  CHECK(f->todays_feeds_text() == "08:00 x1 next; 20:00 x2 later", "next and later");
  f->advance(5 * 60 * 1000LL + 20000);  // 08:00:20, fed at 08:00
  CHECK(f->todays_feeds_text() == "08:00 x1 fed; 20:00 x2 next", "fed");
  CHECK(f->add_oneshot("2026-10-14 12:00", 1), "one-shot today");
  CHECK(f->add_oneshot("2026-10-15 12:00", 1), "one-shot tomorrow (not listed)");
  CHECK(f->todays_feeds_text() == "08:00 x1 fed; 12:00 x1 next one-shot; 20:00 x2 later", "one-shot listed");
  CHECK(f->set_hold_minutes(6 * 60) == Result::OK, "hold until 14:00");
  CHECK(f->todays_feeds_text() == "08:00 x1 fed; 12:00 x1 held one-shot; 20:00 x2 next", "held");
  f->cancel_hold();
  f->cfg.scheduler_enabled = 0;
  f->cfg_changed();
  CHECK(f->todays_feeds_text() == "12:00 x1 next one-shot", "one-shots run with the scheduler off");
  f->clear_oneshots();
  CHECK(f->todays_feeds_text() == "none (scheduler off)", "scheduler off");
  f->cfg.scheduler_enabled = 1;
  f->cfg.profiles[0].fast_days = 0x08;  // Wednesday
  f->cfg_changed();
  CHECK(f->todays_feeds_text() == "none (fast day)", "fast day");
  f->cfg.profiles[0].fast_days = 0;
  f->cfg.profiles[0].every_n = 2;
  f->cfg.profiles[0].anchor_day = static_cast<uint32_t>(days_from_civil(2026, 10, 13));
  f->cfg_changed();
  CHECK(f->todays_feeds_text() == "none (not a feeding day)", "off day of the cycle");

  // Missed (the device was off) and refused (the daily cap) both show as skipped.
  Flash flash2;
  auto g = boot(flash2, T0);
  enable_event(*g, 0, 0, Anchor::CLOCK, 8 * 60);
  enable_event(*g, 0, 1, Anchor::CLOCK, 8 * 60 + 5);
  g->advance(2000);
  Flash flash3 = flash2;
  auto h = boot(flash3, local_epoch(2026, 10, 14, 8, 2));  // off through 08:00
  h->cfg.daily_cap = 1;
  h->set_portions_24h(1);  // the cap is already used
  CHECK(h->portions_24h() == 1, "cap used");
  h->advance(4 * 60 * 1000LL);  // through 08:05, refused by the cap
  CHECK(h->todays_feeds_text() == "08:00 x1 skipped; 08:05 x1 skipped", "missed and refused");
  h->set_clock_valid(false);
  CHECK(h->todays_feeds_text() == "clock not set" && h->device_time_text() == "clock not set", "no clock");

  // Feed Guard: what a one-portion feed would get, without feeding.
  Flash flash4;
  auto k = boot(flash4, T0);
  CHECK(k->feed_guard_text() == "ready", "ready");
  CHECK(k->request_feed(1, Source::ESPHOME) == Result::OK, "feed");
  CHECK(k->feed_guard_text() == "busy", "busy while moving");
  k->advance(5000);
  CHECK(k->feed_guard_text() == "too soon", "minimum gap");
  const size_t records = k->records.size();
  const uint8_t position = k->position();
  CHECK(k->feed_guard_preview() == Result::TOO_SOON, "as a result");
  CHECK(k->records.size() == records && k->position() == position && k->last_result() == Result::OK,
        "the preview reports, records and moves nothing");
  k->set_calibration_mode(true);
  CHECK(k->feed_guard_text() == "calibrating", "calibrating");
  k->set_calibration_mode(false);
  k->advance(61000);
  k->cfg.daily_cap = 1;
  CHECK(k->feed_guard_text() == "daily cap reached", "daily cap");
}

static bool same_rule(const TzRule &r, const TzGoldenRule &g) {
  return r.time_s == g.time_s && r.day == g.day && static_cast<int>(r.type) == g.type && r.month == g.month &&
         r.week == g.week && r.day_of_week == g.day_of_week;
}

static void test_tz_parser() {
  current = "POSIX TZ parser";
  // Every timezone in the IANA database parses to exactly what ESPHome's reference parser gives.
  int n = 0;
  for (const auto &g : TZ_GOLDEN) {
    TzRules tz;
    const bool ok = parse_tz(g.text, tz);
    if (!ok || tz.std_offset_s != g.std_offset_s || tz.dst_offset_s != g.dst_offset_s ||
        !same_rule(tz.dst_start, g.start) || !same_rule(tz.dst_end, g.end)) {
      std::printf("  mismatch: %s\n", g.text);
      CHECK(false, "golden timezone");
    }
    n++;
  }
  CHECK(n > 80, "golden table present");

  TzRules tz;
  CHECK(parse_tz("EST5EDT,M3.2.0,M11.1.0", tz) && tz.has_dst() && tz.std_offset_s == 18000 &&
            tz.dst_offset_s == 14400 && tz.dst_start.time_s == 7200,
        "DST one hour ahead and 02:00 by default");
  CHECK(parse_tz("<+0545>-5:45", tz) && !tz.has_dst() && tz.std_offset_s == -(5 * 3600 + 45 * 60), "quoted name");
  CHECK(parse_tz("UTC0", tz) && tz.std_offset_s == 0, "UTC");
  CHECK(parse_tz("XXX3YYY,J60/-1:30,300/26", tz) && tz.dst_start.type == TzRuleType::JULIAN_NO_LEAP &&
            tz.dst_start.time_s == -5400 && tz.dst_end.type == TzRuleType::DAY_OF_YEAR && tz.dst_end.time_s == 93600,
        "Julian and day-of-year rules with times");

  // Refused: typed by a person, so anything doubtful is an error rather than a guess.
  const char *bad[] = {
      "",                            // empty
      "America/New_York",            // an IANA name, not a POSIX string
      "EST",                         // no offset
      "ES5",                         // name too short
      "<+0>0",                       // quoted name too short
      "<+05-5",                      // unterminated quoted name
      "EST25",                       // offset beyond 24 h
      "EST5:60",                     // minutes out of range
      "EST5 ",                       // trailing space
      "EST5EDT",                     // daylight saving with no rules
      "EST5EDT,M3.2.0",              // only one rule
      "EST5EDT,M13.2.0,M11.1.0",     // month 13
      "EST5EDT,M3.6.0,M11.1.0",      // week 6
      "EST5EDT,M3.2.7,M11.1.0",      // weekday 7
      "EST5EDT,J0,J300",             // Julian day 0
      "EST5EDT,366,300",             // day of year 366
      "EST5EDT,M3.2.0/168,M11.1.0",  // transition beyond 167 h
      "EST5EDT,M3.2.0,M11.1.0,",     // trailing comma
      "EST5EDT,m3.2.0,m11.1.0",      // lower-case rule letter
      "EST5,M3.2.0,M11.1.0",         // rules without a DST name
      "EST999999",                   // number too long
  };
  for (const char *text : bad) {
    TzRules before;
    before.std_offset_s = 123;
    if (parse_tz(text, before)) {
      std::printf("  accepted: '%s'\n", text);
      CHECK(false, "bad timezone refused");
    }
    CHECK(before.std_offset_s == 123, "output untouched on failure");
  }
  std::string longest = "<" + std::string(60, 'A') + ">5";  // 63 characters: the limit
  CHECK(parse_tz(longest.c_str(), tz), "63 characters accepted");
  longest.insert(1, "A");
  CHECK(!parse_tz(longest.c_str(), tz), "64 characters refused");
  CHECK(!parse_tz(nullptr, tz), "null refused");
}

static void test_device_record() {
  current = "device record";
  Flash flash;
  {
    auto f = boot(flash, T0);
    const Device &d = f->device();
    CHECK(d.version == DEVICE_VERSION && d.servo_pin == PIN_BUILD && d.button_pin == PIN_BUILD &&
              d.led_pin == PIN_BUILD,
          "fresh device follows the build's pins");
    CHECK(d.flags == (DEVICE_BLE_GUARD | DEVICE_BLE_CALIBRATION), "BLE toggles default on, setup not done");
    CHECK(f->timezone_text().empty() && f->tz_applies == 0, "no timezone of its own");
    CHECK(flash.have_device && flash.device_saves == 1 && f->setup_needed() && flash.device.timezone[0] == '\0',
          "first boot: the record is made at once, every value the build's, setup not done");
    flash.device_saves = 0;

    CHECK(f->set_timezone("Europe/Berlin") == Result::INVALID_ARG, "IANA name refused");
    CHECK(flash.device_saves == 0 && f->tz_applies == 0, "refused: nothing saved or applied");
    CHECK(f->set_timezone("CET-1CEST,M3.5.0,M10.5.0/3") == Result::OK, "POSIX string accepted");
    CHECK(flash.device_saves == 1, "saved at once");
    CHECK(f->tz_applies == 1 && f->tz_applied && f->tz_rules.std_offset_s == -3600 && f->tz_rules.has_dst(),
          "applied at once");
    CHECK(f->timezone_text() == "CET-1CEST,M3.5.0,M10.5.0/3", "reported");
    CHECK(f->set_timezone("CET-1CEST,M3.5.0,M10.5.0/3") == Result::OK && flash.device_saves == 1,
          "the same again is not rewritten");

    // Home Assistant pushes its own timezone right after a time sync: ours is put back on the next loop.
    f->note_time_sync(TimeSource::HOME_ASSISTANT);
    CHECK(f->tz_applies == 1, "not inside the sync callback");
    f->advance(250);
    CHECK(f->tz_applies == 2 && f->tz_applied, "re-applied after Home Assistant's sync");
    f->note_time_sync(TimeSource::NTP);
    f->advance(250);
    CHECK(f->tz_applies == 2, "an NTP sync does not touch the timezone");
  }
  {
    auto f = boot(flash, T0);
    CHECK(f->tz_applies == 1 && f->tz_rules.std_offset_s == -3600, "applied at boot from the saved record");
    CHECK(f->set_timezone("") == Result::OK && f->tz_applies == 2 && !f->tz_applied, "cleared: build's again");
    CHECK(flash.device.timezone[0] == '\0', "clearing is saved");
    f->note_time_sync(TimeSource::HOME_ASSISTANT);
    f->advance(250);
    CHECK(f->tz_applies == 2, "with none set, Home Assistant's timezone stands");
  }
  {
    // A damaged record: unterminated strings, unknown flag bits and a timezone that no longer parses.
    std::memset(flash.device.hostname, 'x', sizeof(flash.device.hostname));
    std::strcpy(flash.device.timezone, "EST5EDT");
    flash.device.flags = 0xFF;
    flash.device.servo_pin = 4;
    auto f = boot(flash, T0);
    const Device &d = f->device();
    CHECK(d.hostname[HOSTNAME_SIZE - 1] == '\0', "strings terminated");
    CHECK(f->timezone_text().empty() && f->tz_applies == 0, "bad saved timezone dropped, build's used");
    CHECK(d.flags == DEVICE_FLAGS_KNOWN, "unknown flag bits cleared");
    CHECK(d.servo_pin == 4, "the rest kept");
  }
  {
    flash.device.version = DEVICE_VERSION + 1;  // a layout this firmware does not know
    auto f = boot(flash, T0);
    CHECK(f->device().servo_pin == PIN_BUILD, "unknown version: defaults");
  }
}

/// Boot as the adapter does: the saved pins are resolved against the build's before the drivers start.
static std::unique_ptr<FakeFeeder> boot_with_pins(Flash &flash, const Pins &build, std::string *problem = nullptr,
                                                  bool has_button = true, bool has_led = true,
                                                  std::initializer_list<uint8_t> reserved = {}) {
  auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
  for (uint8_t r : reserved)
    f->reserve_pin(r);
  Device saved{};
  saved.servo_pin = saved.button_pin = saved.led_pin = PIN_BUILD;
  if (flash.have_device && flash.device.version == DEVICE_VERSION)
    saved = flash.device;
  f->set_build_pins(build);
  Pins use = resolve_pins(saved, build, problem, &f->reserved_pins());
  if (!has_button)
    use.button = PIN_NONE;
  if (!has_led)
    use.led = PIN_NONE;
  f->set_pins_in_use(use, has_button, has_led);
  f->start();
  f->advance(1500);
  return f;
}

static void test_pins() {
  current = "pins";
  // The validator: the lists in feeder_core.h, which packages/hardware.yaml repeats (tests/test_consistency.py).
  CHECK(check_pin(1) == PinCheck::OK && check_pin(21) == PinCheck::OK && check_pin(48) == PinCheck::OK, "free pins");
  for (uint8_t p : {0, 45, 46})
    CHECK(check_pin(p) == PinCheck::REFUSED, "boot strapping pins refused");
  // GPIO3 only selects the JTAG source, and only with the STRAP_JTAG_SEL eFuse burned (D6, firmware 2.1.0).
  CHECK(check_pin(3) == PinCheck::WARN && std::strstr(pin_note(3), "STRAP_JTAG_SEL") != nullptr,
        "GPIO3 accepted with a warning");
  for (uint8_t p = 26; p <= 32; p++)
    CHECK(check_pin(p) == PinCheck::REFUSED, "flash and PSRAM pins refused");
  CHECK(check_pin(19) == PinCheck::REFUSED && check_pin(20) == PinCheck::REFUSED, "USB refused");
  CHECK(check_pin(22) == PinCheck::REFUSED && check_pin(49) == PinCheck::REFUSED, "pins not on the chip refused");
  CHECK(check_pin(35) == PinCheck::WARN && check_pin(43) == PinCheck::WARN, "octal PSRAM and UART0 warned");
  for (uint8_t p = 0; p <= 60; p++)
    CHECK((check_pin(p) == PinCheck::OK) == (pin_note(p)[0] == '\0'), "every refused or warned pin says why");
  uint8_t g = 0;
  CHECK(parse_pin("GPIO5", g) && g == 5 && parse_pin("gpio17", g) && g == 17 && parse_pin("38", g) && g == 38,
        "pin parsing");
  CHECK(parse_pin("None", g) && g == PIN_NONE && !parse_pin("GPIO", g) && !parse_pin("GPIO123", g) &&
            !parse_pin("x1", g),
        "pin parsing: none, and junk refused");
  CHECK(pin_text(4) == "GPIO4" && pin_text(PIN_NONE) == "none", "pin text");
  Board b;
  CHECK(parse_board("ESP32-S3-DevKitC-1", b) && b == Board::DEVKITC_1 && !parse_board("devkit", b), "board names");

  const Pins build{1, 2, 21};  // the Waveshare ESP32-S3-Zero, as packages/core.yaml
  Flash flash;
  {
    // A build that cannot change its pins (no runtime pins in the YAML) refuses changes.
    auto f = boot(flash, T0);
    CHECK(f->set_pin(PinRole::SERVO, 4) == Result::UNSUPPORTED && !f->restart_needed(), "no runtime pins");
  }
  {
    auto f = boot_with_pins(flash, build);
    CHECK(f->pins_in_use() == build && f->board() == Board::BUILD && !f->restart_needed(), "fresh: the build's pins");
    CHECK(f->pins_text() == "servo GPIO1, button GPIO2, LED GPIO21", "pins text");
    CHECK(f->set_pin(PinRole::SERVO, 0) == Result::INVALID_ARG, "strapping pin refused");
    CHECK(f->set_pin(PinRole::SERVO, PIN_NONE) == Result::INVALID_ARG, "the servo needs a pin");
    CHECK(f->set_pin(PinRole::SERVO, 21) == Result::INVALID_ARG, "the LED's pin refused for the servo");
    CHECK(flash.device.servo_pin == PIN_BUILD && flash.device.button_pin == PIN_BUILD, "nothing saved by refusals");

    CHECK(f->set_pin(PinRole::BUTTON, PIN_NONE) == Result::OK && flash.device.button_pin == PIN_NONE,
          "no button: saved at once");
    CHECK(f->restart_needed() && f->pins_in_use().button == 2, "applies at the next restart");
    CHECK(f->board() == Board::CUSTOM, "no preset matches");
    CHECK(f->set_pin(PinRole::LED, 2) == Result::OK, "the button's old pin is free once the button has none");
    CHECK(f->set_pin(PinRole::SERVO, 35) == Result::OK, "a warned pin is accepted");
    bool warned = false;
    for (const auto &l : f->logs)
      warned |= l.find("GPIO35 for the servo: used by the octal") != std::string::npos;
    CHECK(warned, "with a warning");
    const int saves = flash.device_saves;
    CHECK(f->set_pin(PinRole::SERVO, 35) == Result::OK && flash.device_saves == saves, "the same again: not saved");
  }
  {
    auto f = boot_with_pins(flash, build);
    CHECK((f->pins_in_use() == Pins{35, PIN_NONE, 2}) && !f->restart_needed(), "the saved pins are used after restart");
    CHECK(f->status_led_fitted(), "LED fitted");
    CHECK(f->set_board(Board::DEVKITC_1) == Result::OK && (f->pins_saved() == Pins{4, 5, 38}), "DevKitC-1 preset");
    CHECK(f->board() == Board::DEVKITC_1 && f->restart_needed(), "the preset is recognised");
    CHECK(f->set_board(Board::CUSTOM) == Result::OK && f->board() == Board::DEVKITC_1, "Custom changes nothing");
    CHECK(f->set_pin(PinRole::LED, PIN_NONE) == Result::OK && f->board() == Board::CUSTOM, "then one pin changed");
  }
  {
    auto f = boot_with_pins(flash, build);
    CHECK(!f->status_led_fitted() && f->pins_text() == "servo GPIO4, button GPIO5, LED none", "no LED");
    CHECK(f->set_board(Board::BUILD) == Result::OK && flash.device.servo_pin == PIN_BUILD, "back to the build's");
    CHECK(f->board() == Board::BUILD && f->restart_needed(), "applies at restart");
    CHECK(f->set_pin(PinRole::SERVO, 2) == Result::INVALID_ARG, "the build's button pin counts as taken");
  }
  {
    // A damaged or hand-made record: a refused pin falls back to the build's; clashing pins all do.
    flash.device.servo_pin = 27;
    flash.device.button_pin = 7;
    flash.device.led_pin = PIN_BUILD;
    std::string problem;
    auto f = boot_with_pins(flash, build, &problem);
    CHECK((f->pins_in_use() == Pins{1, 7, 21}) && problem.find("servo pin GPIO27 not usable") != std::string::npos,
          "refused saved pin: the build's servo pin");
    flash.device.servo_pin = 7;
    problem.clear();
    auto g = boot_with_pins(flash, build, &problem);
    CHECK(g->pins_in_use() == build && problem.find("clash") != std::string::npos, "clashing pins: the build's");
    flash.device.servo_pin = PIN_NONE;
    auto h = boot_with_pins(flash, build);
    CHECK(h->pins_in_use().servo == 1, "no servo pin saved: the build's");
  }
  {
    // A build without the button or LED package: those are none, whatever is saved.
    flash.device.servo_pin = PIN_BUILD;
    auto f = boot_with_pins(flash, Pins{1, PIN_NONE, PIN_NONE}, nullptr, false, false);
    CHECK(f->pins_text() == "servo GPIO1, button none, LED none" && !f->status_led_fitted(), "servo only");
    CHECK(!f->restart_needed(), "a saved button pin is no reason to restart a build without the button");
    CHECK(f->set_pin(PinRole::BUTTON, 9) == Result::UNSUPPORTED && f->set_pin(PinRole::SERVO, 9) == Result::OK,
          "only the parts the build has can be changed");
  }
}

static std::vector<uint8_t> unhex(const char *h) {
  std::vector<uint8_t> out;
  for (; h[0] && h[1]; h += 2)
    out.push_back(static_cast<uint8_t>(std::strtoul(std::string(h, 2).c_str(), nullptr, 16)));
  return out;
}

// Shared with tests/test_web.mjs: password "correct horse", salt 00..0f, PBKDF2-HMAC-SHA256 20000 rounds.
static const char *const PW_KEY = "f353b5100691ea4531a49c82821a088e2083178c2c6954a17a1ec53b3900da25";
static const char *const PW_MESSAGE = "/number/Daily Portion Cap/set\n3";
static const char *const PW_TAG = "9b2b5e3df64049cfbdb3b02ebddb43277df22b4cc0fe8c6c96f61ac11ed79330";  // nonce 10..1f
// "battery staple", salt 20..2f, masked with the key above and nonce 10..1f.
static const char *const PW_NEW_KEY = "880ec4efdf8f0c832ce0d85cc76b3191b73014a0c3768c3bbd5a14354698425c";
static const char *const PW_NEW_MASKED = "2e33a9c5afad47f2f7547a0ab2ab9f9ff8f2acea70fc7609bdf3b4903cedea2f";
// A secret text (a Wi-Fi password) masked with PW_KEY and nonce 10..1f; tests/test_web.mjs has the same vector.
static const char *const PW_TEXT = "a wifi password longer than thirty-two bytes";
static const char *const PW_TEXT_MASKED =
    "a8685e68331b4ef6464670da56f92f1fcdcfcab7533f75b6a2a34b3a2eed915476e0934605b6633c2ac93e3d";

static void test_reserved_pins() {
  current = "reserved motor pins";
  // A mechanism that drives its own pins from the YAML (D4): the button and LED can never move onto them.
  Flash flash;
  auto f = boot_with_pins(flash, Pins{1, 3, 21}, nullptr, true, true, {2, 4, 5, 6});
  CHECK(f->reserved_pins().has(4) && !f->reserved_pins().has(3), "reserved");
  CHECK(f->set_pin(PinRole::BUTTON, 4) == Result::INVALID_ARG, "button refused on a motor pin");
  CHECK(f->set_pin(PinRole::LED, 6) == Result::INVALID_ARG, "LED refused on a motor pin");
  CHECK(f->set_pin(PinRole::BUTTON, 7) == Result::OK, "button on a free pin");
  // A record saved before (a motor pin chosen for the button) falls back to the build's at boot.
  flash.device.button_pin = 5;
  std::string problem;
  auto g = boot_with_pins(flash, Pins{1, 3, 21}, &problem, true, true, {2, 4, 5, 6});
  CHECK(g->pins_in_use().button == 3 && problem.find("not usable") != std::string::npos,
        "a saved button pin on a motor pin is not used");
  // A build with no servo pin (its mechanism drives its own): no button and no LED are not a clash with it.
  Flash none;
  auto n = boot_with_pins(none, Pins{PIN_NONE, 3, 21}, nullptr, true, true, {2, 4, 5, 6});
  CHECK(n->set_pin(PinRole::BUTTON, PIN_NONE) == Result::OK, "button none with no servo pin");
  CHECK(n->set_pin(PinRole::LED, PIN_NONE) == Result::OK, "and the LED none too");
}

static void test_settings_password() {
  current = "settings password";
  Flash flash;
  const auto key = unhex(PW_KEY), tag = unhex(PW_TAG);
  uint8_t salt[PASSWORD_SALT_SIZE];
  for (unsigned i = 0; i < PASSWORD_SALT_SIZE; i++)
    salt[i] = static_cast<uint8_t>(i);
  uint8_t nonce[AUTH_NONCE_SIZE];
  uint32_t wait = 0;
  {
    auto f = boot(flash, T0);
    CHECK(!f->password_set(), "none by default");
    f->password_challenge(nonce);
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::OK, "no password: everything passes");

    f->set_password(salt, key.data());
    CHECK(f->password_set() && flash.device.password_scheme == PASSWORD_SCHEME_PBKDF2, "set and saved");
    CHECK(std::memcmp(flash.device.password_hash, key.data(), PASSWORD_HASH_SIZE) == 0, "the derived key is stored");

    f->random_next = 16;
    f->password_challenge(nonce);
    CHECK(nonce[0] == 16 && nonce[15] == 31, "challenge from the random source");
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::OK, "right answer");
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::NO_CHALLENGE, "never twice");

    f->random_next = 16;
    f->password_challenge(nonce);
    CHECK(f->password_check(nonce, tag.data(), "/number/Daily Portion Cap/set\n6", wait) == AuthResult::REFUSED &&
              wait == AUTH_WAIT_MIN_MS,
          "an answer for another request is refused, and a wait starts");
    f->random_next = 16;
    f->password_challenge(nonce);
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::WAIT && wait > 0,
          "inside the wait nothing is checked, even the right answer");
    f->advance(1000);
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::OK,
          "after the wait the same challenge can still be answered");

    uint32_t expect = AUTH_WAIT_MIN_MS;
    for (int i = 0; i < 8; i++) {
      f->password_challenge(nonce);
      CHECK(f->password_check(nonce, key.data(), PW_MESSAGE, wait) == AuthResult::REFUSED && wait == expect,
            "the wait doubles");
      f->advance(wait);
      expect = std::min(expect * 2, AUTH_WAIT_MAX_MS);
    }
    CHECK(expect == AUTH_WAIT_MAX_MS, "and stops at the maximum");

    f->password_challenge(nonce);
    f->advance(AUTH_NONCE_TTL_MS);
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::NO_CHALLENGE, "challenges expire");
    uint8_t other[AUTH_NONCE_SIZE]{};
    f->password_challenge(nonce);
    CHECK(f->password_check(other, tag.data(), PW_MESSAGE, wait) == AuthResult::NO_CHALLENGE, "wrong nonce");
    CHECK(f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::NO_CHALLENGE,
          "a wrong nonce uses up the challenge");

    // Changing it: the new key travels masked by the current one.
    const auto masked = unhex(PW_NEW_MASKED), new_key = unhex(PW_NEW_KEY);
    uint8_t n16[AUTH_NONCE_SIZE], out[PASSWORD_HASH_SIZE];
    for (unsigned i = 0; i < AUTH_NONCE_SIZE; i++)
      n16[i] = static_cast<uint8_t>(16 + i);
    f->password_unmask(n16, masked.data(), out);
    CHECK(std::memcmp(out, new_key.data(), PASSWORD_HASH_SIZE) == 0, "unmasked with the current key");
    const auto text_masked = unhex(PW_TEXT_MASKED);
    CHECK(f->password_unmask_text(n16, text_masked.data(), text_masked.size()) == PW_TEXT,
          "a secret text longer than one block is unmasked too");
  }
  {
    auto f = boot(flash, T0);
    CHECK(f->password_set(), "kept across a reboot");
    // BOOT held for 10 s clears it, and only it.
    f->set_timezone("UTC0");
    f->boot_button(true);
    f->advance(BOOT_HOLD_MS - 500);
    CHECK(f->password_set(), "not before 10 s");
    f->boot_button(false);
    f->boot_button(true);
    f->advance(BOOT_HOLD_MS - 500);
    CHECK(f->password_set(), "a release starts the count again");
    f->advance(500);
    CHECK(!f->password_set() && flash.device.password_scheme == PASSWORD_SCHEME_NONE, "cleared after 10 s");
    CHECK(flash.device.password_hash[0] == 0 && f->timezone_text() == "UTC0", "key wiped, other settings kept");
    uint8_t mask_out[PASSWORD_HASH_SIZE], zero[PASSWORD_HASH_SIZE]{};
    f->password_unmask(nonce, zero, mask_out);
    CHECK(std::memcmp(mask_out, zero, PASSWORD_HASH_SIZE) == 0, "no password: the new key is not masked");
    const std::string plain = "hello";
    CHECK(f->password_unmask_text(nonce, reinterpret_cast<const uint8_t *>(plain.data()), plain.size()) == plain,
          "no password: a secret text is not masked");
  }
  {
    flash.device.password_scheme = 7;  // from a newer firmware
    auto f = boot(flash, T0);
    f->password_challenge(nonce);
    CHECK(f->password_set() && f->password_check(nonce, tag.data(), PW_MESSAGE, wait) == AuthResult::REFUSED,
          "an unknown scheme refuses everything until BOOT clears it");
  }
}

static void test_ble_settings() {
  current = "BLE key and permissions";
  Flash flash;
  uint8_t key[DEVICE_KEY_SIZE], out[DEVICE_KEY_SIZE];
  for (unsigned i = 0; i < DEVICE_KEY_SIZE; i++)
    key[i] = static_cast<uint8_t>(i + 1);
  {
    auto f = boot(flash, T0);
    CHECK(!f->ble_built() && f->ble_key_source() == KeySource::NONE && !f->ble_key(out), "no BLE, no key");
    CHECK(f->ble_may_change_guard() && f->ble_may_change_calibration(), "both permissions on by default");
    const uint8_t none[DEVICE_KEY_SIZE] = {};
    f->set_build_ble_key(none);
    CHECK(f->ble_built() && f->ble_key_source() == KeySource::NONE, "BLE built with no key");
    flash.device_saves = 0;  // the first boot's
    const uint32_t serial = f->ble_key_serial();
    CHECK(f->set_ble_key(key) == Result::OK && flash.device_saves == 1, "saved at once");
    CHECK(f->ble_key_serial() != serial && f->ble_key(out) && std::memcmp(out, key, sizeof(key)) == 0, "in use");
    CHECK(f->set_ble_key(key) == Result::OK && flash.device_saves == 1, "the same key is not rewritten");
    f->set_ble_may_change_guard(false);
    CHECK(!f->ble_may_change_guard() && f->ble_may_change_calibration() && flash.device_saves == 2, "guard off, saved");
    f->set_ble_may_change_guard(false);
    CHECK(flash.device_saves == 2, "no change, no write");
    f->set_ble_may_change_calibration(false);
    CHECK(!f->ble_may_change_calibration() && flash.device_saves == 3, "calibration off, saved");
  }
  {
    auto f = boot(flash, T0 + 60);
    CHECK(!f->ble_may_change_guard() && !f->ble_may_change_calibration(), "permissions kept across a restart");
    CHECK(f->ble_key_source() == KeySource::DEVICE && f->ble_key(out) && std::memcmp(out, key, sizeof(key)) == 0,
          "key kept across a restart");
    f->set_ble_may_change_guard(true);
    CHECK(f->ble_may_change_guard(), "back on");
    CHECK(f->device().flags == DEVICE_BLE_GUARD, "only the guard bit set");
  }
}

static void test_network() {
  current = "network";
  CHECK(check_hostname("precision-feeder") && check_hostname("a") && check_hostname("feeder-2"), "good names");
  std::string why;
  CHECK(!check_hostname("", &why) && why == "empty", "empty");
  CHECK(!check_hostname("Feeder") && !check_hostname("my_feeder") && !check_hostname("feeder.local") &&
            !check_hostname("-feeder") && !check_hostname("feeder-"),
        "uppercase, underscore, dot and edge hyphens refused");
  CHECK(check_hostname(std::string(31, 'a')) && !check_hostname(std::string(32, 'a')), "at most 31 characters");
  CHECK(check_wifi_password("12345678", false) && !check_wifi_password("1234567", false) &&
            check_wifi_password(std::string(63, 'x'), false),
        "access point: 8 to 63 characters");
  CHECK(!check_wifi_password("", false) && check_wifi_password("", true), "open only for a Wi-Fi network");
  CHECK(check_wifi_password(std::string(64, 'a'), true) && !check_wifi_password(std::string(64, 'g'), true) &&
            !check_wifi_password(std::string(64, 'a'), false),
        "64 hex digits: a raw WPA2 key, for Wi-Fi only");
  CHECK(!check_wifi_password("caf\xc3\xa9 latte", true) && !check_wifi_password("tab\there!", true),
        "printable ASCII only");
  CHECK(check_ssid("Home") && check_ssid(std::string(32, 's')) && !check_ssid("") && !check_ssid(std::string(33, 's')) &&
            !check_ssid("bad\nssid"),
        "SSIDs: 1 to 32 bytes, no control characters");

  Flash flash;
  {
    auto f = boot(flash, T0);
    flash.device_saves = 0;  // the first boot's
    CHECK(f->set_hostname("feeder") == Result::UNSUPPORTED && flash.device_saves == 0,
          "nothing before the adapter applies");
    f->set_network_in_use("precision-feeder", false, f->device());
    CHECK(f->hostname_in_use() == "precision-feeder" && f->hostname_saved().empty() && !f->restart_needed(),
          "the build's name");
    CHECK(f->set_hostname("Bad Name") == Result::INVALID_ARG && flash.device_saves == 0, "a bad name is refused");
    CHECK(f->set_hostname("tank-feeder") == Result::OK && flash.device_saves == 1, "saved at once");
    CHECK(f->restart_needed() && f->hostname_in_use() == "precision-feeder", "used after the next restart");
    CHECK(f->set_hostname("tank-feeder") == Result::OK && flash.device_saves == 1, "the same name is not rewritten");
    CHECK(f->set_ap_password("12345678") == Result::UNSUPPORTED, "no access point in this build");
  }
  {
    auto f = boot(flash, T0 + 60);
    CHECK(f->hostname_saved() == "tank-feeder", "kept across a restart");
    f->set_network_in_use("tank-feeder", true, f->device());
    CHECK(!f->restart_needed(), "applied: no restart due");
    CHECK(f->ap_password_source() == KeySource::BUILD, "the access point uses the build's password");
    CHECK(f->set_ap_password("short") == Result::INVALID_ARG, "too short");
    CHECK(f->set_ap_password("feed the fish") == Result::OK && f->ap_password_source() == KeySource::DEVICE &&
              f->restart_needed(),
          "saved; used after the next restart");
    CHECK(f->set_ap_password("") == Result::OK && !f->restart_needed(), "back to the build's: nothing to restart for");
    CHECK(f->set_hostname("") == Result::OK && f->restart_needed(), "back to the build's name at the next restart");
  }
  {
    // A damaged record: a name and a password that are not usable fall back to the build's.
    std::strcpy(flash.device.hostname, "Bad Name");
    std::strcpy(flash.device.ap_password, "short");
    auto f = boot(flash, T0 + 120);
    CHECK(f->hostname_saved().empty() && f->ap_password_source() == KeySource::BUILD, "dropped on load");
  }
}

static void test_setup_flag() {
  current = "setup wizard flag";
  {
    Flash flash;
    auto f = boot(flash, T0);
    CHECK(f->setup_needed() && flash.device_saves == 1, "a new feeder needs setting up (saved at the first boot)");
    // A new feeder saves its settings and tray state soon after the first boot: the next boot must still know.
    f->set_min_gap_s(90);
    f->advance(5000);
    auto again = boot(flash, T0 + 30);
    CHECK(flash.have_settings && again->setup_needed(), "still needed after a restart with settings saved");
    again->set_setup_done(true);
    CHECK(!again->setup_needed() && flash.device_saves == 2, "finished: saved at once");
    again->set_setup_done(true);
    CHECK(flash.device_saves == 2, "no change, no write");
    auto g = boot(flash, T0 + 60);
    CHECK(!g->setup_needed(), "kept across a restart");
    g->set_setup_done(false);
    CHECK(g->setup_needed() && g->ble_may_change_guard(), "offered again; other flags kept");
  }
  {
    // An update from 1.x: settings and a calibration saved, no Device record yet.
    Flash flash;
    { auto f = boot(flash, T0); f->set_min_gap_s(120); f->advance(5000); }
    flash.have_device = false;  // as 1.x left it
    auto f = boot(flash, T0 + 60);
    CHECK(flash.have_settings && !f->setup_needed(), "an existing feeder is not set up again");
    CHECK(flash.have_device && (flash.device.flags & DEVICE_SETUP_DONE), "and that is saved at once");
    flash.have_settings = false;
    auto g = boot(flash, T0 + 120);
    CHECK(!g->setup_needed(), "decided once: the saved record wins from then on");
  }
}

static void test_ntp_servers() {
  current = "NTP servers";
  std::vector<std::string> out;
  std::string problem;
  CHECK(parse_ntp_servers("", out) && out.empty(), "empty: the build's");
  CHECK(parse_ntp_servers("time.cloudflare.com", out) && out.size() == 1, "one host name");
  CHECK(parse_ntp_servers(" 192.168.1.1, ntp.lan ,fd00::1 ", out) && out.size() == 3 && out[0] == "192.168.1.1" &&
            out[1] == "ntp.lan" && out[2] == "fd00::1",
        "three, any separators, IPv4 and IPv6");
  CHECK(!parse_ntp_servers("a,b,c,d", out, &problem) && problem == "more than three servers", "four refused");
  CHECK(!parse_ntp_servers("ntp.lan;x", out), "other characters refused");
  CHECK(!parse_ntp_servers("[fd00::1]", out), "brackets refused");
  CHECK(!parse_ntp_servers(std::string(64, 'a'), out), "longer than 63 refused");
  CHECK(!parse_ntp_servers("-ntp", out) && !parse_ntp_servers("ntp-", out) && !parse_ntp_servers(".lan", out),
        "a leading or trailing '-' or a leading '.' refused");
  {
    Flash flash;
    auto f = boot(flash, T0);
    CHECK(f->ntp_servers_text().empty() && f->ntp_applied == 0, "a new feeder uses the build's servers");
    const int saves = flash.device_saves;
    CHECK(f->set_ntp_servers("pool.ntp.org, 10.0.0.1") == Result::OK, "set");
    CHECK(f->ntp_servers_text() == "pool.ntp.org,10.0.0.1" && f->ntp.size() == 2 && f->ntp[1] == "10.0.0.1",
          "saved normalised and applied at once");
    CHECK(flash.device_saves == saves + 1, "saved at once");
    CHECK(f->set_ntp_servers("pool.ntp.org,10.0.0.1") == Result::OK && flash.device_saves == saves + 1, "no change, no write");
    CHECK(f->set_ntp_servers("a,b,c,d") == Result::INVALID_ARG && f->ntp_servers_text() == "pool.ntp.org,10.0.0.1",
          "a refused list changes nothing");
    auto g = boot(flash, T0 + 60);
    CHECK(g->ntp_applied == 1 && g->ntp.size() == 2, "applied again at boot");
    CHECK(g->set_ntp_servers("") == Result::OK && g->ntp_applied == 2 && g->ntp.empty() && g->ntp_servers_text().empty(),
          "empty: back to the build's");
    flash.device.ntp_servers[0] = ';';  // corrupted on flash
    auto h = boot(flash, T0 + 120);
    CHECK(h->ntp_servers_text().empty() && h->ntp_applied == 0, "an unusable saved list is dropped");
  }
  {
    // Firmware 2.0.0 and 2.0.1 saved a version 1 Device record: migrated, every field kept.
    Flash flash;
    { auto f = boot(flash, T0); f->set_min_gap_s(120); f->set_timezone("CET-1CEST,M3.5.0,M10.5.0/3"); f->set_setup_done(true); f->advance(5000); }
    legacy::DeviceV1 v1{};
    const Device &d = flash.device;
    v1.version = 1;
    v1.servo_pin = 4;
    v1.button_pin = PIN_NONE;
    v1.led_pin = d.led_pin;
    v1.flags = d.flags;
    std::strcpy(v1.hostname, "fish-room");
    std::memcpy(v1.timezone, d.timezone, sizeof(v1.timezone));
    std::strcpy(v1.ap_password, "feed the fish");
    v1.ble_key[0] = 7;
    v1.password_scheme = 1;
    v1.password_salt[3] = 9;
    v1.password_hash[31] = 5;
    flash.have_device = false;
    flash.have_device_v1 = true;
    flash.device_v1 = v1;
    const int saves = flash.device_saves;
    auto f = boot(flash, T0 + 60);
    const Device &n = flash.device;
    CHECK(flash.have_device && flash.device_saves == saves + 1 && n.version == DEVICE_VERSION, "saved as version 2 at once");
    CHECK(n.servo_pin == 4 && n.button_pin == PIN_NONE && std::strcmp(n.hostname, "fish-room") == 0 &&
              std::strcmp(n.ap_password, "feed the fish") == 0 && n.ble_key[0] == 7 && n.password_scheme == 1 &&
              n.password_salt[3] == 9 && n.password_hash[31] == 5 && n.ntp_servers[0] == '\0',
          "every field kept, no NTP servers");
    CHECK(f->timezone_text() == "CET-1CEST,M3.5.0,M10.5.0/3" && !f->setup_needed() && f->password_set(),
          "timezone, setup done and the password kept");
    CHECK(f->cfg.min_gap_s == 120, "settings untouched");
  }
}

// A sink that records what reached the "partition".
struct FakeSink : UpdateSink {
  std::vector<uint8_t> data;
  size_t limit = 1 << 20;
  bool began = false, ended = false, aborted = false, fail_write = false, fail_end = false;
  bool begin(size_t size) override {
    began = true;
    data.clear();
    return size <= limit;
  }
  bool write(const uint8_t *d, size_t n) override {
    if (fail_write)
      return false;
    data.insert(data.end(), d, d + n);
    return true;
  }
  bool end() override { return ended = !fail_end; }
  void abort() override { aborted = true; }
};

static void test_firmware_upload() {
  current = "firmware upload";
  using S = FirmwareUpload::Status;
  std::vector<uint8_t> image(5000);
  for (size_t i = 0; i < image.size(); i++)
    image[i] = static_cast<uint8_t>(i * 7);
  const uint8_t app_magic[4] = {0x32, 0x54, 0xCD, 0xAB};  // an app image: the image magic, the app description's
  image[0] = 0xE9;
  std::memcpy(&image[32], app_magic, 4);
  uint8_t digest[32];
  crypto::sha256(image.data(), image.size(), digest);
  auto send = [&](FirmwareUpload &u, FakeSink &sink, const uint8_t *expected, bool authorised, size_t cut = 0) {
    u.start(expected, image.size(), authorised, &sink);
    const size_t n = image.size() - cut;
    for (size_t i = 0; i < n; i += 1460)
      u.write(image.data() + i, std::min<size_t>(1460, n - i));
    return u.finish();
  };
  {
    FirmwareUpload u;
    FakeSink sink;
    CHECK(send(u, sink, digest, true) == S::OK && sink.ended && sink.data == image, "a signed, whole image is written");
  }
  {
    FirmwareUpload u;
    FakeSink sink;
    CHECK(send(u, sink, digest, false) == S::REFUSED && !sink.began, "not authorised: the partition is not touched");
  }
  {
    FirmwareUpload u;
    FakeSink sink;
    uint8_t other[32];
    std::memcpy(other, digest, 32);
    other[31] ^= 1;
    CHECK(send(u, sink, other, true) == S::MISMATCH && sink.aborted && !sink.ended, "another image: aborted");
  }
  {
    FirmwareUpload u;
    FakeSink sink;
    CHECK(send(u, sink, nullptr, true) == S::BAD_REQUEST && !sink.began, "no hash: refused");
  }
  {
    FirmwareUpload u;
    FakeSink sink;
    CHECK(send(u, sink, digest, true, 100) == S::INCOMPLETE && sink.aborted && !sink.ended, "cut short: aborted");
  }
  {
    FirmwareUpload u;
    FakeSink sink;
    sink.limit = 100;
    CHECK(send(u, sink, digest, true) == S::FLASH && sink.data.empty(), "too big for the partition");
    FakeSink bad;
    bad.fail_write = true;
    CHECK(send(u, bad, digest, true) == S::FLASH && bad.aborted, "a write fails: aborted");
    FakeSink invalid;
    invalid.fail_end = true;
    CHECK(send(u, invalid, digest, true) == S::FLASH, "not a valid image");
  }
  {
    // A factory image (the bootloader first: no app description at byte 32), signed and whole, is still refused.
    std::vector<uint8_t> factory = image;
    factory[32] = 0x50;
    uint8_t fd[32];
    crypto::sha256(factory.data(), factory.size(), fd);
    FirmwareUpload u;
    FakeSink sink;
    u.start(fd, factory.size(), true, &sink);
    u.write(factory.data(), 20);  // the header split across chunks
    u.write(factory.data() + 20, factory.size() - 20);
    CHECK(u.finish() == S::NOT_APP && sink.aborted && !sink.ended, "a factory image is refused");
    FakeSink tiny;
    u.start(fd, 10, true, &tiny);
    u.write(factory.data(), 10);
    CHECK(u.finish() == S::NOT_APP && tiny.aborted, "too short to be an image");
    FakeSink split;
    u.start(digest, image.size(), true, &split);
    for (size_t i = 0; i < image.size(); i += 7)
      u.write(image.data() + i, std::min<size_t>(7, image.size() - i));
    CHECK(u.finish() == S::OK && split.data == image, "an app image in tiny chunks");
  }
  {
    // A dropped connection leaves an upload open; the next one aborts it first.
    FirmwareUpload u;
    FakeSink first, second;
    u.start(digest, image.size(), true, &first);
    u.write(image.data(), 1000);
    CHECK(send(u, second, digest, true) == S::OK && first.aborted && second.data == image, "a new upload aborts the old");
    FakeSink third;
    u.start(digest, image.size(), true, &third);
    u.no_body();
    CHECK(third.aborted && u.status() == S::BAD_REQUEST, "no body: aborted");
  }
}

// ---- the mechanism interface (firmware 2.1.0) ----------------------------------------------------
// A mechanism that is not the servo: 14 portions, a reload that does not move, a feed that takes 300 ms, an injected
// fault, and a tuning table of two values.
class FakeMechanism : public Mechanism {
 public:
  static constexpr TuningParam PARAMS[2] = {
      {"speed", "Speed", "%", 10, 100, 80, 1},
      {"wiggle_tenths", "Wiggle", "°", 1, 50, 20, 10},
  };
  int feeds{0}, reloads{0}, restores{0}, stops{0}, releases{0}, shows{0};
  bool fail_next{false};
  int32_t applied[2]{0, 0};
  uint8_t layout{1};

  const char *name() const override { return "fake"; }
  uint8_t capacity() const override { return 14; }
  void feed_one(uint8_t, uint32_t now) override {
    feeds++;
    busy_ = true;
    start_ = now;
  }
  void reload(uint32_t) override { reloads++; }
  void restore(uint8_t, uint32_t) override { restores++; }
  MoveStatus poll(uint32_t now) override {
    if (!busy_)
      return MoveStatus::DONE;
    if (now - start_ < 300)
      return MoveStatus::BUSY;
    busy_ = false;
    if (fail_next) {
      fail_next = false;
      fault_ = "stalled";
      return MoveStatus::FAULT;
    }
    return MoveStatus::DONE;
  }
  void stop() override {
    stops++;
    busy_ = false;
  }
  void release() override { releases++; }
  uint32_t settle_ms() const override { return 0; }
  const char *fault_text() const override { return fault_; }
  uint8_t tuning_count() const override { return 2; }
  uint8_t tuning_layout() const override { return layout; }
  const TuningParam *tuning_param(uint8_t i) const override { return i < 2 ? &PARAMS[i] : nullptr; }
  void apply_tuning(uint8_t i, int32_t v) override { applied[i] = v; }

 private:
  bool busy_{false};
  uint32_t start_{0};
  const char *fault_{""};
};
constexpr TuningParam FakeMechanism::PARAMS[2];

static std::unique_ptr<FakeFeeder> boot_with(Flash &flash, Mechanism &m, int64_t epoch = T0) {
  auto f = std::make_unique<FakeFeeder>(flash, epoch * 1000, true);
  f->set_mechanism(&m);
  f->start();
  f->advance(1500);
  return f;
}

static void test_mechanism_capacity_and_motion() {
  current = "mechanism: capacity and motion";
  Flash flash;
  FakeMechanism m;
  auto f = boot_with(flash, m);
  CHECK(!f->is_servo() && std::strcmp(f->mechanism().name(), "fake") == 0, "the fake mechanism is in use");
  CHECK(f->capacity() == 14 && f->remaining() == 14, "14 portions per load");
  CHECK(m.restores == 1 && f->angles.empty(), "boot restores through the mechanism; the servo is never driven");
  CHECK(f->set_daily_cap(14) == Result::OK && f->set_daily_cap(15) == Result::INVALID_ARG, "daily cap up to 14");
  CHECK(f->set_max_portions_cmd(14) == Result::OK && f->set_max_portions_cmd(15) == Result::INVALID_ARG,
        "portions per command up to 14");
  CHECK(f->set_min_gap_s(10) == Result::OK, "short gap for the test");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feed accepted");
  CHECK(f->tray_state() == TrayState::MOVING && flash.state.moving_to == 1, "moving, intent saved");
  f->advance(250);
  CHECK(f->tray_state() == TrayState::MOVING && f->position() == 0, "still moving before the mechanism is done");
  f->advance(250);
  CHECK(f->position() == 1 && f->remaining() == 13 && flash.state.moving_to == NO_MOVE, "committed when done");
  // Thirteen more, in one command: 13 separate steps.
  f->advance(11000);
  CHECK(f->request_feed(13, Source::ESPHOME) == Result::OK, "13 at once");
  f->advance(13 * 6000);
  CHECK(f->position() == 14 && f->remaining() == 0 && f->tray_state() == TrayState::EMPTY, "empty after 14");
  CHECK(m.feeds == 14, "one feed_one per portion");
  f->advance(11000);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::EMPTY, "empty refuses");
  f->tray_reload();
  CHECK(m.reloads == 1 && f->tray_state() == TrayState::MOVING, "reload: moving until it has settled");
  f->advance(2000);
  CHECK(f->remaining() == 14 && f->tray_state() == TrayState::LOADED, "reload: full again");
  CHECK(f->angles.empty(), "the servo was never driven");
}

static void test_mechanism_fault() {
  current = "mechanism: fault";
  Flash flash;
  FakeMechanism m;
  auto f = boot_with(flash, m);
  f->set_max_portions_cmd(3);
  m.fail_next = true;
  CHECK(f->request_feed(3, Source::ESPHOME) == Result::OK, "feed of 3 accepted");
  f->advance(500);
  CHECK(f->position_unknown() && f->tray_state() == TrayState::UNKNOWN, "a fault leaves the position unknown");
  CHECK(m.stops == 1 && m.feeds == 1, "motor stopped; no further portions");
  CHECK(std::strcmp(f->mechanism_fault(), "stalled") == 0, "the reason is reported");
  CHECK(f->portions_24h() == 1, "the faulted portion counts as fed, the other two do not");
  CHECK(flash.state.unknown == 1 && flash.state.moving_to == NO_MOVE, "saved as unknown");
  bool logged = false;
  for (const auto &l : f->logs)
    logged = logged || l.find("Motor fault: stalled") != std::string::npos;
  CHECK(logged, "logged as a motor fault");
  CHECK(f->faults_text().find("motor fault: stalled") != std::string::npos, "and shown in Faults");
  f->declare_position(4);
  CHECK(!f->position_unknown() && f->remaining() == 10 && m.restores == 2, "declare recovers through restore()");
}

static void test_mechanism_tuning() {
  current = "mechanism: tuning";
  Flash flash;
  FakeMechanism m;
  auto f = boot_with(flash, m);
  CHECK(f->tuning_count() == 2 && f->tuning_index("wiggle_tenths") == 1 && f->tuning_index("nope") == -1, "table");
  CHECK(f->tuning_value(0) == 80 && m.applied[0] == 80 && m.applied[1] == 20, "defaults applied at boot");
  CHECK(!flash.have_tuning, "nothing saved until a value changes");
  CHECK(f->set_tuning(0, 101) == Result::INVALID_ARG && f->set_tuning(0, 9) == Result::INVALID_ARG, "limits");
  CHECK(f->set_tuning(2, 50) == Result::INVALID_ARG, "no such value");
  CHECK(f->set_tuning(0, 55) == Result::OK && m.applied[0] == 55, "set and applied");
  f->advance(5000);
  CHECK(flash.have_tuning && flash.tuning.values[0] == 55 && std::strcmp(flash.tuning.mechanism, "fake") == 0 &&
            flash.tuning.layout == 1,
        "saved shortly, with the mechanism and layout");
  f->set_max_portions_cmd(1);
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "feeding");
  CHECK(f->set_tuning(1, 30) == Result::BUSY, "not while moving");
  f->advance(2000);

  // Reboot: loaded again.
  FakeMechanism m2;
  auto g = boot_with(flash, m2, T0 + 60);
  CHECK(g->tuning_value(0) == 55 && m2.applied[0] == 55, "kept across a reboot");

  // A saved value out of range (a damaged record) falls back to the default.
  flash.tuning.values[1] = 999;
  FakeMechanism m3;
  auto h = boot_with(flash, m3, T0 + 120);
  CHECK(h->tuning_value(1) == 20 && h->tuning_value(0) == 55, "an out-of-range value is replaced by its default");

  // Another layout (the table changed): defaults, and the old record is left alone until a value is set.
  FakeMechanism m4;
  m4.layout = 2;
  const int saves = flash.tuning_saves;
  auto k = boot_with(flash, m4, T0 + 180);
  CHECK(k->tuning_value(0) == 80, "another layout's values are not used");
  k->advance(5000);
  CHECK(flash.tuning_saves == saves && flash.tuning.layout == 1, "and not overwritten by themselves");

  // The servo has no tuning table, so a servo build never writes the record.
  Flash servo_flash;
  auto s = boot(servo_flash, T0);
  CHECK(s->tuning_count() == 0 && s->set_tuning(0, 1) == Result::INVALID_ARG, "the servo has no tuning values");
  s->set_daily_cap(5);
  s->advance(5000);
  CHECK(!servo_flash.have_tuning, "a servo build never saves a tuning record");
}

static void test_mechanism_servo_only_calibration() {
  current = "mechanism: servo-only calibration";
  Flash flash;
  FakeMechanism m;
  auto f = boot_with(flash, m);
  CHECK(f->set_pulse_range(600, 2300) == Result::UNSUPPORTED, "no pulse range without the servo");
  f->set_calibration_mode(true);
  CHECK(f->tray_state() == TrayState::CALIBRATING, "calibration mode still refuses feeds");
  CHECK(f->request_feed(1, Source::ESPHOME) == Result::CALIBRATING, "refused while calibrating");
  CHECK(f->jog_calibration(0, 5) == Result::INVALID_ARG, "no servo calibration points to jog");
  f->calibration_select(3);
  CHECK(f->calibration_position() == 0 && m.shows == 0, "nothing to select");
  f->set_calibration_mode(false);
  f->advance(2000);
  CHECK(f->tray_state() == TrayState::LOADED && f->angles.empty(), "back to loaded; the servo was never driven");
}

// ---- golden servo trace (firmware 2.1.0, plan S1.0) -------------------------------------------
// A scripted session through every motion path: boot, feeds of one and three portions (with and without the wiggle),
// reload, calibration, stop mid-step and mid-gap, declare, a power cut mid-step, the button, a scheduled feed and an
// empty tray. Every servo command, release and tray-state save is recorded with its time. tests/servo_golden.h holds
// the trace recorded from firmware 2.0.2, before the mechanism interface; the servo must still produce it exactly.
// To record it again (only when a behaviour change is intended): build with -DPRINT_SERVO_TRACE and run.
static std::string servo_session() {
  Flash flash;
  std::string out;
  auto result = [&](FakeFeeder &f, const char *what, Result r) { f.note("%s -> %s", what, FeederCore::result_name(r)); };
  auto f = std::make_unique<FakeFeeder>(flash, T0 * 1000, true);
  f->tracing = true;
  f->start();
  f->advance(1500);
  result(*f, "feed 1", f->request_feed(1, Source::ESPHOME));
  f->advance(5000);
  f->set_max_portions_cmd(3);
  f->set_min_gap_s(10);
  f->set_daily_cap(6);
  f->advance(61000);
  result(*f, "feed 3", f->request_feed(3, Source::ESPHOME));
  f->advance(30000);
  f->set_wiggle(false);
  f->advance(11000);
  result(*f, "feed 1 no wiggle", f->request_feed(1, Source::BLE));
  f->advance(3000);
  f->set_wiggle(true);
  f->tray_reload();
  f->note("reload");
  f->advance(2000);
  f->set_calibration_mode(true);
  f->note("calibration on");
  f->calibration_select(2);
  f->set_calibration_offset(15);
  result(*f, "jog 3 -7", f->jog_calibration(3, -7));
  result(*f, "pulse 600-2300", f->set_pulse_range(600, 2300));
  result(*f, "feed while calibrating", f->request_feed(1, Source::ESPHOME));
  f->advance(1000);
  f->set_calibration_mode(false);
  f->note("calibration off");
  f->advance(12000);
  f->set_portions_24h(0);
  result(*f, "feed 1 then stop", f->request_feed(1, Source::ESPHOME));
  f->advance(500);
  f->stop();
  f->advance(2000);
  result(*f, "feed while unknown", f->request_feed(1, Source::ESPHOME));
  f->declare_position(2);
  f->note("declared 2");
  f->advance(11000);
  f->set_portions_24h(0);
  result(*f, "feed 2 then stop in gap", f->request_feed(2, Source::ESPHOME));
  f->advance(4000);
  f->stop();
  f->advance(12000);
  f->set_portions_24h(0);
  result(*f, "feed 1 then power cut", f->request_feed(1, Source::ESPHOME));
  f->advance(1250);
  f->note("power cut");
  out += f->trace;
  f = std::make_unique<FakeFeeder>(flash, (T0 + 300) * 1000, true);
  f->tracing = true;
  f->start();
  f->advance(1500);
  f->button_press();
  f->note("button (reload)");
  f->advance(2000);
  f->set_reload_pause_min(0);
  f->set_daily_cap(6);
  f->set_min_gap_s(10);
  enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60 + 10, 2);
  f->advance(12 * 60 * 1000);
  f->note("after the 08:10 event");
  f->declare_position(5);
  f->advance(11000);
  f->set_portions_24h(0);
  result(*f, "feed last", f->request_feed(1, Source::BUTTON));
  f->advance(11000);
  result(*f, "feed empty", f->request_feed(1, Source::ESPHOME));
  f->button_press();
  f->note("button (empty: reload)");
  f->advance(2000);
  out += f->trace;
  return out;
}

#include "servo_golden.h"

static void test_servo_golden_trace() {
  current = "golden servo trace";
  const std::string got = servo_session();
  if (got != SERVO_GOLDEN) {
    // Say where it first differs, so a failure is quick to read.
    size_t i = 0;
    while (i < got.size() && i < sizeof(SERVO_GOLDEN) - 1 && got[i] == SERVO_GOLDEN[i])
      i++;
    const size_t line_start = got.rfind('\n', i == 0 ? 0 : i - 1);
    const size_t from = line_start == std::string::npos ? 0 : line_start + 1;
    std::printf("trace differs at byte %zu: got \"%s\"\n", i, got.substr(from, 80).c_str());
  }
  CHECK(got == SERVO_GOLDEN, "the servo does exactly what firmware 2.0.2 did");
}


// ---- Codex reviews of 2.1.1 (fixed in 2.1.2) ------------------------------------------------------------------
static void test_review_212() {
  {
    current = "2.1.2: the daily cap counts every feed of a large load";
    Flash flash;
    FakeMechanism m;
    auto f = boot_with(flash, m);
    f->set_min_gap_s(10);
    f->set_daily_cap(9);
    int ok = 0;
    Result last = Result::OK;
    for (int i = 0; i < 10; i++) {
      last = f->request_feed(1, Source::ESPHOME);
      ok += last == Result::OK;
      f->advance(11000);
    }
    CHECK(ok == 9 && last == Result::DAILY_CAP, "nine single feeds, the tenth refused");
    CHECK(f->portions_24h() == 9 && f->position() == 9, "all nine counted");
  }
  {
    current = "2.1.2: a version 2 tray state is migrated";
    Flash flash;
    flash.have_state_v2 = true;
    legacy::StateV2 &v2 = flash.state_v2;
    v2.version = 2;
    v2.position = 3;
    v2.moving_to = NO_MOVE;
    v2.hist_ts[0] = static_cast<uint32_t>(T0 - 600);
    v2.hist_n[0] = 2;
    v2.hist_src[0] = static_cast<uint8_t>(Source::ESPHOME);
    v2.last_feed_epoch = static_cast<uint32_t>(T0 - 600);
    v2.fired[0][1] = 20000;
    auto f = boot(flash, T0);
    CHECK(f->position() == 3 && !f->position_unknown(), "position kept");
    CHECK(f->portions_24h() == 2, "history kept");
    CHECK(flash.state.version == STATE_VERSION && flash.state.fired[0][1] == 20000, "saved as version 3, all kept");
    Flash cut;
    cut.have_state_v2 = true;
    cut.state_v2 = v2;
    cut.state_v2.moving_to = 4;  // a move in progress when 2.1.1 lost power
    auto g = boot(cut, T0);
    CHECK(g->position_unknown(), "an unfinished move in a version 2 record still means unknown");
  }
  {
    current = "2.1.2: a reload is a move until it has settled";
    Flash flash;
    auto f = boot(flash, T0);
    f->declare_position(5);
    f->advance(2000);
    f->tray_reload();
    CHECK(flash.state.position == 0 && flash.state.moving_to == 0 && flash.state_flushed, "its intent is on flash");
    CHECK(f->busy() && f->tray_state() == TrayState::MOVING, "busy: no restart, no feed");
    CHECK(f->feed_guard_text() == "busy", "the guard says so");
    auto g = std::make_unique<FakeFeeder>(flash, f->now() * 1000, true);  // the power goes now
    g->start();
    CHECK(g->position_unknown(), "a power cut on the way: unknown");
    f->advance(2000);
    CHECK(!f->busy() && flash.state.moving_to == NO_MOVE && f->feed_guard_text() == "ready", "settled: ready");
    f->declare_position(3);
    CHECK(flash.state.moving_to == 3 && f->busy(), "a declare too");
    f->stop();
    CHECK(f->position_unknown() && flash.state.unknown, "stopped on the way: unknown");
  }
  {
    current = "2.1.2: a one-shot never feeds twice after a power cut";
    Flash flash;
    auto f = boot(flash, T0);
    f->set_wiggle(false);
    f->set_min_gap_s(10);
    CHECK(f->add_oneshot("2026-10-14 08:00", 1), "one-shot at 08:00");
    for (int i = 0; i < 2000 && f->count(Source::ONE_SHOT, Result::OK) == 0; i++)
      f->advance(250);
    CHECK(f->count(Source::ONE_SHOT, Result::OK) == 1, "fed");
    CHECK(flash.settings.oneshots[0].done == 1 && flash.settings_flushed, "done on flash before it moved");
    f->advance(1250);  // the move is over; the settings' delayed save has not run
    CHECK(flash.state.position == 1 && flash.state.moving_to == NO_MOVE, "the step committed");
    auto g = std::make_unique<FakeFeeder>(flash, f->now() * 1000, true);  // power cut, back in the same minute
    g->start();
    g->advance(5000);
    CHECK(g->count(Source::ONE_SHOT, Result::OK) == 0 && g->position() == 1, "not fed again");
  }
  {
    current = "2.1.2: a failed save stops the move";
    Flash flash;
    auto f = boot(flash, T0);
    const size_t commands = f->angles.size();
    flash.fail_saves = true;
    CHECK(f->request_feed(1, Source::ESPHOME) == Result::STORAGE_FAILED, "refused");
    f->advance(3000);
    CHECK(f->angles.size() == commands && f->position() == 0 && !f->position_unknown(), "nothing moved");
    CHECK(f->faults_text().find("saving to flash failed") != std::string::npos, "and Faults says so");
    CHECK(f->portions_24h() == 0, "nothing counted");
    f->tray_reload();
    f->advance(2000);
    CHECK(f->angles.size() == commands && f->position_unknown(), "a reload that cannot save its intent: unknown");
    flash.fail_saves = false;
    f->tray_reload();
    f->advance(2000);
    f->advance(60000);
    CHECK(f->request_feed(1, Source::ESPHOME) == Result::OK, "saves work again: feeds work again");
    f->advance(5000);
    CHECK(f->faults_text().find("saving to flash failed") == std::string::npos, "and the fault clears");
  }
  {
    current = "2.1.2: a failed save of a one-shot means no feed";
    Flash flash;
    auto f = boot(flash, T0);
    CHECK(f->add_oneshot("2026-10-14 08:00", 1), "one-shot at 08:00");
    f->advance(2000);
    flash.fail_saves = true;
    f->advance(6 * 60 * 1000);
    CHECK(f->count(Source::ONE_SHOT, Result::OK) == 0 && f->count(Source::ONE_SHOT, Result::STORAGE_FAILED) == 1,
          "refused, reported once");
    CHECK(f->position() == 0, "nothing fed");
  }
  {
    current = "2.1.2: something outside the core can ask for a restart";
    Flash flash;
    auto f = boot(flash, T0);
    CHECK(!f->restart_needed(), "nothing waiting");
    f->note_restart_needed("MQTT settings");
    CHECK(f->restart_needed(), "Restart Needed on");
    auto g = boot(flash, T0 + 60);
    CHECK(!g->restart_needed(), "and off after the restart");
  }
  {
    current = "2.1.2: a clock set back across a date does not repeat an event";
    Flash flash;
    auto f = boot(flash, T0);
    f->set_daily_cap(6);
    f->set_min_gap_s(10);
    enable_event(*f, 0, 0, Anchor::CLOCK, 8 * 60);
    f->advance(6 * 60 * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 1, "Oct 14 08:00 fed");
    f->jump_clock(local_epoch(2026, 10, 15, 7, 59));
    f->advance(3 * 60 * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 2, "Oct 15 08:00 fed");
    f->jump_clock(local_epoch(2026, 10, 14, 7, 59));  // corrected back a day
    f->advance(3 * 60 * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 2, "Oct 14 08:00 not fed again");
    f->jump_clock(local_epoch(2026, 11, 30, 7, 59));  // a wrong clock, far ahead
    f->advance(3 * 60 * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 3, "fed on the wrong date");
    f->jump_clock(local_epoch(2026, 10, 16, 7, 59));  // corrected: far back
    f->advance(3 * 60 * 1000);
    CHECK(f->count(Source::SCHEDULE, Result::OK) == 4, "the schedule is not silenced for weeks");
  }
}

int main() {
#ifdef PRINT_SERVO_TRACE
  std::fputs(servo_session().c_str(), stdout);
  return 0;
#endif
  test_servo_golden_trace();
  test_mechanism_capacity_and_motion();
  test_mechanism_fault();
  test_mechanism_tuning();
  test_mechanism_servo_only_calibration();
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
  test_polar_fallback();
  test_review_fixes();
  test_settings_migration();
  test_bug_batch_b();
  test_projection_matches_firing();
  test_daylight_saving();
  test_web_app_data();
  test_tz_parser();
  test_device_record();
  test_pins();
  test_reserved_pins();
  test_settings_password();
  test_ble_settings();
  test_network();
  test_setup_flag();
  test_firmware_upload();
  test_ntp_servers();
  test_review_212();
  if (failures == 0)
    std::printf("All core tests passed\n");
  return failures == 0 ? 0 : 1;
}
