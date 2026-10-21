# Plan: a feeder mechanism interface, then a direct-drive N20 variant

Status: **stage 1 built on `release/2.1.0-mechanism` (2026-10-21), host-tested; not merged, not on hardware.**
**Stage 2 built on `variant/n20-direct-drive` (2026-10-21), host-tested and compiled; not on hardware, not pushed.**
S2.1 to S2.7 and S2.9's docs are done; S2.0, S2.0b and S2.8 need the feeder on the bench. D6 checked (GPIO3: Espressif's ESP-IDF JTAG guide for the ESP32-S3). Nothing replaces a
working binary before it has been on its own branch.

Stage 1 validation (2026-10-21, host only; no hardware attached):
- The golden servo trace (S1.0, recorded from 2.0.2) matches exactly.
- 2.0.2's own tests, run unchanged against 2.1.0 (only an empty `hal_save_tuning` added to its fakes): every one
  passes except two assertions that encode intended changes, GPIO3 refused (D6) and an unpublished variant's
  update state (`unknown`, now `unlisted`).
- A differential test, outside the repository: 2,000 random sessions of 400 steps (feeds from every source and of
  every size, stops, reloads, declares, calibration, pulse ranges, guard settings, holds, the 24 h edit, events,
  power cuts with and without a clock) through the 2.0.2 core and the 2.1.0 core. Every servo command, release,
  saved record (byte for byte), log line and status text was identical: 2.8 million lines, 3,352 feeds. The same
  test catches a 1 ms change in the servo's step time in 95 of 100 sessions.
- Both examples' resolved ESPHome configuration is unchanged apart from `tray_capacity`.
- Both examples compile without warnings. Flash: basic 1,111,391 to 1,115,419 bytes (+4.0 KB), BLE 1,445,435 to
  1,449,807 (+4.3 KB); RAM +256 bytes each.

Stage 1 as built, where it differs from the steps below (DESIGN.md section 13 has the same list):
- S1.4: the tuning values got their own record, `Tuning` (`precision_feeder.tuning.v1`), instead of a block in a
  Calibration version 2. No existing record changes layout, and a servo build never writes the new one.
- S1.5: as D4 says, no Device record change; motor pins are reserved (`reserve_pin`). Pins by role were not needed.
- S1.6: the servo's YAML moved into `packages/mechanism_servo.yaml`, included by `core.yaml` (a nested package), so
  a build for another mechanism changes one line. `precision_feeder`'s `output:` is optional.
- S1.7: the mechanism descriptions are one file, `web/src/mechanisms.js`, not a folder (the 64 KB budget). Cards
  are `{title, hint, settings, buttons, readings}`.
- Not done in stage 1, left for stage 2: registering a mechanism from YAML (`set_mechanism()` from a component's
  code generation), and runtime button and LED pins in a build with no servo pin (`apply_pins()` returns early
  without one). Nothing in this plan is built.
Stage 1 changes the main firmware; stage 2 is a branch for one specific build that others can copy. Decisions
still open are marked **open**; the rest are the owner's answers of 2026-10-20 and are logged in
[DESIGN.md](../../DESIGN.md) section 13 when the step that needs them lands. Each step updates DESIGN.md, the
guides and CHANGELOG.md in the same change, and `tools/check.sh` passes.

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude); reviewed by
a human, but it may still contain mistakes.

## Premise

Today the firmware assumes one mechanism everywhere: a hobby servo swinging a 7-slot fan tray back and forth, 25
degrees per slot, slot 0 closed, moved open-loop on a timer. The core works in servo angles and tray positions.

The direct-drive disc works differently:
- **15 slots, all open**, 24° apart, on a 360° disc. One slot always sits over the chute, so **14 slots hold food**.
- It **only turns forward**. It never rewinds; the only reverse motion is the small wiggle.
- **Reload does not move.** After 14 feeds the slot over the chute is the one that just dropped; the user refills the
  other 14 and says "reloaded". (On the servo, reload swings back to the closed slot 0 first.)
- Closed loop: a quadrature encoder tells the motor where it is during a move.

So the right abstraction is not "angle" or "slot number" but what the feeder does. Stage 1 gives the core a
**Mechanism** with a handful of verbs, and moves everything servo-specific behind it:

| Verb | Servo fan tray (today's behaviour) | Direct-drive disc |
|---|---|---|
| `capacity()` | 6 | 14 |
| `remaining()` | 6 − position | 14 − portions fed since reload |
| `feed_one()` | step to position + 1 | turn forward one slot (24°) |
| `wiggle()` | step back one slot and forward again | ± amplitude about the slot, N cycles, back to centre |
| `reload()` | move to closed slot 0 | no motion; count reset |
| `restore()` (boot) | command the servo to the saved position | no motion; encoder rebased to the slot grid |
| `stop()` | release the servo | brake, then coast, bridge in standby |
| `poll()` | `BUSY` / `DONE` / `FAULT(reason)` | same |

The core keeps what is the same for every mechanism: the feed guard, persist-intent-before-moving, `UNKNOWN` after an
interrupted move, history, scheduling. The mechanism never decides to feed; it only carries out a verb the core
started after the guard allowed it.

## Constraints from AGENTS.md that shape this

- FeederCore stays free of ESPHome includes; hardware goes through `hal_*` hooks. Both mechanisms' logic (servo
  timing, the encoder control loop) live in core and are host-tested; only the pins are in the adapter.
