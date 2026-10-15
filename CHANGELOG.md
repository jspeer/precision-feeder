# Changelog

Firmware versions (`fw_version` in packages/core.yaml), newest first. The BLE
protocol has its own version (docs/PROTOCOL.md). Bench results are in
[docs/BENCH.md](docs/BENCH.md).

*Written with the assistance of an AI (Anthropic's Claude).*

## 1.1.0: local first (in progress)

The built-in web app, so the feeder is fully usable from a browser with no Home
Assistant and no internet ([docs/plans/1.1.0-local-first.md](docs/plans/1.1.0-local-first.md)).

- New read-only entities, worked out by the same code that runs the feeder:
  - `Today's Feeds`: today's feeds in time order, each fed, skipped, due, next,
    later or held.
  - `Feed Guard`: what a one-portion feed would get right now ("ready" or the
    reason it would be refused), without feeding.
  - `Device Time` (diagnostic): the feeder's local time and UTC offset.
  - `Schedule Data` (diagnostic, disabled by default in Home Assistant): all
    three profiles in one compact line for the web app (format in DESIGN.md
    section 7).
- New optional package `network_info.yaml` (in both examples): Wi-Fi network,
  IP addresses and signal strength, from ESPHome's own sensors.
- `tools/mock_device.py`: a fake feeder that speaks the web server's HTTP, for
  building the web app without hardware.
- The web app's foundation replaces ESPHome's stock dashboard (`packages/web_server.yaml`), which loaded its
  script from the internet:
  - Plain JavaScript and CSS in `web/src/`, built into `web/dist/` by `tools/build_web.py` and embedded in
    the firmware (about 18 KB gzipped).
  - Pages and sections appear only for the entities a build contains.
  - Light and dark follow the browser; phones get a bottom tab bar.
  - A warning banner for the most important problem, with what to do.
  - A Diagnostics page: system, network, memory, live log, and every entity with a control.
  - Dashboard:
    - the tray as a dial of its seven slots, the next feed and runway;
    - Feed now, with the guard's current answer, reporting the device's own result;
    - today's feeds, status, recent feeds;
    - hold with durations, resume, tray reload, and stop while moving.
  - Recovering the tray position after a power cut: pick the slot you see, from the banner.
  - Schedule:
    - all three profiles at a glance, read from Schedule Data, so looking never moves the editor that Home
      Assistant shares;
    - make a profile active; the schedule on or off;
    - an event editor (clock time or sunrise / sunset with an offset, portions, days, on or off);
    - cadence and fast days, with "start the cycle today";
    - one-shot feeds in the feeder's own time, and clearing them.
    Each save writes the editor fields in the documented order, each confirmed by the device. A live event is
    switched off before it moves, so a half-applied change can never fire. The end result is checked against
    Schedule Data.
  - Configuration: the feeding safety limits, reload pause and refill interval, location, and the device's time
    zone, clock and network, each with the device's own limits and units, confirmed when saved.
  - Maintenance:
    - a calibration walkthrough (start; pick a slot and the servo goes there; nudge it by 0.1 or 1 degree;
      next slot; finish and save);
    - the servo pulse range;
    - declaring the tray position or reloading;
    - the 24 h count edit for debugging, behind its explanation.
  - Bluetooth (only in builds with BLE):
    - status, the client (the last two bytes of its address, connected for, last activity, MTU);
    - protocol facts and counters;
    - the activity log of the last 32 connections and writes with their results (from `/feeder/ble`),
      fetched again only when a connection or write happens.
    Read-only; nothing secret is shown, because the device never reports it.
  - Maintenance also appears in builds without the calibration package (tray reload and the 24 h count).
- IPv6 alongside IPv4 (`enable_ipv6` substitution in core.yaml, on by default): the web app, Home Assistant
  and mDNS answer on both, and Diagnostics lists each address with its kind (IPv4, IPv6, link-local,
  private). Wi-Fi still counts as connected on IPv4 alone, so IPv4-only networks are unaffected. IPv6-only
  networks need a static IPv4 address (an ESPHome limit; see the README).
- The web app waits for the device's entities to settle before showing a page, so a link straight to a page
  (for example `#diagnostics`) no longer misses the parts of the build that arrive last.
- Web app hardening:
  - After the feeder restarts with a different app (an update), an open page offers a reload. The build stamps
    each app with an id; a page checks it only when the device's uptime goes backwards.
  - A page hidden for 30 seconds gives its connection back (the web server has only a few) and reconnects when
    shown.
  - Schedule saves re-point the shared editor before every write, and say by name if another client's use of the
    editor changed a different event.
  - Page headings and titles for screen readers, focus moves to the page on navigation, and every Set, Press and
    switch control is named after its setting.
- BLE diagnostics (no protocol change, still protocol 2):
  - New entities `BLE Info` and `BLE Session` (diagnostic, disabled by default in
    Home Assistant): the connection, the last two bytes of the client's address,
    the MTU, and counters since boot.
  - The web server serves the last 32 BLE connections and writes at
    `/feeder/ble`.
  - The device log names opcodes ("Command FEED seq 5 -> ok") and logs
    connections.

## Examples: clone into the config folder (2026-10-15, no firmware change)

- The examples now expect the repository cloned into the ESPHome config folder
  as `precision-feeder/`, with a copy of an example next to it
  (`precision-feeder/...` paths). `git pull` updates the firmware; the user's
  file and `secrets.yaml` stay outside the repository. This replaces
  `examples/local.yaml`. `tools/check.sh` validates the examples in that layout.

## 1.0.1: quieter log (2026-10-15)

- "Clock set by NTP / Home Assistant / BLE" is logged at DEBUG instead of INFO.
  It was added to diagnose the Home Assistant time source; at INFO it appeared
  twice every 15 minutes. Turn on debug logging (`logger: level: DEBUG`) to see it.

## 1.0.0: first release (2026-10-14)

The first release: everything in this changelog, bench-tested on one complete
feeder (docs/BENCH.md). Firmware behaviour is the same as 0.5.2; since then
only internal refactors, all covered by the host tests:

- The BLE protocol runs in a host-tested engine; setting limits have one home;
  the scheduler and the projection share one source of events; the feed
  history is handled as records; the Python client is a package. Details below.
- `tools/check.sh` and a GitHub workflow run every check.

BLE protocol: version 2 (authenticated). Saved settings from 0.3.0 onwards are
migrated on update.

### Internal changes since 0.5.2

- Refactor (no behaviour change): the feed history is handled as records
  (`HistoryEntry`, newest first) through four helpers, instead of five parallel
  arrays shifted and indexed by hand; the saved layout is unchanged. The rule for
  which feeds the daily cap counts lives in one place (`counts_today_`).
- Python client split into the `precision_feeder_client` package: `protocol`
  (pure Python, standard library only, for reuse in other clients such as a PLC
  bridge), `client` (bleak), `selftest` and `cli`. `tools/precision_feeder_ble.py`
  stays the entry point and `import precision_feeder_ble` still works. A
  duplicate `PROTOCOL_VERSION` definition is gone.

## 0.5.2: refactors, tests and CI (2026-10-14)

- Settings limits have one home: `feeder_core.h`, enforced by FeederCore's
  setters, which Home Assistant, BLE and loading from flash all use. The range
  table in `protocol.h` is gone; `tests/test_consistency.py` checks the YAML
  min/max and the component schema against the C++ limits. BLE locations are
  checked as integers before conversion (as a float, 180.00001 degrees rounds to
  180.0).

- Refactor (no protocol change): the BLE protocol moved into `BleEngine`
  (`engine.h/.cpp`), plain C++ on top of FeederCore that talks to the radio only
  through a small `BleTransport` interface. `PrecisionFeederBle` is now a thin
  adapter (characteristics, hardware RNG, log, connect events). Record handlers
  return their answer instead of passing it through member variables. New host
  test `tests/test_ble_engine.cpp` drives the whole protocol against the
  simulated feeder (AUTH, retries, replay after a repeated AUTH, signing rules,
  every command, records, SET_TIME policy, disconnect); with today's two BLE
  bugs put back, it fails. The fake feeder is shared in `tests/fake_feeder.h`.

- Tests and CI: a daylight-saving test (a US Eastern fake clock: the skipped and
  repeated hours, daily times, and a sunset event across the change);
  `tools/check.sh` runs every host test (C++ now with -Werror) and validates the
  examples with ESPHome; `.github/workflows/ci.yml` runs it on every push (a full
  compile on manual dispatch).

- Refactor (no behaviour change): the scheduler and the projection get their
  events from one function, `for_each_occurrence_`, so what fires and what Next
  Feed / runway / Needs Food predict cannot drift apart again (the cause of two
  earlier bugs). New test `test_projection_matches_firing`: 25 random schedules
  over 3 simulated days, every scheduled feed must be in the predicted minute and
  no prediction may silently vanish.

## 0.5.1: bug sweep (2026-10-14)

- `selftest` (Python client) no longer cancels an existing hold: it tests
  retries by re-selecting the active profile, and checks that nothing changed.
- Next Feed (and Needs Food, runway) skip events inside the reload pause, as the
  scheduler does; they used to be shown and then skipped.
- Settings saves are flushed to flash (ESPHome otherwise waits up to 60 s, and a
  power cut in that time lost the edit).
- BLE: a signed command too short to hold an opcode is answered INVALID_ARG
  (the length arithmetic underflowed; harmless, but wrong).
- A multi-portion feed that is stopped part-way records only the portions that
  dropped (the step in progress counts; later ones do not).
- Guard settings are range-checked when loaded.
- Python client: day names are strict ("sunshine" is not Sunday), and bad input
  gives a one-line error instead of a traceback.

## 0.5.0: review fixes, publish on change, settings migration (2026-10-14)

- **BLE fix (found by the owner):** repeating a valid AUTH on an authenticated
  connection reset the last accepted sequence number, and the handler cleared
  the retry cache, so a write already executed in that session (a FEED, say)
  could run again if the guard allowed it. A repeated AUTH is now answered OK and
  changes nothing; the retry cache is only cleared on connect and disconnect.
  Regression test in tests/test_protocol.cpp (fails without the fix).
- Sun events that resolve to the evening before their day (for example
  "sunrise - 10 h") now feed; the scheduler only looked at today's and
  yesterday's events, so they never did, while Next Feed showed them.
- Setting and cancelling a hold is written to flash at once (a power cut within
  ESPHome's write interval could lose it).
- Settings layout version 4: the servo calibration (pulse range and offsets) is
  its own saved record, so a future settings change can never lose it. Version 3
  settings (firmware 0.3.0 to 0.4.x) are **migrated, not discarded**: schedule,
  guard settings, location and calibration are kept. Each record layout now has
  its own preference key, so an old record stays loadable for migration.
- Entities publish only when their value changes: the status, control, editor
  and calibration entities have no polling of their own; one 1-second interval
  per package compares and publishes (`publish_number/text/bool/option` in
  `precision_feeder.h`). Home Assistant and the log no longer see the same value
  every 2 to 30 seconds.
- Scheduler work: day contexts are cached for an hour, the sun-time cache covers
  the whole 30-day projection, and the projection refreshes every minute (and on
  any change) instead of every 30 seconds.
- Day names must be three letters or the full name (plural allowed); "sunshine"
  is no longer Sunday. Profile summaries list events in time order. An ordinary
  boot no longer rewrites the settings.
- `feeder_core.cpp` split into `feeder_core.cpp` (tray, guard, clock),
  `feeder_schedule.cpp` (scheduler, projection) and `feeder_text.cpp` (editor and
  status texts), with shared constants in `feeder_internal.h`.
- Python client: `config get/set`, `profile show/header/event`,
  `calibration get/set`, and `selftest` (checks a feeder's protocol without
  feeding).
- Docs: DESIGN.md is the current design only; this changelog and
  docs/BENCH.md hold the history and the bench results. README: how to put a
  password on the web dashboard.

## 0.4.1: HA time on by default, 2026-10-14

- `examples/basic.yaml` and `examples/ble.yaml` include the
  `time_homeassistant` package (it was commented out). No code change.

## 0.4.0: BLE protocol 2: authentication, 2026-10-14

- Every BLE write must be authenticated with the shared `ble_key` (section 10,
  docs/PROTOCOL.md). New Auth characteristic (000A), AUTH opcode (0x0D), result
  16 NOT_AUTHORIZED, capability bit 6. Profile, Config and Calibration writes
  gained a leading sequence number and are answered with it. **Not compatible
  with protocol 1 clients**: the PLC-side driver must implement protocol 2.
- `esp32_ble`: `auth_req_mode: sc_bond`.
- Retry cache entries carry the characteristic (channel), and the cache is
  cleared on every connect and disconnect.
- Each clock sync is logged ("Clock set by NTP" / "Home Assistant" / "BLE").
- Python client: `--key` / PRECISION_FEEDER_BLE_KEY; authenticates on connect
  and signs every write.

## 0.3.5: polar days, 2026-10-14

- On a day without a sunrise or sunset at the set location, sun events use
  06:00 (sunrise) or 18:00 (sunset) plus their offset instead of being skipped.
  Faults: "no sunrise or sunset today: sun events use 06:00 / 18:00"; BLE flag
  bit 6 stays set (it means a sun event could not use the real sun). No
  location set: still skipped. Test: `test_polar_fallback` (89 degrees N).

## 0.3.4: source label, 2026-10-14

- The feed source for the device's own feed buttons (pressed in Home Assistant
  or the web UI) is labelled "ESPHome" instead of "home assistant" in logs,
  "Last Feed" and "Recent Feeds" (owner request: it is an ESPHome entity call).
  Code name `Source::ESPHOME`. The BLE value is still 1, so the protocol is
  unchanged; only the label in PROTOCOL.md and the Python client changed. The
  clock source "Home Assistant" (HA time) keeps its name.

## 0.3.3: Needs Food, 2026-10-14

- New "Needs Food" binary sensor (status package, device class problem): on
  when the tray is empty, or the next scheduled or one-shot feed needs more
  portions than remain (the firmware would feed what is left, so the meal would
  come up short). Off while the position is unknown. Meant as an automation
  trigger. Also BLE Status flag bit 12 (an addition within protocol 1) and a
  Faults line. The projection now keeps the next feed's portions.

## 0.3.2: scheduling clarity, 2026-10-14

- New docs/SCHEDULING.md: building a profile step by step, field reference,
  rules and worked examples. Linked from the README and the editor package.
- New "Edited Profile" text sensor (editor package): the summary of the
  profile being edited, marked [active] / [not active], with a warning when its
  busiest day exceeds the daily cap. A separate sensor rather than part of
  "Active Schedule", which could then exceed Home Assistant's 255-character
  limit for a text sensor.
- Over-cap warning: when the active profile's busiest weekday (enabled events,
  fast days excluded) needs more portions than the daily cap, Faults says so
  and BLE Status flag bit 11 is set (an addition within protocol 1). The check
  is per weekday; the cap itself is a rolling 24 h window.

