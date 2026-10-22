# AGENTS.md

Guidance for AI agents (and humans) working on this repository.

## What this project is

ESPHome firmware for the Precision Feeder Rev2 fish feeder, a servo-driven
tray with 7 positions (position 0 closed, positions 1 to 6 open, so 6 portions
per load). Since 2.1.0 what moves the tray is a *mechanism* (the servo fan tray is
the reference one), so other motor formats can be added; see "Adding a mechanism". The second is
the direct-drive disc (a GA12-N20 gear motor and encoder behind a DRV8833); one firmware carries both, and the drive
is chosen on the device. It recreates the original Arduino firmware with full parity, then
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
- This project is MIT licensed (LICENSE; since 2.1.2, before that The Unlicense). Every new file gets the project's
  header ("MIT licence, see LICENSE"). Keep the AI-authorship
  disclosure and the credits (FlyingGyroscope as inspiration, Claude) in the
  README, the protocol doc and source file headers. Do not remove them.
- Do not add dependencies with incompatible licences.

### Scope of the hardware
- Only hardware on the ESP32 board is supported, plus the tray's motor (the servo,
  or another mechanism added under "Adding a mechanism") and the **optional** push
  button. A motor's own feedback (an encoder built onto it) and its driver are part
  of the motor. No external RTC, sensors (end stops, index switches, load cells) or
  I2C add-ons. Do not add support for them without an explicit decision.
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
  acts without it, and never let unauthenticated clients read state. With no key
  (none in the YAML, none saved on the device) AUTH is refused; never fall back to
  an all-zero key. The two BLE permissions (guard limits, calibration) refuse with
  NOT_AUTHORIZED detail 7; feeding is never behind them.
- Time is never restored from flash. Scheduled feeds wait for a valid clock;
  manual feeds still work.
- No periodic or automatic reboot. WiFi or API loss must not reboot the device. (A restart the
  user asked for, Restart or a firmware upload, waits until the tray is not moving.)
- Firmware uploads from the browser go only through `POST /feeder/firmware` (signed with the
  settings password, SHA-256 checked on the device; DESIGN.md section 9.5). Never turn on
  ESPHome's own `web_server: ota:`.
- Default to the safe side for the animals: a missed meal beats an overfeed.
- Never make up a missed feed later. Events fire only in the minute they are
  due; after downtime, a hold or a reload pause they are skipped.

### Configuration and style
- Every pin is a substitution with a documented default. No hard-coded GPIOs. Since
  2.0.0 the substitution is the starting value: a pin saved on the device (Device record,
  DESIGN.md section 2.1) wins, and only pins `check_pin()` accepts can be saved. A mechanism
  that drives its own pins (the direct-drive disc) offers them with `offer_motor_pins()`: they
  are chosen on the device too (D19, owner 2026-10-21; it replaced D4, which kept them YAML-only),
  saved in the MotorPins record, applied at the next restart, and reserved so the button and
  LED cannot be moved onto them.
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
  `c++ -std=c++17 -I../components/precision_feeder test_core.cpp ../components/precision_feeder/feeder_*.cpp ../components/precision_feeder/mechanism_*.cpp`,
  `c++ -std=c++17 -I../components/precision_feeder test_logic.cpp`,
  `c++ -std=c++17 -I../components/precision_feeder_ble test_protocol.cpp`,
  `c++ -std=c++17 -I../components/precision_feeder -I. -Ireference test_n20.cpp ../components/precision_feeder/feeder_*.cpp ../components/precision_feeder/mechanism_*.cpp`; and
  `python3 -m unittest test_precision_feeder_ble test_make_release` in `tools/`. The C++ and Python
  protocol tests share golden byte vectors; change them together.
- Feeder logic belongs in `FeederCore` (`feeder_core.h`, implemented in `feeder_core.cpp`,
  `feeder_schedule.cpp`, `feeder_text.cpp`, `feeder_password.cpp`, `feeder_pins.cpp` and `feeder_network.cpp`; the timezone parser is `tz_rules.h`, SHA-256 and
HMAC are `crypto.h`; what moves the tray is a mechanism, `mechanism.h`, the servo's in `mechanism_servo.*`, the disc's in `mechanism_n20.*`), which must stay free of
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
- Do not push a release tag (`v*`: it publishes a GitHub release and the install page) without the
  owner's explicit say-so. A manual run of the Release workflow only builds.
- Do not invent ESPHome options or BLE behaviour from memory; check the docs.

## Adding a mechanism

A mechanism is everything that turns "drop one portion" into motion: a motor, its
driver and the tray or disc it moves. The servo fan tray (`mechanism_servo.*`,
`packages/mechanism_servo.yaml`) is the reference; read DESIGN.md section 3.0 and
[docs/MECHANISMS.md](docs/MECHANISMS.md) (the checklist) before adding one.

1. **Implement the interface, nothing more.** A mechanism implements `Mechanism`
   (`mechanism.h`): `capacity`, `feed_one`, `reload`, `restore`, `poll`, `stop`,
   `release`, `settle_ms`, and optionally calibration points and a tuning table. It
   never calls the guard, the scheduler, the history or the save hooks, and never
   starts a move by itself. The core decides; the mechanism carries out one verb at a
   time and reports `BUSY`, `DONE` or `FAULT`.
2. **Logic in core, pins in the adapter.** Its logic goes in
   `components/precision_feeder/mechanism_<name>.{h,cpp}`, free of ESPHome includes
   (`check.sh` builds `mechanism_*.cpp` into the host tests). It reaches hardware only
   through hooks named for it, implemented in the adapter, with fakes in the tests. A
   control loop faster than ESPHome's main loop runs in its own task in the adapter;
   the pure part takes the time and its inputs as arguments so the host can test it.
