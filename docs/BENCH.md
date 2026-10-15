# Bench results

What has been measured and seen working on real hardware, oldest first. The
design is in [DESIGN.md](../DESIGN.md); what changed in each version is in
[CHANGELOG.md](../CHANGELOG.md).

*Written with the assistance of an AI (Anthropic's Claude).*

## BLE size benchmark (2026-10-13)

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
| v1.0.1, no BLE (`basic.yaml`) | 979,947 B (53.4%) | 115,055 B (33.7%) |
| v1.0.1, BLE build (`ble.yaml`) | 1,309,291 B (71.4%) | 145,903 B (42.7%) |
| v1.1.0 in progress (M7), no BLE (`basic.yaml`) | 1,025,135 B (55.9%) | 115,703 B (33.9%) |
| v1.1.0 in progress (M7), BLE build (`ble.yaml`) | 1,358,551 B (74.0%) | 146,775 B (42.9%) |
| v1.1.0 in progress (M8, IPv6 on), no BLE | 1,053,323 B (57.4%) | 117,503 B (34.4%) |
| v1.1.0 in progress (M8, IPv6 on), BLE build | 1,386,983 B (75.6%) | 148,583 B (43.5%) |

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


## Calibration, first build (2026-10-14, firmware 0.3.0)

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

## Other bench checks (2026-10-14)

- Power-up: no servo twitch on GPIO1, so the 10 kOhm pull-down is not needed on
  this build (open item 3, servo part).
- Release: outside calibration mode the servo goes limp after the detach delay
  (the LEDC output at level 0 stops the pulses). In calibration mode it keeps
  holding, by design, so the slot being adjusted stays put.
- A one-portion feed runs the wiggle as designed: 0 -> 1 -> 0 -> 1.
- No drift after release: the tray stays at its position once the servo goes
  limp (empty tray; recheck with food in it).

- Power loss mid-feed: after power was pulled during a feed, the device booted
  with the tray position Unknown and feeding blocked, and the owner recovered it
  (the persisted-intent design in section 3.1 works on hardware).

- Heap with WiFi, API, web server and BLE all running (`ble.yaml`, diagnostics
  package): free 154.5 KB, largest free block 110.6 KB, fragmentation about 28%.
  Comfortable; PSRAM is not needed (the RAM question above is answered
  for this build). Loop time is normally about 30 ms; a peak of 394 ms was seen,
  probably at boot or WiFi connect; keep an eye on it.
- BLE, from a Linux PC with the reference client (bleak): Info (protocol 1,
  capability bit 5), Status, Config writes and reads (including the pulse keys,
  out-of-range and too-close pulse pairs refused), SET_TIME ignored while NTP is
  fresh, SET_TIMEZONE and unknown opcodes UNSUPPORTED, wrong length INVALID_ARG.
  Retry: an exact repeat replays the stored answer without acting (one event-log
  record), and the same seq with different arguments is a new command.
- Retry of an *accepted* BLE feed: the first run was refused with DAILY_CAP (six
  portions already in the 24 h window). With the owner's permission, a temporary
  debug build allowed a daily cap of up to 12 over BLE (one line in
  `protocol.h`, never committed); with the cap at 7 the feed was accepted (tray
  0 -> 1, "ok, 1 portion") and the exact retry replayed that answer without
  moving the tray, one event-log record. The cap was set back to 4, the source
  reverted, the committed firmware reflashed, and the device then refused a cap
  of 7 again.

## Full BLE and schedule run (2026-10-14, firmware 0.3.0 + temporary debug builds)

With the owner's approval, temporary builds (never committed, since reverted)
allowed a daily cap of up to 12 over BLE, and one had no reachable WiFi.
Tray empty; owner settings saved first and restored afterwards (checked).

- Every command, success and refusal: FEED (OK; BUSY, TOO_SOON,
  PORTION_LIMIT, DAILY_CAP, CALIBRATING, UNKNOWN_POSITION, EMPTY,
  INVALID_ARG), TRAY_RELOAD (OK; CALIBRATING, INVALID_ARG), SET_PROFILE,
  HOLD (minutes, until cancelled, cancel; INVALID_ARG; TIME_INVALID without a
  clock), STOP (mid-feed -> position unknown; INVALID_ARG), SET_TIME (OK
  without network time; IGNORED_NTP_ACTIVE; before 2019 and wrong length
  INVALID_ARG), SET_LOCATION, CALIBRATE, JOG (OK; NOT_CALIBRATING,
  INVALID_ARG), SAVE_CALIBRATION, SET_POSITION (OK; CALIBRATING,
  INVALID_ARG), SET_TIMEZONE (UNSUPPORTED). Profile, Config and Calibration
  records written, read back and refused when out of range. All pass.
- Status updates at most every 500 ms (as PROTOCOL.md says); a client must not
  expect it to change the instant a command is answered.
- Schedules on NTP time: a clock event and a sunset event (a test location,
  offset -131 min onto the test minute) each fed once, inside their minute,
  and not again. An event due during a hold was refused (HOLD), reported once
  when its minute ended, and never made up.
- Real power cut with no WiFi (build with an unknown SSID): clock unset at
  boot, timed HOLD refused TIME_INVALID, indefinite hold and manual feed work,
  SET_TIME accepted, source BLE, and a scheduled event then fed on the BLE
  clock. No reboot from the missing WiFi.
- Found: Next Feed showed an event that had already fed today (fixed in 0.3.1);
  a clock kept by the RTC across a soft reset was reported as NTP (fixed in
  0.3.1); faking the NTP server names does not take network time away on the
  owner's network (something answers NTP anyway, probably a firewall
  redirect), so "no network" tests need WiFi removed.

