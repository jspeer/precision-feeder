# Precision Feeder ESPHome Firmware: Design

Status: **release version 2.4.6; 2.4.5 disc-rig smoke tests passed, broader audit verification remains open** (section 15).
The current release is 2.4.6; 2.4.4 was tested on the disc rig (docs/BENCH.md). This is the current design;
history is in [CHANGELOG.md](CHANGELOG.md), hardware results in [docs/BENCH.md](docs/BENCH.md), and the user guides in
[docs/](docs/) (INSTALL, BUILDING, USING, WEB_UI, SECURITY, SCHEDULING, CALIBRATION, PROTOCOL). First written
2026-10-13.

**Reading this document:** the user guides describe current operation. This is the engineering reference: sections
13 and 14 retain dated decisions, investigations and verification gaps, including superseded approaches. Those
records explain constraints and must not be read as instructions for configuring the current firmware.
[Development plans](docs/plans/README.md) are historical proposals, not additional supported features.

## 0. Disclosure and licence

- **AI disclosure (draft text for README, source headers, protocol doc):**
  > This project was designed and written with the assistance of an AI
  > (Anthropic's Claude). A human reviewed and directed the work, but the code
  > and documentation may contain mistakes. Test on your own hardware, and do
  > not rely on it as the only means of feeding animals you care about.
- **Licence:** MIT (owner, 2026-10-22; replaces The Unlicense, decided 2026-10-14). Copyright (c) 2026 Jeremy
  Speer; copies and substantial portions must keep the copyright and permission notice (LICENSE). Versions up to
  2.1.1 were released into the public domain and stay so for anyone who has them; the Unlicense allows the change.
  Credits (FlyingGyroscope as the inspiration, Claude as the author's tool) are in the README and the source headers.
  ESPHome's C++ runtime is GPLv3 (its Python is MIT, per its LICENSE), so the prebuilt images, which link it, are as a
  whole under the GPLv3's terms; this repository's own files are MIT, which the GPLv3 allows (README, Licence).
- **Provenance:** this is a clean-room ESPHome implementation. The original
  Arduino firmware ([FlyingGyroscope/PrecisionFeederRev2](https://github.com/FlyingGyroscope/PrecisionFeederRev2),
  AGPL-3.0) was read only to learn the mechanism's geometry and behaviour. No
  code is copied. The README credits it and the Printables model
  (<https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2>).
  The model is CC BY-NC-SA 4.0 (checked 2026-10-14); the repository ships
  none of its files.

## 1. Goals and non-goals

Goals
- **Recreate the original firmware, but better.** Everything the original
  does is supported (section 1.1), then extended.
- ESPHome firmware (ESP-IDF framework) for the Precision Feeder Rev2 hardware.
- On-board scheduler, so feeding continues with no HA, PLC or network.
- **Local first (1.1.0):** a complete built-in web app, served from the firmware, so the
  feeder is usable from any browser with no HA, internet or app.
- **An appliance (2.0):** prebuilt binaries installed from a web page, a setup wizard, settings changed at run time
  (pins, network, keys, time zone, NTP servers), and updates from the web app (section 9.5).
- Three control planes onto one core: on-board schedule, Home Assistant (native
  API), and BLE GATT (for a PLC/bridge or any other client).
- One firmware serves both deployments (HA-driven and BLE/PLC-driven).
- A generic public repo: no site-specific naming, documented protocol, example
  configs, and a heavily commented YAML so users can switch features on or off.

Non-goals (v1)
- Fry-style burst feeding (the magazine holds 6 portions; see section 3).
- Remote scheduling by an external controller as the only scheduler.
- Pairing with a passkey (no screen or keypad; section 10 explains the application-layer authentication used instead).
- **External add-on hardware of any kind** (RTC modules, feed or home
  sensors, humidity sensors). If it is not on the ESP32 board, the firmware
  does not support it. The one exception is the optional push button (section
  9), which the original firmware supports.

### 1.1 Parity with the original firmware

| Original feature | Here |
|---|---|
| Manual feed, tray reset | Button (optional), HA button, BLE command |
| Daily routine (hour granularity) | Profiles with minute-accurate events, plus solar anchors |
| Calendar (one-time) events | One-shot events |
| Weekday enable/disable | Per-event weekday mask and per-profile fast days |
| 24 h pause (limited to a year or so) | Hold: any duration in hours, or until cancelled; no maximum |
| Awake hours window (morning/night) | Covered by event times and solar anchors; no separate window |
| Late-feed timeout | Not carried over: late feeds are skipped by design (section 5.5) |
| Tray calibration (per-slot offsets) | Calibration mode, 7 signed offsets (the servo tray; the disc has its own, section 3.3) |
| Feed time log (last 6) | Last-feeds text sensor and BLE event log |
| Web UI and dashboard | Built-in web app (`web_server` package, firmware 1.1.0): served from the firmware, no internet needed; plus HA |
| WiFi setup portal | ESPHome WiFi, fallback AP and `captive_portal` |
| Hostname, mDNS, timezone, NTP server | ESPHome native, plus YAML options |
| Password and lockout | The settings password (section 10.1: protected changes signed by the browser, a wait after failures), the API and BLE keys, encrypted OTA; ESPHome's `web_server` auth optional on top |
| Periodic restarts (a workaround) | Not carried over: ESP-IDF memory management does not need them |
| Upcoming-events list | Not carried over; a single "Next Feed" value instead |
| WiFi connection wizard | Standard ESPHome `wifi: ap:` fallback and `captive_portal` |

Improvements over the original: minute-accurate scheduling, three profiles,
solar anchoring, exact portions-remaining and runway, a source-agnostic feed
guard, interrupted-move recovery, non-blocking motion, a BLE control plane and
a documented protocol.

## 2. Hardware

| Item | Value |
|---|---|
| MCU | Waveshare ESP32-S3-Zero (H4R2 variant), ESP32-S3FH4R2 (4 MB flash, 2 MB quad PSRAM). USB-C, WS2812 RGB LED on GPIO21, BOOT on GPIO0, no USB-UART chip (native USB). |
| Servo | MG90S. The original firmware was built with a TowerPro SG92R; the two are pin and size compatible, but their travel per microsecond may differ (section 3). |
| Button | **Optional.** Not fitted on every build. |
| Network | WiFi for OTA and HA; BLE for data |
| Shell | PET-GF print (builder's choice) |

Pin map (from the board's pin definition,
<https://docs.waveshare.com/ESP32-S3-Zero>). All pins are YAML substitutions,
so other builds can move them; since 2.0.0 they can also be chosen on the device
(section 2.1), and those win:

| Substitution | Default | Notes |
|---|---|---|
| `servo_pin` | `GPIO1` | servo signal (LEDC PWM). On the top-left header, next to the 5V/GND/3V3 pads so the 3-wire servo lead lands close. |
| `button_pin` | `GPIO2` | optional button to GND, internal pull-up. Only used if the button package is included. |
| `status_led_pin` | `GPIO21` | onboard WS2812 (fixed by the board) |

Avoid: GPIO0 (BOOT, strapping, not broken out), GPIO45, 46 (strapping),
GPIO43/44 (UART0), GPIO33 to 37 (not broken out). GPIO1 floats briefly at
power-up, so the servo may twitch once at boot. A 10 kOhm pull-down on the
signal line prevents that. The board is powered from USB-C or 3.7 to 6 V on
the 5V pad (800 mA LDO for the 3.3 V rail). Power the servo from a separate or
adequately rated 5 V path, not the board's 3.3 V LDO.

### 2.1 Runtime pins (firmware 2.0.0)

A prebuilt binary cannot know the board, so the servo, button and LED pins are saved
in the Device record (section 9.2) and chosen on the device: the `hardware` package
(Board, Servo Pin, Pins In Use, Restart Needed, Restart), Button Pin in `button.yaml`
and LED Pin in `status_led.yaml`. The YAML substitutions are the starting values; a
pin saved on the device wins, and "Build Default" goes back to the YAML's.

- **Applied before the drivers start.** LEDC, the LED strip and the GPIO binary
  sensor read their pin in `setup()` at HARDWARE priority. `PinLoader`, a small
  component at BUS priority, loads the Device record first and rewrites their pins
  (`set_pin()`); the preferences are open before any `setup()`. So a change applies
  at the **next restart**, and nothing restarts by itself: Restart Needed turns on,
  and the Restart button (refused while the tray moves) restarts.
- **"None"** for the button or the LED: their driver components are put in ESPHome's
  "loop done" state before setup, which ESPHome supports for components that disable
  themselves early, so they are never set up and the pin stays free. Nothing else
  calls them: the button's interrupt is only attached in its setup, and the LED is
  internal and driven only by the status_led interval, which checks
  `status_led_fitted()` first.
- **The validator** (`check_pin`, `feeder_core.h`, host-tested), for the ESP32-S3:
  - refused: not on the chip (22 to 25), the SPI flash and PSRAM (26 to 32), USB
    (19, 20), strapping pins that decide how it boots (0, 45, 46);
  - accepted with a warning: 3 (a strapping pin only for the JTAG source, read at reset only when the
    `STRAP_JTAG_SEL` eFuse is burned, which it is not by default: Espressif's ESP-IDF JTAG guide for the ESP32-S3;
    firmware 2.1.0, D6), 33 to 37 (octal flash or PSRAM on some modules, for
    example N8R8) and 43, 44 (UART0: boot messages at every reset);
  - two parts on one pin are refused; the servo always needs a pin;
  - pins a mechanism other than the servo drives (the disc's five) are chosen on the device too (D19, section 3.3)
    and reserved while it is the drive: the button and LED are refused on them, and a build's own button or LED pin
    that falls on one (the servo's button default, GPIO2, is the disc's IN2) is left off with a warning until another
    pin is chosen.
  The pin selects offer exactly the pins not refused (`tests/test_consistency.py`).
- **A bad saved record** (a refused pin, or a clash) falls back to the build's pins at
  boot, with a warning in the log, so a damaged record cannot stop the feeder.
- **Board presets:** Waveshare ESP32-S3-Zero (servo GPIO1, button GPIO2, LED GPIO21:
  the bench build), Espressif ESP32-S3-DevKitC-1 (GPIO4, GPIO5, LED GPIO38 as on v1.1
  boards; v1.0 has it on GPIO48, so Custom). The board is not saved: it is worked out
  from the pins (all following the build: Build Default; matching a preset: that
  preset; else Custom). Choosing Custom changes nothing.
- A part the build does not include (no button or LED package) has no pin, whatever
  is saved, and its pin cannot be changed.
- Pins are protected settings (section 10.1). BLE has no access to them.

## 3. Mechanism and magazine model

### 3.0 Mechanisms (firmware 2.1.0)

What moves the tray is a **mechanism** (`components/precision_feeder/mechanism.h`). The core asks it for a few verbs
and keeps everything else itself:

| Verb | What the mechanism does | The servo fan tray |
|---|---|---|
| `capacity()` | portions per load | 6 |
| `feed_one(from)` | drop one portion: position `from` to `from + 1`, wiggle included | step to the next slot, back, forward |
| `reload()` | get the tray ready to load (afterwards position 0) | back to the closed slot 0 |
| `restore(p)` | put or keep the tray at `p` (boot, declare, end of calibration) | command the servo to slot `p` |
| `poll()` | the move started last: BUSY, DONE, or FAULT (stopped; position not known) | DONE after the step times |
| `stop()`, `release()`, `settle_ms()` | abort; let go of the motor after it has been still | release the PWM after `detach_delay` |
| `calibration_points()`, `calibration_show(i)` | points a user fine-tunes in calibration mode | the 7 slots |
| tuning table | `TuningParam` values: name, unit, limits, default | none |

- **The core keeps:** the feed guard, persist-intent-before-moving, the position (portions fed since the last reload,
  0 to `capacity()`), `UNKNOWN` after an interrupted move, the history and the schedule. A mechanism never starts a
  move by itself and never sees the guard. A FAULT takes the existing mid-step abort path: the portion counts as fed,
  later portions of the feed are uncounted, the position is `UNKNOWN` (Faults says "motor fault: ...").
- **Portions per load** come from `capacity()`: remaining, EMPTY, the guard limits (daily cap, portions per command,
  event and one-shot portions: 1 to capacity) and the BLE ranges. The YAML repeats it as `${tray_capacity}`, set by
  the mechanism's package. The capacity limits what can be *set*; saved values are never clamped to it at boot (only
  to CAPACITY_MAX), so a smaller load (another slot count, or another drive in a single binary) never rewrites what
  the user set. They are held to it when used: a scheduled event asking for more than a load feeds what the load
  holds; a manual request for more is refused (found on the bench, 2026-10-21: a daily cap of 14 became 6 for good
  after a round trip through the servo drive).
- **Two drives in one firmware, chosen on the device** (D17, superseding the original one-mechanism build):
  `core.yaml` includes the servo package; adding `packages/mechanism_n20.yaml` beside it offers the disc through
  Drive (section 3.4). Only the selected driver starts; a changed choice applies after a restart.
- **Tuning values** of a mechanism are saved in their own record (`Tuning`, section 9.2), checked against the
  mechanism's table by one setter (`set_tuning`). A record for another mechanism or table layout is not used (and is
  left as it is until a value changes). The servo has no tuning table: its settings are the YAML's, and its pulse
  range and offsets stay in the Calibration record, so a servo build never writes the Tuning record.
- **Entities:** Mechanism (`servo` or `n20-direct`) and Capacity (6, or the disc's), both diagnostic. The web app picks the mechanism's tray drawing,
  wording and calibration cards by Mechanism (`web/src/mechanisms.js`), the servo's when a firmware reports none.
  Maintenance shows servo slot/pulse controls only for the running servo mechanism, even when a combined build
  exposes those entities beside the disc's (2.4.6). Setup uses the same running-mechanism choice.
- Adding one: AGENTS.md, "Adding a mechanism", and [docs/MECHANISMS.md](docs/MECHANISMS.md). The first other
  mechanism is the direct-drive disc (section 3.3; [docs/plans/drive-decoupling.md](docs/plans/drive-decoupling.md),
  stage 2), carried beside the servo in one firmware (section 3.4).
- Adapter registration uses `offer_drive()` for the mechanism and driver, and `offer_motor_pins()` separately for
  motor pins; `offer_drive()` takes no pin arguments.

### 3.0.1 The servo fan tray

Facts taken from the original firmware and the builder's reading of the
Printables instructions (verify on the physical print):

- 7 tray positions, numbered 0 to 6. Servo sweep 175 degrees, so 25 degrees
  per position (`pitch = sweep / 7`). Angle for position n is
  `margin + n * pitch` plus a per-position calibration offset, where
  `margin = (180 - sweep) / 2` = 2.5 degrees. Positions therefore run from
  2.5 to 152.5 degrees. (The original firmware's margin works out to 2
  degrees only because it does the division in integer maths; the intent is
  2.5.)
- **Measured from the printed model** (`REV2 (Feeding hole, Small) v6` 3MF,
  2026-10-13): the tray's 7 slots are exactly 25.0 degrees apart around the
  servo axis and together span 175 degrees (2.5 to 177.5 degrees in the tray's
  own frame). So "sweep 175" is the tray's slot span, not the servo's travel:
  going from position 0 to position 6 turns the servo only 150 degrees. The
  closed slot (solid floor) is the one at the end of the fan. The feeding
  hole in the base is a sector about 24.3 degrees wide, and the slot openings
  are about 22.5 degrees between walls, so there is little slack: a few
  degrees of error starts to cover the hole. The short and tall trays have the
  same pitch.
- The original drives the servo with ESP32Servo's default pulse range, 544 to
  2400 us for 0 to 180 degrees, and this firmware uses the same default (from
  v0.2.2). The original was built and tuned with a TowerPro **SG92R**; this
  build uses an **MG90S**, which is pin and size compatible but not
  necessarily the same degrees per microsecond. That is unknown until the
  bench test, and any mismatch adds up over the six steps.
- Position 0 is the **closed slot** (idle and reload position). Positions 1 to
  6 are open slots. **Each feed advances one position, so a full magazine
  holds 6 portions.** At position 6 the magazine is empty.
- Setup: drive fully clockwise to the end stop, then align slot 0 to the
  alignment mark on the base.
- A feed is: step to next position, step back, step to next position again,
  about 1 s per move (a wiggle to dislodge food).
- Open-loop: there is no position feedback.

Consequences
- **Remaining portions = 6 - position.** This is exact, not an estimate.
- Schedules have a runway: portions per day against portions remaining.
  The firmware computes it and warns when a schedule cannot last until the
  expected refill.
- Fry or multi-meal-per-day schedules are only sustainable for a day or two.

### 3.1 Tray state machine

States: `LOADED(n)` (position n, 0 to 5), `EMPTY` (position 6), `UNKNOWN`
(position not trusted), `CALIBRATING`, `MOVING`.

- "Tray reload" (button, HA, BLE): move to position 0 and set `LOADED(0)`.
  It is pressed **before** loading: position 0 is the closed slot, so food
  cannot fall out while the user fills the tray (at position 6 an open slot
  sits over the outlet). As in the original, scheduled feeding then pauses for
  a configurable reload window (default 10 minutes) so the user can finish
  loading. Events due during the window are skipped, not made up (section 5.5).
- **Persist intent before moving.** Write `moving_to = n+1` to flash, move,
  then commit `position = n+1` and clear the intent. If boot finds an
  unfinished intent, the state is `UNKNOWN`: feeding is blocked until the user
  declares the position or reloads. (The reference saved after the move, so a
  power loss mid-move desynchronised it.)
- **Positioning moves too (2.1.2).** A reload, a declared position and the move back at the end of calibration save
  their target as the intent (`moving_to = target`) before the motor is told, and clear it only once the tray has
  settled. Until then the tray counts as `MOVING`: the guard answers `BUSY`, and a restart (Restart, a firmware
  upload) waits. A power cut or Stop on the way leaves it `UNKNOWN`. (Before 2.1.2 these saved the new position at
  once and counted as stationary while the servo was still turning: Codex review.) The move to the saved position at
  boot has no intent: the tray is normally already there.
- **A save that must come first and fails stops the move (2.1.2).** The flash saves report whether they reached
  flash (`hal_save_state`, `hal_save_settings`: written and synced). If a feed's intent cannot be saved, nothing moves
  and the feed answers `STORAGE_FAILED` (17), uncounted; a positioning move that cannot save its intent leaves the
  position `UNKNOWN` without moving. Faults shows "saving to flash failed" until a save works again.
- **Calibration that can move the tray (2.1.2).** Before any calibration action of a mechanism that moves the tray
  (the disc's), and before a change to a value that moves the slots, the position is saved as `UNKNOWN`, so a power
  cut in calibration never leaves a trusted position behind (before, it was marked only when calibration was turned
  off). The servo's calibration only points the servo at absolute angles and keeps the position. Stop stops a
  calibration move too, and a restart waits for one (`FeederCore::busy()`, `Mechanism::moving()`). Unknown in memory
  is not enough (2.3.1): unless the last save that reached flash said unknown, it is saved again, and the action is
  refused (`STORAGE_FAILED`) if that fails.
- On boot, command the servo to the saved position, never to 0.
- Detach the PWM output after a short hold time to avoid jitter and heat.
  Needs a bench check that the tray does not drift when detached.

### 3.2 Motion and calibration

- Calibration offsets: 7 signed values (tenths of a degree), clamped.
  The reference used an unsigned type, which can underflow.
- Settings: sweep, margin, step time, wiggle on/off and amplitude, servo pulse
  range (MG90S end stops may differ from a nominal 0 to 180 degrees; do not
  assume the 2.5 degree margin at position 0 is safe until tested), direction
  invert. The default pulse range is the original's, 544 to 2400 us for 0 to
  180 degrees (tuned on an SG92R; check it on an MG90S).
- Calibration mode: jog the servo, save per-position offsets. Scheduled and
  remote feeds are rejected while calibrating.

### 3.3 The direct-drive disc (firmware 2.1.0)

The first mechanism after the servo, built for the owner's own print on its branch and moved to main in 2.1.0 (owner,
2026-10-21; AGENTS.md, "Adding a mechanism", rule 8). Plan and decisions:
[docs/plans/drive-decoupling.md](docs/plans/drive-decoupling.md), stage 2 (D2, D7 to D19). Package
`packages/mechanism_n20.yaml`, example `examples/disc.yaml`.

**Pins chosen on the device (D19, owner 2026-10-21; replaces D4).** The disc's five pins (IN1, IN2, STBY, encoder A
and B) are chosen on the feeder like the servo's: the YAML's are the starting values, the MotorPins record
(`precision_feeder.motor_pins.v1`, read at boot before any driver starts) wins, a change applies at the next restart
(Restart Needed). The same validator refuses a pin the chip cannot spare, two motor pins on one GPIO, and the button's
or the LED's; a damaged record falls back to the build's pins. Restart, Restart Needed and Pins In Use moved to
`packages/restart.yaml`, which hardware.yaml includes.

**Hardware.** A GA12-N20 gear motor (6 V, 35 rpm) with a two-channel Hall encoder, behind channel A of a DRV8833,
turns a 3D-printed disc of open slots directly: 15 on the bench print, 24 degrees apart. Wiring: AIN1 GPIO1, AIN2
GPIO2, STBY GPIO6, encoder C1 GPIO4 and C2 GPIO5, encoder VCC 3V3, VM 5V, grounds common, 100 µF across 5V and GND.
The button goes on GPIO3 (GPIO2 is AIN2; examples/disc.yaml sets it). The pins are chosen on the device (D19, above) and reserved, so the button
and LED cannot be moved onto them.

**Model.** All slots are open and one is always over the chute, so a load holds slots − 1 = 14 portions (D8). The
disc only turns forward (D7); Tray Reload moves nothing and sets the count back. A feed is one slot forward and the
wiggle: the wiggle is part of positioning, because the bench showed the slot is centred over the hole only after the
wiggle's last leg (D13). Slot k is at origin + round(k × ticks per turn / slots) + trim, counted from the origin each
time, so stopping and rounding errors never add up (15 slots land on exactly one turn).

**Motion** (`mechanism_n20.*`, `N20Motion`): the owner's bench sketch (tests/reference/motor-test.cpp, v7) as a state
machine with no I/O. Full power to the braking distance (160 ticks), brake 100 ms, creep with adaptive pulses (2 to
12 ms; longer after a pulse that moved nothing, shorter after one that moved more than 6 ticks) and 15 ms brakes,
stop on crossing the target, brake 100 ms, release, settle 100 ms. The wiggle: ± its amplitude (2°) four times with
faster brakes, then back to the centre. `tests/test_n20.cpp` runs the sketch itself and the firmware on the same
simulated motor and checks they drive the bridge identically, change for change and millisecond for millisecond.

**Faults** (each stops the motor; the core then marks the position unknown, the portion counting as fed): the encoder
counting more than 32 ticks the wrong way; no encoder edge for 500 ms (50 ms while driving at full power, so an encoder
that stops counting cannot let the motor run on: at most about 240 ticks, under half a slot); the bridge driving, in any
closed-loop phase (creep and the wiggle included, either way), for longer without an edge than three quarters of a
slot takes at full speed, 25 to 50 ms (2.1.2: before, creep had the 500 ms window while its pulses grew, and a lost
encoder could carry a 60-slot disc about 4.8 slots in the simulation: Codex review); a move taking longer
than 3 s plus three times its expected time at 35 rpm; ending more than half a slot past its target; the control step
running late (more than 25 ms between steps while driving). The adapter's main loop also cuts STBY if the control
timer stops while driving.

**On the device** (`components/precision_feeder_n20`): the component claims its GPIOs through ESP-IDF itself (LEDC
timer 3, channels 6 and 7, 8-bit at 20 kHz; PCNT in quadrature x4 with pull-ups and a 1 µs glitch filter; STBY a
GPIO, driven low first thing at setup and on every shutdown), so the YAML claims none. An esp_timer runs the control
every millisecond under a spinlock shared with the main loop. Motor Loop Max Gap reports the longest gap between two
control steps (the S2.0 timing check, open item 23).

**Tuning** (the Tuning record, runtime, D10): ticks per turn (8344, the sketch's estimate, until measured), drive
power, braking distance, wiggle amplitude and cycles (at least one), slot trim (± half a slot), calibration turns,
reverse motor and reverse encoder. The motion's timings and limits are YAML-only (`precision_feeder_n20:`).

**Calibration** (D16; docs/CALIBRATION.md): a guided flow in calibration mode. A direction test (a short open-loop push;
the user says which way it turned and the firmware checks the encoder counted up, then offers Reverse Motor and
Reverse Encoder rather than a rewire), Zero on a slot over the hole, a full-turn measurement of ticks per turn (an
error spread over the slots of a turn, not repeated on each; refused if more than 20% from the value it turned by),
a check pass that moves to every slot exactly as a feed does, and one global trim, set by **Trim here** from a slot
the user nudged to the centre (no typed value, no sign to get wrong). Nudges go forward only; Overshot backs off 5° and
comes forward 4°, so every stop is approached from the same side (gearbox backlash). A disc moved in calibration, or
given another slot count, leaves the position unknown when calibration ends, so it is reloaded before anything is fed.
Every step's text names the step (1 to 5) and the button to press, under 255 characters (Home Assistant's limit).

**Slot count** (Disc Slots, 3 to 60, owner 2026-10-21): a tuning value, the YAML's `disc_slots` only its starting
value, so a 14- or 16-slot print needs no rebuild; capacity follows it at run time, and the portion input boxes go up
to 59. Values that move where the slots are or which way is forward (Disc Slots, Ticks Per Turn, Slot Trim, Reverse
Motor, Reverse Encoder) can only be changed in calibration mode (`Mechanism::tuning_needs_calibration`). Adding Disc
Slots made the tuning table layout 2; it only appends, so values saved under layout 1 are kept
(`Mechanism::tuning_layout_extends`).

**Recovery** after a power cut mid-move: in calibration mode, nudge forward until a slot is over the hole, then
Declare Tray Position (portions fed since the last reload); the grid restarts from where the disc is.

### 3.4 Two drives in one firmware (firmware 2.1.0, D17)

`core.yaml` always carries the servo. Including `packages/mechanism_n20.yaml` adds the disc as a second drive, and
**Drive** (`packages/drive_select.yaml`, included by the disc's package; Build Default, Servo, Direct-drive disc)
chooses which one the feeder uses. The prebuilt binaries carry both.

- **Chosen at boot, before any driver starts.** `PinLoader` reads the Device record (the choice is in
  `reserved[0]`, so the record's layout is unchanged) and the feeder uses that drive; the other's driver is put in
  ESPHome's "loop done" state before its setup, so it never claims its pins (the servo's LEDC, or the disc's LEDC,
  PCNT, timer and STBY). A change applies at the next restart (Restart Needed).
- **Build Default:** the servo, unless the disc's package says otherwise (`disc_build_default`, the component's
  `build_default`). The prebuilt binaries default to the servo, so every feeder flashed before 2.1.0 stays a servo
  feeder; `examples/disc.yaml` defaults to the disc.
- **A change of drive marks the tray position unknown.** The drive the last boot used is in `reserved[1]`; a
  different one, or none recorded (a firmware from before 2.1.0), means the saved position belongs to another tray,
  so it is reloaded before anything is fed.
- **Saved limits are kept as set.** The guard limits are saved up to the largest capacity any drive may have
  (`CAPACITY_MAX`) and held to the drive's capacity only when used, so a round trip through the servo never cuts a
  disc's daily cap to 6. Portions above the capacity: scheduled ones are clamped, manual ones refused.
- **Entities.** Both drives' entities exist internally; since 2.2.0 Home Assistant sees neither drive's
  configuration entities. The web app shows the drive in use (Mechanism). The servo's pin is offered only while the servo is the
  drive; the disc's pins only while the disc is. Calibration Mode and the recovery entities, which both use, are in
  `packages/tray_recovery.yaml`, included by `core.yaml`.
- Cost: about 4.4 KB of flash for the second drive.

## 4. Feed guard (source-agnostic)

Every feed request, whatever its source, goes through one gate:

| Rule | Default | Notes |
|---|---|---|
| Minimum gap between feeds | 60 s | the reference also paused 1 min after each feed |
| Rolling 24 h cap | 4 portions | configurable 1 to the mechanism's capacity (6 for the servo). It counts the feed history, which keeps 64 feeds (`HISTORY_SIZE`, the largest capacity; 8 before 2.1.2, which let a disc's 9th feed in 24 h go uncounted: Codex review). The count it uses can be edited by hand for debugging and recovery: in the web app only (Maintenance), behind the settings password (decision 2026-10-14, moved from Home Assistant in 2.2.0, D25) |
| Max portions per command | 1 | manual/remote requests only; configurable 1 to the capacity ("drop all" is opt-in) |
| Tray state | must be `LOADED` | empty or unknown rejects |
| Calibration lockout | on | |
| Hold or reload pause active | scheduled feeds only | manual feeds still allowed |
| Time valid | scheduled feeds only | manual feeds still allowed |
| Duplicate sequence number | replay stored result | BLE idempotency, section 8.6 |

Each rejection returns a result code (section 8.4) and is logged.

Requests within the load's capacity are clamped to the portions remaining; an empty load still refuses. A manual
request above the mechanism's capacity is refused, while a scheduled event saved for a larger drive is clamped to
the current capacity. Scheduled events do not use the manual per-command limit, but still use the daily cap.

With no valid clock, feeds made during this boot count toward the daily cap using uptime. When the clock becomes
valid they are stamped with the current epoch. After a reboot, feeds saved without an epoch are dropped because
their age is unknowable; feeds with an epoch count again once the clock is valid. Until then, pre-reboot history
does not count. An interrupted feed with a known timestamp remains counted. The minimum gap uses uptime within a
boot and the saved feed timestamp when available after a restart.

An event with N portions makes N separate steps, with a configurable gap
between steps (default a few seconds), each with its own wiggle.

## 5. Scheduling

The scheduler runs on the device. The web app, BLE and MQTT's own topics can **edit** it; Home Assistant can
select the active profile and turn Scheduler on or off. All can **trigger** feeds, but never need to be present.

### 5.1 Profiles

Three profiles (A, B, C), one active at a time. Switching profile is a single
select or one BLE command.

Per profile:
- profile labels A, B and C (custom names are not implemented; open item 8)
- `cadence`: `daily`, `every_2nd`, or `every_n` (n = 1 to 14), with an anchor
  date defining cycle day 0
- `fast_days`: 7-bit weekday mask of no-feed days
- 4 events

Per event:
- `enabled`
- `anchor`: `clock`, `sunrise`, `sunset`
- `offset`: for `clock`, minutes after midnight (0 to 1439); for solar
  anchors, signed minutes from -720 to +720
- `portions`: 1 to the capacity (6 for the servo), subject to the guard cap
- `days`: 7-bit weekday mask

Global:
- `hold`: pause scheduled feeding for any number of hours, or until cancelled
  (an indefinite hold needs no clock); cancel hold
- `one-shot` events: feed at a given date/time (calendar-style). They run even
  when the Scheduler switch is off (it only stops recurring profile events), but
  a hold or reload pause still applies (decision 2026-10-14).
- `refill_interval_days` (for runway warnings)

Example uses (not built in; [docs/SCHEDULING.md](docs/SCHEDULING.md) walks through building profiles): A "Normal"
(2 events, Sunday fast); B "Vacation" (1 event, every 2nd day); C "Growout" (3 events a day, short runway warning).

### 5.2 Solar anchoring

- Inputs: latitude, longitude, timezone (POSIX TZ string, so DST works).
- On-device sunrise/sunset calculation (NOAA algorithm, standard refraction
  correction). Resolve each event's time once per local day, at midnight
  rollover and whenever profile, location or time changes.
- Weekday mask applies to the **anchor day**. An event resolving after
  midnight (for example sunset + 3 h) fires early the next morning.
- **No sunrise/sunset on that day (polar night or midnight sun):** the event
  uses a fixed fallback, 06:00 for sunrise and 18:00 for sunset, plus its
  offset, so the animals are still fed at a sensible hour; Faults says so and
  BLE Status flag bit 6 is set (owner decision 2026-10-14, replacing "skip").
  With **no location set**, sun events are still skipped with a fault: that is
  a setup mistake, not geography.
- Events resolving within the minimum gap of each other: the guard handles it
  (later one is rejected `TOO_SOON` and logged).

### 5.3 Dedupe across reboot

Persist `today` and a fired-events bitmask. After reboot, an event already
fired today is not repeated. The same rule means each event feeds at most once
per day: an event that already fed today and is then moved later in the day
next feeds tomorrow. "Next Feed" and the runway follow the same rule (fixed in
0.3.1; before that they showed the moved event as due today).

Since 2.1.2 (Codex review):
- The day an event fed is saved **before** it feeds (the feed's own intent save carries it), and it only moves
  forward. An occurrence on or before that day counts as fed, so a clock set back across a date (an NTP correction, a
  clock pushed over BLE) never repeats an event. A mark more than 7 days ahead of the occurrence
  (`FIRED_AHEAD_MAX_DAYS`) is taken to come from a wrong clock and is ignored, so a bad date cannot silence the
  schedule for weeks.
- A one-shot feed is saved as done, and flushed, before it feeds: a power cut after the feed can no longer repeat it
  in the same minute. If that save fails, it does not feed (`STORAGE_FAILED`, reported once). A missed one-shot
  beats a repeated one.

### 5.4 Runway

Project the active profile forward (cadence, fast days, weekday masks, hold)
against remaining portions. Expose `runway_days`, `empty_on` (date), and a
`schedule_oversubscribed` flag when the next `refill_interval_days` need more
portions than remain.

### 5.5 Missed events are skipped

Late feeds are skipped by design. An event fires only during the minute it is
due (the one-minute window exists only so a `BUSY` or `TOO_SOON` answer can be
retried on the next tick). If the device was offline, on hold, in the reload
pause, or the guard refused the event for that whole minute, it is lost and
logged. Nothing is dispensed afterwards to make up for it. This also removes the
need to cancel a backlog on tray reload.

Refusals are reported once. While the window is open, a `BUSY`, `TOO_SOON` or
hold/reload-pause answer is retried silently; only the final outcome is
reported (log, "Last Result", BLE event log): the feed when it happens, or the
last refusal once when the window closes. An event that was missed because the
device was off is skipped without a report.

## 6. Time sources

The clock must be right for the scheduler and solar maths, and the device can
end up with no network in oddball cases (fallback AP mode; power returns while
the internet is down). BLE exists as a control plane, so it is also a time
path.

- **NTP is the truth.** Up to three servers: the YAML's (`ntp_server_1` to `3`, the public pool by default), or
  ones set on the feeder (firmware 2.0.2): the **NTP Servers** text entity (web app, Configuration and the setup
  wizard; internal, hidden from Home Assistant), host names or IPv4/IPv6 addresses separated by commas, saved in the Device record and
  applied at once. ESPHome sets its servers once and has no setter, so the adapter stops ESP-IDF's SNTP client, gives
  it the new names and starts it again (empty goes back to the build's, read from ESP-IDF at boot). Not protected by
  the settings password, like the timezone (owner, 2026-10-16). **Found in 2.0.2:** ESP-IDF's default
  `CONFIG_LWIP_SNTP_MAX_SERVERS` is 1, so before 2.0.2 only the first server was ever asked; core.yaml now sets 3. Checked on
  the bench (docs/BENCH.md, 2.0.2): a local server, an unreachable one, back to the build's, kept across a restart.
- **BLE `SET_TIME` is the fallback.** It carries `epoch u32` (UTC) and
  `utc_offset_min i16`. Policy `ble_time_policy`:
  - `fallback_only` (recommended): accepted only if neither NTP nor Home Assistant has synced within
    `ble_network_time_stale_after` (default `24h`). Otherwise the result is
    `IGNORED_NTP_ACTIVE`. This stops a wrong PLC clock from corrupting a
    correct device clock, and lets a PLC push time on every connect.
  - `always`: accepted any time.
  - `off`: rejected.
- **Home Assistant time** is a second source, enabled in all three examples (decision
  2026-10-14), useful when the internet is down but HA is up. By default HA also
  pushes its timezone; a build can pin its own (packages/time_homeassistant.yaml). With NTP and HA both enabled, last sync wins;
  the policy gate applies only to BLE.
- **Timezone:** three sources, strongest first (firmware 2.0.0):
  1. The **Timezone** entity: a POSIX TZ string set at run time, saved in the
     Device record (section 9.2) and applied at once, no reboot. It wins over
     the other two, and is put back after Home Assistant pushes its own.
  2. Home Assistant's timezone, pushed with each time sync when the HA time
     package is enabled and does not pin one.
  3. The build's `timezone` substitution (IANA name or POSIX string).
  **Finding (ESPHome 2026.9.1):** ESPHome parses its timezone at build time,
  and Home Assistant sends a parsed one; nothing parses a string on the device.
  So `tz_rules.h` is our own parser, written fresh, producing exactly the fields
  of ESPHome's reference parser (checked against every IANA zone,
  `tests/tz_golden.h`). The adapter applies it with ESPHome's
  `time::set_global_tz()`. It is stricter than the reference: no trailing
  characters, no daylight-saving name without rules, offsets up to 24 h. IANA
  names are converted to POSIX strings off the device (the setup wizard, N6).
  A change never repeats a feed: fired events are stored by day number.
  BLE `SET_TIMEZONE` stays reserved (protocol unchanged); adding it needs a decision.
- **Time quality is exposed** (web app and GATT): source (none / BLE / NTP / HA) and
  sync age. A BLE-set clock that has aged is flagged "time stale" and keeps
  running. The clock free-runs at a few seconds of drift per day, so a week
  without sync is harmless for feeding.
- **Clock kept across a soft reset.** After an OTA update, a crash or a
  reflash (not a power cut) the chip's RTC still has the right time, so the
  clock is valid at boot and scheduling carries on. The source is shown as
  "kept across restart" until NTP, Home Assistant or BLE sets it; BLE
  `SET_TIME` is accepted meanwhile. (Found on the bench: ESPHome's SNTP
  component fires its sync trigger once at boot when the clock is already valid,
  which 0.3.0 wrongly counted as an NTP sync. From 0.3.1 only a real SNTP reply
  counts, checked with ESP-IDF's sync status.)
- **Time is never restored from flash** after power loss: it would be wrong by
  the length of the outage. Scheduled feeds wait for a valid source; manual
  feeds still work.
- **No external RTC.** After a power loss with no network and no BLE client,
  the feeder waits for a valid time before scheduled feeding resumes. This is
  documented plainly in [docs/USING.md](docs/USING.md) (Limits).
- No periodic reboot (the reference restarts weekly; we do not).

## 7. Home Assistant entities

**Since 2.2.0 Home Assistant and MQTT see only the slim set** (docs/plans/2.2.0-scopes.md, D22): Feed One Portion, Feed
Selected Portions with Manual Portions, Tray Reload, Stop, Hold Scheduled Feeding with Hold Duration, Cancel Hold,
Scheduler, Active Profile, Portions Remaining, Tray State, Next Feed, Last Feed, Last Result, Recent Feeds, Today's
Feeds, Faults, Needs Food and Firmware Version. Every other entity below is `internal: true`: the web app reads and
changes it (`web_server: include_internal`), Home Assistant and MQTT do not see it. `tools/check_scopes.py` checks every
build's resolved configuration for exactly that set (and that no internal entity has its own MQTT topic, which would
publish it); check.sh and build_bins.sh run it. The rest of this section describes the entities as the web app sees
them.

Editor pattern, to keep the entity count low.

Status (read-only): tray position, remaining portions, tray state, needs food
(binary, for automations), active profile name, next feed time, last feed time and source, last feeds log (text),
feeds today, runway days, empty-on date, schedule summary (text), time source
and sync age, fault flags, firmware version, uptime, WiFi RSSI.

Controls: `Feed One Portion` and `Feed Selected Portions` with `Manual Portions`, `Tray Reload`, Active Profile
(select), `Hold Duration` (hours; 0 means until cancelled) with `Hold Scheduled Feeding` and `Cancel Hold`,
Scheduler (switch), and Stop. These are in the slim set.

Editor: profile to edit, event to edit, and fields (enabled, anchor, time,
portions, days), plus profile fields (every N days, fast days). Edits apply
immediately and are saved a couple of seconds later, so there is no Save
button. Event time and days are text entities (`HH:MM` or signed `+03:00`, the whole text: anything after it is
refused;
`mon,wed,fri` / `weekdays` / `all`). Config: guard settings, gap between
portions, latitude, longitude, reload pause, refill interval
and the timezone (a POSIX string, see section 6). Hardware (firmware 2.0.0, section 2.1):
Board, Servo Pin, Button Pin, LED Pin (selects), Pins In Use, Restart Needed, Restart. Calibration: mode switch, position
select, one offset number, servo min/max pulse (saved, tuned at run time),
declare-position recovery.

Read-only data for the web app (firmware 1.1.0), worked out by FeederCore so the app
never repeats scheduler or guard logic. Each fits Home Assistant's 255-character state:
- `Today's Feeds`: today's feeds of the active profile and one-shots, in time order:
  `HH:MM xN <status>[ one-shot]` joined by `; `. Status is `fed`, `skipped` (missed or
  refused; a scheduled feed counts as fed if the history has a feed from its source in
  its minute), `due` (in its minute now), `next`, `later`, or `held` (inside a hold or
  the reload pause, so it will be skipped). With none: `none`, `none (fast day)`,
  `none (not a feeding day)` or `none (scheduler off)`; `clock not set` without a
  clock. Cut short with `; ...` past 240 characters.
- `Feed Guard`: the guard's answer to a one-portion feed from ESPHome right now,
  without feeding: `ready`, or the refusal in Last Result's words.
- `Device Time` (diagnostic): `Thu 2026-10-15 17:30 -05:00`, or `clock not set`.
- `Schedule Data` (internal diagnostic):
  `1|<A>|<B>|<C>`. The leading `1` is the format version. Each profile is
  `every_n,fast,ev1,ev2,ev3,ev4`, with `fast` the fast-day weekday mask as 2 hex
  digits. Each event is `<0|1><c|r|s><offset>/<portions>/<days>`: enabled, anchor
  (clock, sunrise, sunset), offset in minutes as stored (minutes after midnight, or
  signed from the sun), portions, and the weekday mask as 2 hex digits (bit 0 =
  Sunday). About 165 characters at most.
- The optional `network_info` package adds ESPHome's `wifi_info` (network, up to four
  addresses) and `wifi_signal` sensors.
- With the BLE package, two internal diagnostic text sensors and one web endpoint. They never contain the key, a nonce or a tag:
  - `BLE Info`: `service=<uuid>;protocol=2;auth=hmac-sha256;time=<policy>;stale_h=<h>;key=<none|build|device>;
    guard=<0|1>;calibration=<0|1>;bonds=<n>` (where the key comes from, and the two BLE permissions, firmware 2.0.0;
    the clients that paired, 2.4.3, once the stack has counted them).
  - `Forget BLE Bonds` (2.4.3, internal, protected): a button that unpairs every bonded client (section 10).
  - Two config switches (firmware 2.0.0, section 10): `BLE Can Change Guard Limits` and
    `BLE Can Change Calibration`.
  - `BLE Session`: `connected;auth;peer;mtu;since;last;conns;wr;wrb;rd;rdb;nt;ntb;authok;
    authfail;refused;invalid`, as `key=value` pairs joined by `;`:
    - connected and authenticated (0 or 1);
    - the last two bytes of the client's address;
    - the MTU;
    - the uptime in seconds of the connection and of the last read or write
      (`-1` = never);
    - counters since boot: connections, writes and bytes written, reads and bytes
      read, notifications and bytes notified, AUTH accepted and refused, writes
      refused as not authorized, and writes answered INVALID_ARG or UNSUPPORTED.
  - `GET /feeder/ble` (web server, behind its auth when that is set): the last 32
    connections and writes, oldest first, as `{"entries":[{"id":1,"t":12,
    "kind":"connect"},{"id":2,"t":12,"kind":"command","name":"FEED","seq":5,
    "result":"ok","detail":1},...]}`.
    - `kind`: connect, disconnect, auth, command, record or replay (a retry
      answered from the cache).
    - `t`: uptime in seconds.
    - `result`: the result's name, as in Last Result.
    - `id` increases by one per entry.
    - Reads and Status notifications are only counted.

## 8. BLE GATT (secondary control plane)

**The protocol is specified in [docs/PROTOCOL.md](docs/PROTOCOL.md), which is
authoritative.** This section records the design intent and how the built
protocol differs from the first draft.

Implemented on ESP-IDF's NimBLE host since 2.4.0 (D33; before, ESPHome's `esp32_ble_server` on Bluedroid). The
GATT table (UUIDs, properties) is C++ in `gatt_table.h` (D34; before, YAML in `packages/ble_gatt.yaml`), and the
meaning of the characteristics is C++ in `components/precision_feeder_ble/`: byte layouts and
validation in `protocol.h`, the cryptography in `crypto.h`, and every handler in
`BleEngine` (`engine.*`), all plain C++ tested on a host against the simulated
feeder; `precision_feeder_ble.*` only connects the engine to the radio. NimBLE calls the adapter from its own
task, so each callback only copies into a fixed queue or reads a locked value store (`gatt_link.h`, host-tested);
the engine runs on the main loop, as it did on Bluedroid. A write that finds the queue full is answered with an ATT
error (insufficient resources), never dropped silently; the client retries, and the retry cache makes that safe.
The advertising is the Bluedroid build's: the flags and the 128-bit service UUID, the name (with the MAC suffix
rules of `esp32_ble`) and the transmit power in the scan response; one client at a time. NimBLE is trimmed to one
link (small buffer pools, no BLE 5 extras, no scanning, two controller activities): its defaults cost as much heap
as Bluedroid (docs/BENCH.md). The controller keeps its central role (`CONFIG_BT_CTRL_BLE_MASTER`): without it the
ESP32-S3's controller refuses every advertising start with HCI error 0x12, so the feeder was never seen (found on the
bench with 2.4.3, fixed in 2.4.4, D42). The package cannot share a build with `esp32_ble` or anything built on it; the build
refuses the combination. BLE is a build option: HA-only builds leave the package out and pay nothing.
`bluetooth_proxy` must not be enabled on this device. All records fit the default
20-byte payload, so no MTU negotiation is needed. UUID base:
`02dd1dd9-ba75-4bbc-bdea-306d2101xxxx` (generated 2026-10-13; this project's own).

Protocol 2 (firmware 0.4.0 and later) authenticates every write (section 10). In
short: a HOLD takes 32-bit minutes (0 cancels, `0xFFFFFFFF` until cancelled);
Profile, Config and Calibration use a query-then-read pattern (GATT reads take no
parameters); every write carries a sequence number and a tag and is answered on
Response with that sequence number; Response is also readable; exact retries are
answered from a 30 s cache of the last 8 writes and never act twice;
`SET_TIMEZONE` is reserved (section 6). Status reports the tray, the schedule,
the clock source and age, and fault flags.

## 9. Operational behaviour

- ESP-IDF framework; WiFi fallback AP; OTA password; API encryption.
- Dual stack (firmware 1.1.0): IPv6 next to IPv4 (`enable_ipv6`, default on). ESPHome 2026.9.1's Wi-Fi only
  counts as connected with an IPv4 address, so an IPv6-only network needs a static IPv4 address
  (`manual_ip`); decided 2026-10-15 (section 14, item 7).
- No reboot on API or WiFi loss. No scheduled reboot.
- Versioning `major.minor.revision`, exposed as a text sensor and in the Info
  characteristic.
- **Optional push button**, matching the original: with the button enabled in
  the YAML, a press feeds one portion, and a press with the tray empty
  reloads the tray (the original's single-button behaviour). Long press enters
  calibration. With no button fitted, nothing is configured and the feature
  does not exist. Status LED for state and faults if the board has one.

### 9.1 YAML configuration surface

Each feature is its own package (`packages/`), and the examples
(`examples/basic.yaml`, `examples/ble.yaml`) include them; a feature is switched
off by commenting its line out. Every setting is a substitution with a comment
saying what it does and when to change it. A user clones the repository into the ESPHome
config folder as `precision-feeder/` and copies an example next to it; the copy
reads the components and packages from the clone (`precision-feeder/...` paths),
so `git pull` updates the firmware without touching the user's file or secrets.
`tools/check.sh` validates the examples in exactly that layout. [docs/BUILDING.md](docs/BUILDING.md) lists every
package and substitution.

Entities publish only when their value changes: each package refreshes its
entities from one 1-second interval through `publish_number/text/bool/option`
(`precision_feeder.h`), instead of each entity polling on its own.

### 9.2 Device record (firmware 2.0.0)

The settings a prebuilt binary needs at run time are saved in their own record,
`Device` (`feeder_core.h`, preference key `precision_feeder.device.v2`, version 2 since 2.0.2; version 1, key
`.device.v1`, is migrated at boot with every field kept),
next to Settings, Calibration and State. Its layout already holds every field 2.0
sets at run time. 2.0.0 laid out every field the milestones needed; 2.0.2 added the NTP servers as version 2:

| Field | Meaning | "Use the build's value" | Milestone |
|---|---|---|---|
| `servo_pin`, `button_pin`, `led_pin` | GPIO numbers; `PIN_NONE` for no button or LED (section 2.1) | `PIN_BUILD` | N3 (built) |
| `flags` | setup done; BLE may change guard limits; BLE may change calibration (both on by default) | | N4, N6 (built) |
| `hostname` | device name (section 9.3) | empty | N5 (built) |
| `timezone` | POSIX TZ string | empty | N1 (built) |
| `ap_password` | fallback access-point password (section 9.3) | empty | N5 (built) |
| `ble_key` | BLE key (section 10) | all zero (no key set here) | N4 (built) |
| `password_scheme`, `password_salt`, `password_hash` | settings password (0: none) | | N2 |
| `ntp_servers` | up to three NTP servers, "a,b,c" (version 2) | empty | 2.0.2 |
| `reserved[0]`, `reserved[1]` | the drive chosen on the device, and the drive the last boot used (section 3.4); spare bytes until 2.1.0, so the layout is unchanged | 0 (the build's) | 2.1.0 |

The mechanism's tuning values (firmware 2.1.0) are not in the Device record but in their own, `Tuning`
(`precision_feeder.tuning.v1`): the mechanism's name, its table layout and up to 16 values. Only a mechanism with a
tuning table creates it; a value is checked against the table when it is set and again when it is loaded. The disc's
motor pins are in a third, `MotorPins` (`precision_feeder.motor_pins.v1`, D19), read at boot before any driver starts.

- **The YAML stays in charge until the device is changed.** A fresh record says "use
  the build's value" everywhere, so a YAML build keeps following edits to its YAML. A
  value set on the device wins. The record is saved at the first boot that has none
  (2.0.0 N6, to remember whether setup is needed; section 9.4), with every value still
  the build's.
- Saved at once (not debounced): these change rarely and matter after a reboot.
- On load, strings are terminated, unknown flag bits cleared, and a timezone that no
  longer parses is dropped (with a warning) in favour of the build's.
- The API key and Wi-Fi networks are not in it: ESPHome keeps them in its own
  preference slots (section 14, item 10). The API key is set at run time only in a
  build with `api: encryption: {}` and no `key:` (packages/api_key_on_device.yaml):
  Home Assistant provisions it on adoption, or the web app generates, takes or
  clears it (`save_noise_psk()`, `clear_noise_psk()`). A YAML key cannot be changed
  at run time; ESPHome compiles those calls out.

### 9.3 Network (firmware 2.0.0)

The name, the Wi-Fi network and the fallback access point's password are set in the web app (Configuration,
Network) through endpoints of the settings guard (section 10.1), not entities: they are protected, and Home
Assistant has no use for them. Checked against the ESPHome 2026.9.1 source.

- **Name** (`Device.hostname`): a DNS label, 1 to 31 lowercase letters, digits and hyphens (`check_hostname()`).
  ESPHome sets its name once from the YAML (`App.pre_setup()`) with no setter, and Wi-Fi (DHCP host name), mDNS,
  the API, BLE and the access point read it when they start. So the loader (`PinLoader`, BUS priority, before all
  of them) replaces it with a saved name through the protected member (`AppName` in precision_feeder.cpp; it stops
  compiling if ESPHome renames the member). A saved name is used exactly: no MAC suffix. Applies at the next
  restart (Restart Needed).
- **MAC suffix:** the `name_add_mac_suffix` substitution (default off; on in the prebuilt binaries, section 9.5),
  ESPHome's own option.
- **Access point:** with no `ssid:` in the YAML, the loader names it `<name in use>-setup` (cut to 32 bytes), so a
  MAC suffix or a saved name tells feeders apart; one set in the YAML stands. A password saved on the device
  (`Device.ap_password`, 8 to 63 printable characters) replaces the YAML's at the next restart. Empty: the YAML's.
- **Wi-Fi network:** not in the Device record. Saved with ESPHome's own `save_wifi_sta()`, the slot the captive
  portal and Improv use. That call only connects when Wi-Fi is idle, so a connected feeder stayed on its old network
  (found on the bench); a second after answering, the adapter drops the link (`esp_wifi_disconnect()`) and ESPHome's
  own reconnect joins the saved network, or opens the access point if it cannot (the second is so the answer gets
  out over the old link). ESPHome keys that slot to the build (config and ESPHome version hash)
  when the YAML has a network, so a YAML build's own network returns after an update built from a changed YAML; with
  no network in the YAML (the prebuilt binaries) the key is fixed and the saved network stays.
- **Forget** (2.3.0, D30): ESPHome has no call to delete the slot, so in a build with no network in the YAML (code
  generation sets `set_wifi_forget_built()`) the adapter erases it from NVS itself (namespace `esphome`, key
  `88491487`: ESPHome's fixed hash for that case, checked against its source by tests/test_consistency.py) and
  restarts once the tray is still, into the setup access point. Only while a network is saved (`has_sta()`). A YAML
  build's network is in its YAML and cannot be forgotten. Everything else stays.
- **Scan** (2.3.0, D31; with the web server): code generation asks ESPHome to keep its scan results after
  connecting, guard them with a lock (they are read on the web server's task, under `ScanResultsLock`) and report
  each scan's results (a listener). `POST /feeder/wifi/scan` starts one only while connected and ESPHome is not
  scanning for a roam: ESP-IDF's `esp_wifi_scan_start()`, active, background-coexistent (back to the home channel
  between channels, so the link holds), as ESPHome's own roaming check does; not ESPHome's `start_scanning()`, a
  step of its connection state machine that would reconnect. Otherwise 409 `offline`, and the list is what ESPHome
  found last (on the setup access point, the captive portal's scan). `GET /feeder/wifi/networks`: up to 20, strongest
  first, each name once, hidden ones left out, the name as hex (any bytes); whether a scan can start, and whether one
  of ours is running (until its results arrive, at most 15 s; about 9 s on the bench). Neither is protected: the captive portal shows the
  same to anyone on the access point. If the link drops while our scan runs, ESPHome's reconnect scan may be refused
  or use our results; its own scan timeout and retries cover that. The web app's Join dialog (Configuration and the
  setup wizard) offers the list with "Other network..." for a typed name, and the typed name alone without the route.
- **Improv serial** (`packages/improv_serial.yaml`, on in all three examples): ESP Web Tools sets Wi-Fi over the USB
  cable after flashing. Physical access is trusted (section 10.1).
- **Endpoints** (signed like any protected change; section 10.1): `GET /feeder/network` reports the name in use,
  saved and the build's, where the access-point password comes from (`build`, `device`, `off`), whether Wi-Fi can
  be set, whether it can be forgotten (`forget`), and whether a restart is due; never a password. `POST /feeder/network/name?name=` (empty: the build's),
  `/feeder/network/ap?password=<hex>` (empty: the build's), `/feeder/network/wifi?ssid=<hex>&password=<hex>` and
  `/feeder/network/forget` (signed with nothing after the path).
  The passwords travel masked by the settings password: the text XOR HMAC-SHA256(key, nonce || "new secret 1")
  || HMAC-SHA256(key, nonce || "new secret 2"), cut to its length (`password_unmask_text()`, golden vector shared
  by tests/test_core.cpp and tests/test_web.mjs). 400 for a value FeederCore's checks refuse, 409 when the build
  has no access point or no Wi-Fi, or (forget) a YAML network or none saved.
- **The MQTT password** (2.4.5, D43) is masked the same way: `POST /feeder/mqtt/password?password=<hex>` (empty:
  none), signed over the path and the hex. It sets the MQTT Password text (`mqtt_password_id` in packages/mqtt.yaml),
  which never reads back (ESPHome sends a password text as `********`). A plain `POST /text/MQTT Password/set` is
  refused (403 `masked`).
- On load, a saved name or password that no longer passes the checks is dropped (with a warning) for the build's.

### 9.4 Setup wizard (firmware 2.0.0)

A new feeder (no Device record, no saved settings or calibration) reports `"setup":true` in `GET /feeder/network`,
and the web app opens `#setup` by itself (unless a page other than the dashboard was asked for). The wizard is a page
of the app, not a firmware feature: it steps through the network (section 9.3), time zone and location, board and
pins (with the hardware package), keys, the settings password, then points at the calibration walkthrough. Each step
uses Configuration's own controls and saves as it goes; any step, or the rest, can be skipped.

- `DEVICE_SETUP_DONE` in the Device record ends it: `POST /feeder/setup?done=1` (finished or skipped; signed like any
  protected change), `done=0` offers it again. Configuration links to the wizard at any time without touching the flag.
- Decided once, at the first boot with no Device record, and saved at once: a feeder updated from 1.x (Settings or a
  Calibration saved) counts as set up, so it is never sent through the wizard; any other is new. Found on the bench:
  a new feeder saves its settings and tray state soon after its first boot, so deciding at every boot (the first
  version) lost the wizard after one restart.
- **Time zone by place:** the app carries every IANA Area/Location name with the POSIX string ESPHome compiles for it
  (`web/src/tz_zones.js`, generated by `tools/gen_tz_zones.py` with ESPHome's own extraction), so a zone chosen in the
  app behaves exactly as `timezone:` in the YAML. It writes the Timezone entity; a typed POSIX string still works.
  tests/test_web.mjs checks that every string in the table is in `tests/tz_golden.h`, which the device's parser is
  tested against. About 5 KB gzipped.

### 9.5 Prebuilt binaries and updates (firmware 2.0.0)

GitHub Actions builds two variants from `firmware/full.yaml` (with BLE) and `firmware/no-ble.yaml`
(`tools/build_bins.sh`), each as a factory image (first flash, from address 0) and an OTA image. They carry nothing
personal: no Wi-Fi network, no API key (`api_key_on_device`), no BLE key, a UTC timezone, `name_add_mac_suffix` on,
and every package except none (the button and status LED included; their pins can be "none"). Since 2.1.0 they
carry both drives, servo first (section 3.4), and MQTT, off until set up (section 9.6). The fallback access
point is **open** on a clean flash (owner, 2026-10-16); the wizard strongly recommends setting a password, and
`GET /feeder/network` reports `"ap":"open"` until one is set (`"ap_build_open"` says the build has none, so
removing a saved one leaves it open). `tools/build_bins.sh --config` (in check.sh) fails if any secret reaches a
binary's config.

- **Firmware Variant** (status.yaml, `fw_variant` in core.yaml): `full`, `no-ble` or `yaml` (any build from a
  user's own YAML), fixed at build time.
- **Release** (`.github/workflows/release.yml`): a tag `v<fw_version>` runs every check, builds both variants, and
  publishes a GitHub release (images, `manifest.json`, the CHANGELOG section as notes) and GitHub Pages (the
  install page, `install/index.html`, with ESP Web Tools 10.4.0; the factory images it flashes; `manifest.json`).
  `tools/make_release.py` refuses a tag that is not `fw_version`. Run by hand, it only builds (an artifact).
- **`manifest.json`**: `version`, `notes` (the release page), `releases`, and per variant the `factory` and `ota`
  image's `name`, `size` and `sha256`. Pages serves it with `Access-Control-Allow-Origin: *` (section 14, item 10).
- **Update check, in the browser** (web app, `#firmware`, linked from Diagnostics): fetches `manifest.json` when
  the page opens, unless turned off in that browser, and compares it with Firmware Version and Firmware Variant.
  Up to date, update available (release notes link), "Couldn't check" (releases link; also after 10 s with no
  answer), or for a `yaml` build the latest version and "update with ESPHome". The feeder itself never contacts the internet.
- **Upload** (`POST /feeder/firmware?sha256=<hex>`, the image as the raw body, `application/octet-stream`):
  a route of the settings guard (section 10.1), signed over the path and the hash. The browser hashes the file and
  names it from the manifest (release, variant; a factory image is refused; an unknown file needs a confirmation).
  The feeder streams the body into ESPHome's OTA backend (`ota::make_ota_backend()`, as its own web upload does),
  hashing as it goes (`FirmwareUpload`, firmware_upload.h, host-tested), and only finishes the update if the whole
  image arrived, it is an app image (the app description's magic at byte 32: a `.factory.bin` starts with the
  bootloader, which ESPHome's backend would accept and only the bootloader would refuse at the next boot; found on the
  bench, N8), and its SHA-256 is the signed one; otherwise it aborts and the running firmware stays (422: `image`,
  `sha256`, `incomplete`, `flash`). The Firmware page checks for the app description too. Then it restarts half a second later, or once the tray stops if it is moving.
  ESPHome's own `/update` is off for the web server (`ota: false`), but the captive portal brings its upload into
  every build and takes an image while the access point is up, with no settings password. Since 2.4.5 (D43) a handler
  of ours, registered before ESPHome's in every build (headless too), claims `POST /update` and refuses it (403).
- Flash (ESPHome 2026.9.1, 1.8 MB app slot of the 4 MB layout): **full 79.1%** (1,451,555 bytes), **no-BLE 61.1%**
  (1,120,447 bytes) in 2.0.2; 78.9% and 60.9% in N7. 2.1.0 with both drives and MQTT: **full 88.5%** (1,623,115
  bytes, RAM 48.2%); **no-BLE 70.4%** (1,291,243 bytes, RAM 39.0%). 2.1.2 with ESPHome 2026.10.0: **full 88.3%**
  (1,619,707 bytes, RAM 48.1%), **no-BLE 69.8%** (1,281,675 bytes, RAM 38.9%); the examples basic 61.1%, BLE 79.7%,
  disc 62.4%. No 8 MB variant needed.

### 9.6 MQTT (firmware 2.1.0, D20; scopes and own topics 2.2.0, D22 to D24)

`packages/mqtt.yaml`, optional, in the prebuilt binaries: ESPHome's MQTT client beside the native API. It publishes
every entity Home Assistant sees over the API (since 2.2.0 the slim set, section 7), with Home Assistant discovery (unique IDs from the MAC, so feeders
never clash), and answers ESPHome's device discovery (`discover_ip`: `esphome/discover` and `esphome/ping/<name>`,
which the dashboard and Home Assistant use to find the feeder's addresses). The live log is not published.

- **Nothing about the broker is in the image.** MQTT, MQTT Broker, Port, Username, Password and Topic Prefix are
  template entities saved on the feeder (restore_value), set in the web app (Configuration, MQTT). They are
  `internal`, so neither Home Assistant nor MQTT itself can point the feeder at another broker; the web server
  shows them (`include_internal`), and they are protected settings (section 10.1). The password is a password-mode
  text: the web server sends only a mask, never the value (the web app's box starts empty).
- **At boot** (on_boot, after the values are restored and before the client starts): the saved settings go into
  the client, the topic prefix is the saved one or else the device's name in use (ESPHome works its default out
  from the build's name, before a name saved on the feeder is applied: found on the bench, fixed in 2.1.1), the birth, will and shutdown messages follow the topic prefix in use (ESPHome builds them from the
  YAML name, without the MAC suffix), and the client starts if MQTT is on and a broker is set.
- **At run time:** turning MQTT off applies at once. ESPHome's ESP32 backend builds its MQTT client once per boot, with
  the broker and login it has then (`initialize_()`), and a second start does nothing: so before the client has
  started this boot, turning it on and the broker, port and login apply at once; after that a change is saved and
  Restart Needed turns on (`FeederCore::note_restart_needed`), and turning it on again after it was off waits for the
  restart too. (2.1.0 and 2.1.1 said "at once"; found on the bench with 2.2.0, fixed in 2.1.2.) The topic prefix
  applies at the next restart: ESPHome subscribes to the command topics while starting up.
- **No restart on loss** (`reboot_timeout: 0s`), as for the API (AGENTS.md).
- **Its own topics (2.2.0, docs/MQTT.md):** plain topics, one value each, under `<prefix>/feeder/`: the schedule
  (`schedule/<A|B|C>/event/<1-4>/{enabled,anchor,time,portions,days}`, `every_n_days`, `fast_days`, `restart_cycle`),
  one-shot feeds (`oneshot/add`, `oneshot/clear`, `oneshot/list`) and the feed guard's limits
  (`policy/{daily_cap,min_gap_s,portion_gap_s,max_portions_cmd}`), each with a retained state topic and `/set`. The
  limits are **read-only** (2.3.0, D28): their `/set` is refused with `NOT_ALLOWED` (18) and answered with the value in
  force, and `policy/writable` is always `OFF`. (2.2.0 let them change behind a switch, MQTT Can Change Guard Limits,
  D24; its Device flag bit, 0x08, is retired and cleared on load.) Every value goes through FeederCore's setters
  (`set_event_*`, `set_profile_*`, `set_daily_cap` ...), which the web app's editor uses too. The routing is
  `FeederMqtt` (feeder_mqtt.*, pure, tests/test_mqtt.cpp); `components/precision_feeder_mqtt` subscribes to
  `<prefix>/feeder/#` at run time (a YAML `on_message` topic is fixed at build time, the prefix is not), publishes
  what changed once a second (so changes from the web app, BLE and the schedule reach MQTT) and everything again after
  a reconnect, a few messages per loop pass. A refused value is answered with the one in force, plus a `result`
  topic. Never calibration, pins, keys, network or the 24 h count. A one-shot identical to a pending one is not added
  again (a retained `oneshot/add`). Cost: 8.8 KB of flash.
- **Trust:** whoever the broker lets publish to the feeder's topics can feed (through the feed guard), edit the
  schedule and, with the switch on, change the guard's limits. The settings password does not apply over MQTT. Use
  broker accounts and ACLs (docs/SECURITY.md). Components are never on MQTT (D26; closes open item 24).
- Cost: about 132 KB of flash (mostly per-entity discovery code, generated for every entity even when internal).

### 9.7 HTTPS, the trust store, MQTT over TLS and ACME (firmware 2.4.1 to 2.4.3, D36 to D41)

`components/precision_feeder_tls`, with `packages/tls.yaml` (HTTPS), `packages/mqtt_tls.yaml` (MQTT over TLS,
needs `mqtt.yaml`) and `packages/acme.yaml` (the certificate from an ACME server, 2.4.2); all three in the prebuilt
binaries, HTTPS in the examples. The decisions are plain C++ in `tls_policy.h` (`tests/test_tls.cpp`), the ACME
client in `acme.h` (`tests/test_acme.cpp`); `precision_feeder_tls.cpp` is the device's side. Measurements and the options
weighed: docs/plans/https-tls.md.

- **HTTPS is a relay (D37).** A task listens on port 443; each connection gets its own task (6 KB stack, about 20 KB
  of heap while open), does the TLS handshake with esp-tls (TLS 1.2, session tickets, ECDHE with P-256) and copies
  plain bytes to and from ESPHome's web server on 127.0.0.1:80. Every handler (the app, `/events`, the settings guard,
  the signed firmware upload) works unchanged over both. At most `max_sessions` (3, 1 to 6; 4 until 2.4.4, D42) at
  once; a connection beyond waits in the backlog (3). A session idle both ways for `idle_timeout` (20 s, 12 s to
  5 min) is closed (an open page's event stream sends something every 10 s). mbedTLS buffers: 16 KB in, 4 KB out,
  allocated while needed. Every TLS task runs on core 0, never on core 1 where ESPHome's loop runs the feeder. The
  relay closes the browser's socket itself (`esp_tls_server_session_delete` frees only the TLS state), gives its
  connection to port 80 at most 5 s, and no write blocks longer than the idle timeout; lwIP's TCP connections are
  raised to 16 + 3 per session + 3 (all three found on the bench, D42).
- **The identity.** At its first start (in the first loop pass, so the name saved on the device is in force) the
  feeder makes a self-signed certificate: P-256, `CN=<name>.local, O=Precision Feeder`, SAN `<name>.local`, valid
  2025 to 2049 (the feeder has no clock at its first boot), a random positive serial. It is kept in NVS (namespace
  `pf_tls`, new in 2.4.1: `ss_id` holds the certificate, key and name) and made again when the name changes. An
  uploaded certificate chain and key (`up_id`) win while they still check (every certificate parses, the key
  matches the first one, the chain verifies against the trust store's CA when one is set; dates only once the clock
  is valid); one that no longer checks at a start falls back to the self-signed one, and the HTTPS status says why.
  An uploaded identity is used after a restart (Restart Needed); ACME's is served at once (below). The keys are in
  flash in the clear (no flash encryption, as the other keys: docs/SECURITY.md).
- **ACME (2.4.2, D40).** With ACME Directory set (an `https://` URL; internal, protected), the feeder asks that ACME
  server (RFC 8555; step-ca on the LAN, say) for a certificate for ACME Name (empty: `<name>.local`) and serves it
  ahead of an uploaded one. `acme.h` holds everything that is not I/O or cryptography: a small JSON reader, the JWS
  (ES256: the protected header with `jwk` until the account is known, then `kid`; DER signatures turned into r || s),
  the exchange (directory, nonce, account, order, each pending authorization through http-01, finalize, polls every
  2 s for at most a minute each, the certificate), one retry on `badNonce`, the checks on what is asked for (a DNS
  name without a wildcard; an https directory), and when to ask. The device's side (`AcmeIo`) is esp_http_client
  checked against the trust store with the server's name (no trust store, no ACME: never unchecked; no public CA
  bundle), the account key (P-256, made at the first run and kept, `ac_acct`), and a CSR with a new P-256 key for
  every certificate. The challenge's key authorization is served on port 80 at
  `/.well-known/acme-challenge/<token>` only for the token being asked about (D38's filter never touches that path).
  What comes back is checked before it is kept: the chain parses, its key is the CSR's, and it verifies against the
  trust store for the name. Saved as `ac_id` (chain, key, name, directory) and served at once: new sessions get
  it, open ones keep theirs. It runs in its own task (10 KB stack, priority 2), never on the main loop, which only
  checks every 30 s whether it is due: only with a valid clock; at once with no certificate, or one for another name
  or directory; else two thirds into its lifetime (16 h into step-ca's default 24 h). A failure keeps the current
  certificate and waits 5 min, 15 min, 1 h, then 3 h between tries; a certificate past its end gives way to the
  uploaded one, else the self-signed one, without a restart (and never a reboot). A certificate for the name in use
  stays served after the directory changes, until the new directory delivers or it expires, so a mistyped directory
  costs nothing that works (2.4.4, D42); one for another name is never served. Clearing the directory (ACME off) goes
  back at once. A server that does not answer is reported as "not reachable", "name not found" or "its certificate
  does not check against the trust store" where esp-tls can tell (2.4.4). Status: the ACME text sensor and `GET /feeder/tls`'s `acme`; `POST /feeder/tls/acme/renew` (signed, empty
  body) asks again now. Flash: about 47 KB (the HTTP client and its parser most of it; its WebSocket transport is
  turned off).
- **Port 80 (D38).** Home Assistant's ESPHome integration builds the device's link itself, always
  `http://<address>:<web server port>` (Home Assistant core, `manager.py`), and ESPHome's MQTT discovery sends none,
  so the feeder cannot make Home Assistant show an https link. Instead, with HTTPS Redirect on (the default), a GET of
  `/` or `/index.html` on port 80 is answered `302` to `https://<the certificate's first DNS name, else
  <name>.local>/` (port added when not 443; an IPv6 literal in brackets), so the link lands on HTTPS. With HTTPS Only
  on (off by default), everything else on port 80 is refused with 403 `{"error":"https"}`. Never filtered: the relay's
  own requests (from 127.0.0.1 or ::1), anything while the access point is up (captive portal, setup wizard), ACME's
  `/.well-known/acme-challenge/`, and everything until HTTPS is listening, so a failed HTTPS never locks the owner
  out. The filter is `PrecisionFeeder::RequestFilter`, a handler registered before the settings guard. Both switches
  are internal and protected.
- **The trust store.** One PEM bundle (`ca`: one or more CA certificates) the owner uploads; MQTT and ACME check
  their servers against it, and an uploaded identity is checked against it.
- **Endpoints.** `GET /feeder/tls` (JSON: HTTPS state, sessions, the certificate's source, name, subject, expiry and
  SHA-256 fingerprint, the trust store's subject, MQTT's TLS status and pin). `POST /feeder/tls/ca` (body: the PEM,
  empty clears it), `/feeder/tls/identity` (body: the private key's hex, masked, on the first line, then the chain's
  PEM; empty goes back to the self-signed one), `/feeder/tls/self-signed` (make a new one at the next start),
  `/feeder/tls/mqtt-pin/forget`, and since 2.4.2 `/feeder/tls/acme/renew`. Bodies are raw (`application/octet-stream`; ESPHome's form parser takes at most
  1 KB), at most 10 KB. Each is signed with the settings password as the firmware upload is (section 9.5): the
  `?sha256=` parameter must be the body's hash, and the tag covers `<path>\n<sha256>`. The private key is masked with
  `FeederCore::password_unmask_blob` (HMAC-SHA-256 blocks keyed with the password's derived key over the signature's
  nonce, "new secret blob" and a counter; no mask with no password, as for the other keys).
- **The web app (2.4.3, D41).** Configuration has an "HTTPS and certificates" card, drawn from `GET /feeder/tls` and
  read again when the HTTPS or ACME status changes: the certificate in use (source, name, expiry, fingerprint, open
  sessions, why an upload is not used, a link to the app over HTTPS), uploading a certificate and key (pasted or
  from files; the browser picks the PEM blocks out, refuses an encrypted key and the sizes the device refuses, and
  masks the key: `maskBlob` in `auth.js`, tested against the device's vector), removing it, making a new self-signed
  one, the two port 80 switches, the trust store (upload, replace, clear), and ACME (status, Renew now, the two
  settings). The MQTT card has MQTT TLS, MQTT TLS Check, the TLS status and the pinned certificate's fingerprint with
  Forget. The page never decides anything: the device checks every upload again.
- **MQTT over TLS (D36, D39).** MQTT TLS (off by default) and MQTT TLS Check ("CA certificate" or "Pin on first
  contact") are internal, protected and applied at boot (on_boot priority 590, after `mqtt.yaml`'s 600 and before
  the client starts); changing them sets Restart Needed. "CA certificate": the client checks the broker against the
  trust store, with the name check. "Pin on first contact": the first time, a task connects to the broker with
  verification optional, takes the chain it shows and pins its top certificate if it can anchor a check (a
  self-signed certificate or a CA; a lone leaf signed by a CA it does not send cannot be pinned: upload the CA), saves
  it (`mq_pin`) and starts the client against it without the name check; it tries again every minute until it can.
  **With TLS on, MQTT never connects in plain text (D39):** without a CA, or before the pin, the client gets a
  placeholder certificate that can never verify and is not started; turning MQTT on later waits for a restart
  (`mqtt_can_start()`). Forgetting the pin pins again at the next start.

## 10. BLE security

Decided 2026-10-14 (owner: "use best practices"), built in protocol 2
(firmware 0.4.0). Details in docs/PROTOCOL.md, "Authentication".

What ESPHome 2026.9.1 offered, checked in its source: `esp32_ble` can set the
Security Manager parameters (`io_capability`, `auth_req_mode` including
`sc_bond`, key sizes), but `esp32_ble_server` hard-codes every characteristic's
permissions as plain read/write (no "encryption required"), and there is no
static passkey. So link-layer pairing cannot keep a stranger out: they can skip
pairing and write anyway, and with no screen or keypad pairing is "Just Works".
NimBLE (2.4.0) could require encryption per characteristic, but with "Just Works" that would not keep a stranger out
either, and it would break clients that do not pair (PLC bridges), so the permissions stay plain (D35).

Hence application-layer authentication, plus link encryption where offered:
- A shared 32-byte key. Since 2.0.0 (N4) a key saved on the device (Device record,
  set from the web app) wins over the build's (`ble_key`, secrets.yaml, now
  optional). With neither, AUTH is always refused (NOT_AUTHORIZED detail 6): never
  an all-zero key, which anyone could compute with (the YAML validator refuses one
  too). A new key ends an authenticated session at once (new nonce); the client
  authenticates again with the new key.
- Two permissions, saved on the device and both on by default (the BLE key is an
  admin credential): "BLE can change guard limits" (Config keys 1, 2, 5, 6) and "BLE
  can change calibration" (Calibration writes, Config keys 10 and 11, CALIBRATE,
  JOG, SAVE_CALIBRATION). Off, those writes are answered NOT_AUTHORIZED with detail 7
  (still counted for the sequence number and the retry cache). Queries, feeding, the
  schedule, the clock, hold and SET_POSITION are never refused by them. They are
  switches (packages/ble_gatt.yaml) protected by the settings password (10.1).
- Per connection: a random 16-byte nonce (hardware RNG) readable from a new Auth
  characteristic; the client proves the key with AUTH (HMAC-SHA256 over the
  nonce); both derive a session key.
- Every write carries a sequence number (strictly increasing, window 127) and an
  8-byte HMAC tag bound to the characteristic. This stops injection, tampering
  and replay (recorded traffic is useless on the next connection's nonce, and a
  stale sequence number is refused within a connection). The 30 s retry cache
  still makes exact retries safe.
- Until AUTH, only Info and Auth carry data; nothing is notified.
- After a failed AUTH: a new nonce and a one-second wait. A failed AUTH that leaves the connection unauthenticated
  also clears what the session published (Status, Profile, Config, Calibration, Response read as zeros again), as a
  disconnect does (2.1.2; Codex review: before, they stayed readable).
- LE Secure Connections with bonding, so a client that pairs gets an encrypted link (also
  hiding what it reads). Since 2.4.0 (NimBLE) at most 3 bonds are kept in NVS and the oldest makes room; a bonded peer
  that pairs again replaces its old bond. Bonds made with 2.3.x (Bluedroid) are not carried over: a client that had
  paired pairs again (D35). Forget BLE Bonds (2.4.3, D41; the web app's Configuration, Keys, behind the settings
  password) unpairs every bonded client with `ble_gap_unpair`, which also ends a bonded client's connection; clients
  that never paired are untouched, and the key does not change (it, not the bond, is what lets a client act). BLE
  Info reports how many bonds are kept.
- Not covered: the feeder does not prove its identity to the client, and
  answers are not signed (no room in the 20-byte Status). The feeder only acts on
  authenticated writes, so the animals are protected; a client that must trust
  what it reads should pair.

SHA-256 and HMAC are a small pure implementation (crypto.h, in precision_feeder since 2.0.0) so the device code
is the host-tested code (FIPS 180-4 and RFC 4231 vectors, plus golden vectors
shared with the Python client).

### 10.1 Settings password (firmware 2.0.0)

Protects the guard limits, the calibration, the pins and the keys from changes through
the web server. None by default. Home Assistant (API key) and BLE (BLE key) are not
affected: they are already authenticated (BLE's access has its own two permissions,
section 10).

- **What it protects** is the firmware's list, `protected_entities` in
  `packages/web_server.yaml`: Minimum Gap Between Feeds, Gap Between Portions,
  Daily Portion Cap, Portions Counted (24 h), Max Portions Per Command, every
  calibration entity except Declare Tray Position (everyday recovery), the
  pins (Board, Servo Pin, Button Pin, LED Pin; section 2.1) and the two BLE
  permissions (BLE Can Change Guard Limits, BLE Can Change Calibration). Never the
  schedule, feeding, hold, tray reload, location or timezone.
  `tests/test_consistency.py` checks the list names real entities and covers the
  guard limits and calibration. The network, keys, setup and firmware endpoints (below) are protected the same way.
- **How:** ESPHome cannot hide one entity from the web server (section 14,
  item 10), so a handler of ours (`SettingsGuard`, precision_feeder.cpp) is
  registered ahead of the web server's: handlers are matched in order, and ours is
  added from code generation, before any component starts. It takes POSTs to
  protected entities, checks the proof, and passes good ones on to the web
  server's own handler. If `auth:` is configured, it wraps ours too (our code
  generation runs after the web server's).
- **The password never crosses plain HTTP.** The browser turns it into a key with
  PBKDF2-HMAC-SHA256 (20 000 rounds, a random 16-byte salt kept on the device).
  The device stores only the salt and that key, in the Device record (section 9.2).
- **Every protected change is signed:** `GET /feeder/auth` returns a one-time
  16-byte challenge (it also gives the salt, the rounds, the wait and the
  protected list). The request carries `X-Feeder-Auth: hex(nonce) hex(tag)`,
  where the tag is HMAC-SHA256(key, nonce || path || "\n" || value) and value is
  the `value` or `option` parameter. A challenge answers one request, right or
  wrong, and expires after a minute, so a recorded request cannot be replayed or
  moved to another value.
- **Failures:** a wait of 1 s after the first failure, doubling to 60 s; inside
  it nothing is checked (429). A success resets it. ESPHome's ESP-IDF request (2026.9.1) only sends 200, 204, 400,
  401, 404, 409 and 422 and turns any other code into 500, so 403 and 429 are sent through ESP-IDF directly (found
  on the bench in 2.0.0 N6; until then every refusal arrived as 500).
- **One challenge at a time** for the whole feeder: each `GET /feeder/auth` replaces the outstanding one. Two pages
  making protected changes at the same moment can spoil each other's challenge ("challenge" refusal); the app's
  next try works. Seen on the bench (section 14, item 18).
- **Endpoints:** `POST /feeder/auth/check` (the app's Unlock), and
  `POST /feeder/auth/password?salt=&key=` to set, change or (no parameters)
  remove it, signed with the current one. A new key travels masked:
  key XOR HMAC-SHA256(current key, nonce || "new key"); with no current password
  it travels as it is (it is a derived key, not the password).
- **Setup (2.0.0 N6):** `POST /feeder/setup?done=1|0` (section 9.4), signed over the path and the value.
- **Firmware (2.0.0 N7):** `POST /feeder/firmware?sha256=<hex>` (section 9.5), signed over the path and the hash;
  the challenge is checked before the first byte is written, and a refused upload never touches the partition.
- **Network (2.0.0 N5):** the name, Wi-Fi network and access-point password endpoints (section 9.3) are
  always signed when a password is set; the passwords travel masked by it.
- **Keys (2.0.0 N4):** `POST /feeder/keys/api?key=<hex>` and `/feeder/keys/ble?key=<hex>`
  save a 32-byte key, masked the same way and signed over the path and the hex;
  `/feeder/keys/api` with no key clears the API key. 409 when the build cannot (a
  YAML API key, no API encryption, no BLE), 400 for a malformed or all-zero key.
  `GET /feeder/auth` reports where each key comes from
  (`"keys":{"api":"none|build|device|off","ble":...}`), never a key. Keys are made in
  the browser (`crypto.getRandomValues` works on plain HTTP) and shown there once.
  With no password set they cross the network as they are (anyone there could set
  their own anyway); with one, only masked.
- **Cross-site requests:** the auth header makes browsers ask permission first (a
  CORS preflight), which the web server does not grant, so another website cannot
  make a protected change through a visitor's browser.
- **Recovery:** hold BOOT (GPIO0, `boot_pin`) for 10 s while the feeder runs. It
  clears the password and nothing else (FeederCore::boot_button, host-tested).
- **Stack:** the web server's task has about 4.3 KB of stack (ESP-IDF's 4 KB plus 256 bytes), and an authorised
  write runs the web server's own handler and the entity's action on top of ours. So our routes and URL buffer live
  in a non-inlined function that returns before the write is passed on (32 bytes left on the stack), and the upload's
  flash writes run the same way. Measured on the bench: 472 bytes to spare after a protected write, 616 after an
  upload (docs/BENCH.md, N8). A stack overflow there (2.0.0 N7 build) crashed the feeder on every protected write.
- **Threads:** the handler runs on the web server's task. FeederCore's password
  state is touched only under a lock (also held around the main loop's
  core_loop()); saving to flash is deferred to the main loop.
- **Limits, documented:** someone recording the network sees a challenge and its
  answer, and can try to guess the password offline; PBKDF2 slows that down, but
  a short password falls. The key is in flash without flash encryption (section
  13, appliance). An unknown scheme saved by a newer firmware refuses everything
  until BOOT clears it.

### 10.2 Who can change what (firmware 2.2.0, D21 to D27)

Everything the feeder offers is one of three kinds, and each channel reaches what matches how well it knows who is
talking ([docs/plans/2.2.0-scopes.md](docs/plans/2.2.0-scopes.md)):

| | Operate: feeding, the schedule, status | Safety policy: guard limits, the 24 h count | Components: calibration, tuning, drive, pins, keys, network, firmware |
|---|---|---|---|
| Web app | yes | yes, settings password | yes, settings password |
| BLE (a key, every write signed) | yes | guard limits behind BLE Can Change Guard Limits (never the 24 h count) | calibration only, behind BLE Can Change Calibration |
| Home Assistant (the API key) | the slim set (section 7); no schedule editing | no | no |
| MQTT (the broker's login) | the slim set, and the schedule and one-shots on its own topics (section 9.6) | no: published, read-only (2.3.0, D28) | never |

The 24 h count's hand edit is the web app's only (D25). A build without the web app sets its components in the YAML and
runs headless over BLE (D27).

## 11. Out of scope hardware

No external RTC, no feed or hopper sensors, no home or position sensor, no
humidity sensor, no reserved I2C header. Position is open-loop (section 3.1
describes how an interrupted move is recovered). The optional push button is
the only add-on, because the original firmware supports it.

## 12. Repository layout

```
precision-feeder/
  README.md  LICENSE  DESIGN.md  CHANGELOG.md  AGENTS.md
  docs/PROTOCOL.md     BLE protocol (authoritative)
  docs/MQTT.md         the MQTT topics (2.2.0)
  docs/SCHEDULING.md   building schedule profiles
  docs/CALIBRATION.md  setup and calibration walkthrough
  docs/BENCH.md        hardware test results
  docs/WEB_UI.md       the built-in web app, page by page
  docs/INSTALL.md      installing the prebuilt binaries, updates, recovery, the binary and flash matrix
  docs/BUILDING.md     building from YAML: packages, substitutions, flashing
  docs/USING.md        everyday use, settings, safety behaviour, troubleshooting, limits
  docs/SECURITY.md     the security model, for users
  docs/HTTPS.md        HTTPS, certificates, ACME, the trust store and MQTT over TLS, for users (2.4.3)
  docs/MECHANISMS.md   the checklist for adding a mechanism
  docs/plans/          release plans: milestones and the decisions behind them
  components/precision_feeder/      FeederCore (plain C++, host-tested): feeder_core.h,
                                    feeder_core.cpp (tray, guard, clock), feeder_schedule.cpp
                                    (scheduler, projection), feeder_text.cpp (editor, texts),
                                    feeder_password.cpp (settings password, BLE key and
                                    permissions), feeder_pins.cpp
                                    (runtime pins, ESP32-S3 pin validator), feeder_network.cpp
                                    (name, access-point password, Wi-Fi checks), crypto.h,
                                    feeder_internal.h, calendar.h, solar.h, tz_rules.h (POSIX TZ
                                    parser), firmware_upload.h (the checked upload); mechanism.h (the
                                    mechanism interface), mechanism_servo.* (the servo fan tray), mechanism_n20.*
                                    (the direct-drive disc's motion and calibration); precision_feeder.*
                                    (ESPHome adapter: clock, servo, flash, log, publish helpers)
  components/precision_feeder_n20/  the direct-drive disc on the device: DRV8833 through LEDC, the encoder through
                                    PCNT, the 1 ms control timer
  components/precision_feeder_mqtt/ MQTT's own topics on the device (their logic: feeder_mqtt.*, host-tested)
  components/precision_feeder_tls/  HTTPS relay, certificates and trust store, MQTT over TLS, ACME;
                                    tls_policy.h and acme.h are host-tested
  components/precision_feeder_ble/  optional BLE protocol: protocol.h and the
                                    BleEngine (engine.*: every handler), the GATT table
                                    (gatt_table.h) and the hand-off between tasks
                                    (gatt_link.h), all host-tested; precision_feeder_ble.*
                                    is the thin adapter on NimBLE (2.4.0)
  packages/            core (which includes mechanism_servo and tray_recovery), status, controls, schedule_editor,
                       calibration, mechanism_n20 (the disc, with drive_select), status_led, web_server, button,
                       hardware (with restart), time_homeassistant, diagnostics, network_info, ble_gatt,
                       api_key_on_device, mqtt, tls, mqtt_tls, acme
  examples/            basic.yaml, ble.yaml, disc.yaml, secrets.example.yaml
  firmware/            full.yaml, no-ble.yaml: the prebuilt binaries (built by tools/build_bins.sh)
  install/             index.html: the install page (ESP Web Tools), published with each release
  .github/workflows/   ci.yml (tools/check.sh), release.yml (binaries, GitHub release, Pages)
  web/                 the web app: src/ (plain JavaScript and CSS, no dependencies; mechanisms.js holds each
                       mechanism's dial, wording and cards), dist/ (built by
                       tools/build_web.py, committed, embedded by packages/web_server.yaml)
  tests/               host-side C++ tests (core, logic, protocol, BLE engine, n20: the disc against the bench
                       sketch on a simulated motor, motor_sim.h; reference/ holds the sketch); servo_golden.h (the servo's
                       2.0.2 behaviour, which test_core checks); test_consistency.py;
                       test_web.mjs (the web app's parsers, under Node); tz_golden.h (generated by
                       tools/gen_tz_golden.py)
  tools/               Python reference client: precision_feeder_ble.py (entry point) and
                       precision_feeder_client/ (protocol, client, selftest, cli); check.sh;
                       build_bins.sh and make_release.py (the prebuilt binaries and their manifests);
                       mock_device.py (a fake device for building the web app)
```

## 13. Decisions log

- Goal: recreate the original firmware with full parity, then improve it.
- Portion = one tray step; commands default to 1 portion; opt-in higher.
- Three profiles, editor-entity pattern, 4 events per profile.
- Solar anchoring (clock, sunrise, sunset, signed offset) is core.
- No burst/fry mode.
- MIT licence (owner, 2026-10-22; public domain under The Unlicense from 2026-10-14 to 2.1.1), AI disclosure in repo.
- Time: NTP is the truth; BLE `SET_TIME` is a fallback gated by
  `ble_time_policy`; HA time a second source (on in the examples since 2026-10-14); time never restored from flash.
- No external hardware (RTC, sensors). Optional push button supported.
- Features are YAML packages and build options, documented in comments.
- **No catch-up feeding (2026-10-13, replaces "configurable grace, default off"):**
  an event that is missed (device offline, hold, reload pause, or refused by the
  guard for its whole minute) is skipped, never made up later.
- Hold has no maximum duration, and can last until cancelled (needs no clock).
- If memory gets tight, turn on PSRAM; it may become a requirement of the
  firmware (2026-10-13). Not enabled until measured on hardware.
- OTA authenticates with the API key; no OTA password; no web-server OTA. (2.0.0 N4: a build with the API key
  provisioned at run time also sets a random OTA password at every boot, so plaintext OTA is refused; see below.
  2.0.0 N7 adds an upload in the web app, signed with the settings password and checked on the device, section 9.5;
  ESPHome's own web-server OTA stays off.)
- One-shot feeds are independent of the Scheduler switch (2026-10-14).
- While the clock is unknown after a reboot, pre-reboot feeds do not count toward
  the daily cap (2026-10-14).
- All feeder logic lives in `FeederCore` with no ESPHome dependencies, so it can be
  tested on a host (2026-10-14).
- Periodic reboots and an upcoming-events list are not carried over from the
  original.
- Clean-room code; PLC/bridge driver lives outside this repo.
- **The 24 h feed count is editable in Home Assistant (owner, 2026-10-14).**
  This reverses part of "nothing bypasses the guard": the guard still checks
  every feed, but the count it checks against can be set by hand, for
  debugging and recovery by the owner and future developers. Home Assistant
  only (diagnostic entity "Portions Counted (24 h)"), not BLE; every change is
  logged as a warning and raising it shows as an "adjustment" in the feed
  history. AGENTS.md is updated to match. **Moved in 2.2.0 (D25):** the web app
  only (Maintenance), behind the settings password; not Home Assistant, BLE or MQTT.
- Each event feeds at most once per day, also when it is moved later after it
  fed (2026-10-14; the safe side).
- Polar days (no sunrise or sunset): sun events fall back to 06:00 / 18:00 plus
  their offset, with a fault shown (owner, 2026-10-14; was "skip").
- The Home Assistant time package ships enabled in both examples (owner,
  2026-10-14), after it was seen setting the clock on hardware.
- BLE security: application-layer authentication with a shared key (protocol 2),
  plus LE Secure Connections pairing when the client pairs (owner, 2026-10-14;
  section 10).
- **Local first (owner, 2026-10-15; planned for 1.1.0, [docs/plans/1.1.0-local-first.md](docs/plans/1.1.0-local-first.md)):**
  the feeder must be fully usable with no Home Assistant, no internet and no app,
  from a browser on the same network. Planned, not built yet:
  - The stock ESPHome web UI (which loads its script from the internet) is replaced
    by an embedded app (`js_include`/`css_include`, no external runtime
    dependencies). Its "All entities" page is the debug fallback.
  - The existing entities are the app's API: entity names become a contract.
  - Tabs and sections appear only for the entities a build contains; no UI
    configuration file.
  - The app never repeats guard or scheduler logic.
  - Light and dark themes follow the browser. Phones are first-class (a web UI, no
    native app).
  - Web auth stays optional and off by default.
  - Read-only additions: Schedule Data, Device Time, Today, Feed Guard (a
    side-effect-free dry run of the guard), BLE Info and BLE Session, ESPHome's
    Wi-Fi info sensors.
  - A BLE activity ring buffer served behind web auth.
  - Dual-stack networking with full IPv6 readiness: everything local works on an
    IPv6-only network; only outside services (GitHub is IPv4-only) may fail, gracefully.
  - No BLE packet change (protocol 2).
- **Appliance (owner, 2026-10-15; planned for 2.0.0, [docs/plans/2.0.0-appliance.md](docs/plans/2.0.0-appliance.md)):**
  prebuilt GitHub binaries that are set up in a browser. Planned, not built yet:
  - Binaries: full and no-BLE, each a factory and an OTA image, for a generic
    ESP32-S3 with 4 MB flash. An 8 MB BLE variant only if BLE does not fit.
  - Runtime settings in a new saved "Device" record: GPIO pins (servo, button, LED),
    Wi-Fi, hostname, timezone (our own POSIX parser), API key (kept in flash through
    ESPHome's provisioning, `encryption: {}`), BLE key.
  - A setup wizard and a browser install page.
  - Update checks run in the browser, not the feeder. The browser reads a manifest
    from GitHub Pages; the user downloads the image and uploads it. A failed check
    only shows a link to the releases page. The feeder makes no update requests
    and has no HTTPS client. (Since 2.4.2, D40, it has one for ACME alone, talking only to the directory the owner
    sets; it still makes no update requests.)
  - Web auth is not required. A settings password (none by default) protects
    Wi-Fi, hostname, pins, keys, the password, firmware, factory reset, guard
    limits and calibration; never the schedule or feeding. Holding BOOT for 10 s
    after startup clears it.
  - The BLE and API keys are admin credentials, so BLE keeps its access, gated by
    two protected toggles (guard limits, calibration; both default on).
  - A blank BLE key refuses AUTH.
  - Refusals use NOT_AUTHORIZED detail values 6 and 7. This stays protocol 2:
    checked that 1.x clients do not break.
  - YAML builds stay as the advanced path; their values become defaults.
  - When built, this reverses "no web-server OTA" and "every pin is a
    substitution", and drops the build-time-only timezone (sections 6 and 13,
    AGENTS.md); each is updated in the change that builds it.
- **2.0.0 N1 (2026-10-15):**
  - The Device record holds the whole 2.0 layout now, with "use the build's value"
    markers, so YAML builds keep following their YAML (section 9.2).
  - The timezone is a POSIX string set at run time; one set on the device wins over
    Home Assistant's and the build's. Our own parser, stricter than ESPHome's
    reference and checked against it (section 6).
  - Owner, 2026-10-15: the API key is meant to be provisioned by Home Assistant on
    adoption. N4 keeps Generate and Clear as well (and a way to enter a key of your
    own, for one API key shared across devices): options do not hurt.
- **2.0.0 N2 (2026-10-15):**
  - The settings password guards web-server writes with a handler of ours in front
    of the web server's (section 10.1), not `api: actions:`, so Home Assistant keeps
    its ordinary entities.
  - Protected: the guard limits and the calibration, except Declare Tray Position.
    The timezone is not (it is not in the owner's protected list).
  - Each protected change is signed over a one-time challenge (PBKDF2 key,
    HMAC-SHA256); waits after failures double from 1 s to 60 s.
- **2.0.0 N3 (2026-10-15):**
  - Runtime pins (section 2.1). This replaces "every pin is a substitution" (AGENTS.md):
    the substitution is now the starting value and a pin saved on the device wins, as the
    servo pulse range already did.
  - Pins apply at the next restart, never by themselves (no automatic reboot). A Restart
    button, refused while the tray moves, is not a protected setting.
  - "None" for the button or LED keeps its driver from being set up at all, rather than
    marking it failed (which would log an error at every boot).
  - The board is worked out from the pins, not saved, so the Device layout is unchanged.
  - Preset pins for the ESP32-S3-DevKitC-1 (GPIO4, GPIO5, LED GPIO38) were chosen in N3,
    not by the owner; to confirm (section 14, item 14).
- **2.0.0 N4 (2026-10-15):**
  - BLE key: one saved on the device wins over the YAML's, which becomes optional. No key at
    all refuses every AUTH (detail 6). No Clear for the BLE key (as planned); a new one replaces it. (Clear added in
    2.3.0, D29.)
  - The two BLE permissions are switches in ble_gatt.yaml, protected by the settings password;
    refusals are NOT_AUTHORIZED detail 7. Still protocol 2; the Python client names 6 and 7 and
    shows any other value by number.
  - API key at run time is opt-in for YAML builds (packages/api_key_on_device.yaml), so existing
    YAML users keep their key; the prebuilt binaries include it. Generate, enter and
    clear in the web app, as decided in N1.
  - Keys are generated in the browser and shown once; the device never sends a key back. They
    travel masked by the settings password, like a new password key (section 10.1).
  - With a runtime API key, ESPHome's OTA can only offer encryption (section 14, item 10), so our
    component sets a random OTA password at each boot (`ota_id`): plaintext OTA is refused, and
    before a key exists OTA is not possible at all (USB, or the web upload of N7). ESPHome warns
    about this setup and suggests `provisioning:`; not used (it closes the fallback access point).
  - Key status is in `GET /feeder/auth` and BLE Info, not new entities.
- **2.0.0 N5 (2026-10-15):**
  - The name, the Wi-Fi network and the access-point password are web-app settings (endpoints of the settings
    guard), not entities, so the REST interface cannot change them unsigned; their status is in
    `GET /feeder/network` (section 9.3).
  - The name and the access-point password apply at the next restart, as pins do (no automatic reboot); a Wi-Fi
    network is joined at once, as the captive portal does, so the access point remains the way back if it is wrong.
  - A saved name replaces ESPHome's through its protected member, before anything reads it, rather than calling
    `App.pre_setup()` again (that re-runs `arch_init()`: a second watchdog subscription of the loop task, and in
    a MAC-suffix build it writes the MAC into the end of the name it is given).
  - The Wi-Fi network stays in ESPHome's own slot, not the Device record, so the captive portal, Improv and the web
    app share it. No "forget" until factory reset.
  - The access point's SSID is made at run time from the name in use (`<name>-setup`) instead of the YAML's
    `${name}-setup`, so a MAC suffix or a saved name changes it too.
  - `name_add_mac_suffix` is a substitution, off for YAML builds (an existing feeder keeps its name); the prebuilt
    binaries turn it on.
  - Improv serial is on in both examples and in the binaries; BLE Improv is not used.
- **2.0.0 N6 (2026-10-16):**
  - The setup wizard is a page of the web app (`#setup`); the firmware only keeps whether it is finished
    (`DEVICE_SETUP_DONE`, section 9.4). It never blocks feeding or any page, and every step can be skipped.
  - A feeder updated from 1.x counts as set up, so it is never sent through the wizard.
  - The time zone is chosen by place in the app, from a table of the POSIX strings ESPHome itself would compile
    (generated, not derived in the browser), as N1 planned. The device still only takes POSIX strings.
  - Location is typed in: the browser's geolocation needs HTTPS, which the feeder does not serve.
  - Wi-Fi in the wizard is "join another network" (section 9.3): to reach the wizard the feeder is already on a
    network (set by the captive portal or Improv).
- **2.0.2 (2026-10-16):** NTP servers set at run time (section 6), treated like the timezone: not protected by the
  settings password (owner). Up to three, in Device record version 2 (version 1 migrated). Three servers asked for real
  (`CONFIG_LWIP_SNTP_MAX_SERVERS`).
- **2.0.0 N7 (2026-10-16):**
  - 2.0.0 merged to main (N1 to N6); N7 onward is built on main, as it needs GitHub Actions and Pages.
  - **The prebuilt binaries' access point is open on a clean flash** (owner), reversing the plan's "documented
    default password". The wizard strongly recommends setting one. Anyone in range can join the open access point
    while it is up (no Wi-Fi yet, or Wi-Fi lost) and reach the captive portal and the web app, as on any feeder
    with no settings password.
  - Releases: a `v*` tag builds and publishes; a manual run only builds (owner). Pages is deployed by the workflow
    (source "GitHub Actions", no gh-pages branch); it holds the latest release only.
  - **Reverses "no web-server OTA"** (section 13, 1.x): the web app uploads firmware through a handler of ours
    behind the settings password and checks the SHA-256 on the device; ESPHome's own `/update` stays off (refused
    in every build since 2.4.5, D43: the captive portal had kept it open while the access point was up).
  - The upload is signed over the image's SHA-256, so a challenge answers for one image only; the device checks the
    bytes against it before switching.
  - The update check is the browser's (as planned); the manifest has only the latest release.
  - The web app's size budget is 64 KB gzipped (was about 60 KB in the 1.1.0 plan): the Firmware page put it
    100 bytes over, and the binaries have about 380 KB of flash to spare.
  - The bins' BLE name is "Feeder" (with the MAC suffix, "Feeder-a1b2c3"): ESPHome refuses more than 13 characters
    before the suffix.

- **Mechanisms (firmware 2.1.0; owner, 2026-10-20; [docs/plans/drive-decoupling.md](docs/plans/drive-decoupling.md)):**
  - D1: a Mechanism interface of verbs (section 3.0); one mechanism per firmware, chosen in the YAML (a dual build
    may be tried later). Superseded by D17: two drives in one firmware, chosen on the device. The servo moved behind it with its behaviour unchanged: a golden trace recorded from 2.0.2
    (tests/servo_golden.h) must still match exactly.
  - D1a: the saved position means portions fed since reload, so remaining = capacity − position; no State change.
  - D1b: the guard limits go up to the mechanism's capacity instead of 6.
  - D3: no existing YAML, saved setting or servo behaviour breaks (the owner runs a servo feeder).
  - D4: a non-servo mechanism's motor pins are YAML-only; no Device record change. Reverses, for those pins only,
    "a pin saved on the device wins" (AGENTS.md). Replaced by D19.
  - D5: BLE stays protocol 2 while no packet layout or opcode changes: Info's positions and its calibration and
    pulse-range capability bits follow the mechanism; Config keys 10 and 11 answer UNSUPPORTED without the servo.
  - D6: GPIO3 accepted with a warning (section 2.1), after checking Espressif's documentation.
  - D14: AGENTS.md allows further mechanisms under its "Adding a mechanism" rules; a motor's own encoder is part of
    the motor, a separate sensor still needs its own decision.
  - D15: one web app for every build, with a description per mechanism.
  - Built differently from the plan: the tuning values got their own record instead of a block in a Calibration
    version 2, so no existing record changes layout; the web app's descriptions are one file, `mechanisms.js`, not a
    folder, to stay within the 64 KB budget.

- **The direct-drive disc, both drives and MQTT (firmware 2.1.0; owner, 2026-10-21):**
  - D17: one firmware carries both drives, chosen on the device (section 3.4); the single binary is the way forward,
    with selectable pins. The prebuilt binaries default to the servo (owner: "it can default to servo"), so no
    existing feeder changes.
  - D18: the disc does not rewind (dropped for this version; plan stage 2).
  - D19: the disc's five motor pins are chosen on the device (MotorPins record; section 3.3). Replaces D4.
  - The disc moves from its branch to main and joins CI and the release (AGENTS.md, "Adding a mechanism", rule 8).
  - D20: MQTT as an optional package (section 9.6), configured on the device, publishing what Home Assistant sees
    (the owner: "expose the same to MQTT as we do for Home Assistant"). ESPHome stays (MQTT was first tried as an
    experiment, with the API swapped out: the API costs only about 28 KB, so it stays too).
  - The web app's size budget is 66 KB gzipped (was 64 KB): the disc's calibration cards and the MQTT card. The
    binaries still have about 210 KB of flash to spare.
  - Calibration Mode and position recovery moved to `packages/tray_recovery.yaml` in `core.yaml`, shared by both
    drives (ESPHome does not merge a package included twice).
  - A build's own button or LED pin that the drive in use drives is turned off rather than refused, so the servo's
    button default (GPIO2) cannot stop a disc feeder (section 2.1).

- **Scopes (firmware 2.2.0; owner, 2026-10-22; [docs/plans/2.2.0-scopes.md](docs/plans/2.2.0-scopes.md)):**
  - D21: what the feeder offers is of three kinds: operate (the feeder: feeding, the schedule, status), safety policy
    (the feed guard's limits and the 24 h count) and components (calibration, tuning, drive, pins, keys, network,
    firmware). Each channel's reach follows how well it knows who is talking (the plan's channel table).
  - D22: Home Assistant and MQTT's discovery see only the slim set (section 7); every other entity is internal, web
    app only. Checked in every build by tools/check_scopes.py. No build option for the full set.
  - D23: schedule editing is not for Home Assistant (owner: "If you want that functionality, re-adopt under
    MQTT"); MQTT edits it through its own plain topics.
  - D24: MQTT may change the guard limits only with MQTT Can Change Guard Limits on (a Device flag, off by default;
    web app only, protected), through its own plain topics, one value each.
  - D25: the 24 h count's hand edit is web app only (behind the settings password); it left Home Assistant. Closes
    open item 25.
  - D26: components are never on MQTT. Closes open item 24.
  - D27: BLE keeps calibration behind its switch (headless recalibration); pins, drive, keys and network are web app
    or YAML only. A headless build leaves the web app out and runs over BLE.
- **Loose ends (firmware 2.3.0; owner, 2026-10-22; [docs/plans/2.3.0-loose-ends.md](docs/plans/2.3.0-loose-ends.md)):**
  - D28: every protected setting is read-only over MQTT, the guard's limits included ("MQTT is somewhere in between
    basic and very advanced"). **Reverses D24**: MQTT Can Change Guard Limits is gone; the limits stay published.
  - D29: the BLE key saved on the device can be cleared in the web app (Configuration, Keys; `/feeder/keys/ble` with no
    key, signed), as the Home Assistant key can. The build's key is used again, or with none every AUTH is refused
    (detail 6; never an all-zero key); a connected session ends (`clear_ble_key()`, the key serial changes).
  - D30: the Wi-Fi network can be forgotten in the web app (Configuration, Network), only in a build with no network
    in its YAML (the prebuilt binaries): "a YAML build can't forget its network, that's fine". It restarts into the
    setup access point (section 9.3).
  - D31: the Join dialog lists the networks the feeder sees, from a scan while connected (ESP-IDF's background scan,
    which keeps the link, as ESPHome's roaming does) or the access point's own scan; a typed name stays possible
    (section 9.3).
  - D32: open item 23's calibration is always checked by eye, also for a printed disc (owner, 2026-10-22); open
    item 22 (the servo bench leftovers) is shelved until the servo rig is rebuilt.

- **Codex review of 2.3.0 (firmware 2.3.1, 2026-10-23; owner asked for the fixes):** each finding reproduced in a
  host test before it was fixed; no new decision, they make existing rules hold.
  - Calibration that can move the tray needs "unknown" on flash, not only in memory: the core tracks whether the
    last save that reached flash said unknown (`unknown_durable_`), and saves again (or refuses, `STORAGE_FAILED`)
    when it did not. A failed reload had left a trusted position on flash under an unknown one in memory (section 3.1).
  - Event times and one-shots are parsed whole, digit counts bounded before any arithmetic: a suffix (`08:00 PM`) or
    an oversized hour is refused, not read as another time (section 7).
  - The Join dialog keeps the user's choice across a scan refresh ("Other network..." included), and keeps a picked
    network listed when a refresh misses it (section 9.3). The web app's budget is 67 KB gzipped (2.3.0 left 127
    bytes).

- **NimBLE, TLS and certificates (firmware 2.4.0 onwards; owner, 2026-10-23; [docs/plans/nimble.md](docs/plans/nimble.md),
  [docs/plans/https-tls.md](docs/plans/https-tls.md)):** the order is NimBLE (2.4.0), then all of TLS at once
  (HTTPS, MQTT over TLS, the trust store), then ACME, then the web app's configuration page, then the docs, each its
  own revision of 2.4 (2.4.1 TLS, 2.4.2 ACME, 2.4.3 the configuration page; owner: 2.4.x, not 2.5, 2.6...). The docs
  revision planned as 2.4.4 stays in 2.4.3 (owner, 2026-10-23: a docs-only change needs no firmware version); then testing and validation, and a comparison against the earlier audit reports before any GitHub
  release. The intermediate versions are not tagged.
  - D33: BLE runs on ESP-IDF's NimBLE instead of Bluedroid, trimmed to one link (section 8). `packages/ble_gatt.yaml`
    keeps its name, substitutions and entities, so a YAML build that includes it changes nothing ("non-breaking");
    there is no overlap release with a Bluedroid package (the plan's B1 is replaced by this). The protocol stays at
    version 2.
  - D34: the GATT table is C++ (`gatt_table.h`), checked against the Python client's UUIDs and the protocol's
    channels; it replaces the YAML table (section 8 said YAML).
  - D35: characteristic permissions stay plain (no "encryption required"); pairing stays optional, LE Secure
    Connections with bonding, at most 3 bonds in NVS, the oldest replaced. Old Bluedroid bonds are not carried over.
    BLE's clients are a PLC bridge and scripts, not phones: bench tests use the Python client; phone pairing is not
    tested (owner). "Forget BLE bonds" came with the configuration page (2.4.3, D41).
  - D36: MQTT's "insecure" mode pins the broker's certificate on first contact instead of skipping verification
    (owner; E5 of the TLS plan). Built in 2.4.1 (section 9.7).
  - D37: HTTPS is a relay from port 443 to ESPHome's web server on 127.0.0.1:80, not a second web server, so every
    handler and the settings guard work unchanged; TLS 1.2 only (E1 of the plan).
  - D38: Home Assistant's link to the device is always `http://<address>:80` (Home Assistant builds it), so the
    feeder cannot report an https URL; port 80 redirects the app's page to `https://<name>.local/` instead (HTTPS
    Redirect, on by default). HTTPS Only (off by default) refuses the rest of port 80, but never the relay, the access
    point's captive portal, ACME's challenge, or anything while HTTPS is not running.
  - D39: with MQTT TLS on, MQTT never falls back to plain text: no CA (or no pin yet) means the client does not start.
  - D40: ACME (2.4.2, owner's order): an HTTPS client on the feeder, which reverses part of the 2.0.0 appliance
    decision ("no HTTPS client"; E7 of the TLS plan). It talks only to the ACME directory the owner sets, checked
    against the trust store; no public CA bundle (E4: out, since MQTT and ACME both use the trust store); http-01
    only, no dns-01 (E6: it would keep DNS credentials on the feeder). ACME's certificate wins over an uploaded one
    while ACME is on, and is served without a restart.
  - D41: the configuration page (2.4.3). HTTPS, the trust store and ACME get their own card in Configuration; MQTT's
    TLS settings join the MQTT card. "Forget BLE bonds" is in Configuration, Keys, beside the Bluetooth key, not in
    Maintenance as the NimBLE plan's B4 had it: it is about who may connect, like the key. It is a protected button
    entity (`Forget BLE Bonds`), not a new endpoint. The web app's size budget goes to 71 KB gzipped (from 67 KB):
    these cards are about 4 KB, and the full image has about 280 KB of flash to spare.
  - D42: the bench run of 2.4.3 (2026-10-23, the disc rig; owner: "do the fixes", as 2.4.4). The NimBLE controller
    keeps its central role (BLE_MASTER), which the trim had removed: without it the feeder never advertised (about
    11 KB of flash and 5 KB of heap back). HTTPS allows 3 sessions at once instead of 4 (E8 and D37 said 4 with
    NimBLE trimmed): measured on the real build, 3 held open left 50 KB of heap at the lowest and ten requests at once
    25 KB; 4 went down to 31 KB. The relay's socket leak, its connection to port 80 without a time limit and its
    tasks on either core were bugs, fixed. An ACME certificate survives a change of directory (above).
  - D43: the audits against 2.4.4 (2026-10-24; owner: "fix what needs fixing", as 2.4.5). Only what broke a rule the
    project already had; the logged decisions they questioned stand (firmware upload with no settings password, the
    API key on first use, NTP and the time zone unprotected, HTTPS Only off by default, D36, no flash encryption).
    - BLE (2.4.3 review R1): a full queue of writes could drop a disconnect or connect, so a new client inherited the
      old session's readable values. A link event now displaces a read, then a write, then an MTU (a dropped write
      is acknowledged by the radio but never acted on: a missed command, never a doubled one); a disconnect also
      closes `ValueStore` on NimBLE's task, so nothing protected is readable until the main loop has seen it.
    - TLS identities (R2) are saved as one NVS blob each (`ss_id`, `up_id`, `ac_id`: length-prefixed fields,
      `pack_fields` in tls_policy.h), so a failed write never pairs a new certificate with an old key. NVS replaces a
      blob whole (it writes the new one before erasing the old, checked in ESP-IDF 5.5.6). 2.4.1 to 2.4.4's separate
      keys are read once and erased after the first save; nothing to do on upgrade.
    - Turning MQTT off cancels a pin on first contact still under way (R3); the pin, once kept, stays.
    - HTTPS (R4): after the handshake the client socket has a receive timeout too (the idle timeout), so a client
      that sends part of a record cannot hold a session for good.
    - ESPHome's `/update` (security review S F2) is refused in every build, ahead of ESPHome's handler (section 9.5):
      it enforces section 9.5's rule that browser uploads go only through `/feeder/firmware`.
    - The MQTT password travels masked by the settings password, like the access point's, through
      `POST /feeder/mqtt/password`; the plain `?value=` write is refused (403 `masked`).

- **Codex reviews of 2.1.1 (firmware 2.1.2, 2026-10-22; owner asked for the fixes):** each finding reproduced in a
  host test before it was fixed.
  - The feed history holds 64 feeds (the largest cap), not 8: State version 3, migrated (section 4).
  - Reload, declare and the end of calibration save their move as an intent and count as moving until settled
    (section 3.1). The servo's golden trace was recorded again for it: every servo command and release, with its time,
    is unchanged; only those saves differ.
  - Safety-critical saves report failure, and a failure stops the move: result `STORAGE_FAILED` (17), a new code, not a
    new layout or opcode, so BLE stays protocol 2 (section 3.1, PROTOCOL.md).
  - Calibration that can move the tray saves the position as unknown first; Stop stops a calibration move; a restart
    waits for one (section 3.1).
  - An event's fed day only moves forward and is saved before it feeds; a one-shot is saved as done before it feeds
    (section 5.3).
  - The disc bounds powered motion without encoder feedback in every phase (section 3.3).
  - BLE: event portions follow the capacity; a failed AUTH clears what the session published (section 10).
  - `check.sh --compile` and `build_bins.sh` fail on a failed build (the exit status was masked).
  - ESPHome 2026.10.0 (owner): the release builds and CI use it; the facts our code relies on were checked again in its
    source (the app name's `StringRef name_`, the web server's status codes; its task stack is now 4,512 bytes).

## 14. Open items

1. ~~Polar/no-sun fallback.~~ Decided 2026-10-14: fixed 06:00 / 18:00 fallback (section 5.2).
2. ~~Confirm the slot/position mapping on the printed tray.~~ Done on the
   bench (docs/BENCH.md). Still to watch: whether the servo strains at
   position 6 in daily use.
3. ~~Servo (GPIO1) and button (GPIO2) pins.~~ Done on the bench (docs/BENCH.md).
4. ~~ESPHome BLE server, pairing, two time sources, packages, flash size.~~
   Checked in ESPHome's source and on the bench (section 10, docs/BENCH.md).
5. ~~Check the Printables model licence.~~ Checked 2026-10-14: CC BY-NC-SA 4.0
   (attribution, non-commercial, share-alike). The repository ships none of the
   model's files, credits the author, and the README states the model's licence;
   `.gitignore` keeps 3MF/STL files out.
6. ~~HA-time package in v1.~~ Decided 2026-10-14: enabled in both examples.
7. Checks for 1.1.0 (milestone M0 in [docs/plans/1.1.0-local-first.md](docs/plans/1.1.0-local-first.md)).
   Answered 2026-10-15 from the ESPHome 2026.9.1 source, and by validating a test
   configuration:
   - **`js_include` path:** `cv.file_` tries the config folder, then the folder of
     the YAML file that declares it. So `packages/web_server.yaml` can name
     `../web/dist/app.js`, whatever the clone is called.
   - **`disabled_by_default` entities:** sent on `/events` like any other, with
     `"is_disabled_by_default": true`. Only `internal` entities are left out (unless
     `include_internal`).
   - **Auth:** `add_handler()` wraps a handler in the auth middleware when credentials
     are set (they are set before any component's `setup()`). The ESP-IDF server
     implements both `basic` and `digest`, one per build. Still to see on hardware:
     the browser re-sending credentials for `/events` and the app's requests.
   - **Custom handlers run on the HTTP server's task, not the main loop,** so any
     state they read (M2's ring buffer) needs a lock.
   - **IPv6:** `network: enable_ipv6: true` (default false on ESP32) and
     `min_ipv6_addr_count`. **Finding:** ESPHome's Wi-Fi only counts as connected
     once it has an IPv4 address (`wifi_sta_connect_status_()`). On a network with
     no DHCPv4 it never connects, and falls back to its access point. A static IPv4
     address (`manual_ip`) may work around this; to check on hardware. See the plan,
     M8.
   - **BLE adapter:**
     - A component can register a GATT server event handler
       (`esp32_ble.register_gatts_event_handler`). Events arrive on the main loop, so
       the peer address (`ESP_GATTS_CONNECT_EVT`) and MTU (`ESP_GATTS_MTU_EVT`) are
       available.
     - `BLECharacteristic::on_read(conn_id)` exists, so reads can be counted.
     - Advertising restarts after a disconnect (`advertising_refresh()`).
8. Profile names: section 5.1 lists a per-profile `name`, but the `Profile` record
   has none, so profiles show as A, B and C. Adding one changes the saved Settings
   layout (new version, key and migration); deferred (2026-10-15).
9. The schedule editor (Edit Profile / Edit Event plus field entities) is one cursor shared by every client.
   If Home Assistant moves it while the web app is saving, a field write can land on another event. The app
   re-points the editor before every write and, after a save (or a failed one), compares every other event with
   a snapshot and names any that changed (found and handled in 1.1.0 M9; reproduced only by moving the cursor
   continuously). A complete fix needs a firmware change, for example a write-only text entity taking a whole
   event at once ("A 2 1c405/2/7e"). Not planned yet (2026-10-15).
10. Checks for 2.0.0 (milestone N0 in [docs/plans/2.0.0-appliance.md](docs/plans/2.0.0-appliance.md)).
    Answered 2026-10-15 from the ESPHome 2026.9.1 source, by validating and compiling a
    2.0-shaped test configuration, and by querying GitHub Pages. Two items wait for the bench.
    - **API key provisioning.** `api: encryption: {}` boots with no key so that Home Assistant
      can provision one on adoption (the owner's intent, 2026-10-15): until a key is saved the
      device accepts Noise with the well-known all-zero key (the key travels encrypted; this path
      stays after 2027.2.0) and, deprecated, plaintext. Home Assistant sends
      `NoiseEncryptionSetKeyRequest`, the key is saved in its own preference slot (NVS) and
      plaintext stops. `save_noise_psk()` and `clear_noise_psk()` are public, so our component
      can also set or clear the key. Until the first key arrives, whoever reaches the device
      first can provision it (trust on first use); documented in the security model.
    - **OTA with the provisioned key.** Works: `ota: esphome` uses the API server's live key
      (or the saved key in safe mode). **Finding:** with a runtime key, OTA can only *offer*
      encryption; `ota: encryption:` (required) fails validation, so plaintext OTA is accepted.
      Fix: `ota: password: ""` compiles the password path, and our component sets a random
      password at runtime with `set_auth_password()`. Noise clients skip the password (the key
      authenticates them); plaintext clients then need a password nobody knows. Validated.
    - **ESPHome's browser upload (`ota: web_server`)** is guarded only by web auth, which 2.0
      does not use. N7 adds its own upload handler on ESPHome's OTA backend
      (`ota::make_ota_backend()`), behind the settings password, instead.
    - **Captive portal:** active only while the access point is up. It starts with the access
      point (first boot with no network, or the fallback after `ap_timeout`), ends when Wi-Fi
      connects, and only answers GET requests while active.
    - **BLE at runtime:** yes. `esp32_ble: enable_on_boot: false`, then `enable()` from our
      component; the GATT server registers its services once BLE is active and does not hold
      up boot while it is off. Validated.
    - **Excluding one entity from the web server:** not possible. The per-entity `web_server:`
      block only sorts; only `internal: true` hides an entity, and that hides it from Home
      Assistant too. Two ways remain: the plan's fallback (read-only entities, changed through
      `api: actions:`), or a handler of ours registered before `web_server`'s (handlers are
      matched first come, first served) that answers POSTs to protected entities. N2 picks one.
    - **Improv serial:** validates with the default logger on the ESP32-S3 (USB Serial/JTAG;
      only `USB_CDC` is refused). Still to try on the bench with ESP Web Tools.
    - **Runtime servo, LED and button pins:** feasible. LEDC, the RMT LED strip and the GPIO
      binary sensor read their pin in `setup()` at HARDWARE priority; ESP32 pin objects have a
      public `set_pin()`, and a pin can take a YAML `id` (validated). Our component, set up
      earlier, can load the Device record and rewrite the pins first. "None" for the button and
      LED needs its own handling in N3 (the drivers always claim their pin).
    - **GPIO0 (BOOT) after startup:** reads cleanly on the S3-Zero (2.0.0 N8, item 13).
    - **GitHub Pages:** sends `Access-Control-Allow-Origin: *`, so the browser can read
      `manifest.json`. Pages is also reachable over IPv6 (github.com is not), so the update
      check works on IPv6-only networks; only downloading the image needs IPv4. Release
      downloads from github.com send no CORS header, which is fine: the user downloads them.
    - **Flash budget:** measured by compiling (ESP-IDF, 1.8 MB app slot of the 4 MB layout).
      The 2.0-shaped test build (both 1.1.0 examples plus the button, runtime API key, OTA
      password path, browser upload, Improv serial, BLE off at boot) uses **76.3% with BLE**
      (1,400,991 bytes) and **58.3% without** (1,070,315 bytes); the 1.1.0 examples use 75.7%
      and 57.5%. So those ESPHome pieces cost only 12 to 15 KB, and about 430 KB is left in the
      BLE build for 2.0's own code (settings, TZ parser, password, upload handler, wizard). Both
      variants fit 4 MB; no 8 MB variant is needed on current numbers. Measured again in N7: 78.9% and 60.9%
      (section 9.5).
11. Roadmap after 2.0.0 (owner, 2026-10-15), not planned in detail yet:
    - 2.1.x: more 3D-printed feeder models driven by a servo (other trays and geometries).
    - 2.2.x (maybe; needs hardware to test): auger-style feeders, which portion by motor time or steps, not tray
      slots, and may need a continuous-rotation servo or a stepper. That needs explicit decisions against the
      "no external hardware" rule (section 11, AGENTS.md) and the 6-slot magazine model (section 3).
12. Cross-site requests to unprotected controls (found in 2.0.0 N2). ESPHome's web server sends
    `Access-Control-Allow-Origin: *`, and a plain POST needs no preflight, so a web page the user
    visits could press, for example, Feed One Portion on the feeder. The feed guard still applies to
    every feed, and protected settings need the settings password, so this is limited, but not
    nothing. A fix (for example requiring a header the browser cannot send cross-site on every
    write) would touch every web-server write; not planned yet (2026-10-15).
13. BOOT button on the bench: N2 reads GPIO0 after startup for the password reset. Checked on the Waveshare
    ESP32-S3-Zero 2026-10-16 (docs/BENCH.md, N8 part 6): held 10 s, it cleared the password and nothing else, and the
    feeder kept running. Still to see on a DevKitC-1.
14. Runtime pins on the bench (2.0.0 N3): compiled and host-tested only. Still to see on
    hardware: a saved servo pin moving the servo to another GPIO after a restart; Button Pin
    and LED Pin "none" leaving the drivers unset with no error in the log; the LED and button
    on new pins; and the ESP32-S3-DevKitC-1 preset (GPIO4, GPIO5, LED GPIO38) on a real
    DevKitC-1, chosen without the owner (decisions log, N3).
15. Keys on the bench (2.0.0 N4): partly checked 2026-10-16 (docs/BENCH.md): an API key made in the web app used by
    an API client (plaintext and a wrong key refused), Clear, plaintext OTA refused, a BLE key made in the web app
    (selftest 14/14), and detail 7 with the guard permission off. Checked in N8 (2026-10-16): adoption as Home
    Assistant does it (aioesphomeapi: the all-zero Noise key, then a key set over the API; afterwards plaintext, the
    all-zero key and a wrong key refused), encrypted OTA with that runtime key, and detail 6 with no BLE key. Still to
    see: Home Assistant itself adopting and reconnecting, and a new BLE key ending a connected session.

16. Network on the bench (2.0.0 N5): checked 2026-10-16 (docs/BENCH.md): the MAC-suffixed name, a saved name for
    mDNS and the API after a restart, the access-point password saved, joining another network from the web app,
    an unreachable network taking the feeder off the LAN, and Improv serial recovering it. Still to see: the access
    point's SSID on air (macOS hides scanned SSIDs), the DHCP host name in a router, whether Home Assistant
    reconnects after a rename or needs the feeder added again, and Improv from ESP Web Tools itself. Checked in N8
    (2026-10-16): the open access point's SSID on a phone, the DHCP host name (`precision-feeder-add020` in a GL.iNet
    router's client list), and ESP Web Tools' install. Still to see: Home Assistant after a rename.

17. ~~Setup wizard on the bench (2.0.0 N6).~~ Done 2026-10-16 (docs/BENCH.md): a freshly erased feeder opens the
    wizard and keeps it across a restart (after a fix: decided at the first boot), and setup done persists.
18. One settings-password challenge is shared by every client (section 10.1). Rare in practice (two people saving
    protected settings within the same second), and the app recovers on the next try. A per-client challenge would
    need a small table of outstanding nonces; not planned yet (2026-10-16).

19. Prebuilt binaries on the bench (2.0.0 N7): built and compiled only (section 9.5). Still to see: a first flash
    from the install page (ESP Web Tools, then Improv), the open access point and its captive portal, an upload on
    the Firmware page (and a refused one: wrong password, a changed file, a cut connection), the restart waiting
    for a moving tray, and the update check against a published manifest (the first release publishes it). N8.
    Checked 2026-10-16 (docs/BENCH.md, N8 parts 1 to 6, and the release) except a feeder whose own internet is
    blocked.

20. IPv6-only networks and the prebuilt binaries (found in 2.0.0 N8). ESPHome 2026.9.1 counts Wi-Fi as connected only
    with an IPv4 address (item 7), and the YAML workaround, a static IPv4 address (`manual_ip`), is build-time. So a
    prebuilt binary on an IPv6-only network never joins and falls back to its access point; a YAML build with
    `manual_ip` still works. Fixing it would need a static IPv4 address set at run time (a Device record field and a
    call to ESPHome's Wi-Fi before it connects). Documented; not planned yet (2026-10-16).

21. Factory reset: listed among the protected settings (owner, 2026-10-15) but not built in 2.0. Wiping a feeder is
    an erase-and-flash from the install page (docs/INSTALL.md, Recovery). To decide: a protected "factory reset" in
    the web app (erase our records and ESPHome's Wi-Fi slot), or keep erase-and-flash. Since 2.3.0 (D30) the Wi-Fi
    network alone can be forgotten in the web app (prebuilt binaries); a full wipe is still erase-and-flash.
22. 2.1.0 on the bench (servo). Checked 2026-10-21 (docs/BENCH.md): the upgrade from 2.0.2 kept settings,
    calibration, pins and the tray state; a feed, reload, calibration and the guard behaved as before; the
    two-drive firmware fed as a servo. Still to check: a multi-portion feed, a power cut mid-feed, the BLE selftest on
    the servo, the release build itself (the two-drive firmware with MQTT, as published), and GPIO3 as the button pin
    with the button held at power-up. **Shelved 2026-10-22 (D32):** the servo rig fell apart; picked up again when it
    is rebuilt.

23. The direct-drive disc on the bench (section 3.3, plan S2.0 and S2.8). Checked 2026-10-21 (docs/BENCH.md): one
    turn is exactly 8344 ticks; feeds land within 2 ticks of the grid; the control timer's only long gap (about
    52 ms) is at boot; feeds from the web app, BLE and MQTT; switching drives both ways. Still to check: the guided
    calibration end to end with a slot lined up by eye; 300 feed cycles; the wiggle with real food; a power cut
    mid-move; a held disc and an unplugged encoder; the STBY level at power-up (and whether a pull-down is needed);
    which way the disc turns, by eye. And the feed time: about 3.9 s on the first run against the sketch's 2.5 s,
    2.3 s on a later feed; not explained (not seen since: 2.2.0, and 2.3.0's first feed after a restart took 2.29 s). **Decided 2026-10-22 (D32):** the guided calibration, checked by eye, is the
    method (also for a printed disc), not a stand-in for a mark.

24. MQTT and the protected settings (section 9.6). Over MQTT, whoever the broker let publish could change the guard
    limits, the calibration and the pins, which the web app guards with the settings password. **Decided 2026-10-22
    (firmware 2.2.0, D22 to D26):** MQTT sees only the slim set; its own topics edit the schedule, and the guard's
    limits only with MQTT Can Change Guard Limits on; components never. **Since 2.3.0 (D28):** the guard's limits are
    read-only over MQTT too; the switch is gone.

25. Editing the 24 h count (Portions Counted (24 h)). AGENTS.md allowed it by hand in Home Assistant only (the owner's
    exception, 2026-10-14), but the web app's Maintenance page also offered it, behind the settings password (found by
    a Codex review of 2.1.1). **Decided 2026-10-22 (D25, firmware 2.2.0):** web app only, behind the settings password;
    no longer in Home Assistant.

26. ESPHome's Wi-Fi internals the 2.3.0 plan relies on (docs/plans/2.3.0-loose-ends.md, M0; **checked 2026-10-22
    against ESPHome 2026.10.0's source**): a build with no network in its YAML keeps the one set on the device in NVS
    namespace `esphome`, key `"88491487"` (a fixed hash), and only erasing that key forgets it (an empty record is
    loaded as a network with no name); with none, the setup access point, captive portal and Improv come up as after
    a fresh flash (forgetting on the rig not yet checked: docs/BENCH.md). ESPHome's own `start_scanning()` drops a live connection; an ESP-IDF scan with
    `coex_background_scan`, as ESPHome's post-connect roaming uses, keeps it, and ESPHome collects its results
    (full ones only with `request_wifi_scan_results()`; read across tasks under `ScanResultsLock`). Recheck on every
    ESPHome upgrade.

27. HTTPS and MQTT over TLS on hardware (section 9.7, firmware 2.4.1). Checked on the disc rig with 2.4.3 and 2.4.4
    (docs/BENCH.md, "Firmware 2.4.3 and 2.4.4 on the disc rig"): the upgrade from 2.3.0, the self-signed certificate,
    sessions and heap (3 open, 4 asked for, ten requests at once, 200 handshakes, resumption), the signed firmware
    upload over HTTPS, the redirect (Home Assistant's "Visit" link, by the owner), HTTPS Only, uploaded EC and RSA
    chains from a private CA with and without a settings password and their refusals, the CA bundle, MQTT over TLS
    against a CA, pinned on first contact, a changed broker certificate, a leaf-only broker and a plain port, and ACME
    against the owner's step-ca (first certificate, Renew now with a page open, a dead or untrusted directory). BLE on
    NimBLE with 2.4.4 (2026-10-24): the selftest, 100 connect cycles, a retry storm (a feed's exact bytes 50 times fed
    one portion) and the heap with a client connected. Still open: Forget BLE Bonds with a client that bonds (macOS
    does not, as no attribute asks for encryption); HTTPS Only while the
    access point is up; an ACME certificate expiring by itself and the 16 h renewal staying quiet (the soak, with MQTT
    over TLS and BLE together). 2.4.5 on the disc rig (2026-10-24): BLE after 200 commands and a stalled TLS record cut off at 20 s passed.
    `/update` rejected a bogus upload on both HTTP and HTTPS, but returned 500 rather than the intended 403
    (the ESPHome response wrapper maps this status; the refusal body was correct). Correct that status and still
    verify refusal while the access point is up. Other audit checks remain: the masked MQTT password and the
    upgrade of 2.4.4's saved certificates.

## 15. Implementation status (v2.4.6)

The current source implements the behavior in sections 3 to 10, subject to the limits and open items in section 14.
The current source passed host checks, both prebuilt builds and disc-rig BLE, motion, guard and
HTTPS smoke tests (docs/BENCH.md). This does not close the broader audit and endurance checks in section 14.

| Area | Current state |
|---|---|
| Feeding and schedules | Shared guard, persist-before-motion, profiles, solar anchors, one-shots, hold and missed-event skipping |
| Mechanisms | Servo fan tray and direct-drive disc, chosen on the device; only the selected driver starts |
| Configuration | Built-in web app, setup wizard, runtime pins/network/time/keys, settings password and checked firmware upload |
| Integrations | Home Assistant's everyday set; MQTT with schedule topics and read-only guard limits; authenticated BLE on NimBLE |
| Transport security | HTTPS with self-signed/uploaded/ACME certificates; CA trust store and MQTT TLS with CA verification or pinning |
| Current audit fixes | ESPHome `/update` blocked, masked MQTT password, BLE disconnect/read closure and reliable link events, atomic TLS identity records, MQTT disable during pinning, HTTPS receive timeout |
| Deliberate omissions | No web factory reset, profile names, external RTC/sensors, food-drop detection or disc per-slot offsets |

Recorded prebuilt measurements with ESPHome 2026.10.0, using the 1.8 MB app slot on a 4 MB board:

| Image | App bytes | App slot used | Static RAM |
|---|---|---|---|
| Full | 1,561,067 | 85.1% | 155,187 bytes |
| No BLE | 1,380,551 | 75.2% | See the build output for the matching configuration |

These are recorded build measurements, not a promise about free runtime heap or the size of a later build. HTTPS
sessions and other active interfaces add dynamic demand.

Hardware coverage is recorded in [docs/BENCH.md](docs/BENCH.md). The servo's baseline feeding and power-loss behavior,
and the disc's motion, drive switching and integrations have bench evidence. HTTPS, MQTT TLS, ACME and NimBLE have
been exercised on the disc rig. Outstanding checks include the current audit fixes, extended daily operation,
selected servo recovery scenarios, disc faults and long-run motion, certificate renewal/expiry and some client or
network combinations. Consult section 14 for exact outstanding checks; a successful build is not a hardware result.

Saved core data consists of versioned Settings, Calibration, State, Device, Tuning and MotorPins records. MQTT and
TLS keep their own records. Older saved layouts are migrated, not discarded; their definitions remain required for
upgrades. Time itself is never restored from flash.

Version-by-version implementation history is in [CHANGELOG.md](CHANGELOG.md), detailed bench evidence in
[docs/BENCH.md](docs/BENCH.md), and design rationale in section 13.

## 16. Bench results

Moved to [docs/BENCH.md](docs/BENCH.md).