- Every feed goes through the one feed guard; the mechanism sits below it.
- After an interrupted move the position is `UNKNOWN`. The encoder does not change that: it is relative, has no index
  and is lost on a power cut.
- Saved record layouts change only with a new version, a new key and a migration.
- Hardware scope is "the ESP32 board, the servo and the optional button". Stage 1 widens it (D14, S1.9): new motor
  formats may be added as mechanisms, following rules written into AGENTS.md. Sensors, RTCs and I2C add-ons stay out.
- Entities are the API for Home Assistant and the web app. Stage 1 renames none.
- No release tag, no push, no flashing without the owner's say-so.

## Branches

| Branch | From | Holds | Built |
|---|---|---|---|
| `release/2.1.0-mechanism` | `main` | stage 1 | `ci.yml` runs `check.sh`; merged and released as 2.1.0 when the owner says |
| `variant/n20-direct-drive` | the stage 1 tip (rebased onto main once 2.1.0 merges) | stage 2 | `check.sh` only. **No release workflow, no prebuilt binaries, no install page entry, no tag** |

---

# Stage 1: the Mechanism interface (firmware 2.1.0)

Goal: same feeder, same entities, same BLE bytes, same behaviour for every servo build; underneath, the core talks
only to a Mechanism.

## Stage 1 decisions

| # | Decision | Status |
|---|---|---|
| D1 | Mechanism verbs as above; the mechanism is chosen at build time (YAML), one per firmware, so no build carries two. A dual build might be tried later; not in this plan | owner, 2026-10-20 |
| D1a | Saved State keeps `position` and means "portions fed since reload" (0 to capacity), so `remaining = capacity − position` for both mechanisms. For the servo that is exactly today's meaning (position n = n fed). **No State layout change**. For the disc: remaining = slots − 1 − portions fed; reload moves nothing and sets remaining back to slots − 1 | owner, 2026-10-20 |
| D1b | Guard limits (daily cap, max per command, event portions) are bounded by `capacity()` instead of a literal 6. Unchanged for the servo | owner, 2026-10-20 |
| D3 | Old YAML keys (`sweep_degrees`, `min/max_pulse_width`, `invert`, `step_time`, `wiggle`, `detach_delay`) keep working as the servo's settings; no user YAML, saved setting or existing servo behaviour breaks (the owner runs one) | owner, 2026-10-20 |
| D4 | Follows D1: the build chooses the mechanism, so a non-servo mechanism's motor pins are YAML substitutions only. **No Device record change.** The servo keeps its runtime Servo Pin; button and LED keep their runtime selects, which refuse a pin the build's motor uses. AGENTS.md's pin rule is reworded to say so | owner, 2026-10-20 |
| D5 | BLE stays protocol 2 as long as no packet layout or opcode changes. Info's `positions` byte becomes capacity + 1 (7 for the servo, so unchanged); accepted ranges follow it. Config keys 10 and 11 answer `NOT_SUPPORTED` on a mechanism without a pulse range. If a step turns out to need a layout change, it stops and asks | owner, 2026-10-20 |
| D15 | One web app for every build. Shared pages read `Capacity` and `Mechanism`; each mechanism has a description file in `web/src/mechanisms/` (tray drawing, calibration cards, labels), mostly data, drawn by one shared renderer | owner, 2026-10-20 |
| D14 | AGENTS.md allows additional motor formats as mechanisms, under the rules in S1.9. The motor's own feedback (an encoder on the motor) counts as part of the motor; separate sensors (end stops, index or hall switches, load cells) still need their own decision. The N20 has no separate sensor: its encoder is built onto the motor and wired as in stage 2's table | owner, 2026-10-20 |
| D6 | GPIO3 moves from refused to **accepted with a warning** in `check_pin()`, so the variant can use it for the button. On the ESP32-S3 GPIO3 is a strapping pin only for JTAG source selection, and the chip reads it only when an eFuse that is off by default is burned. Check the ESP32-S3 datasheet before relying on that. Done on main so the validator doesn't differ between branches | owner chose GPIO3; **open: Claude checks the datasheet** before S1.5 |

## Stage 1 steps

Each step is one commit on `release/2.1.0-mechanism`, `tools/check.sh` green.

### S1.0 Record today's behaviour as a test (before any refactor)

- A scripted session through the fake HAL: boot, feed 1, feed 3 with gaps, wiggle on and off, reload, calibration
  jog and save, power loss mid-step, declare position. Record every `hal_servo_write(angle)` and
  `hal_servo_release()` with its `hal_millis()` time, and every saved State.
- Store as a golden trace in `tests/`. Every stage 1 step must reproduce it exactly.

### S1.1 The Mechanism interface in core

- `Mechanism` (pure, in `feeder_core.h`): the verbs in the table, plus `name()`, `has_calibration()` and
  `tuning()` (S1.3).
- FeederCore motion phases become `FEED` (wait for `DONE`/`FAULT`), `WIGGLE`, `GAP`, `RELOAD`, `SETTLE`. FeederCore
  still persists the intent, commits, aborts (`FAULT` takes today's mid-step abort path: `UNKNOWN`, the portion
  counts as fed, later portions uncounted) and writes history.