0.3.1 on hardware (2026-10-14): after the soft reset of the flash the clock
showed "kept across restart", then "NTP" about 30 s later; a daily cap of 7 was
refused again; "Portions Counted (24 h)" set from 8 to 0 in Home Assistant,
logged as a warning ("set by hand: 8 -> 0").

Push button (GPIO2 to GND, `packages/button.yaml`, 2026-10-14): a short press
fed one portion; presses inside the minimum gap were refused (TOO_SOON) without
moving; a 3 s press turned calibration mode on and another turned it off; a
short press after the gap fed again. Some refusals came in pairs 0.3 to 0.4 s
apart: the test used a bare wire, not a switch, so those were real repeated
contacts. The 50 ms filter stays; recheck with the real button if presses
double up.

Food (owner, 2026-10-14): hard food dispenses cleanly, and the tray holds its
position with food in it. Flake food is finicky: it sticks to the sides and
bottom of the slots while the disc turns. Flake is not advised (README).

0.3.3 on hardware: flashed over USB with a new API key (OTA could not be used for
the key change, since OTA is encrypted with the old key); the API connects with
the new key; "Edited Profile" and "Active Schedule" report; "Needs Food" off with
6 portions left and nothing scheduled. Needs Food on hardware (owner, from Home
Assistant): with a 21:00 x2 event and the tray declared at 5 (1 left), "Needs
Food" went to Problem and Faults read "next feed needs more portions than
remain; schedule needs more portions than the tray holds before the next
refill"; "Schedule Over Capacity" on. Next Feed showed tomorrow 21:00, correctly:
that event slot had already fed today during testing (once a day per event).
Disabling the event cleared it within a second: Needs Food OK (nothing
scheduled, so 1 portion is enough), Schedule Over Capacity off, Next Feed none,
Tray Empty On "not within 30 days". Over-cap warning on hardware: with events
21:00 x2 and 12:00 x3 and a daily cap of 3, Edited Profile read "A: 21:00 x2,
12:00 x3 [active] ! 5 portions/day > cap 3" and Faults added "schedule feeds
more per day than the daily cap allows (some events will be refused)". Every
v0.3.3 feature has now been seen on hardware. (The summary lists events in slot
order, not time order; a possible tidy-up.)

## BLE protocol 2 on hardware (2026-10-14, firmware 0.4.0)

From a Linux PC with the reference client, never feeding: Info shows protocol 2
and capability bit 6; the Auth nonce is 16 random bytes; Status reads as zeros
before AUTH; an unsigned command and a record write are refused (not
authenticated); AUTH with a wrong key is refused, the nonce then changes, and even
the right key is refused within a second; the right key is accepted after that.
Authenticated: Status, Config and Profile reads, a Config write answered with its
sequence number, SET_TIMEZONE UNSUPPORTED, HOLD and its exact retry (the stored
answer replayed), and refusals for a stale sequence number, a tampered byte, a
write signed for another characteristic, and a proof for an earlier
connection's nonce. 22/22. `sc_bond` did not get in the way of the client.

## Home Assistant time on hardware (2026-10-14, firmware 0.4.0)

