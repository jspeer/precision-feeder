# Precision Feeder ESPHome Firmware: Design

Status: **firmware v0.2.1 built and host-tested; not yet run on hardware**
(implementation status in section 15). First written 2026-10-13.

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
| Servo | MG90S. The original firmware was built with a TowerPro SG92R; the two are pin and size compatible, but their travel per microsecond may differ (section 3). |
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
- `one-shot` events: feed at a given date/time (calendar-style). They run even
  when the Scheduler switch is off (it only stops recurring profile events), but
  a hold or reload pause still applies (decision 2026-10-14).
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
select, one offset number, servo min/max pulse (saved, tuned at run time),
declare-position recovery.

## 8. BLE GATT (secondary control plane)

**The protocol is specified in [docs/PROTOCOL.md](docs/PROTOCOL.md), which is
authoritative.** This section records the design intent and how the built
protocol differs from the first draft.

Implemented with ESPHome's `esp32_ble_server` on ESP-IDF (Bluedroid). The GATT
table (UUIDs, properties) is YAML in `packages/ble_gatt.yaml`; the meaning of the
characteristics is C++ in `components/precision_feeder_ble/`, with the byte
layouts and validation in a pure header (`protocol.h`) that is unit-tested on a
host. BLE is a build option: HA-only builds leave the package out and pay nothing.
`bluetooth_proxy` must not be enabled on this device. All records fit the default
20-byte payload, so no MTU negotiation is needed. UUID base:
`02dd1dd9-ba75-4bbc-bdea-306d2101xxxx` (generated 2026-10-13; this project's own).

Changes from the first draft, all deliberate:
- `HOLD` takes a 32-bit number of minutes (0 cancels, `0xFFFFFFFF` until
  cancelled), because the hold has no maximum (section 13).
- Profile records follow the data model that was built: a header
  (`every_n`, `fast_mask`, `anchor_day`) and events, plus a *query* write that
  selects the record the next read returns (GATT reads take no parameters). Config
  and Calibration use the same query-then-read pattern (`key | 0x80`).
- Writes to Profile, Config and Calibration are answered on Response with `seq 0`
  and opcodes `0x80` to `0x82`, so a client learns about validation failures.
- Response is also readable, in case a notification is missed.
- New result codes 14 (UNSUPPORTED) and 15 (NOT_CALIBRATING).
- `SET_TIMEZONE` is reserved and answered UNSUPPORTED (section 6: no on-device
  timezone parser). `SET_TIME` still carries a UTC offset for forward
  compatibility; the device ignores it.
- Idempotency: the last 8 commands are remembered for 30 seconds, not forever,
  and a retry must repeat the *whole* command (sequence number, opcode and
  arguments). A command that reuses a sequence number with different contents is
  executed as new, so a client that restarts and reuses numbers is not mistaken
  for a retry. A replay returns the original answer.
- Status gained flag bits for hold, reload pause, stale clock, unknown position
  and empty tray, and reports the clock source and age.

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

## 12. Repository layout

```
precision-feeder/
  README.md  LICENSE  DESIGN.md  AGENTS.md
  docs/PROTOCOL.md     BLE protocol (authoritative)
  docs/CALIBRATION.md  setup and calibration walkthrough
  components/precision_feeder/      feeder_core.* (all logic, plain C++), calendar.h,
                                    solar.h (pure, host-tested), precision_feeder.*
                                    (ESPHome adapter: clock, servo, flash, log)
  components/precision_feeder_ble/  optional BLE protocol: protocol.h (pure,
                                    host-tested) and the GATT handlers
  packages/            core, status, controls, schedule_editor, calibration,
                       status_led, web_server, button, time_homeassistant,
                       diagnostics, ble_gatt
  examples/            basic.yaml (HA + web UI), ble.yaml (adds BLE, button,
                       HA time, diagnostics), secrets.example.yaml
  tests/               host-side C++ tests (core, logic, protocol)
  tools/               Python bleak reference client and its tests
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
- If memory gets tight, turn on PSRAM; it may become a requirement of the
  firmware (2026-10-13). Not enabled until measured on hardware.
- OTA authenticates with the API key; no OTA password; no web-server OTA.
- One-shot feeds are independent of the Scheduler switch (2026-10-14).
- While the clock is unknown after a reboot, pre-reboot feeds do not count toward
  the daily cap (2026-10-14).
- All feeder logic lives in `FeederCore` with no ESPHome dependencies, so it can be
  tested on a host (2026-10-14).
- Periodic reboots and an upcoming-events list are not carried over from the
  original.
- Clean-room code; PLC/bridge driver lives outside this repo.

## 14. Open items

1. Polar/no-sun fallback: skip (proposed) or clock-time fallback.
2. ~~Confirm the slot/position mapping on the printed tray.~~ Done on the
   bench, 2026-10-14 (section 17): all 7 positions line up. Still to watch:
   whether the servo strains at position 6 in daily use.
3. Servo (GPIO1) and button (GPIO2) pins chosen; confirm on the bench that the
   servo does not twitch at boot, and whether the pull-down is needed.
4. Confirm ESPHome `esp32_ble_server`, pairing, multi-`time:` and conditional
   package behaviour against current docs; check flash size.
5. Check the Printables model licence before shipping derived files.
6. Confirm whether the HA-time package should ship in v1.

## 15. Implementation status (v0.3.0, 2026-10-14)

**Compiles with no warnings; not yet run on hardware.** Compiled with ESPHome
2026.9.1 / ESP-IDF 5.5.5 for the ESP32-S3: about 972 KB (53% of the 1.8 MB app
partition) without BLE, about 1.30 MB (71%) with it (section 16).

What has been run, on a host (a `ziglang` C++ compiler installed with pip into a
scratch virtualenv; any C++17 compiler works):
- `tests/test_logic.cpp`: calendar maths and sunrise/sunset for Indianapolis,
  London and Sydney, plus polar night and midnight sun. Passes.
- `tests/test_protocol.cpp`: BLE packing, record validation, config ranges, the
  time-acceptance policy and the retry cache, against golden byte vectors. Passes.
- `tools/test_precision_feeder_ble.py`: the Python client against the same
  vectors, and a check that its UUIDs match `packages/ble_gatt.yaml`. Passes.
- `tests/test_core.cpp` (from v0.2.1): the tray state machine, guard, scheduler,
  one-shots, hold, reload pause, cadence, fast days, sunset-after-midnight,
  runway and clock tracking, against a simulated clock with a fixed UTC-5
  timezone. Passes.

Only compiled, not run: the ESPHome adapter (`precision_feeder.cpp`: real clock,
PWM, flash, daylight-saving resolution) and the BLE handlers
(`precision_feeder_ble.cpp`).

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
| BLE GATT table, diagnostics package | `ble_gatt.yaml`, `diagnostics.yaml` | built, size benchmarked (section 16) |
| BLE protocol v1: commands, status, profile/config/calibration records, event log, retry cache | `precision_feeder_ble` component | built, compiled, pure parts tested on a host |
| Clock: source tracking (NTP, Home Assistant, BLE), `SET_TIME` policy, age and stale flag | core + BLE component | built |
| Reference client | `tools/precision_feeder_ble.py` | built; pure parts tested, the BLE client class needs a device |
| Runtime timezone | | **blocked** (section 6 finding) |
| BLE authentication | | not started (section 10) |

Deviations from this design, all deliberate:
- A feed with more portions than remain is clamped to what remains (design said
  reject); empty still rejects.
- The per-command portion limit applies to manual/remote sources only; scheduled
  events are limited by the daily cap.
- Feeds made before the clock is set count toward the daily cap using uptime,
  then are re-stamped when the clock arrives; the cap memory for such feeds is
  dropped on reboot.
- After a reboot, until the clock is set again, feeds saved before the reboot
  are not counted toward the daily cap (their age is unknown); they count again
  once the clock is back (decision 2026-10-14, replacing "count them for 24 h of
  uptime", which refused manual feeds for a day after a power cut with no
  network). A feed interrupted by the power cut still counts once the clock is
  known.
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

Changes in v0.2.1 (review fixes, 2026-10-14):
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
- Daily cap after a reboot without a clock (section 15 deviations).
- One-shot feeds run with the Scheduler switch off; "Next Feed", runway and the
  over-capacity flag no longer count recurring events while it is off.
- The sunrise/sunset fault is evaluated every tick, and clears while the
  scheduler is off.
- The reload pause survives a reboot when the clock was known (state version 2;
  the saved tray state from earlier versions is discarded).
- "Minutes Since Clock Sync" is unknown until the clock has been set.
- Python client: answers matched on (seq, opcode); commands never use seq 0.

Changes in v0.3.0 (servo pulse range settable at run time, 2026-10-14):
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

Changes in v0.2.2 (servo default, 2026-10-13):
- Default `min_pulse` is 544 us (was 500), the pulse range the original firmware
  drives (ESP32Servo's default, 544 to 2400 us), so a fresh build starts the tray
  where the original did. The original was tuned on an SG92R; this build uses an
  MG90S, so the range still needs the bench check. Existing builds that set
  `min_pulse` themselves are unaffected; others will see every position shift by
  up to about 4 degrees and should re-check calibration.
- Docs: the tray geometry measured from the printed model (section 3).

Changes in v0.2.0 (BLE protocol, and the two warnings fixed):
- BLE protocol v1 implemented (section 8, `docs/PROTOCOL.md`).
- OTA authenticates with the API encryption key (no separate OTA password), and
  the web server's own `/update` page is disabled (`ota: false`), because it would
  accept an image in plaintext. Firmware uploads go through the encrypted ESPHome
  OTA only.
- `rgb_order` replaced by `channel_colors` (deprecated in ESPHome).
- The clock source is tracked and shown in Home Assistant ("Time Source",
  "Minutes Since Clock Sync").

## 16. BLE benchmark (2026-10-13)

The question was whether BLE fits the 4 MB flash of the ESP32-S3FH4R2. Measured
by compiling `examples/basic.yaml` and `examples/ble.yaml` (same packages, plus
`packages/ble_gatt.yaml`, a skeleton of the full planned GATT table whose
handlers only log) with ESPHome 2026.9.1 / ESP-IDF 5.5.5, Bluedroid, ESP-IDF
framework, WiFi, API, web server and status LED all enabled.

| Build | Flash (app slot 1,835,008 B) | Static RAM (341,760 B) |
|---|---|---|
| No BLE (`basic.yaml`) | 993,835 B (54.2%) | 112,079 B (32.8%) |
| BLE skeleton | 1,312,107 B (71.5%) | 142,663 B (41.7%) |
| BLE + diagnostics package | 1,315,179 B (71.7%) | 142,879 B (41.8%) |
| BLE with the protocol implemented (v0.2.0) | 1,319,791 B (71.9%) | 143,231 B (41.9%) |
| v0.2.1, no BLE (`basic.yaml`) | 971,847 B (53.0%) | 112,223 B (32.8%) |
| v0.2.1, BLE + button + HA time + diagnostics | 1,300,123 B (70.9%) | 143,739 B (42.1%) |
| v0.2.1 rebuilt on the dev machine, no BLE | 971,863 B (53.0%) | 112,223 B (32.8%) |
| v0.2.1 rebuilt on the dev machine, BLE build | 1,297,611 B (70.7%) | 143,383 B (42.0%) |

- **BLE costs about 318 KB of flash and about 31 KB of static RAM.**
- **Flash is not a problem.** With BLE the app is 72% of its OTA slot, leaving
  about 520 KB for the protocol code, which should be a few tens of KB. OTA still
  needs only one free slot of the same size, so it keeps working.
- **RAM is the open question.** About 199 KB of static RAM remains, but Bluedroid
  also allocates working memory from the heap when it starts, and that cannot be
  measured without running hardware. The `diagnostics` package reports free heap,
  the largest free block and fragmentation so it can be read on the first boot,
  with BLE on and off.
- **PSRAM is not a switch.** ESPHome's `esp32_ble: use_psram` only enables
  Bluedroid's dynamic environment memory on the S3 (it does not force the stack
  into PSRAM; the "allocate from SPIRAM first" option applies to the classic
  ESP32 only), and it needs the `psram` component with the right mode for the
  module (quad for the H4R2; a wrong mode fails at boot). Not enabled; try it
  only if the heap numbers say it is needed.
- Decision rule for the MCU question: flash does not justify a different MCU.
  Re-evaluate only if measured free heap with WiFi, API, web server and BLE all
  running leaves too little headroom.

The protocol itself added only about 4.6 KB over the skeleton. If the heap turns
out tight on real hardware, enabling PSRAM is an accepted fallback and may become
a requirement of the firmware (decision 2026-10-13); the board's 2 MB of quad
PSRAM is otherwise unused.

Two ESPHome warnings found while validating were fixed in v0.2.0: the deprecated
`rgb_order` and the OTA password.

## 17. Bench results

### Calibration, first build (2026-10-14, firmware 0.3.0)

Waveshare ESP32-S3-Zero, MG90S, tray printed from the `REV2 (Feeding hole,
Small) v6` model. The servo horn was re-seated once so that slot 0 needs a
positive correction (position 0 is commanded to 2.5 degrees and the firmware
never goes below 0).

With the default 544 to 2400 us range, slot 0 needed +11 degrees, slot 1 about
+11 and slot 2 +14.5, and an uncalibrated position 6 stopped just past slot 5:
this MG90S turns less per microsecond than the range (taken from the original's
SG92R) assumes. The per-slot offsets (+-15) could not absorb it, which is why
v0.3.0 made the pulse range settable at run time.

Final calibration, every position lined up with the opening:

| Setting | Value |
|---|---|
| Servo Min Pulse | 637 us |
| Servo Max Pulse | 2675 us |
| Offsets, positions 0 to 6 | 0, 0, 0, +2.8, +3.3, +3.3, 0 degrees |

That is about 283 us per 25 degree step (2038 us span; about 11.3 us per
degree), against 258 us with the default range, about 10% more. Position 6 is
driven at about 2364 us. The bump at positions 3 to 5 that returns to 0 at 6 is
ordinary servo non-linearity; the offsets absorb it.

Conclusions:
- The pulse range has to be calibrated per servo; the default is only a start.
  Calibrate the range first (slot 0 with the minimum, slot 6 with the maximum),
  then the offsets. Written up in docs/CALIBRATION.md.
- The YAML defaults stay at the original's 544 to 2400 us: one servo is not
  enough to change them, and the run-time range makes that unnecessary.

Not yet checked: the rest of the bench plan (power-up twitch, wiggle and drift
after release, power loss mid-feed, heap, BLE, schedules).

