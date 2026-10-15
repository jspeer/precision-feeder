# Precision Feeder ESPHome Firmware: Design

Status: **firmware v1.0.0, tested on a bench build** (section 15). This is the
current design; history is in [CHANGELOG.md](CHANGELOG.md) and hardware results
in [docs/BENCH.md](docs/BENCH.md). First written 2026-10-13.

## 0. Disclosure and licence

- **AI disclosure (draft text for README, source headers, protocol doc):**
  > This project was designed and written with the assistance of an AI
  > (Anthropic's Claude). A human reviewed and directed the work, but the code
  > and documentation may contain mistakes. Test on your own hardware, and do
  > not rely on it as the only means of feeding animals you care about.
- **Licence:** public domain, under The Unlicense (owner, 2026-10-14; replaces
  the MIT proposal). No copyright holder. Credits (FlyingGyroscope as the
  inspiration, Claude as the author's tool) are in the README; the dedication
  cannot require them, but they travel with the README.
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
| Rolling 24 h cap | 4 portions | configurable 1 to 6. The count it uses can be edited in Home Assistant for debugging and recovery (decision 2026-10-14, section 13) |
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
- **Home Assistant time** is a second source, enabled in both examples (decision
  2026-10-14), useful when the internet is down but HA is up. By default HA also
  pushes its timezone; a build can pin its own (packages/time_homeassistant.yaml). With NTP and HA both enabled, last sync wins;
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
  documented plainly in the README.
- No periodic reboot (the reference restarts weekly; we do not).

## 7. Home Assistant entities

Editor pattern, to keep the entity count low.

Status (read-only): tray position, remaining portions, tray state, needs food
(binary, for automations), active profile name, next feed time, last feed time and source, last feeds log (text),
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
characteristics is C++ in `components/precision_feeder_ble/`: byte layouts and
validation in `protocol.h`, the cryptography in `crypto.h`, and every handler in
`BleEngine` (`engine.*`), all plain C++ tested on a host against the simulated
feeder; `precision_feeder_ble.*` only connects the engine to ESPHome. BLE is a build option: HA-only builds leave the package out and pay nothing.
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
saying what it does and when to change it. A user's own settings go in a
git-ignored `examples/local.yaml` that includes an example and overrides
substitutions. The README lists every package and substitution.

Entities publish only when their value changes: each package refreshes its
entities from one 1-second interval through `publish_number/text/bool/option`
(`precision_feeder.h`), instead of each entity polling on its own.

## 10. BLE security

Decided 2026-10-14 (owner: "use best practices"), built in protocol 2
(firmware 0.4.0). Details in docs/PROTOCOL.md, "Authentication".

What ESPHome 2026.9.1 offers, checked in its source: `esp32_ble` can set the
Security Manager parameters (`io_capability`, `auth_req_mode` including
`sc_bond`, key sizes), but `esp32_ble_server` hard-codes every characteristic's
permissions as plain read/write (no "encryption required"), and there is no
static passkey. So link-layer pairing cannot keep a stranger out: they can skip
pairing and write anyway, and with no screen or keypad pairing is "Just Works".

Hence application-layer authentication, plus link encryption where offered:
- A shared 32-byte key (`ble_key`, secrets.yaml, required whenever the BLE
  package is built in).
- Per connection: a random 16-byte nonce (hardware RNG) readable from a new Auth
  characteristic; the client proves the key with AUTH (HMAC-SHA256 over the
  nonce); both derive a session key.
- Every write carries a sequence number (strictly increasing, window 127) and an
  8-byte HMAC tag bound to the characteristic. This stops injection, tampering
  and replay (recorded traffic is useless on the next connection's nonce, and a
  stale sequence number is refused within a connection). The 30 s retry cache
  still makes exact retries safe.
- Until AUTH, only Info and Auth carry data; nothing is notified.
- After a failed AUTH: a new nonce and a one-second wait.
- `auth_req_mode: sc_bond` so a client that pairs gets an encrypted link (also
  hiding what it reads).
- Not covered: the feeder does not prove its identity to the client, and
  answers are not signed (no room in the 20-byte Status). The feeder only acts on
  authenticated writes, so the animals are protected; a client that must trust
  what it reads should pair.

SHA-256 and HMAC are a small pure implementation (crypto.h) so the device code
is the host-tested code (FIPS 180-4 and RFC 4231 vectors, plus golden vectors
shared with the Python client).

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
  docs/SCHEDULING.md   building schedule profiles
  docs/CALIBRATION.md  setup and calibration walkthrough
  docs/BENCH.md        hardware test results
  components/precision_feeder/      FeederCore (plain C++, host-tested): feeder_core.h,
                                    feeder_core.cpp (tray, guard, clock), feeder_schedule.cpp
                                    (scheduler, projection), feeder_text.cpp (editor, texts),
                                    feeder_internal.h, calendar.h, solar.h; precision_feeder.*
                                    (ESPHome adapter: clock, servo, flash, log, publish helpers)
  components/precision_feeder_ble/  optional BLE protocol: protocol.h, crypto.h and the
                                    BleEngine (engine.*: every handler), all host-tested;
                                    precision_feeder_ble.* is the thin ESPHome adapter
  packages/            core, status, controls, schedule_editor, calibration,
                       status_led, web_server, button, time_homeassistant,
                       diagnostics, ble_gatt
  examples/            basic.yaml, ble.yaml, secrets.example.yaml
  tests/               host-side C++ tests (core, logic, protocol)
  tools/               Python reference client: precision_feeder_ble.py (entry point) and
                       precision_feeder_client/ (protocol, client, selftest, cli); check.sh
```

## 13. Decisions log

- Goal: recreate the original firmware with full parity, then improve it.
- Portion = one tray step; commands default to 1 portion; opt-in higher.
- Three profiles, editor-entity pattern, 4 events per profile.
- Solar anchoring (clock, sunrise, sunset, signed offset) is core.
- No burst/fry mode.
- Public domain (The Unlicense), AI disclosure in repo (licence decided 2026-10-14; was MIT).
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
- OTA authenticates with the API key; no OTA password; no web-server OTA.
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
  history. AGENTS.md is updated to match.
- Each event feeds at most once per day, also when it is moved later after it
  fed (2026-10-14; the safe side).
- Polar days (no sunrise or sunset): sun events fall back to 06:00 / 18:00 plus
  their offset, with a fault shown (owner, 2026-10-14; was "skip").
- The Home Assistant time package ships enabled in both examples (owner,
  2026-10-14), after it was seen setting the clock on hardware.
- BLE security: application-layer authentication with a shared key (protocol 2),
  plus LE Secure Connections pairing when the client pairs (owner, 2026-10-14;
  section 10).

## 14. Open items

1. ~~Polar/no-sun fallback.~~ Decided 2026-10-14: fixed 06:00 / 18:00 fallback (section 5.2).
2. ~~Confirm the slot/position mapping on the printed tray.~~ Done on the
   bench (docs/BENCH.md). Still to watch: whether the servo strains at
   position 6 in daily use.
3. ~~Servo (GPIO1) and button (GPIO2) pins.~~ Done on the bench (docs/BENCH.md).
4. ~~ESPHome BLE server, pairing, two time sources, packages, flash size.~~
   Checked in ESPHome's source and on the bench (section 10, docs/BENCH.md).
5. Check the Printables model licence before shipping derived files.
6. ~~HA-time package in v1.~~ Decided 2026-10-14: enabled in both examples.

## 15. Implementation status (v1.0.0)

Everything in sections 3 to 10 is built. On a bench build (Waveshare
ESP32-S3-Zero, MG90S) it has been calibrated and tested on hardware: feeding,
power cuts, schedules on NTP, Home Assistant and BLE time, every BLE command,
the button, Needs Food and the over-cap warning ([docs/BENCH.md](docs/BENCH.md)).
Host tests (`tests/`, `tools/`) cover the core logic with a simulated clock, the
calendar and sun maths, the BLE protocol and its cryptography, and the Python
client. Version history: [CHANGELOG.md](CHANGELOG.md).

Compiled with ESPHome 2026.9.1 / ESP-IDF 5.5.5: about 53% of the 1.8 MB app
partition without BLE, 71% with it.

Deviations from the first draft of this design, all deliberate:
- A feed with more portions than remain is clamped to what remains (design said
  reject); empty still rejects.
- The per-command portion limit applies to manual/remote sources only; scheduled
  events are limited by the daily cap.
- Feeds made before the clock is set count toward the daily cap using uptime,
  then are re-stamped when the clock arrives; the cap memory for such feeds is
  dropped on reboot.
- After a reboot, until the clock is set again, feeds saved before the reboot
  are not counted toward the daily cap (their age is unknown); they count again
  once the clock is back (decision 2026-10-14). A feed interrupted by the power
  cut still counts once the clock is known.
- The minimum gap uses uptime when available, otherwise the saved epoch.
- Polar days use a 06:00 / 18:00 fallback instead of skipping (section 5.2).

Saved data: three records, each with its own preference key and version
(`feeder_core.h`): settings (schedule, guard, location), calibration (pulse range
and offsets) and tray state. A layout change gets a new key and a migration from
the old layout; saved data is never simply discarded.

Not yet exercised: a real daylight-saving changeover (event times use wall-clock
conversion through the platform's timezone rules); a BLE client that pairs.

## 16. Bench results

Moved to [docs/BENCH.md](docs/BENCH.md).