`time_homeassistant` package enabled, timezone pinned (`!extend ha_time` with
`timezone`). After a hard power cycle (clock lost), the log showed "Clock set by
Home Assistant" first, which made the clock valid, and "Clock set by NTP" 4.4 s
later, so the source then reads NTP (last sync wins, as designed). Home
Assistant alone is therefore enough to set the clock when the internet is down.
In normal running both re-sync every 15 minutes within seconds of each other, so
"Time Source" almost always shows NTP.

## Firmware 0.5.0 on hardware (2026-10-14)

- **Settings migration:** before flashing, every Config key, all seven
  calibration offsets, the pulse range and all three profiles were read over BLE;
  after flashing 0.5.0 over WiFi the board logged "Settings migrated from version
  3 (schedule, guard settings and calibration kept)" and the same read-out was
  byte-for-byte identical (pulse 637 / 2675 us, offsets 0, -0.7, 0, +2.5, +3.3,
  +2.3, 0, daily cap 3, profile A events 21:00 x2 and 12:00 x3).
- **Publish on change:** state messages on the API fell from about 680 a minute
  (0.4.x, 11,082 in 16 minutes) to about 10 a minute (only the diagnostics
  sensors); a hold set and cancelled over BLE appeared in "Hold" within 0.25 to
  0.35 s. Loop time 19 to 20 ms, down from 26 to 30 ms.
- **BLE client:** `config get all`, `calibration get`, `profile X show` and
  `selftest` (14/14) against the board.
- **Repeated-AUTH fix:** authenticate, HOLD, cancel, replay the original AUTH
  bytes (answered OK), wait 31 s so the retry cache cannot answer, replay the old
  HOLD: refused as stale (NOT_AUTHORIZED detail 2), hold still off. Before the
  fix the replayed HOLD would have run again.

## Firmware 0.5.1 on hardware (2026-10-14)

- `selftest` with an indefinite hold set: 14/14 and "nothing changed on the
  feeder"; Status still showed the hold afterwards (0.5.0's selftest would have
  cancelled it).
- Client input errors: `profile A event ... sunshine` answers "unknown day
  'sunshine'" without touching the feeder.

## BLE engine refactor on hardware (2026-10-14)

After moving the protocol into `BleEngine` (no protocol change), flashed over
WiFi: `selftest` 14/14; the repeated-AUTH replay test refused the old write as
stale; the full Config / Calibration / Profile read-out matched the earlier one
exactly.

## Firmware 0.5.2 on hardware (2026-10-14)

Settings now go through FeederCore's validated setters. Through the ESPHome API
(as Home Assistant does it), Daily Portion Cap was changed 3 -> 4 -> 3 and each
change was applied and published; BLE `selftest` 14/14 (its Config writes go
through the same setters).

## Release 1.0.0 (2026-10-14)

Flashed over WiFi: the board reports Firmware Version 1.0.0, Faults "none", tray
loaded; BLE `selftest` 14/14. Builds: no warnings; flash 53.4% (no BLE) and
71.4% (BLE) of the 1.8 MB app slot; static RAM 33.7% / 42.7%.

## Firmware 1.0.1 on hardware (2026-10-15)

Built from the new layout (the owner's config in the ESPHome config folder, next
to the clone as `precision-feeder/`: `ble.yaml` plus the button package, the
timezone `<-06>6` and the Home Assistant timezone pin) and flashed over WiFi:
the board reports Firmware Version 1.0.1, Faults "none", tray loaded; BLE
`selftest` 14/14 (settings read back unchanged: pulse 637 / 2675 us, offsets 0,
-0.7, 0, +2.5, +3.3, +2.3, 0, daily cap 3). Builds of both examples in that
layout: no warnings; flash 979,947 B (53.4%, no BLE) and 1,309,291 B (71.4%,
BLE) of the 1.8 MB app slot; static RAM 33.7% / 42.7%, the same as 1.0.0. Not
watched on hardware: the "Clock set by ..." lines now being DEBUG (they come
once per 15-minute sync; the change is a log level only).

## The web app on hardware (2026-10-15, firmware 1.1.0 in progress, after M7)

A second board, new from the box: an ESP32-S3 with 4 MB embedded flash and 2 MB PSRAM (the same chip as the
Waveshare ESP32-S3-Zero H4R2), a bare MG90S servo on GPIO1, no printed tray. `examples/ble.yaml` built with
ESPHome 2026.9.1 and flashed over USB (native USB Serial/JTAG). Joined a private IPv4 network on another subnet
from the computer used for testing.

