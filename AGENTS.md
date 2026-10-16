# AGENTS.md

Guidance for AI agents (and humans) working on this repository.

## What this project is

ESPHome firmware for the Precision Feeder Rev2 fish feeder, a servo-driven
tray with 7 positions (position 0 closed, positions 1 to 6 open, so 6 portions
per load). It recreates the original Arduino firmware with full parity, then
improves on it: minute-accurate on-board scheduling with sunrise/sunset
anchors, three profiles, a source-agnostic feed guard, Home Assistant control,
and an optional BLE GATT control plane for external controllers.

Target hardware: Waveshare ESP32-S3-Zero (ESP32-S3FH4R2, 4 MB flash, 2 MB
PSRAM) and an MG90S servo. Other ESP32 boards should work by changing pin
substitutions.

## Read first

- [DESIGN.md](DESIGN.md) is the source of truth for behaviour, the BLE
  protocol, entities and decisions. Read it before changing anything.
- Its **Decisions log** and **Open items** sections record what was decided and
  what is unverified. Do not silently reverse a logged decision; say which one
  applies and ask.
- Keep DESIGN.md (and the guides in docs/) updated in the same
  change as the code. Docs and code must not drift.

## Rules

### Provenance and licence
- The original firmware
  ([FlyingGyroscope/PrecisionFeederRev2](https://github.com/FlyingGyroscope/PrecisionFeederRev2))
  is AGPL-3.0. **Do not copy its code.** Learn behaviour and geometry, then
  write fresh code. Physical facts (positions, pitch, sweep) are fine to reuse.
- This project is public domain (The Unlicense). Keep the AI-authorship
  disclosure and the credits (FlyingGyroscope as inspiration, Claude) in the
  README, the protocol doc and source file headers. Do not remove them.
- Do not add dependencies with incompatible licences.

### Scope of the hardware
- Only hardware on the ESP32 board is supported, plus the servo and the
  **optional** push button. No external RTC, sensors or I2C add-ons. Do not add
  support for them without an explicit decision.
- The button is optional. Everything must work with no button fitted.
- Position is open-loop. Never assume the tray position is correct after an
  interrupted move; follow the tray state machine in DESIGN.md.

### Safety behaviour (do not weaken)
- Every feed, from every source (schedule, HA, BLE, button), goes through the
  one feed guard. Never add a path that bypasses it. The one owner-approved
  exception (2026-10-14): the 24 h count the cap checks against can be edited
  by hand in Home Assistant for debugging and recovery, logged as a warning.
  Do not extend it (for example to BLE) without a new decision.
- BLE commands are idempotent via sequence numbers. A retry must never feed
  twice.
- Every BLE write is authenticated (protocol 2: AUTH, then a sequence number and
  tag on each write; docs/PROTOCOL.md). Never add a characteristic or opcode that
  acts without it, and never let unauthenticated clients read state.
- Time is never restored from flash. Scheduled feeds wait for a valid clock;
  manual feeds still work.
- No periodic or automatic reboot. WiFi or API loss must not reboot the device.
- Default to the safe side for the animals: a missed meal beats an overfeed.
- Never make up a missed feed later. Events fire only in the minute they are
  due; after downtime, a hold or a reload pause they are skipped.

### Configuration and style
- Every pin is a substitution with a documented default. No hard-coded GPIOs.
- Every feature is a package or build option with comments saying what the
  setting does and when to change it. Optional features (BLE, HA time, button,
  web UI) must compile out cleanly.
- Keep the BLE protocol backwards compatible. Changing a packet layout or an
  opcode means bumping the protocol version and updating docs/PROTOCOL.md.
- Bump the firmware version (`major.minor.revision`) when behaviour changes, and
  add the change to CHANGELOG.md. Record hardware results in docs/BENCH.md.
- A saved record's layout (Settings, Calibration, State in `feeder_core.h`) only
  changes with a new version, a new preference key and a migration from the old
  layout. Never let an update silently discard a user's settings or calibration.
- No secrets, WiFi credentials, API keys or personal data in the repo. Use
  `!secret` and keep `secrets.yaml` out of version control.
- Generic naming only. No references to any particular user's home setup.

### Verification
- Validate and compile with ESPHome before claiming a change works. Report
  what was and was not tested.
- Things listed as unverified in DESIGN.md must be checked against current
  ESPHome docs and, where relevant, real hardware before being relied on.
- Run `tools/check.sh` after any change: it builds and runs every host test (C++ with
  -Werror, and Python) and validates both examples with ESPHome; CI runs the same script.
  The individual commands, if you need one: With any C++17 compiler, from `tests/`:
  `c++ -std=c++17 -I../components/precision_feeder test_core.cpp ../components/precision_feeder/feeder_*.cpp`,
  `c++ -std=c++17 -I../components/precision_feeder test_logic.cpp`,
  `c++ -std=c++17 -I../components/precision_feeder_ble test_protocol.cpp`; and
  `python3 -m unittest test_precision_feeder_ble` in `tools/`. The C++ and Python
  protocol tests share golden byte vectors; change them together.
- Feeder logic belongs in `FeederCore` (`feeder_core.h`, implemented in `feeder_core.cpp`,
  `feeder_schedule.cpp` and `feeder_text.cpp`; the timezone parser is `tz_rules.h`), which must stay free of
  ESPHome includes so it can be tested on a host. Anything that needs the device
  (clock, servo, flash, log) goes through a `hal_*` hook implemented in
  `precision_feeder.cpp`. New behaviour gets a test in `tests/test_core.cpp`.
- Setting limits live in `feeder_core.h` and are enforced by FeederCore's setters
  (`set_min_gap_s()` and so on). Home Assistant, BLE and loading from flash all go
  through them; never write `cfg` fields directly from YAML or BLE. The YAML min/max
  repeat the limits for the input boxes, and `tests/test_consistency.py` checks them.
- BLE protocol behaviour belongs in `BleEngine` (`engine.*`), which must stay free of
  ESPHome includes; the radio is reached only through `BleTransport`. New protocol
  behaviour gets a test in `tests/test_ble_engine.cpp`.
- The web app (`web/src/`) uses the firmware's entities as its API, by name, all listed in
  `web/src/entities.js`; `tests/test_consistency.py` checks them against `packages/`. Renaming an entity
  means changing that table in the same change. The app never repeats guard or scheduler logic: it
  shows what the device reports and requests changes. After editing `web/src/`, run
  `python3 tools/build_web.py` and commit `web/dist/` too (check.sh fails if it is stale).
- Do not flash or upload to a device without the owner's explicit say-so.
- Do not invent ESPHome options or BLE behaviour from memory; check the docs.

## Working agreements

- Prefer the smallest change that does the job. Do not refactor unrelated code.
- If a request is ambiguous or conflicts with DESIGN.md, ask before acting.
- Record new decisions and open questions in DESIGN.md as part of the change.

## Layout

```
README.md  LICENSE  DESIGN.md  CHANGELOG.md  AGENTS.md
docs/         PROTOCOL.md, CALIBRATION.md, SCHEDULING.md, BENCH.md, WEB_UI.md; plans/ (release plans)
components/   precision_feeder/ (FeederCore: feeder_core.cpp tray, guard, clock; feeder_schedule.cpp
              scheduler and projection; feeder_text.cpp editor and status texts; calendar.h and
              solar.h, pure and host-tested; precision_feeder: the ESPHome adapter)
              and precision_feeder_ble/ (optional BLE protocol: protocol.h, crypto.h and BleEngine in
              engine.*, all host-tested; precision_feeder_ble.* is the ESPHome adapter)
packages/     reusable YAML packages
examples/     basic.yaml, ble.yaml
web/          the web app: src/ (plain JavaScript and CSS, no dependencies) and dist/ (built by
              tools/build_web.py, committed; packages/web_server.yaml embeds it)
tests/        host-side C++ tests (test_core, test_logic, test_protocol, test_ble_engine; fake_feeder.h),
              test_consistency.py, and test_web.mjs (the web app's parsers, run with Node)
tools/        precision_feeder_ble.py (entry point) and precision_feeder_client/ (protocol: pure, no
              dependencies; client: bleak; selftest; cli), with tests; check.sh runs every check;
              mock_device.py imitates the device's web server for building the web app
```

See DESIGN.md section 12 for the intended layout.
