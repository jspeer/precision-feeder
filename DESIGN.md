# Precision Feeder ESPHome Firmware: Design

Status: **draft for review, no firmware written yet**. Date: 2026-10-13.

## 0. Disclosure and licence

- **AI disclosure (draft text for README, source headers, protocol doc):**
  > This project was designed and written with the assistance of an AI
  > (Anthropic's Claude). A human reviewed and directed the work, but the code
  > and documentation may contain mistakes. Test on your own hardware, and do
  > not rely on it as the only means of feeding animals you care about.
- **Licence:** MIT (proposed; the goal is "as open as possible").
- **Provenance:** this is a clean-room ESPHome implementation. The original
  Arduino firmware ([FlyingGyroscope/PrecisionFeederRev2](https://github.com/FlyingGyroscope/PrecisionFeederRev2),
  AGPL-3.0) was read only to learn the mechanism's geometry and behaviour. No
  code is copied. The README credits it and the Printables model
  (<https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2>).
  The model's own licence should be checked before the repo ships any
  derived files.

## 1. Goals and non-goals

Goals
- **Recreate the original firmware, but better.** Everything the original
  does is supported (section 1.1), then extended.
- ESPHome firmware (ESP-IDF framework) for the Precision Feeder Rev2 hardware.
- On-board scheduler, so feeding continues with no HA, PLC or network.
- Three control planes onto one core: on-board schedule, Home Assistant (native
  API), and BLE GATT (for a PLC/bridge or any other client).
- One firmware serves both deployments (HA-driven and BLE/PLC-driven).
- A generic public repo: no site-specific naming, documented protocol, example
  configs, and a heavily commented YAML so users can switch features on or off.

Non-goals (v1)
- Fry-style burst feeding (the magazine holds 6 portions; see section 3).
- Remote scheduling by an external controller as the only scheduler.
- Real BLE authentication (see section 10).
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
| Tray calibration (per-slot offsets) | Calibration mode, 7 signed offsets |
| Feed time log (last 6) | Last-feeds text sensor and BLE event log |
| Web UI and dashboard | Optional ESPHome `web_server` (build flag), plus HA |
| WiFi setup portal | ESPHome WiFi, fallback AP and `captive_portal` |
| Hostname, mDNS, timezone, NTP server | ESPHome native, plus YAML options |
| Password and lockout | ESPHome `web_server` auth, API encryption, OTA password |
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
| Servo | MG90S (pin and size compatible with SG92R) |
| Button | **Optional.** Not fitted on every build. |
| Network | WiFi for OTA and HA; BLE for data |
| Shell | PET-GF print (builder's choice) |

Pin map (from the board's pin definition,
<https://docs.waveshare.com/ESP32-S3-Zero>). All pins are YAML substitutions,
so other builds can move them:

| Substitution | Default | Notes |
|---|---|---|
| `servo_pin` | `GPIO1` | servo signal (LEDC PWM). On the top-left header, next to the 5V/GND/3V3 pads so the 3-wire servo lead lands close. |
| `button_pin` | `GPIO2` | optional button to GND, internal pull-up. Only used if the button package is included. |
| `status_led_pin` | `GPIO21` | onboard WS2812 (fixed by the board) |

Avoid: GPIO0 (BOOT, strapping, not broken out), GPIO3, 45, 46 (strapping),
GPIO43/44 (UART0), GPIO33 to 37 (not broken out). GPIO1 floats briefly at
power-up, so the servo may twitch once at boot. A 10 kOhm pull-down on the
signal line prevents that. The board is powered from USB-C or 3.7 to 6 V on
the 5V pad (800 mA LDO for the 3.3 V rail). Power the servo from a separate or
adequately rated 5 V path, not the board's 3.3 V LDO.

## 3. Mechanism and magazine model

Facts taken from the original firmware and the builder's reading of the
Printables instructions (verify on the physical print):

- 7 tray positions, numbered 0 to 6. Servo sweep 175 degrees, so 25 degrees
  per position. Angle for position n is `margin + n * 25` degrees, minus a
  per-position calibration offset (reference margin is 2 degrees at each end).
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
- On boot, command the servo to the saved position, never to 0.
- Detach the PWM output after a short hold time to avoid jitter and heat.
  Needs a bench check that the tray does not drift when detached.

### 3.2 Motion and calibration

- Calibration offsets: 7 signed values (tenths of a degree), clamped.
  The reference used an unsigned type, which can underflow.
- Settings: sweep, margin, step time, wiggle on/off and amplitude, servo pulse
  range (MG90S end stops may differ from a nominal 0 to 180 degrees; do not
  assume a 2 degree margin is safe until tested), direction invert.
- Calibration mode: jog the servo, save per-position offsets. Scheduled and
  remote feeds are rejected while calibrating.

## 4. Feed guard (source-agnostic)

Every feed request, whatever its source, goes through one gate:

| Rule | Default | Notes |
|---|---|---|
| Minimum gap between feeds | 60 s | the reference also paused 1 min after each feed |
| Rolling 24 h cap | 4 portions | configurable 1 to 6 |
| Max portions per command | 1 | configurable 1 to 6 ("drop all" is opt-in) |
| Tray state | must be `LOADED` | empty or unknown rejects |
| Calibration lockout | on | |
| Hold or reload pause active | scheduled feeds only | manual feeds still allowed |
| Time valid | scheduled feeds only | manual feeds still allowed |
| Duplicate sequence number | replay stored result | BLE idempotency, section 8.6 |

Each rejection returns a result code (section 8.4) and is logged.

An event with N portions makes N separate steps, with a configurable gap
between steps (default a few seconds), each with its own wiggle.

## 5. Scheduling

The scheduler runs on the device. HA and BLE can **edit** it and can
**trigger** feeds, but never need to be present.

### 5.1 Profiles

Three profiles (A, B, C), one active at a time. Switching profile is a single
select or one BLE command.

Per profile:
- `name` (short text)
- `cadence`: `daily`, `every_2nd`, or `every_n` (n = 1 to 14), with an anchor
  date defining cycle day 0
- `fast_days`: 7-bit weekday mask of no-feed days
- 4 events

Per event:
- `enabled`
- `anchor`: `clock`, `sunrise`, `sunset`
- `offset`: for `clock`, minutes after midnight (0 to 1439); for solar
  anchors, signed minutes from -720 to +720
- `portions`: 1 to 6, subject to the guard cap
- `days`: 7-bit weekday mask

Global:
- `hold`: pause scheduled feeding for any number of hours, or until cancelled
  (an indefinite hold needs no clock); cancel hold
- `one-shot` events: feed at a given date/time (calendar-style)
- `refill_interval_days` (for runway warnings)

Examples (README): A "Normal" (2 events, Sunday fast); B "Vacation" (1 event,
every_2nd); C "Growout" (3 events/day, short runway warning).

### 5.2 Solar anchoring

- Inputs: latitude, longitude, timezone (POSIX TZ string, so DST works).
- On-device sunrise/sunset calculation (NOAA algorithm, standard refraction
  correction). Resolve each event's time once per local day, at midnight
  rollover and whenever profile, location or time changes.
- Weekday mask applies to the **anchor day**. An event resolving after
  midnight (for example sunset + 3 h) fires early the next morning.
- **No sunrise/sunset on that day (polar):** skip the event and raise a
  `solar_no_event` fault flag. (Default chosen as the safe side for animals:
  a missed meal beats an overfeed. Proposed, not confirmed.)
- Events resolving within the minimum gap of each other: the guard handles it
  (later one is rejected `TOO_SOON` and logged).

### 5.3 Dedupe across reboot

Persist `today` and a fired-events bitmask. After reboot, an event already
fired today is not repeated.

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

## 6. Time sources

The clock must be right for the scheduler and solar maths, and the device can
end up with no network in oddball cases (fallback AP mode; power returns while
the internet is down). BLE exists as a control plane, so it is also a time
path.

- **NTP is the truth.** Uses ESPHome's defaults; server addresses are
  configurable in the YAML.
- **BLE `SET_TIME` is the fallback.** It carries `epoch u32` (UTC) and
  `utc_offset_min i16`. Policy `ble_time_policy`:
  - `fallback_only` (recommended): accepted only if NTP has not synced within
    `ble_time_ntp_stale_hours` (default 24). Otherwise the result is
    `IGNORED_NTP_ACTIVE`. This stops a wrong PLC clock from corrupting a
    correct device clock, and lets a PLC push time on every connect.
  - `always`: accepted any time.
  - `off`: rejected.
- **Home Assistant time** is an opt-in source (off by default), useful when the
  internet is down but HA is up. With NTP and HA both enabled, last sync wins;
  the policy gate applies only to BLE.
- **Timezone:** a build-time YAML substitution (IANA name or POSIX string), or
  pushed by Home Assistant when the HA time package is enabled. **Finding
  (ESPHome 2026.9.1):** the timezone is pre-parsed at build time into a struct;
  there is no on-device POSIX parser, so a runtime timezone entity is not
  possible without writing our own parser. For BLE-only units with no way to
  receive a timezone, the offset from `SET_TIME` could be used (the client
  would send the current offset each time to cover DST). `SET_TIMEZONE`
  (chunked POSIX string) is only worth it if we write that parser; deferred.
- **Time quality is exposed** (HA and GATT): source (none / BLE / NTP / HA) and
  sync age. A BLE-set clock that has aged is flagged "time stale" and keeps
  running. The clock free-runs at a few seconds of drift per day, so a week
  without sync is harmless for feeding.
- **Time is never restored from flash** after power loss: it would be wrong by
  the length of the outage. Scheduled feeds wait for a valid source; manual
  feeds still work.
- **No external RTC.** After a power loss with no network and no BLE client,
  the feeder waits for a valid time before scheduled feeding resumes. This is
  documented plainly in the README.
- No periodic reboot (the reference restarts weekly; we do not).

## 7. Home Assistant entities

Editor pattern, to keep the entity count low.

Status (read-only): tray position, remaining portions, tray state, active
profile name, next feed time, last feed time and source, last feeds log (text),
feeds today, runway days, empty-on date, schedule summary (text), time source
and sync age, fault flags, firmware version, uptime, WiFi RSSI.

Controls: `Feed` (button, 1 portion), `Tray reload` (button), active profile
(select), hold (number of minutes, plus cancel button), internal scheduler
enabled (switch), stop/abort (button).

Editor: profile to edit, event to edit, and fields (enabled, anchor, time,
portions, days), plus profile fields (every N days, fast days). Edits apply
immediately and are saved a couple of seconds later, so there is no Save
button. Event time and days are text entities (`HH:MM` or signed `+03:00`;
`mon,wed,fri` / `weekdays` / `all`). Config: guard settings, gap between
portions, latitude, longitude, reload pause, refill interval
(timezone is build-time, see section 6). Calibration: mode switch, position
select, one offset number, declare-position recovery.

## 8. BLE GATT (secondary control plane)

Implemented with ESPHome's `esp32_ble_server` on ESP-IDF (Bluedroid). Do **not**
enable `bluetooth_proxy` on this device. **Verify binary size against the
4 MB flash with OTA partitions early.** Notifications need testing at default
MTU. BLE is a build option (section 9.1): HA-only builds leave it out.

All multi-byte values little-endian. Design keeps each write within the
default 20-byte payload (no MTU negotiation needed).

### 8.1 Service and characteristics (UUIDs to be generated before release)

| Characteristic | Props | Content |
|---|---|---|
| Info | R | protocol version u8, capability bits u16, positions u8, profiles u8, events per profile u8, firmware version string |
| Command | W | `seq u8, opcode u8, args...` |
| Response | N | `seq u8, opcode u8, result u8, detail u8` |
| Status | R, N | see 8.3 |
| Profile | R, W | header or one event (8.5) |
| Config | R, W | `key u8, value i32` |
| Calibration | R, W | `index u8, offset i16` (tenths of a degree) |
| Event log | N | `epoch u32, source u8, portions u8, result u8` |

### 8.2 Opcodes

| Op | Name | Args |
|---|---|---|
| 0x01 | FEED | portions u8 |
| 0x02 | TRAY_RELOAD | none |
| 0x03 | SET_PROFILE | index u8 |
| 0x04 | HOLD | minutes u16 (0 cancels) |
| 0x05 | STOP | none |
| 0x06 | SET_TIME | epoch u32, utc_offset_min i16 |
| 0x07 | SET_LOCATION | lat i32, lon i32 (1e-5 degrees) |
| 0x08 | CALIBRATE | on/off u8 |
| 0x09 | JOG | position u8, delta i16 |
| 0x0A | SAVE_CALIBRATION | none |
| 0x0B | SET_POSITION | position u8 (declare after UNKNOWN) |
| 0x0C | SET_TIMEZONE | chunk index u8, chunk count u8, up to 17 bytes of a POSIX TZ string |

### 8.3 Status

`tray_position u8, remaining u8, tray_state u8, active_profile u8,
flags u16 (time_valid, hold, calibrating, moving, scheduler_enabled, faults...),
last_feed_epoch u32, last_feed_source u8, feeds_today u8, next_event_epoch u32,
runway_days u8, time_source u8 (0 none, 1 BLE, 2 NTP, 3 HA), time_age_min u16`.

### 8.4 Results and sources

Results: 0 OK, 1 BUSY, 2 TOO_SOON, 3 DAILY_CAP, 4 EMPTY, 5 UNKNOWN_POSITION,
6 CALIBRATING, 7 HOLD, 8 INVALID_ARG, 9 DUPLICATE (replayed result),
10 TIME_INVALID, 11 PORTION_LIMIT, 12 IGNORED_NTP_ACTIVE (BLE time rejected by
policy), 13 BLE_TIME_DISABLED.

Sources: 0 schedule, 1 home assistant, 2 BLE, 3 button, 4 one-shot.

### 8.5 Profile writes

Two write types so each fits 20 bytes:
- Header: `0x00, profile u8, cadence u8, n u8, anchor_day u16, fast_mask u8`
- Event: `0x01, profile u8, event u8, flags u8 (enabled, anchor type),
  offset i16, portions u8, days_mask u8`

### 8.6 Idempotency

The device keeps the last 8 sequence numbers and their results. A repeated
sequence number returns the stored result and does not act, so a client retry
after a lost response cannot feed twice.

## 9. Operational behaviour

- ESP-IDF framework; WiFi fallback AP; OTA password; API encryption.
- No reboot on API or WiFi loss. No scheduled reboot.
- Versioning `major.minor.revision`, exposed as a text sensor and in the Info
  characteristic.
- **Optional push button**, matching the original: with the button enabled in
  the YAML, a press feeds one portion, and a press with the tray empty
  reloads the tray (the original's single-button behaviour). Long press enters
  calibration. With no button fitted, nothing is configured and the feature
  does not exist. Status LED for state and faults if the board has one.

### 9.1 YAML configuration surface

Each feature is its own package. The example YAML enables or disables them
and documents every setting with a comment saying what it does and when to
change it. Sketch:

```yaml
substitutions:
  # Pins. Defaults suit the Waveshare ESP32-S3-Zero; change for other boards.
  servo_pin: "GPIO1"       # servo signal
  button_pin: "GPIO2"      # optional button to GND (button package only)
  status_led_pin: "GPIO21" # onboard WS2812 on the S3-Zero
  # NTP is the primary time source. Leave the default pool servers, or point
  # at a local NTP server if your network has no internet.
  ntp_server_1: "0.pool.ntp.org"
  # BLE time push, for fallback AP mode or a PLC-managed unit:
  #   fallback_only = accepted only if NTP has not synced recently (recommended)
  #   always        = accepted any time
  #   off           = SET_TIME is rejected
  ble_time_policy: "fallback_only"
  ble_time_ntp_stale_hours: "24"

packages:
  core: !include packages/core.yaml
  scheduler: !include packages/scheduler.yaml
  ble_gatt: !include packages/ble_gatt.yaml        # comment out for HA-only builds
  time_ha: !include packages/time_homeassistant.yaml  # internet down, HA up
  button: !include packages/button.yaml            # only if your print has the button
  web_ui: !include packages/web_server.yaml        # the original's web dashboard
```

Leaving BLE out of an HA-only build saves its RAM and flash. **Verify how
ESPHome handles conditional package inclusion;** commenting a package in or out
is the safe fallback.

## 10. BLE security

Start simple: optional bonding, with the feed guard bounding the damage a
stranger in range could do (a few portions). A passkey or application-level
shared key is a later option. **ESPHome's pairing options need checking
against current docs.** This is documented in the README as a known limitation.

## 11. Out of scope hardware

No external RTC, no feed or hopper sensors, no home or position sensor, no
humidity sensor, no reserved I2C header. Position is open-loop (section 3.1
describes how an interrupted move is recovered). The optional push button is
the only add-on, because the original firmware supports it.

## 12. Repository layout (proposed)

```
precision-feeder/
  README.md            overview, wiring, disclosure, licence
  LICENSE
  DESIGN.md            this document (later split into docs/)
  docs/PROTOCOL.md     BLE protocol
  docs/CALIBRATION.md  setup and calibration walkthrough
  components/precision_feeder/   external component (C++ core + ESPHome glue)
  packages/            reusable YAML packages (core, scheduler, ble, time, button, web)
  examples/            ha-only.yaml, ble-plc.yaml, standalone.yaml
  tools/               Python bleak example client
```

Importable with ESPHome `packages:` / `github://`.

## 13. Decisions log

- Goal: recreate the original firmware with full parity, then improve it.
- Portion = one tray step; commands default to 1 portion; opt-in higher.
- Three profiles, editor-entity pattern, 4 events per profile.
- Solar anchoring (clock, sunrise, sunset, signed offset) is core.
- No burst/fry mode.
- MIT licence, AI disclosure in repo.
- Time: NTP is the truth; BLE `SET_TIME` is a fallback gated by
  `ble_time_policy`; HA time opt-in; time never restored from flash.
- No external hardware (RTC, sensors). Optional push button supported.
- Features are YAML packages and build options, documented in comments.
- **No catch-up feeding (2026-10-13, replaces "configurable grace, default off"):**
  an event that is missed (device offline, hold, reload pause, or refused by the
  guard for its whole minute) is skipped, never made up later.
- Hold has no maximum duration, and can last until cancelled (needs no clock).
- Periodic reboots and an upcoming-events list are not carried over from the
  original.
- Clean-room code; PLC/bridge driver lives outside this repo.

## 14. Open items

1. Polar/no-sun fallback: skip (proposed) or clock-time fallback.
2. Confirm the physical servo end stops and the slot/position mapping on the
   printed tray.
3. Servo (GPIO1) and button (GPIO2) pins chosen; confirm on the bench that the
   servo does not twitch at boot, and whether the pull-down is needed.
4. Confirm ESPHome `esp32_ble_server`, pairing, multi-`time:` and conditional
   package behaviour against current docs; check flash size.
5. Check the Printables model licence before shipping derived files.
6. Confirm whether the HA-time package should ship in v1.

## 15. Implementation status (v0.1.1, 2026-10-13)

First parity build. **Compiles with no warnings; not yet run on hardware.**
Compiled with ESPHome 2026.9.1 / ESP-IDF 5.5.5 for the ESP32-S3: about 996 KB,
54% of the 1.8 MB app partition, 33% RAM. The pure logic (solar maths and
calendar helpers) is covered by `tests/test_logic.cpp`; there is no C++
compiler in the authoring environment, so the maths was checked with a
line-for-line Python port (London and Sydney match published values to the
minute; Indianapolis agrees with the expected 19:09 EDT sunset). **The C++
test itself, and all runtime behaviour, still need to be run.**

| Area | Where | State |
|---|---|---|
| Tray state machine, persisted intent, interrupted-move recovery | component | built |
| Feed guard (gap, 24 h cap, per-command limit, state, hold, time) | component | built |
| Scheduler: 3 profiles x 4 events, clock/sunrise/sunset anchors, weekdays, every-N-days, fast days | component | built |
| One-shot feeds, hold (any duration or until cancelled), reload pause | component | built |
| Runway, next feed, oversubscription flag | component | built |
| Calibration (7 offsets), position declaration | component | built |
| HA entities (status, controls, editor, calibration) | YAML templates | built |
| Optional button, status LED, web UI, HA time source | YAML packages | built |
| BLE GATT control plane, `SET_TIME`, time-quality reporting | | **not started** |
| Runtime timezone | | **blocked** (section 6 finding) |
| Duplicate-sequence idempotency | | with BLE |

Deviations from this design, all deliberate:
- A feed with more portions than remain is clamped to what remains (design said
  reject); empty still rejects.
- The per-command portion limit applies to manual/remote sources only; scheduled
  events are limited by the daily cap.
- Feeds made before the clock is set count toward the daily cap using uptime,
  then are re-stamped when the clock arrives; the cap memory for such feeds is
  dropped on reboot.
- The minimum gap uses uptime when available, otherwise the saved epoch.
- Sunrise/sunset events skip (and flag a fault) when there is no sun event or
  no location set, as proposed.

Known gaps and risks to check on the bench:
1. Servo end stops, release-without-drift, power-up twitch (section 14).
2. Template entities poll every few seconds; fine for HA but chatty on the API.
3. Event times on daylight-saving change days use wall-clock conversion; not
   exercised on a real changeover.
4. `esp32_rmt_led_strip` defaults to `use_psram: true`; harmless without PSRAM
   (falls back to internal RAM) but worth knowing.

Changes in v0.1.1 (cleanup after reviewing the original firmware's behaviour):
- Late feeds removed: the "Late Start Grace" setting is gone and events fire only
  in the minute they are due (section 5.5). Settings version bumped to 2.
- Hold has no maximum: "Hold Duration" is in hours, 0 means until cancelled, and
  an indefinite hold needs no clock. A "Hold" status sensor shows what is active.
- The cancel-backlog step on tray reload is gone (nothing to cancel).