- Replace `NUM_POSITIONS` / `MAX_PORTIONS` in logic with `capacity()`. Keep them only where a legacy record layout
  needs them. A `MAX_CAPACITY` (15) sizes anything new.
- The setters for the guard limits clamp to `capacity()` (D1b).

### S1.2 ServoMechanism (today's code, moved)

- Move `angle_for_()`, the timed steps, the slot-back wiggle, reload to slot 0, boot restore and detach into
  `ServoMechanism`. Hardware calls unchanged: `hal_servo_write()`, `hal_servo_release()`.
- The servo's calibration (7 offsets, pulse range, calibration position and jog) is the servo mechanism's.
- The S1.0 golden trace must match exactly.
- A host test with a fake 14-capacity mechanism runs feed, guard, schedule, empty and reload end to end.

### S1.3 Tuning parameters as a table

- Each mechanism publishes `{key, name, unit, min, max, step, default}` for its tuning values. The limits live there
  (one source of truth), enforced by one setter; Home Assistant, BLE and flash loading all go through it.
- Servo: min and max pulse (runtime, as today). Step time, wiggle, detach, invert and sweep stay YAML-only.
- `test_consistency.py` checks each tuning entity's YAML min/max against the table.

### S1.4 Calibration record version 2

- New key `precision_feeder.calibration.v2`: `mechanism u8`, a tuning block (`int32_t tuning[TUNING_MAX]` in the
  table's order, with its own layout number) and the servo's offsets.
- Migrated from v1 with the offsets and pulse range carried over. `legacy::CalibrationV1` kept.
- If the saved record is for a different mechanism than the build, it is not applied: defaults are used, the old
  record is left untouched until the user saves calibration, and a warning is logged. Never discarded silently.

### S1.5 Pins and GPIO3 (D4, D6)

- No Device record change. A mechanism declares its build-time motor pins to core (`build_motor_pins()`), and the
  runtime button and LED selects refuse those pins, as they refuse a clash today.
- AGENTS.md "Every pin is a substitution" gains: a non-servo mechanism's motor pins are YAML-only (D4).
- D6 first: check GPIO3 in Espressif's ESP32-S3 datasheet (strapping pins and JTAG source selection) and record the
  source in DESIGN.md. If it holds, GPIO3 moves to `PINS_WARNED` with its note; pin selects and `test_consistency.py` updated; DESIGN.md section 2.1.

### S1.6 Adapter and YAML

- `__init__.py`: an optional `mechanism:` block (`servo:` with the existing keys; old top-level keys still
  accepted). `precision_feeder.cpp` builds the mechanism from it.
- Diagnostic entities so the web app hard-codes nothing: `Mechanism` (text) and `Capacity` (portions per load).
- Calibration entities stay in `calibration.yaml` for the servo; stage 2 brings its own.

### S1.7 Web app, BLE, update check

- Web app, one app for every build (D15). Today it already builds itself from the entities present: a tab shows
  only if its key entity exists (`app.js`), and each card is wrapped in `store.has()`. What is servo-specific is the
  Dashboard's tray dial (seven slots on an arc, "of 6 left", `dashboard.js`) and the calibration cards on the
  Maintenance page. So:
  - **Shared pages stay shared.** Dashboard, Schedule, Configuration, Bluetooth, Firmware use `Capacity` for every
    portion count and limit, never a literal 6.
  - **`web/src/mechanisms/servo.js`**, a description of the mechanism: `dial(state)` (the arc of 7), the
    calibration cards as data (`{title, hint, entities: [E.servoMinPulse, ...], actions}`, in order), and labels
    ("slot 3", "of 6 left"). On main this is the only one.
  - **`web/src/mechanisms/index.js`** picks the description from the `Mechanism` entity's value; if the entity is
    missing (a 2.0.x device), it uses the servo's.
  - **One shared renderer** (`ui.js`) turns card descriptions into cards: numbers, switches, selects and buttons
    from their entities, a card hidden when none of its entities exist, protected ones through `auth.js` as now.
    The Maintenance page's calibration section becomes "render the mechanism's cards"; position recovery and the
    24 h count stay shared.
  - Rule: a description file holds layout and wording only. Limits, steps and behaviour come from the device.
  - `entities.js` gains `mechanism` and `capacity`; `test_consistency.py` also checks that every entity a
    description file names is in `entities.js`; `tests/test_web.mjs` tests the picker and the renderer (cards hidden
    when their entities are missing). `web/dist/` rebuilt, and its size measured against the 2.0.2 build.
- BLE engine: ranges from `capacity()`; Config 10/11 per D5. The C++ and Python golden vectors stay the same.
- Update check: a `Firmware Variant` the manifest does not list shows "No published updates for this variant"
  instead of offering an image. A branch device must never be offered main's servo firmware.

### S1.8 Docs, version, bench

- DESIGN.md: sections 3 to 3.2 rewritten around the Mechanism (servo facts kept as the servo's); D1 to D6 logged.
- BUILDING.md (`mechanism:`), CALIBRATION.md (per mechanism), CHANGELOG 2.1.0.
- Servo bench: the S1.0 scenario by hand; a 2.0.2 → 2.1.0 upgrade keeps settings, calibration, pins and state; BLE
  selftest; the calibration section of the web app's Maintenance page. Results in docs/BENCH.md.

### S1.9 AGENTS.md: rules for adding a motor format (D14)

Change "Scope of the hardware" to allow mechanisms, add a new section "Adding a mechanism", and add
`docs/MECHANISMS.md` (a short guide with the servo as the worked example and a checklist). Draft text for AGENTS.md:

> **Adding a mechanism (a motor format)**
>
> A mechanism is everything that turns "feed one portion" into motion. The servo fan tray is the reference
> mechanism; others (for example the direct-drive disc on `variant/n20-direct-drive`) follow the same structure.
>
> 1. **Implement the interface, nothing more.** A mechanism implements `Mechanism` (`feeder_core.h`): `capacity`,
>    `remaining`, `feed_one`, `wiggle`, `reload`, `restore`, `stop`, `poll`, `tuning`. It never calls the guard,
>    the scheduler, history or the save hooks, and it never starts a move by itself. The core decides; the
>    mechanism carries out one verb at a time and reports `BUSY`, `DONE` or `FAULT`.
> 2. **Logic in core, pins in the adapter.** The mechanism's logic goes in `components/precision_feeder/mechanism_<name>.{h,cpp}`,
>    free of ESPHome includes. Hardware access is only through new `hal_*` hooks, named for the mechanism
>    (`hal_<name>_...`), with fakes in `tests/fake_feeder.h`. A loop faster than ESPHome's main loop runs in its
>    own task in the adapter; the pure part takes time and inputs as arguments so the host can test it.
> 3. **Faults are the safe side.** Any `FAULT`, timeout or interrupted move ends with the motor stopped and the
>    core marking the position `UNKNOWN`. A mechanism that can keep turning must have a travel limit, a move timeout
>    derived from the distance, and a control watchdog. It never retries a feed and never reverses past the slot
>    it started from (except a bounded wiggle). Missed meal over overfeed.
> 4. **Tuning is a table.** Every tuning value is declared once in the mechanism's tuning table with name, unit,
>    limits and default; one setter enforces it for Home Assistant, BLE and flash. The YAML entities repeat the
>    limits and `tests/test_consistency.py` checks them. Runtime values are saved in the Calibration record's
>    tuning block under the mechanism's id and tuning-layout number; changing that layout means a new layout number
>    and a migration, like any saved record.
> 5. **One package, one id.** A mechanism gets a unique id (`Mechanism` entity and Calibration record), one YAML
>    package `packages/mechanism_<name>.yaml` (pins as substitutions with documented defaults, tuning entities in
>    the config category and protected, calibration controls), and an example. Pins use motor roles and go through
>    `check_pin()`. Everything compiles out when the package is not included.
> 6. **The web app follows the device.** A mechanism adds one description file, `web/src/mechanisms/<name>.js`
>    (its tray drawing, its calibration cards as data, its labels), registers it in `mechanisms/index.js` under its
>    `Mechanism` value, and lists its entities in `web/src/entities.js`. Shared pages are not edited for a
>    mechanism. The file holds layout and wording only: limits and behaviour come from the device.
> 7. **Tests and bench.** A new mechanism comes with host tests against a simulated plant (normal moves, a full
>    load fed to empty, each fault, stop mid-move) and a bench section in `docs/BENCH.md`. Bench reference code may
>    be committed under `tests/reference/` with the project's header.
> 8. **Where it lives.** A mechanism for one person's build lives on a `variant/<name>` branch: not tagged, no
>    prebuilt binaries, its own `Firmware Variant` and a README banner. It moves to main only with the owner's
>    decision, and then it joins `check.sh`, CI and, if wanted, the release.
> 9. **Docs.** DESIGN.md gets a subsection for the mechanism (geometry, motion, faults, calibration, decisions);
>    CALIBRATION.md a section; CHANGELOG an entry.

`tests/test_consistency.py` gains checks that make some of these rules mechanical: every `hal_<name>_*` hook has a
fake, every tuning entity matches its table, and every `mechanism_*.yaml` has its entities in `entities.js`.

## Stage 1 done when

- The golden trace passes unchanged; check.sh green; examples and firmware YAMLs compile within a few KB of 2.0.2.
- A 2.0.2 device upgraded to 2.1.0 keeps everything (bench).
- The fake 14-capacity mechanism test passes.

---

# Stage 2: direct-drive N20 variant (branch `variant/n20-direct-drive`)

## Hardware (as built)

| Part | Detail |
|---|---|
| Driver | DRV8833 dual H-bridge, channel A |
| Motor | GA12-N20, 6 V, 35 rpm, two-channel Hall encoder |
| Disc | 3D printed, 15 open slots, 24° apart, 360°, direct drive; one slot over the chute, 14 hold food |
| Power | Driver VM from the ESP32 5 V, 100 µF across 5 V and GND, encoder from 3V3, grounds common |

| Connection | ESP32-S3 pin | Substitution (default) |
|---|---|---|
| Motor M1 → DRV AO1, M2 → AO2 | (driver outputs) | |
| DRV AIN1 | GPIO1 | `motor_in1_pin` |
| DRV AIN2 | GPIO2 | `motor_in2_pin` |
| DRV STBY | GPIO6 | `motor_standby_pin` |
| Encoder C1 (A) | GPIO4 | `encoder_a_pin` |
| Encoder C2 (B) | GPIO5 | `encoder_b_pin` |
| Button (optional) | **GPIO3** | `button_pin` (main's default GPIO2 is AIN2 here) |
| Status LED | GPIO21 | `status_led_pin` |

GPIO1 and GPIO2 float briefly at reset; the firmware drives STBY low first so the bridge stays off. Whether a
pull-down on STBY is also needed is a bench item.

## What the bench sketch established

From `motor-test` v7 (`src/main.cpp`, its README and `bench/`). These are the defaults and the tuning parameters:

| Parameter | Bench value | Tuning entity |
|---|---|---|
| Encoder ticks per output revolution (quadrature ×4) | 8344 (provisional until checked against a mark) | Ticks Per Revolution |
| Slots, degrees per slot | 15, 24° | Slots (build), Degrees Per Slot (derived 360/slots; shown read-only) |
| Calibration turns for measuring ticks/rev | 10 | Calibration Turns |
| Drive power | 100 % (8-bit LEDC, 20 kHz) | Drive Power |
| Braking distance (start creeping this far before the target) | 160 ticks | Braking Distance |
| First brake pause before creeping | 100 ms (10 ms in wiggle legs) | Approach Brake |
| Brake pause between creep pulses | 15 ms (8 ms in wiggle legs) | Creep Brake |
| Creep pulse | adaptive 2 to 12 ms, start 3; +1 ms if a pulse moved nothing, −1 ms if it moved > 6 ticks; ≥ 5 ms in a wiggle leg with > 20 ticks to go | Creep Pulse Min / Max |
| Final brake, release, settle | 100 ms, then 100 ms | Final Brake, Settle Time |
| Brake between wiggle extremes | 20 ms | Wiggle Brake |
| Wiggle amplitude | 2° each side (0.1 to 5) | Wiggle Amplitude |
| Wiggle cycles | 4 (1 to 10), then back to centre | Wiggle Cycles |
| Fault: reverse motion | > 32 ticks against the commanded direction | Reverse Fault |
| Fault: stall | 500 ms with no encoder edge | Stall Timeout |
| Fault: move time | 120 s on the bench; derived from the distance here (S2.6) | Move Timeout |
| Target planning | fractional ticks accumulated from the plan, not from where the last move stopped; a wiggle restores the plan; stop or fault rebases to actual | (behaviour) |
| Braking | both inputs high = dynamic brake; release = coast | (behaviour) |

Results: index test 30/30 targets within 3 ticks, 15 forward slots land on exactly 8344; 0.735 s per 24° move (v5);
endurance 300 cycles of move + wiggle over 20 revolutions, max grid error 5 ticks (0.22°), about 2.5 s per cycle.

## Stage 2 decisions

| # | Decision | Status |
|---|---|---|
| D2 | The DRV8833 and the N20 (with its encoder) are added as a mechanism under the S1.9 rules, living on this branch (rule 8) | owner, 2026-10-20 |
| D7 | Feed = forward one slot, then the wiggle. Never reverse except inside a wiggle, bounded by its amplitude | owner, 2026-10-20 |
| D8 | All 15 slots open; capacity 14; reload does not move | owner, 2026-10-20 |
| D9 | Tuning values live in the calibration section of the web app's **Maintenance** page (and as Home Assistant config entities), protected by the settings password | owner, 2026-10-20 |
| D10 | Runtime (saved, on the Calibration cards and in HA): ticks per revolution, calibration turns, drive power, braking distance, wiggle amplitude, wiggle cycles. YAML-only: brake and pulse timings, fault limits, PWM frequency | owner, 2026-10-20 |
| D11 | The bench sketch is committed as `tests/reference/motor-test.cpp` (from `src/main.cpp`; the zip and the rest of the bench folder stay out of the repo) and used as a reference in a test (S2.2) | owner, 2026-10-20 |
| D12 | 2.1.0 is the decoupling on main. The branch is `2.1.0-n20.1` (`-n20.N` counts branch changes on top of a main release; after a rebase onto 2.2.0 it becomes `2.2.0-n20.1`), `Firmware Variant` `n20-direct`, and `Mechanism` reports `n20-direct`. Check the web app's version parser (`parse.js`) accepts the suffix | owner, 2026-10-20 |
| D13 | No alignment control and no per-slot offsets. Position comes from counting encoder ticks as the tuned sketch does. A feed is 24° **plus the wiggle**: without the wiggle the bench left a sub-1° error, and the wiggle's last leg returns to the planned centre, which put the opening over the hole every time. So on the disc the wiggle is part of positioning: always on, at least 1 cycle. Refined by D16: one global trim (all slots shift together) is allowed; per-slot offsets are still out | owner, 2026-10-20 |
| D16 | Calibration is a guided flow (S2.5): slot count, direction check, zero, a full-turn measurement of ticks per revolution, a check pass that moves exactly as a feed does, and one global trim. Nudges forward only, with an "overshot" button that backs off and comes forward again. Claude's design, the owner's go-ahead | owner, 2026-10-21 |
| D17 | Test a single binary with a choosable drive early (S2.0b), on this branch. The disc driver claims its GPIOs itself (ESP-IDF, WLED-style) from saved settings at boot, so the YAML claims none; changing the drive or its pins applies at the next restart, which marks the tray position unknown. If the test passes, Claude brings the owner the question of moving the disc driver to main and into releases (that would revisit D1 and AGENTS.md rule 8); if it fails, the single binary is dropped for now and revisited later | owner, 2026-10-21 |
| D18 | No rewind version in this plan: neither a 360° servo drive nor a rewind mode for the disc. To revisit | owner, 2026-10-21 |

## Stage 2 steps

Each step is a commit on `variant/n20-direct-drive`, `tools/check.sh` green.

### S2.0 Hardware test: can ESPHome run the control loop fast enough? (gates the rest)

The sketch runs its loop every 1 ms and uses 2 to 15 ms pulses. ESPHome's main loop runs about every 16 ms and
stalls on flash writes and Wi-Fi. Test on hardware with a throwaway build:
- **Encoder:** ESP32-S3 PCNT in quadrature mode, versus GPIO interrupts as in the sketch (about 5 k edges/s at
  35 rpm). Check which ESP-IDF version ESPHome 2026.9.1 builds with and which PCNT driver it offers.
- **Control timing:** a FreeRTOS task or `esp_timer` at 1 ms running only the motor state machine, versus ESPHome's
  high-frequency loop request. Check the ESPHome docs and source for what exists.
- **PWM:** two LEDC channels at 20 kHz alongside the rest of the build.
- Measure endpoint error over 15 forward slots with Wi-Fi, API and the web server active, and with a flash write
  just before the move (the core saves State before every feed).
- Exit: the approach chosen and recorded in DESIGN.md with the measured jitter.

### S2.0b Test: one binary, the drive chosen on the device (D17; gates the single-binary option only)

ESPHome makes every pin a build-time constant, sets each component's pin up once at boot, and refuses two components
on one GPIO (unless `allow_other_uses`). Since 2.0 `PinLoader` already rewrites the servo, button and LED pins from the
Device record before those components start. WLED changes pins at run time because it owns its drivers; the disc
driver is ours, so it can do the same. Throwaway build on this branch, before any disc logic:
- A drive setting (servo or disc) in a small new record, protected, applied at the next restart; switching marks the
  tray position unknown, so nothing feeds until a reload.
- A stub disc driver that claims its five GPIOs through ESP-IDF (LEDC, GPIO, PCNT) from saved pins at boot. The servo's
  output stays as today in servo mode; in disc mode it is never set up (ESPHome's "loop done" state before setup, as
  the button and LED use for "none"), so its GPIO is free.
- Measure: flash and RAM of the BLE build (2.1.0 is at 79.0%); both layouts boot from saved settings with no
  build-time claim; a pin clash between the drives' saved pins is refused by `check_pin()` and the clash check.
- On the owner's servo feeder: the servo behaves as before in servo mode (the golden trace and the differential test
  cover the logic; this covers the pins).
- Entities of both drives exist in the build (Home Assistant fixes them at build time); the web app shows only the
  active drive's (Mechanism); Home Assistant lists both, in the config category, named for their drive.
- Exit: a recorded yes or no, with the measurements. Yes: the question of main (D17). No: S2.1 onward as planned,
  YAML chooses the drive.

**S2.0b, build half (2026-10-21, branch `experiment/single-binary`; the bench half needs the feeder):**
- Built: `firmware/dual-test.yaml` carries the servo and the disc; **Drive** (`packages/drive_select.yaml`) chooses at
  the next restart. The choice is in a reserved byte of the Device record (no layout change), with the drive the last
  boot used beside it, so a change marks the tray position unknown. At boot the pin loader picks the drive before any
  driver starts and puts the other one's driver (the servo's LEDC, or the disc's component) in ESPHome's "never set
  up" state, so its pins stay free; only the chosen drive's pins are reserved. Host-tested (`test_single_binary`).
- Cost, the full BLE firmware, ESPHome 2026.9.1: servo only 1,455,927 bytes flash (79.3%), 151,787 RAM; disc only
  1,474,079 (80.3%), 153,735; both 1,478,471 (80.6%), 154,327. Both cost 4.4 KB more than the disc alone and 22.5 KB
  more than the servo alone; about 357 KB of the app partition stays free. No compiler warnings.
- Left for a real single binary: the servo's slot calibration package and the disc's both define Calibration Mode and
  the recovery entities, so `dual-test.yaml` leaves the servo's calibration out; they would be split into a shared
  package first. Home Assistant lists both drives' entities.
- Still to do on the bench: boot each way and switch servo -> disc -> servo; the servo feeder unchanged in servo mode;
  the disc's pins free in servo mode (GPIO1 is both the servo signal and AIN1 by default).

### S2.1 `DirectDriveMechanism` in core (pure, host-tested)

- The stage 1 verbs: `feed_one()` = forward one slot, then `wiggle()`; `reload()` and `restore()` move nothing.
  The wiggle is not optional here (D13): a feed is DONE only after the wiggle's final return to the planned centre,
  and the tuning table's Wiggle Cycles starts at 1.
- The timing-critical part is a pure `MotorController::step(now_us, ticks)` returning a bridge command (forward
  duty, reverse duty, brake, coast). It runs in the S2.0 control task; the core sees only `poll()`, through a small
  lock-free handoff.
- Phases as in the sketch: RUN → APPROACH_BRAKE ⇄ CREEP_PULSE → FINAL_BRAKE → SETTLE; adaptive creep pulse; the
  fractional plan accumulator (15 slots land on exactly one revolution); wiggle about the slot that restores the
  plan; the reverse, stall and time faults.
- New tests in `tests/test_core.cpp` (or a new `test_direct_drive.cpp`, added to `check.sh`) with a simple simulated
  motor (inertia, friction, a few ticks of coast): one slot, a full revolution, 14 feeds then empty, wiggle
  restores the plan, stall, reverse, timeout, stop mid-move, creep pulse adapting both ways.

### S2.2 The bench sketch as a reference test (D11)

- Commit `src/main.cpp` as `tests/reference/motor-test.cpp`, with the project's header (public domain, AI
  disclosure, credits) and a note on what it is and how to flash it alone (PlatformIO, `esp32-s3-devkitc-1`, the
  sketch's build flags).
- `tests/reference/arduino_shim.h`: just enough of `millis`, `digitalRead/Write`, `ledcWrite`, `Serial` and the
  interrupt calls to compile the sketch on a host against the same simulated motor.
- `tests/test_motor_reference.cpp`: run the same moves (one slot, 15 slots, a wiggle, a stall) through the sketch
  and through `MotorController`, and check they produce the same bridge commands at the same times and end on the
  same tick. Then a future tuning change in the firmware can be compared with the behaviour that was measured.
- Added to `check.sh`, with `-Werror` for the project's code (the sketch may need a relaxed flag set; say so in the
  script).

### S2.3 HAL hooks and adapter

- Hooks: `hal_bridge(cmd, duty)` (forward, reverse, brake, coast, standby), `hal_encoder_ticks()`,
  `hal_encoder_set(value)`; fakes in `fake_feeder.h`.
- Left over from stage 1: register the mechanism from YAML (`set_mechanism()` from the disc component's code
  generation, before the feeder's setup) and runtime button and LED pins in a build with no servo pin
  (`apply_pins()` returns early without one today).
- `precision_feeder.cpp`: the S2.0 approach; STBY low at the earliest setup priority; bridge to coast and STBY low on
  any fault, on `stop()`, and before a restart or firmware upload (both already wait until the tray is idle).

### S2.4 Position, boot and recovery

- State `position` = portions fed since reload (D1a). At boot, nothing moves; the encoder is set to the grid point
  of the current slot.
- An unfinished intent at boot means `UNKNOWN`, as now. Recovery: in Calibration mode, jog forward until a slot sits
  over the chute, then declare how many portions are left (or reload); declaring takes the current encoder count as
  the slot's planned position (the same as Zero in S2.5), so the next feed counts from there.
- Reload: no motion, remaining set back to slots − 1, the reload pause as today. USING.md says the slot over the chute must stay empty
  when loading.

### S2.5 Calibration section and entities (D9, D10)

- `packages/mechanism_n20.yaml`: config-category numbers for the runtime tuning values (protected, refused while
  moving), from the mechanism's tuning table; YAML substitutions for the rest, each with a comment saying what it
  does and when to change it. Saved in the Tuning record (stage 1).
- **The calibration flow (D16)**, in calibration mode (feeds refused), as a step-by-step card on the Maintenance page
  and the same entities in Home Assistant:
  1. **Slots:** how many the disc has (15); degrees per slot follow (24°). With a fixed-geometry build this is shown,
     not asked; with the single binary (S2.0b) it is a setting.
  2. **Direction:** the disc turns forward a few degrees; "did it turn the feeding way?" The firmware also checks
     the encoder counted upward. If either is wrong it offers Reverse Motor and Reverse Encoder (tuning values) rather
     than a rewire. Nothing else runs until this passes.
  3. **Zero:** "put one empty slot exactly over the hole", nudging forward; then Zero. That slot is the grid's origin.
  4. **Full turn:** the disc turns one revolution (or three: an Accuracy choice) on the current ticks-per-revolution
     value and stops near the same slot; the user nudges forward until it is centred again and presses Done. Ticks
     per revolution = the counted ticks ÷ turns. One revolution, not one slot: an error in one slot would repeat 15
     times a turn, while over a turn it is shared across 15. About 1.7 s per turn at 35 rpm.
  5. **Check pass:** the disc steps through every slot exactly as a feed does, wiggle and final approach included
     (the bench shows it is centred only after the wiggle, so calibration must stop where feeding stops), and asks
     "centred?" at each. All yes: saved.
  6. **Trim:** if the slots are off by the same amount, one global trim (tenths of a degree, a tuning value) shifts
     the grid. If they are off by different amounts, the flow says so: the print is uneven, which per-slot offsets
     would need (a new decision, D13).
  - **Nudges:** forward only (+0.1°, +1°), so gearbox backlash never enters. **Overshot** backs off about 5° and
    comes forward again, so every stop is approached from the same side.
  - Every move in the flow has the S2.6 limits (travel, timeout, watchdog); Stop works throughout.
- Diagnostics: `Motor Fault` (last reason and when), `Encoder Ticks`.
- Web app (D9, D15): an `n20-direct` entry in `MECHANISMS` (`web/src/mechanisms.js`), with:
  - `dial(state)`: a ring of 15 slots with the chute marked, used slots, slots with food, "5 of 14 left".
  - The calibration flow above as its first card (the card renderer gains a step-by-step card type, shared, so the
    next mechanism can use it), then **Motion** (Drive Power, Braking Distance), **Wiggle** (Wiggle Amplitude,
    Wiggle Cycles), **Motor status** (Motor Fault, Encoder Ticks, read-only).
  - Its entities added to `entities.js`; no shared page changes.
  The YAML-only values (D10) are not shown; the app only shows and requests, the firmware enforces limits.
- CALIBRATION.md: a direct-drive section.

### S2.6 Safety specific to a motor that can keep turning

A servo cannot pass its end stop; a DC motor can keep turning and empty the disc. On top of the sketch's faults:
- **Travel limit:** a move may not go more than half a slot past its target; past that, brake and `UNKNOWN`.
- **Move timeout from the distance:** expected time at 35 rpm plus a margin (one slot is about 0.12 s; default limit
  about 3 s per slot), not 120 s.
- **Control watchdog:** if the control task misses its deadline while driving, brake and fault.
- Reverse only inside a wiggle, bounded by its amplitude.
- A fault counts the portion as fed (the safe side, as today) and never retries by itself.
- A host test for each, and on the bench by holding the disc and cutting power mid-move.

### S2.7 YAML, examples, firmware

- `packages/mechanism_n20.yaml`, `examples/n20.yaml`, `firmware/n20.yaml` (variant `n20-direct`), with the pin
  defaults above (button GPIO3).
- Motor pins are YAML substitutions (D4), checked by the same validator at build time; no board presets. The
  Servo Pin and Board selects are left out of this build.
- `check.sh` validates the new YAMLs; the release workflow and `build_bins.sh` are not touched.

### S2.8 Bench on the real feeder

Recorded in docs/BENCH.md on the branch:
1. The calibration flow end to end (D16), from a fresh device: direction check (with the motor leads swapped once,
   to see Reverse Motor offered), zero, one-turn and three-turn measurements (compared with the sketch's 8344 and a
   10-turn mark run), check pass, trim.
2. 300 feed cycles over 20 revolutions inside ESPHome, with Wi-Fi and Home Assistant connected; grid error.
3. Feeds from every source (schedule, HA, web, BLE, button on GPIO3) through the guard: min gap, daily cap, empty
   after 14, reload.
4. Wiggle with real food: it releases and doesn't jam; amplitude and cycles tuned.
5. 15 feeds with the wiggle: the opening sits over the hole every time (D13); with 1 wiggle cycle as well as 4.
6. Power cut mid-move → `UNKNOWN` at boot; jog and declare recover it.
7. Stall (hold the disc) and an unplugged encoder → fault, no runaway.
8. Restart and firmware upload wait while moving.
9. No movement at power-up; with and without a pull-down on STBY. GPIO3 button held at power-up boots normally.
10. If S2.0b passed: the single binary switched servo → disc → servo on the bench, the servo feeder unchanged.

### S2.9 Publish the branch

- README on the branch: a banner saying this is a community variant (DRV8833 + N20, 15-slot disc), not built or
  released; how to build it from YAML; what to change for another motor or disc (ticks per revolution, slots,
  tuning).
- DESIGN.md on the branch: the mechanism section, D2 and D7 to D13, open items. CHANGELOG `2.1.0-n20.1`.
- Housekeeping once S2.2 has the sketch in `tests/reference/`: delete `motor-test.zip`; add `output/` (a work
  folder) to `.gitignore`.
- Push `variant/n20-direct-drive` **only with the owner's go-ahead**. No tag.
- Kept current by rebasing onto main after each main release; the Mechanism interface keeps that small.
- The branch is also the worked example `docs/MECHANISMS.md` points to for a closed-loop motor.

## Stage 2 done when

- Every S2.8 item passes and is recorded; check.sh green on the branch, including the reference test.
- The branch builds from a clean clone with only the example YAML and `secrets.yaml`.
- A device on the branch is never offered main's firmware.

## Risks

| Risk | Mitigation |
|---|---|
| ESPHome too slow or jittery for 2 to 15 ms pulses | S2.0 gates stage 2; control in its own task |
| Runaway motor empties the disc | S2.6 travel limit, timeout, watchdog; faults never retry |
| A single binary costs too much flash, or runtime pins misbehave | S2.0b measures it first; it is dropped for now if it fails |
| 8344 ticks/rev is wrong and the error adds up each revolution | Measured with the mark procedure; the plan accumulator stops per-move rounding from adding up |
| Brownout on motor start (5 V shared with the ESP32) | 100 µF fitted; watch the bench log for brownout resets |
| Stage 1 breaks servo builds | S1.0 golden trace; servo bench check before 2.1.0 |
| GPIO3 behaves as a strapping pin on some chip | D6 checked against the datasheet; bench item 8 |
| Branch drifts from main | One mechanism and one package on the branch; rebase each release |