3. **Faults are the safe side.** A fault, a timeout or an interrupted move ends with
   the motor stopped and `poll()` answering `FAULT`; the core then marks the position
   unknown. A motor that can keep turning must have a travel limit, a move timeout
   worked out from the distance, and a watchdog on its control loop. It never retries
   a feed and never reverses past the slot it started from (a bounded wiggle aside).
   A missed meal beats an overfeed.
4. **Tuning is a table.** Every tuning value is declared once in the mechanism's
   `TuningParam` table (key, name, unit, limits, default, scale) and changed only
   through `FeederCore::set_tuning`, which enforces the limits for Home Assistant, the
   web app and flash. Its YAML entities repeat the limits and `tests/test_consistency.py`
   checks them. Changing the table's entries or order means a new `tuning_layout()`;
   saved values of another layout are never read as the new one.
5. **One package, one name.** A mechanism has a unique `name()` (the Mechanism
   entity, the Tuning record), one package `packages/mechanism_<name>.yaml` (its
   `tray_capacity`, pins as substitutions with documented defaults, its tuning and
   calibration entities, protected and in the config category), and an example.
   `core.yaml` always includes the servo's; another mechanism's package is added beside
   it in the build's YAML and becomes a drive chosen on the device (Drive, DESIGN.md
   section 3.4), its driver never set up while it is not the drive. The prebuilt
   binaries default to the servo. Its motor pins go through `offer_motor_pins()` (chosen on the device,
   checked by `check_pin()`, reserved from the button and LED) and get pin selects in its package.
6. **The web app follows the device.** A mechanism adds its description to
   `MECHANISMS` in `web/src/mechanisms.js` under its `name()` (its tray drawing, its
   wording, its calibration cards as data) and lists its entities in
   `web/src/entities.js`. Shared pages are not edited for a mechanism. A description
   holds layout and wording only: limits and behaviour come from the device.
7. **Tests and the bench.** It comes with host tests against a simulated motor
   (normal moves, a full load fed to empty, every fault, a stop mid-move) and a section
   in docs/BENCH.md. Bench code it was tuned with may be committed under
   `tests/reference/` with the project's header. The servo's golden trace
   (`tests/servo_golden.h`) must keep passing.
8. **Where it lives.** A mechanism for one person's build lives on a
   `variant/<name>` branch: never tagged, no prebuilt binaries, its own
   `fw_variant` and version suffix (`2.1.0-<name>.N`), and a README banner. It moves
   to main only with the owner's decision, and then joins CI and, if wanted, the
   release. The direct-drive disc moved to main in 2.1.0 (owner, 2026-10-21).
9. **Docs.** DESIGN.md gets a subsection under section 3 for it (geometry, motion,
   faults, calibration, decisions), docs/CALIBRATION.md a section, CHANGELOG.md an
   entry.

## Working agreements

- Prefer the smallest change that does the job. Do not refactor unrelated code.
- If a request is ambiguous or conflicts with DESIGN.md, ask before acting.
- Record new decisions and open questions in DESIGN.md as part of the change.

## Layout

```
README.md  LICENSE  DESIGN.md  CHANGELOG.md  AGENTS.md
docs/         user guides: INSTALL.md (prebuilt firmware), BUILDING.md (YAML builds), USING.md, WEB_UI.md,
              SCHEDULING.md, CALIBRATION.md, SECURITY.md; PROTOCOL.md (BLE, authoritative); BENCH.md (hardware
              results); plans/ (release plans)
components/   precision_feeder/ (FeederCore: feeder_core.cpp tray, guard, clock; mechanism.h the mechanism
              interface, mechanism_servo.* the servo fan tray, mechanism_n20.* the direct-drive disc; feeder_schedule.cpp
              scheduler and projection; feeder_text.cpp editor and status texts; feeder_password.cpp
              settings password, BLE key and BLE permissions; feeder_pins.cpp runtime pins and pin validator;
              feeder_network.cpp name, access-point password and Wi-Fi checks; calendar.h and
              solar.h, pure and host-tested; precision_feeder: the ESPHome adapter)
              and precision_feeder_ble/ (optional BLE protocol: protocol.h and BleEngine in
              engine.*, all host-tested; precision_feeder_ble.* is the ESPHome adapter)
              and precision_feeder_n20/ (the disc on the device: DRV8833 through LEDC, encoder through PCNT)
packages/     reusable YAML packages (core.yaml includes mechanism_servo.yaml and tray_recovery.yaml;
              mechanism_n20.yaml adds the direct-drive disc and drive_select.yaml; mqtt.yaml is optional)
examples/     basic.yaml, ble.yaml, disc.yaml
firmware/     full.yaml, no-ble.yaml: the prebuilt binaries (tools/build_bins.sh; nothing secret may reach them)
install/      index.html: the install page, published to GitHub Pages by .github/workflows/release.yml
web/          the web app: src/ (plain JavaScript and CSS, no dependencies; tz_zones.js is generated by
              tools/gen_tz_zones.py) and dist/ (built by
              tools/build_web.py, committed; packages/web_server.yaml embeds it)
tests/        host-side C++ tests (test_core, test_logic, test_protocol, test_ble_engine, test_n20 with motor_sim.h and
              reference/, the disc's bench sketch; fake_feeder.h),
              test_consistency.py, and test_web.mjs (the web app's parsers, run with Node)
tools/        precision_feeder_ble.py (entry point) and precision_feeder_client/ (protocol: pure, no
              dependencies; client: bleak; selftest; cli), with tests; check.sh runs every check;
              mock_device.py imitates the device's web server for building the web app; build_bins.sh and
              make_release.py build the prebuilt binaries and their manifests
```

See DESIGN.md section 12 for the intended layout.