## 0.3.1: bench fixes, 2026-10-14

- "Next Feed" and the runway no longer include an event that already fed today
  (it could be moved later in the day, then shown as due while it would not
  feed). Test: `test_next_feed_skips_fired_event`.
- A clock kept by the RTC across a soft reset is no longer reported as an NTP
  sync (section 6). It shows as "kept across restart" (BLE: source 0 with the
  clock-valid flag) and does not block BLE `SET_TIME`. The sntp hook now calls
  `note_sntp_sync()`, which checks ESP-IDF's SNTP sync status.
- New Home Assistant diagnostic number "Portions Counted (24 h)": shows and
  sets the count the daily cap uses (decision log, 2026-10-14). New history
  source "adjustment" (internal; never sent over BLE).
- Docs: MG90S datasheet values and the deadband in docs/CALIBRATION.md;
  `examples/local*.yaml` for personal overrides (git-ignored).

## 0.3.0: servo pulse range settable at run time, 2026-10-14

- First bench result: the MG90S turned about 12% less per microsecond than the
  544 to 2400 us range assumes (slot 0 needed +11 degrees, slot 1 +14.5, and an
  uncalibrated position 6 stopped just past slot 5). The +-15 degree per-slot
  offsets cannot absorb that, and fixing it in YAML needs a reflash per try.
