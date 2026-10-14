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
- Keep DESIGN.md (and docs/PROTOCOL.md once it exists) updated in the same
  change as the code. Docs and code must not drift.

## Rules

### Provenance and licence
- The original firmware
  ([FlyingGyroscope/PrecisionFeederRev2](https://github.com/FlyingGyroscope/PrecisionFeederRev2))
  is AGPL-3.0. **Do not copy its code.** Learn behaviour and geometry, then
  write fresh code. Physical facts (positions, pitch, sweep) are fine to reuse.
- This project is MIT. Keep the AI-authorship disclosure in the README, the
  protocol doc and source file headers. Do not remove it.
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
  one feed guard. Never add a path that bypasses it.
- BLE commands are idempotent via sequence numbers. A retry must never feed
  twice.
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
- Bump the firmware version (`major.minor.revision`) when behaviour changes.
- No secrets, WiFi credentials, API keys or personal data in the repo. Use
  `!secret` and keep `secrets.yaml` out of version control.
- Generic naming only. No references to any particular user's home setup.

### Verification
- Validate and compile with ESPHome before claiming a change works. Report
  what was and was not tested.
- Things listed as unverified in DESIGN.md must be checked against current
  ESPHome docs and, where relevant, real hardware before being relied on.
- Do not flash or upload to a device without the owner's explicit say-so.
- Do not invent ESPHome options or BLE behaviour from memory; check the docs.

## Working agreements

- Prefer the smallest change that does the job. Do not refactor unrelated code.
- If a request is ambiguous or conflicts with DESIGN.md, ask before acting.
- Record new decisions and open questions in DESIGN.md as part of the change.

## Layout

```
README.md  LICENSE  DESIGN.md  AGENTS.md
docs/         PROTOCOL.md, CALIBRATION.md
components/   precision_feeder/ (external component)
packages/     reusable YAML packages
examples/     ha-only.yaml, ble-plc.yaml, standalone.yaml
tools/        Python bleak example client
```

Not all of this exists yet; see DESIGN.md section 12.