- **Boot:** no saved data (expected on a new board), Wi-Fi connected, the clock set by NTP about 20 s after
  boot, scheduler running.
- **Serving:** the index page is ESPHome's, loading `/0.css` and `/0.js`; the app arrives gzipped (33 KB of
  script, 5 KB of styles), with no internet involved. `/events` sends the config ping and every entity.
- **Every page in a browser on the device:**
  - Feed now (the servo moved, the dial went to the next slot, the guard went from "Ready to feed" to "Too soon").
  - An immediate second feed, refused with "too soon".
  - An event edited and then restored (ordered writes, each confirmed, the result checked against Schedule
    Data); Today's Feeds showed the edited event as next.
  - A one-shot in the past refused by the feeder, one for the next day added, then cleared.
  - The calibration walkthrough (start, slots 3 and 4, nudges of 1 and 0.1 degree, back to 0, finish and
    save), with the servo following each step.
  - Configuration, Maintenance and Diagnostics showing the device's values.
  - Bluetooth showing the BLE build waiting for a client, with counters at zero and the activity endpoint
    answering.
  - The "schedule needs more portions than the tray holds" banner when a test event made it true.
- **Response times:** a switch change appears on `/events` about 0.1 to 0.2 s after the request. An event edit
  (editor, time, portions, enabled, final check) took about 4 s.
- **Found:**
  - Heap fragmentation is reported with decimals; it is now rounded on the page.
  - ESPHome refuses a POST with no body and no Content-Length (411); browsers always send the length, so the
    app is not affected.
- **Seen once, not reproduced:** in the first calibration run, the page still showed calibration on several
  seconds after Finish, with no confirmation toast. In two later runs both came within 0.1 s. To watch in M9.
- **BLE with a client** (run from the owner's own terminal; the test shell could not use Bluetooth):
  - The selftest (`tools/precision_feeder_ble.py ADDRESS selftest`, which never feeds or holds) passed 14/14.
  - The BLE diagnostics matched it exactly: 1 connection and a disconnect; AUTH 1 accepted, 0 refused; 3
    refused writes (a stale sequence number, a tampered write, a write signed for another characteristic);
    2 invalid (an out-of-range Config value, the reserved SET_TIMEZONE opcode); the exact retry logged as a
    replay with the same answer; 29 writes, 23 reads, 30 notifications with their bytes.
  - MTU 517, negotiated by the client, so the GATT server event hook works.
  - The Bluetooth page showed all of it, the refusals in amber, and the activity log (31 entries).
  - Not seen: the masked client address. It is kept only while connected, and the selftest had disconnected
    before the page was read.
- **Not checked:** web auth on the device, and a phone.

## IPv6 on hardware (2026-10-15, firmware 1.1.0 in progress, after M8)

The same board, updated over Wi-Fi (encrypted OTA with the API key, 13 s upload) to the M8 build with
`enable_ipv6: "true"`, on a dual-stack network (DHCPv4 plus router advertisements with a global prefix and a
ULA prefix), reached from a computer on another subnet of the same network.

- **Addresses:** after boot the feeder had four, all reported by `wifi_info`: IPv4, IPv6 link-local, a global
  IPv6 address and a ULA. Diagnostics listed each with its kind.
- **Web app over IPv6:** by the global address and by the ULA, the index page, the app (33 KB gzipped) and
  `/events` all answered. In a browser at the global address the app connected, and a setting written from it
  was confirmed (then put back).
- **mDNS:** `precision-feeder.local` resolved to the IPv4 address and all three IPv6 addresses, and the web app
  answered by name over IPv4 and over IPv6.
- **IPv4-only and IPv6-only networks** were not tried (the fallback for IPv6-only is a static IPv4 address;
  README, Limits).

## Several browsers at once (2026-10-15, firmware 1.1.0 in progress, M8 build)

Measured on the same board from one browser page plus extra `/events` connections opened from it.

- **Heap:** about 0.8 KB per extra connection (146.6 KB free with the page alone, 144.1 KB with three more).
  Memory is not the limit.
- **Connections:** ESPHome configures the ESP-IDF HTTP server to close the least recently used connection when
  its sockets run out. With the page plus six extra event streams, only about half stayed connected at a time;
  the rest kept being closed and reconnecting. Page loads still answered (74 ms), and the app's own stream stayed
  online. In practice about four pages can stay connected at once. Since M9 a page hidden for 30 s releases its
  connection.