- The pulse range is now part of the saved settings and can be changed at run
  time: Home Assistant "Servo Min Pulse" / "Servo Max Pulse" (calibration
  package), BLE Config keys 10 and 11, capability bit 5. In calibration mode the
  servo moves to the selected position after each change. The YAML
  `min_pulse` / `max_pulse` are the defaults for a fresh device; saved values win.
  Limits 300 to 1500 / 1500 to 3000 us, at least 500 us apart.
- Settings version 3: **saved settings from earlier versions are discarded** on
  the first boot (schedule, guard settings, calibration offsets, location). The
  tray state is kept.
- BLE: an addition within protocol 1 (new keys and capability bit); nothing
  existing changed.

## 0.2.2: servo default, 2026-10-13

- Default `min_pulse` is 544 us (was 500), the pulse range the original firmware
  drives (ESP32Servo's default, 544 to 2400 us), so a fresh build starts the tray
  where the original did. The original was tuned on an SG92R; this build uses an
  MG90S, so the range still needs the bench check. Existing builds that set
  `min_pulse` themselves are unaffected; others will see every position shift by
  up to about 4 degrees and should re-check calibration.
- Docs: the tray geometry measured from the printed model (section 3).

## 0.2.1: review fixes, 2026-10-14

- Refactor: all logic moved into `FeederCore` (`feeder_core.h/.cpp`), plain C++
  with the clock, servo, storage and log behind `hal_*` hooks. `PrecisionFeeder`
  is now a thin ESPHome adapter. `tests/test_core.cpp` runs the tray, guard and
  scheduler against a simulated clock (13 test groups).
- The push button reloads the tray when its position is unknown, not only when
  empty, so a button-only build can recover from a power cut mid-feed.
- Scheduled and one-shot feeds refused with BUSY / TOO_SOON / hold are retried
  silently; the outcome is reported once (section 5.5). Previously every retry
  logged, changed "Last Result" and sent a BLE event-log notification.
- The BLE retry cache matches the whole command, not just the sequence number.
- Daily cap after a reboot without a clock (CHANGELOG.md deviations).
- One-shot feeds run with the Scheduler switch off; "Next Feed", runway and the
  over-capacity flag no longer count recurring events while it is off.
- The sunrise/sunset fault is evaluated every tick, and clears while the
  scheduler is off.
- The reload pause survives a reboot when the clock was known (state version 2;
  the saved tray state from earlier versions is discarded).
- "Minutes Since Clock Sync" is unknown until the clock has been set.
- Python client: answers matched on (seq, opcode); commands never use seq 0.

## 0.2.0: BLE protocol, and the two warnings fixed

- BLE protocol v1 implemented (section 8, `docs/PROTOCOL.md`).
- OTA authenticates with the API encryption key (no separate OTA password), and
  the web server's own `/update` page is disabled (`ota: false`), because it would
  accept an image in plaintext. Firmware uploads go through the encrypted ESPHome
  OTA only.
- `rgb_order` replaced by `channel_colors` (deprecated in ESPHome).
- The clock source is tracked and shown in Home Assistant ("Time Source",
  "Minutes Since Clock Sync").

## 0.1.1: cleanup after reviewing the original firmware's behaviour

- Late feeds removed: the "Late Start Grace" setting is gone and events fire only
  in the minute they are due (section 5.5). Settings version bumped to 2.
- Hold has no maximum: "Hold Duration" is in hours, 0 means until cancelled, and
  an indefinite hold needs no clock. A "Hold" status sensor shows what is active.
- The cancel-backlog step on tray reload is gone (nothing to cancel).
