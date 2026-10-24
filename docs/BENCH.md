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

## Web auth on hardware, and the 1.1.0 bench status (2026-10-15)

A test build of the 1.1.0 firmware with `web_server: auth:` added (the package keeps it commented out), updated
over Wi-Fi, then the normal build put back.

- With auth on (basic, ESPHome 2026.9.1's default): every route answered 401 without credentials or with a wrong
  password, and 200 with the right ones. That includes the index, the app files, writes, `/events` (which
  streamed every entity) and the BLE activity endpoint `/feeder/ble` (registered with `add_handler()`, so it
  sits behind auth as intended).
- 1.1.0 has been checked on hardware:
  - every page;
  - BLE with a client;
  - a dual-stack network with IPv6, and mDNS;
  - encrypted OTA;
  - several browsers at once;
  - web auth.
- **Still for the owner to check** (they need changes to the home network or Home Assistant):
  - an IPv4-only network;
  - an IPv6-only network with a static IPv4 address;
  - Home Assistant connected and adopting the feeder, alongside the web app.


## Firmware 2.0.0 N5 and N6 on hardware (2026-10-16)

Waveshare ESP32-S3-Zero (4 MB, 2 MB PSRAM), flash erased by the owner, then a build shaped like the planned
prebuilt binary: `examples/ble.yaml` plus `api_key_on_device` and `name_add_mac_suffix: "true"` (78.6% of the app
slot). Flashed over USB. The checks were scripted: the web app's own code (the served bundle, re-run on the feeder's
page in a browser), Python for the API, OTA, Improv and signed requests, and the BLE client from a terminal.

Passed:
- **Fresh feeder:** named `precision-feeder-add020` (MAC suffix); `GET /feeder/network` reports setup needed, and the
  app opens the setup wizard by itself. Still needed after a reset (once fixed, below).
- **Network endpoints:** bad values answered 400; with a settings password, an unsigned change 403, a wrong key 403
  then 429 during the wait; signed: the name and the access-point password (masked) saved, Restart Needed on.
- **Setup flag:** done, offered again, done, and still done after a restart.
- **Time zone:** the Berlin rule set through the Timezone entity; Device Time showed +02:00.
- **Restart:** the feeder came back as `pf-bench-test.local` (mDNS), the access-point password from the device, the
  settings password kept.
- **Keys made in the web app:** an API client connected with the new key and saw the new name (86 entities);
  plaintext and a wrong key were refused. BLE selftest with the new BLE key: 14/14. With BLE Can Change Guard Limits
  off (a signed write), `config set daily_cap` was refused with detail 7 and Reload Pause still accepted.
- **Plaintext OTA** refused ("Authentication invalid"), as the runtime-key build intends.
- **Wi-Fi from the web app:** the same network again: answered, dropped after 3 s, back after 11 s. An unreachable
  network: off the LAN and stayed off.
- **Improv serial** (a script speaking the protocol over USB): device info with the saved name, the current URL, and
  sending the real network brought the feeder back from the unreachable one, twice.

Found and fixed during the run:
- **HTTP 500 instead of 403/429:** ESPHome's ESP-IDF request maps only a few status codes and sends the rest as 500.
  Every settings-password refusal (since N2) arrived as 500. Now sent through ESP-IDF directly.
- **Setup wizard lost after one restart:** a new feeder saves settings soon after its first boot, and the next boot
  took it for an update from 1.x. The decision is now made once, at the first boot, and saved.
- **Joining another network did nothing while connected:** ESPHome's `save_wifi_sta()` only connects when idle. The
  feeder now drops the link a second after answering, and ESPHome's reconnect joins the saved network.
- **BLE name with a MAC suffix:** ESPHome refuses a BLE name over 13 characters when it adds the suffix; the bins
  need a short `ble_name` (N7).
- **The BLE client's `config set`** never worked: the `--key` option overwrote the setting's name.

Not checked: the access point's SSID on air (macOS redacts scanned SSIDs), the DHCP host name, Home Assistant
adoption and renaming, encrypted OTA, ESP Web Tools itself. Two harness notes: Node could not reach the LAN here
(macOS Local Network permission), and Node's `fetch` sends a body-less POST with no Content-Length, which the
feeder's server waits on (browsers send `Content-Length: 0`).

Left on the board: the build's name and access-point password, no settings password, no API key, the BLE key made
during the test (a BLE key cannot be cleared, only replaced), setup offered again, the Wi-Fi network the YAML has.

## Firmware 2.0.0 N8 on hardware, part 1: upgrade from 1.1 (2026-10-16)

Waveshare ESP32-S3-Zero, `examples/ble.yaml`. Flash erased, `v1.1.0` flashed over USB, then given non-default
settings (guard limits, reload pause, refill interval, location, servo pulse range 560 to 2380 µs), calibration
offsets (slot 3 +1.5°, slot 5 −0.8°), an event in profile B and a fed tray (slot 1, 5 left). Then the 2.0.0 YAML
build of the same file over ESPHome OTA.

- **First try: rolled back to 1.1.0.** The 2.0 build crashed in its first minute and ESPHome's safe mode rolled the
  OTA back ("reset before the boot was marked successful"). The crash was a **stack overflow in the web server's
  task** on the first protected write (here, the test turning Calibration Mode on). It was not about the upgrade:
  every protected write through the web server crashed the 2.0 build, settings password or not (the N5/N6 run
  used signed writes on an earlier build). Fixed: see below.
- **On 2.0** (the feeder came up on a second try, its first boot behind it): Firmware Variant `yaml`;
  `GET /feeder/network` said `"setup":false` (an updated feeder is not sent through the wizard). With the fix,
  protected writes answered 200 in about 0.1 s. The full comparison with the 1.1 snapshot was not made (below).
- **Firmware upload** (`POST /feeder/firmware`, this build's own image, no settings password): a wrong SHA-256 was
  refused with 422 `sha256` and the feeder kept running; an upload cut after a third left it running, and the next
  upload aborted the open session; a good one answered 200 `{"restart":true}`, and the feeder restarted into it.
  About 12 s for 1.4 MB.
- **Stack headroom** (a temporary build logging the web server task's high-water mark): 472 bytes left after a
  protected write, 616 after an upload, 1032 after a refused one.
- **Clean repeat with the fixed build** (erased, 1.1.0, seeded, then the 2.0 YAML build over OTA): every value in
  the 1.1 snapshot was the same on 2.0 (guard limits, reload pause, refill interval, location, pulse range, both
  offsets read back through calibration mode, the schedule, the active profile, the tray at slot 1 with 5 left). Only
  the Wi-Fi network differed: the feeder was moved to a phone hotspot with Improv serial (a script) in between.
- **Upgrade to the prebuilt binary** (erased, 1.1.0 built for the hotspot, seeded, a minute for ESPHome to write its
  preferences, then `precision-feeder-full.factory.bin` written at 0x0 without erasing, as the install page does
  when its erase prompt is declined): it booted as `precision-feeder-add020`, variant `full`, `"setup":false`,
  `"ap":"open"`. Every setting, offset, the schedule and the tray state were kept. **Not kept, as expected:** the
  Wi-Fi network (it was in the 1.1 YAML, not in ESPHome's saved slot; the feeder stayed off the network until
  Improv set it), the API key and the BLE key (both `none`): Home Assistant must adopt it again, and BLE clients are
  refused until a key is made.

Found and fixed during the run:
- **Stack overflow on every protected write** (since 2.0.0 N7, possibly marginal before): the web server's task has
  ESP-IDF's 4 KB stack plus 256 bytes, and the settings guard's request handler, with its URL buffer and every route
  inlined (about 720 bytes), stayed on the stack while it passed the write to the web server's own handler. The
  routes now live in a separate, non-inlined function that returns first; the pass-through costs 32 bytes. The
  firmware upload's body handler had the same pattern while writing to flash, fixed the same way.

## Firmware 2.0.0 N8 on hardware, part 2: a fresh install from the install page (2026-10-16)

The site `tools/make_release.py` makes (install page, manifests, the fixed `full` image) served on
`http://localhost:8123` (a secure context, so Web Serial works), Chrome on macOS, the S3-Zero over USB.

- **ESP Web Tools 10.4.0** found the board, offered to erase, erased and installed the `full` factory image; its
  Wi-Fi step was skipped. The page's version line came from `manifest.json`.
- **Open access point:** `precision-feeder-add020-setup` showed on a phone with no password; joining it opened the
  captive portal's page (owner's check).
- **Improv serial** (a script, after the installer let go of the port): joined the phone hotspot and answered with
  the feeder's address.
- **First boot:** `GET /feeder/network` said `"setup":true`, `"ap":"open"`, `"ap_build_open":true`; no API or BLE key,
  no settings password; Firmware Variant `full`; build defaults (Daily Portion Cap 4). The web app opened `#setup` by
  itself; the wizard's network step said the access point has no password, with a "Strongly recommended" badge and
  "Set a password" as the main button.
- Fixed: the badge stretched across the card; it now sits inline.
- Harness: the Claude desktop app's browser pane has Web Serial, but its port chooser is drawn outside the page,
  so a person picks the port; only one program can hold the port at a time (close the installer before Improv).

## Firmware 2.0.0 N8 on hardware, part 3: the Firmware page and uploads (2026-10-16)

The feeder from part 2 (`full`, on a phone hotspot), a settings password set for the test; uploads signed by a
Python client doing what `auth.uploadFirmware` does (PBKDF2 key, HMAC over the path and the SHA-256).

- **Firmware page** (Diagnostics, "Updates and firmware upload"): "2.0.0, full"; the update check said "Couldn't
  check for updates" with the releases link (nothing published on Pages yet); turning the check off showed "The update
  check is off" and was remembered in the browser; Upload stayed disabled with no file.
- **Refused, the running firmware kept:** no auth header (403 `challenge`); a wrong password (403 `refused`, wait 1 s;
  logged); a file one byte different from the one signed (422 `sha256`, after the whole body); a connection cut after
  a third (still running; the next upload aborted the open session).
- **Factory image: accepted at first.** `precision-feeder-full.factory.bin`, signed, answered 200 and the feeder
  restarted; the bootloader then refused it ("OTA app partition slot 1 is not bootable") and booted the old app, so
  nothing was lost, but it should never have been taken. ESPHome's OTA backend accepts any image that starts with the
  image magic, and a factory image starts with the bootloader. Fixed: the feeder now checks for the app description
  (magic `0xABCD5432` at byte 32) in the first bytes (`FirmwareUpload`, host-tested), and the Firmware page checks
  the same before uploading. With the fix: 422 `image`, nothing switched.
- **Good uploads:** the fixed `full` image over the old one (200, restarted into it); then `no-ble` (Firmware Variant
  `no-ble`, BLE `off`, BLE Info gone) and back to `full`. Every setting and the settings password were kept across
  both switches. About 10 s per upload over the hotspot.
- **Restart waits for the tray:** a feed started 8.5 s into a 9.3 s upload. The log shows the feed starting, then
  "Restarting into the uploaded firmware"; after the restart the tray was at slot 1, loaded, 5 left, no faults (a
  restart mid-move would have left the position unknown).
- The test password was removed afterwards.

## Firmware 2.0.1 N8 on hardware, part 4: the update check without internet (2026-10-16)

The 2.0.1 `full` image (uploaded through `/feeder/firmware`), the Firmware page in the Claude desktop app's browser.
Nothing is published on GitHub Pages yet, so the outcomes were driven by stubbing `fetch` for the manifest's URL in
the page; the rest of the page was the feeder's own.

| Manifest | Page | Time |
|---|---|---|
| 2.0.1 (same) | Up to date (latest release 2.0.1) | 0.4 s |
| 2.1.0 | Version 2.1.0 is available, with the release notes link | 0.4 s |
| 2.1.0 with no image for this variant | Latest release 2.1.0, releases link | 0.4 s |
| 404, or the connection refused | Couldn't check for updates, releases link | 0.4 s |
| No answer at all (a firewall dropping packets) | Couldn't check for updates | 10.2 s |
| The real network today (nothing published) | Couldn't check for updates | 0.4 s |

- Found and fixed: with no answer at all the check had no timeout and would have said "Checking…" for as long as
  the browser waited. It now gives up after 10 s.
- **The feeder's own internet blocked:** not tested. The phone hotspot's per-client block refused the feeder's
  Wi-Fi association altogether (it could not join), so "on the LAN without internet" could not be set up there. By
  design the check never involves the feeder (the browser fetches the manifest), and the feeder's only outbound
  traffic is NTP (and Home Assistant).
- Harness: opening the S3's USB serial port from this Mac resets the board (USB_UART_CHIP_RESET), even with DTR and
  RTS held low; a tab left open across an upload keeps the old app until reloaded (the app's "feeder was updated"
  banner covers that).

## Firmware 2.0.1 N8 on hardware, part 5: IPv4-only and dual-stack networks (2026-10-16)

The 2.0.1 `full` image on a GL.iNet travel router (the phone's hotspot behind it).

- **Dual stack** (DHCPv4 and a ULA prefix): the feeder had IPv4, link-local and ULA addresses; mDNS announced all
  three. Over the ULA address: the web app (index and the 57 KB app), `GET /feeder/network`, a plain and a protected
  write, and a firmware upload (200, restarted into it).
- **IPv4 only** (IPv6 turned off on the router, LAN included; after a restart): IPv4 and link-local only, NTP synced,
  no faults; the app, a protected write, mDNS (IPv4 and link-local) and a firmware upload all worked. On the first
  try the router's "IPv6 off" still advertised its ULA prefix on the LAN; that needed its LAN IPv6 off too.
- **IPv6 only: not tested.** Turning DHCPv4 off would have cut the test machine off as well. Known outcome for the
  prebuilt binaries: ESPHome counts Wi-Fi as connected only with an IPv4 address, and the binaries cannot be given a
  static one (`manual_ip` is build-time), so on an IPv6-only network they never join and open their access point.
  DESIGN.md section 14, item 20.

## Firmware 2.0.1 N8 on hardware, part 6: keys, BOOT and the leftovers (2026-10-16)

The same feeder (`full`, no keys yet, on the hotspot).

- **Adoption as Home Assistant does it** (aioesphomeapi, the library Home Assistant uses): connected with the
  all-zero Noise key (plaintext was accepted too while no key was set), set a new key over the API (true); the web
  app then reported the API key as `device`. Afterwards plaintext ("requires encryption"), the all-zero key and a wrong
  key were refused, the new key connected. Home Assistant itself was not available on this network.
- **Encrypted OTA with the runtime key** (ESPHome's own uploader, `espota2`, with the key as its Noise key): "Encrypted
  connection established", 1.4 MB in 13.7 s, OTA successful, back on 2.0.1. Plaintext OTA was refused (the device
  asks for a password nobody knows).
- **BLE with no key:** AUTH refused with detail 6, which the Python client names ("no BLE key is set on the feeder").
  The client printed a traceback around it; it now prints the one line.
- **BOOT held 10 s** with a settings password set (S3-Zero, owner): the password was cleared (scheme 0, Settings
  Password off); the API key, the settings and the Wi-Fi network were kept, and the feeder kept running.
- **DHCP host name:** `precision-feeder-add020` in the router's client list (owner).
- Not checked: Home Assistant itself adopting and reconnecting after a rename; the BLE name on air (macOS showed a name
  cached from an earlier run); a new BLE key ending a connected session.

Left on the board: 2.0.1 `full`, on the phone hotspot, the API key from this test (not in Home Assistant), no BLE
key, no settings password, setup offered, IPv4-only router.

## Release 2.0.1 (2026-10-16)

Tag `v2.0.1`: the Release workflow checked, built both variants and published the GitHub release (four images and
`manifest.json`; notes from CHANGELOG.md with the unreleased 2.0.0 section and absolute links) and GitHub Pages (the
install page, the install manifests, the factory images, `manifest.json` with `Access-Control-Allow-Origin: *`). The
bench feeder's Firmware page, reading the live manifest: "Up to date (latest release 2.0.1)".

## Firmware 2.0.2 on hardware: NTP servers (2026-10-16)

The owner's bench feeder, set up from a fresh, erased install of the released 2.0.1 `full` (saved name, timezone
`CST6`, location, setup finished, no keys or password), on a GL.iNet travel router whose own NTP server answers at
192.168.8.1. The 2.0.2 `full` image uploaded through `/feeder/firmware` (200, restarted). The log was read from the web
server's `/events`.

- **Device record v1 → v2:** after the upload every value matched the 2.0.1 snapshot (name and saved name, access
  point `open`, setup done, password scheme, keys, timezone, board and the three pins, both BLE permissions, a guard
  limit, the location, the Wi-Fi network); only the version and the time source ("kept across restart" until the
  next sync) differed. NTP Servers empty: the build's.
- **A local server:** with an unreachable server (192.0.2.1) the clock went 2 minutes without a sync; set to
  `192.168.8.1` it synced 3 s later (Minutes Since Clock Sync back to 0, Time Source NTP). Logged "NTP servers set to
  192.168.8.1" and "NTP: asking the servers set on the feeder".
- **Unreachable server:** 150 s with no sync, as expected; the feeder kept running on its clock.
- **Refused:** four servers ("more than three servers", logged); the saved list stayed.
- **Back to the build's** (empty): "NTP: asking the build's servers", synced 5 s later.
- **Kept across a restart:** set to 192.168.8.1, restarted: the setting was kept and the clock synced from it (the
  only server set).
- Left on the feeder: the build's servers (as before the test), 2.0.2.

## Firmware 2.1.0 on the servo (2026-10-21)

The mechanism interface (DESIGN.md section 3.0) on a servo test rig: Waveshare ESP32-S3-Zero, MG90S with its horn, no
printed tray. The board ran the prebuilt 2.0.2 `full` binary; 2.1.0 was built exactly as its prebuilt binary
(`firmware/full.yaml`, ESPHome 2026.9.1) and flashed over USB, which leaves the settings partition as it is: an
upgrade of the saved data, not a fresh device.

- **Upgrade: everything kept.** Every entity's state was recorded from the web server's event stream before and
  after. The only differences: Firmware Version 2.0.2 -> 2.1.0, the two new diagnostics (Mechanism `servo`,
  Capacity 6), and the time zone the clock was shown in (UTC before, the local -06:00 after the restart, as Home
  Assistant's time zone arrived; no time zone is saved on this device). Kept: tray position 2, the pulse range
  637-2675 us, all seven slot offsets (0, -0.7, 0, +2.5, +3.3, +2.3, 0 degrees, read one by one before and after),
  daily cap 2, minimum gap 60 s, the scheduler and its 18:00 event, the pins.
- **Feeding:** a feed moved slot 2 -> 3 (result ok, remaining 3). Tray Reload went back to slot 0 (remaining 6).
- **Calibration:** calibration mode on, slot 3 shown with its +2.5 degree offset, a feed refused (calibrating);
  mode off left the tray loaded at slot 0 (the servo returns to its slot, so the position stays known).
- **Guard:** a feed straight after another refused (too soon); after 60 s a second one fed; a third refused (daily
  cap reached, cap 2).
- Not run this time: the BLE selftest (it needs the Terminal panel), a power cut mid-feed, the web app on the device.

## The direct-drive disc, first run (2026-10-21, the disc's branch build, 2.1.0-n20.1)

The disc firmware (DESIGN.md section 3.3) on the owner's bench rig: Waveshare ESP32-S3-Zero, DRV8833, GA12-N20
(35 rpm, encoder), wired as in packages/mechanism_n20.yaml, the 15-slot disc fitted. Built from `examples/basic.yaml`
(with diagnostics) under its own name, `precision-feeder-disc`, flashed over USB; it replaced the bench sketch.

- **It runs.** Mechanism `n20-direct`, Capacity 14, no faults at boot. The bridge stayed off until asked.
- **Encoder through PCNT:** counts up when driven forward, as the sketch's interrupts did (direction test: 228 ticks
  in the 120 ms push). Which way the disc turned was not watched; the answer Yes was given on the strength of the
  same wiring and the same forward as the sketch. **To check by eye.**
- **One full turn: exactly 8344 ticks** (Run Turns from a zero, no error). The sketch's count holds.
- **Feeds:** nine feeds, each one slot and the wiggle; the three checked against the grid ended -1, +2 and -1 ticks
  from it (at most 0.09 degrees; the sketch's endurance run: within 5). All ok, remaining counted down, no faults;
  the guard's daily cap (4) refused the fifth until it was raised for the bench.
- **Timing: slower than the sketch.** Slot move about 1.19 s (sketch 0.745 s), whole feed with the wiggle about
  3.93 s (sketch 2.49 s), from the new Last Feed Move Time and Last Feed Time diagnostics. Not yet explained; a
  guess is that the sketch's loop (delay(1) plus its body) ran a little slower than the firmware's exact 1 ms timer,
  which lengthens each creep pulse. To try: a longer starting creep pulse (pulse_start in the YAML).
- **Control loop (S2.0):** Motor Loop Max Gap is about 52 ms at every boot and does not grow afterwards, through
  moves, a full turn and the web server under load (two threads of requests during a feed). So the one long gap is
  during start-up, before anything can move; while running, the 25 ms watchdog was never reached.
- Not run yet: the calibration with a slot lined up by eye, real food, a power cut mid-move, a held disc and an
  unplugged encoder, STBY at power-up, the 300-cycle endurance run.

## One firmware with both drives, on both rigs (2026-10-21, a test build of 2.1.0)

The disc and the servo in one firmware, the drive chosen on the device (D17), built as `firmware/full.yaml` is with
the disc package added; flashed over USB to both rigs. Carrying both drives cost 4.4 KB of flash.

- **Disc rig:** it booted as the disc and fed. **Drive → Servo**, then Restart: it came up as the servo, capacity 6,
  the tray position unknown and feeds refused until a reload. The disc's driver was never set up (the encoder stayed
  at 0, its control loop never ran). A reload and a feed in servo mode sent servo pulses down GPIO1, the bridge's
  AIN1, and the bridge stayed asleep (STBY low). **Drive → Direct-drive disc**: capacity 14 again, the calibration
  kept (8349 ticks per turn), and after a reload it fed normally.
- **Servo rig** (from plain 2.1.0): Drive set to Servo; it fed normally (slot 0 -> 1).
- **Found and fixed:**
  - A daily cap of 14 became 6 for good after a round trip through the servo: the saved limits were clamped to the
    drive's capacity at boot and saved. Saved limits are now kept as set and held to the capacity only when used; a
    second round trip kept 14 (and Max Portions Per Command 3).
  - "Servo Pin: Invalid option none" logged every second in disc mode: the servo's pin is now published only while
    the servo is the drive.
  - The first boot after a single-drive firmware trusted the old tray position (that firmware never recorded its
    drive). An unrecorded drive now counts as a change, so the tray must be reloaded (host-tested; both boards were
    past that boot).
- One disc feed took 2.3 s (whole feed), close to the bench sketch's 2.5 s, where the first run's took about 3.9 s.
  Not explained yet.

## BLE and MQTT on the disc (2026-10-21, a test build of 2.1.0)

The disc rig with the two-drive firmware, BLE and (second build) `packages/mqtt.yaml`; the disc the drive.

- **BLE:** it advertised as "Feeder Disc"; the Python client's selftest passed 13/13 (protocol 2, Info positions 15,
  every Config key readable, idempotent retries, a tampered or replayed write refused). Status: 14 portions, loaded,
  clock from NTP.
- **MQTT, against Mosquitto 2.1.2 with an account (anonymous refused):** 88.4% of the app partition, 48.0% of RAM.
  - Set up in the web app's MQTT card. Broker, username and password took effect without a restart (the client
    reconnected to a changed broker at once); MQTT off stopped the attempts.
  - The feeder's network (an IoT VLAN) first could not reach the broker on another VLAN: a firewall rule for MQTT
    from that VLAN fixed it. Nothing on the feeder's side.
  - 120 Home Assistant discovery configs (44 sensors, 23 numbers, 21 buttons, 14 selects, 7 switches, 6 texts,
    5 binary sensors), `<name>/status online`; none for the MQTT settings themselves, and the password never
    appeared on the broker.
  - ESPHome's device discovery: `esphome/discover` answered with the IPv4 and IPv6 addresses, API port and MAC.
  - Feed One Portion pressed over MQTT fed one portion (14 -> 13, 2.3 s); two presses at once fed one, the second
    refused by the minimum gap.
  - After a restart the settings were kept and it reconnected by itself. The broker saw the old session "taken over"
    rather than an `offline` message.
  - Found: ESPHome's default discovery unique IDs (`ESPsensortray_state`) are the same on every feeder. The package
    now uses the MAC (`discovery_unique_id_generator: mac`).
- The web app's MQTT card did not show at first: the firmware had been compiled before the web app was rebuilt.
  Rebuilt and reflashed, it showed (a browser holding the old app needs a reload).

## Release builds of 2.1.0, and 2.1.1 (2026-10-22)

The images from a manual run of the Release workflow (GitHub Actions, ESPHome 2026.9.1), checked against the
manifest's SHA-256, uploaded through the web app's firmware upload (`POST /feeder/firmware`, no settings password set).

- **Servo rig** (from the two-drive test build): `no-ble` then `full`. Each came up as 2.1.0 with its variant,
  Mechanism `servo`, Drive Servo, the settings kept; a feed each (6 -> 5, 5 -> 4); the MQTT entities and the web
  app's MQTT card present; BLE absent in `no-ble`, present in `full` (no key yet, as a prebuilt image starts).
- **Disc rig** (from a YAML build with its Wi-Fi in the YAML): the `no-ble` image has no network, so it came up
  as its setup access point; Wi-Fi was given back over USB (Improv). Then: Drive still the disc, the calibration
  kept (8349 ticks per turn, 15 slots, trim -0.7 degrees), the daily cap 14 and max per command 3 kept. **A feed
  took 2.46 s** (the move 756 ms), the bench sketch's time; the first run's 3.9 s is not seen with this build.
  - The button came up off: the prebuilt's button default, GPIO2, is the disc's IN2 (the 2.1.0 rule); Button Pin
    set to GPIO3 on the feeder, after a restart it was in use.
  - `full`: the BLE key saved on the feeder (Configuration, Keys); the selftest passed 13/13.
  - MQTT against Mosquitto: connected, 124 discovery configs, unique IDs from the MAC. **Found:** the topics used
    the build's name (`precision-feeder-b00604`), not the name saved on the feeder (`precision-feeder-disc`). Fixed in
    2.1.1.

## Release 2.1.1 on both rigs (2026-10-21)

The 2.1.1 images from the Release workflow (manual run), checked against the manifest, uploaded through the web app's
firmware upload; `no-ble` then `full` on each rig.

- **Servo rig:** each came up as 2.1.1 with its variant and fed after the upload's restart (three feeds on `full`,
  two on `no-ble`, all ok, uptime unbroken). An earlier run of the same steps rebooted the board at each feed: the
  servo's supply capacitor had fallen off, so the servo browned the board out at the start of a move. With it back,
  no resets; the saved state then showed "unfinished move", as designed after a reset mid-move.
- **Disc rig:** each fed after the upload's restart (2.33 s and 2.25 s, whole feed), the drive, calibration, pins
  (button GPIO3) and BLE key kept. MQTT: topics under the name saved on the feeder (`precision-feeder-disc/`, the
  2.1.1 fix), `offline` then `online` across the restart, every discovery config under that prefix, a feed over MQTT ok (9 -> 8).
  BLE selftest 13/13. Left on `full` 2.1.1.

## Firmware 2.2.0 on the disc rig (2026-10-22, bench builds)

Who can change what (docs/plans/2.2.0-scopes.md), on the disc rig with the owner's test Home Assistant (a Docker
container, the ESPHome integration) and a Mosquitto broker with an account.

- **Home Assistant:** on 2.1.2 the device had 124 entities; after the upload of 2.2.0 it had exactly the 20 of the slim
  set, with live states, and **the integration removed the other 104 by itself** (none left as unavailable). A feed
  from Home Assistant worked (the owner's test).
- **MQTT:** discovery exactly the 20, plus the 72 retained state topics of the feeder's own (M3). Schedule commands
  applied and answered; the daily cap refused with MQTT Can Change Guard Limits off ("not allowed"), applied with it on,
  out of range refused; one-shots added and cleared; a calibration topic ignored; a change in the web app reached MQTT
  within a second. After a restart, MQTT reconnected and republished.
- **MQTT settings and restarts (the 2.1.2 fix):** with MQTT started at boot, a port change kept it connected on the old
  settings and turned Restart Needed on; after the restart it connected with the saved ones. Off took effect at once;
  on again set Restart Needed. With MQTT off since the boot, setting the broker and login and turning it on connected at
  once.
- **Web app:** every page and card complete with the internal entities (the event stream carries all 135); the MQTT
  card's new switch sets the device's flag both ways.
- **BLE:** selftest 13/13.
- **Headless:** the disc example without the web app, with BLE (75.3%), uploaded through the web app's firmware upload:
  no web server; BLE status, selftest 13/13 and a feed over BLE alone. Building it found that **no build without the
  web app validated** (captive_portal auto-loads ESPHome's web OTA, which the feeder took for the web app): fixed in
  2.1.2, and check.sh now checks a headless build.
- Then the full 2.2.0 image went back over USB.
- The servo rig and the release images: next section.

## Release images of 2.2.0 on both rigs (2026-10-22)

The images from a manual run of the Release workflow on `release/2.2.0` (ESPHome 2026.10.0), checked against the
manifest, uploaded through the web app's firmware upload. (The first run failed in CI: GCC's -Wcomment on a backslash
at the end of a comment in tests/test_mqtt.cpp, which the Mac's clang does not flag; fixed.)

- **Servo rig, 2.1.1 to 2.2.0 `no-ble`:** every value read before and after was identical: tray position 5, the five
  feeds in Recent Feeds and the 24 h count (the tray state migrated from version 2 to 3), the pulse range 637-2675 us,
  the limits, Drive Servo. A feed emptied the tray; Tray Reload showed **moving** with the guard **busy** while the
  servo turned, then loaded with 6; the next feed was refused by the daily cap (the five migrated feeds plus one). No
  resets (the capacitor in place).
- **Servo rig, `full`:** two feeds (6 -> 5 -> 4), uptime unbroken; BLE up with no key (as a fresh prebuilt starts).
- **Disc rig, `full`** (its final state): the drive, the tray, MQTT connected again, the BLE key kept; BLE selftest
  13/13.


## Firmware 2.3.0 on the disc rig (2026-10-22, bench build)

The `full` image built from `release/2.3.0-loose-ends` (M6 of docs/plans/2.3.0-loose-ends.md), flashed over USB from
2.2.0 `full`. The servo rig is shelved (D32).

- **Upgrade:** all 136 entities read before and after: every value the same (drive, calibration, tuning, limits, pins,
  tray position, the 24 h count, keys), except the expected ones: MQTT Can Change Guard Limits is gone (D28), and the
  encoder count and heap figures restarted with the boot. `/feeder/network` reports `"forget":true` (the name and
  Wi-Fi were set on the device).
- **Wi-Fi scan while connected (D31), five runs:** no request failed (polled every 0.3 s), no disconnect in the log,
  and the results were fresh (the signals changed). A scan took about 9 to 10.6 s, longer than the 10 s the first
  build allowed before it stopped reporting "scanning": raised to 15 s (the web app waits as long). While it runs, a
  request to the feeder can stall for up to about 3 s (the radio is off the home channel), then completes. 20
  networks listed (the cap), strongest first.
- **Feed times (open item 23):** the first feed after a restart took 2,291 ms (the move 613 ms); later feeds 2,266 ms
  (move 631 ms). The first run's 3.9 s is not seen (as with 2.2.0).
- **Not run this time:**
  - Forget Wi-Fi on the rig: putting the network back needs its Wi-Fi password over Improv, which needs the owner.
  - Clearing the BLE key: the rig's key is not the one in `examples/secrets.yaml`, so it could not be put back.
  - MQTT `policy/*/set` refused: MQTT is off on the rig; the host tests (tests/test_mqtt.cpp) cover it.
  - A power cut mid-move and a held disc.
- Left on this 2.3.0 bench build, with its settings, BLE key and Wi-Fi as before.

## HTTPS, ACME and the BLE stacks on the disc rig (2026-10-23, scratch bench builds)

M0 of [docs/plans/https-tls.md](plans/https-tls.md) and [docs/plans/nimble.md](plans/nimble.md), in one run. Five
scratch images, none of them a release, each shaped like `firmware/full.yaml` (components of 2.3.1, ESPHome 2026.10.0,
ESP-IDF 5.5.6), flashed over USB from 2.3.0 `full`. The whole 4 MB flash was read first and written back at the end:
the rig is on its 2.3.0 image again, settings, keys and Wi-Fi as before (only one NVS sector differs, written by the
first boot).

Every image carries a scratch heap probe and a scratch TLS layer (about 12 KB of heap at idle). The probe samples the
internal heap every 20 ms; `GET /lab/heap` returns its lowest point since the last read. The TLS layer is a proxy on
443 relaying to the web server on 80, TLS 1.2 only, ECDSA P-256, at most 4 sessions, session tickets, lean buffers
(16 KB in, 4 KB out), upload checks and an ACME client. The load ran from the Mac over Wi-Fi. A "session" is one
connection holding `/events` for 20 s, the way an open page does.

| Image | Flash | Static RAM | Free at idle | BLE costs |
|---|---|---|---|---|
| No BLE | 1,331,547 (72.6%) | 134,167 | about 201 KB | |
| Bluedroid, as ESPHome builds it (today's `full`) | 1,681,367 (91.6%) | 165,367 | about 110 KB | about 91 KB |
| NimBLE, ESP-IDF's defaults (the scratch stub) | 1,527,727 (83.3%) | 159,099 | about 110 KB | about 91 KB |
| Bluedroid, controller trimmed (`esp32_ble: max_connections: 1`, 2 controller activities instead of 6, no scanning) | 1,659,307 (90.4%) | 156,327 | about 125 KB | about 76 KB |
| NimBLE trimmed (1 connection, small buffer pools, no BLE 5 extras, the same controller trim) | 1,478,143 (80.6%) | 146,443 | about 143 KB | about 58 KB |

The stock 2.3.0 `full` (no lab code) read 123.4 KB free at idle, and 120.8 KB with a BLE client connected.

Lowest free heap during the load (KB, several runs where repeated):

| Image | 1 session | 3 sessions | 4 sessions | 10 requests at once |
|---|---|---|---|---|
| No BLE | 181 | 137 | 127 to 129 | 124 |
| Bluedroid, today | 90 | 44 to 52 | 11 to 33 | 15 (the all-time low reached 3) |
| NimBLE, defaults | 89 | 50 | 33 | 10 (all-time 3) |
| Bluedroid, trimmed | 106 | 67 to 69 | 51 to 52 | 50 (all-time 35) |
| NimBLE, trimmed | 121 | 86 to 88 | 68 to 73 | 66 (all-time 52) |

- **BLE is the largest user of heap, not TLS.** Either stack costs about 91 KB with its defaults. NimBLE saves nothing
  until it is trimmed: its defaults (3 connections, 24 ACL and 30 event buffers, BLE 5 features) are larger than
  ESPHome's Bluedroid. Trimming the controller saves about 15 KB on Bluedroid. NimBLE, trimmed the same way, saves
  about 18 KB more (58 KB in all, in line with the 60 to 88 KB quoted on Espressif's forums).
- **A TLS session costs about 15 KB on top of the plain connection it relays** (one plain `/events` costs about 5 KB,
  one over TLS about 19 to 22 KB). Four sessions take about 72 KB. The low point varies with how many handshakes
  overlap.
- **Four TLS sessions are not safe on today's build:** the heap went down to 3 KB once. Three leave 44 to 52 KB.
  Trimmed NimBLE leaves 65 KB or more with four.
- Handshakes: 0.33 to 0.6 s one at a time, 0.7 to 1.4 s with four at once, 0.1 s resumed (tickets work). 100
  handshakes in a row: no leak (0.6 KB lower after, within the noise). All 10 parallel requests answered (the cap
  queues the rest).
- **ACME against a local step-ca works:** http-01, the board's mDNS name, 23.7 s from start to certificate. The
  certificate and its intermediate are served after a restart, and curl verifies them against the CA's root alone.
  It needed a firewall rule from the IoT network to the CA (443), and mDNS on the CA host to resolve the board's name.
  The step-ca default lifetime is 24 hours, so renewal will run about every 16 hours.
- **BLE, Bluedroid:** advertises the 128-bit service UUID and the name with the MAC suffix; the Device Information
  Service and the 9 characteristics; MTU 517 with macOS. The NimBLE stub advertised its name only, so the client could
  not find it by service UUID, and one byte of its UUIDs was in the wrong order: both are scratch bugs, and B5 and B2
  of the NimBLE plan are there to catch them. macOS kept the same device identifier across the two stacks.
- One NimBLE run with a BLE client connected was disturbed by another HTTPS client (relay tasks the load did not
  open), so there is no clean "NimBLE, BLE connected" figure. Not run: MQTT over TLS, PSRAM for TLS, pairing from
  iOS or Android.

## Firmware 2.4.3 and 2.4.4 on the disc rig (2026-10-23)

Open item 27 (DESIGN.md section 14), on the disc rig (ESP32-S3, `precision-feeder-disc`, the servo-less direct-drive
build), with the owner's go-ahead. The whole 4 MB flash was read first. The release `full` image of 2.4.3, then bench
images: `firmware/full.yaml` plus a scratch heap probe (`GET /lab/heap`: free internal heap, its lowest point every
20 ms; not in the repo), rebuilt as each fix went in. 2.4.4 is 2.4.3 with those fixes (DESIGN.md D42). The load ran
from the Mac over Wi-Fi with Python; a "session" holds `/events` for 20 s, as an open page does.

**The upgrade from 2.3.0** (the release image, the web app's signed upload over plain HTTP, 13 s, back in 22 s): all
135 entities kept their values; the new ones came up at their defaults. HTTPS was listening at once with a self-signed
certificate for `precision-feeder-disc.local` (until 2049). The signed upload of the next image over HTTPS also took
13 s. A restart is back with HTTPS in about 11 to 20 s.

**Found and fixed (2.4.4):**

| What | Seen | Cause | Fix |
|---|---|---|---|
| BLE invisible | no scanner saw the feeder; the log said "Advertising did not start (530)" | HCI error 0x12 on every advertising start: the controller trim turned off its central role (`CONFIG_BT_CTRL_BLE_MASTER`), and the ESP32-S3's controller then refuses to advertise at all. M0's "NimBLE trimmed" image was never scanned for, so it went unnoticed | the central role stays on (about 11 KB of flash, 5 KB of heap); the log now names the step that failed. Found by bisecting the trim, one option at a time |
| Every TCP port dead after about 20 HTTPS sessions | ports 80, 443 and 6053 stopped accepting; ping still answered; `httpd_accept_conn: error in accept (23)`; no recovery in 4 minutes | `esp_tls_server_session_delete` frees the TLS state but, unlike `esp_tls_conn_destroy`, never closes the socket: one leaked per session until lwIP's 29 were gone | the relay closes the socket itself |
| The feeder's loop stalled under HTTPS load | `interval took 649 ms`, `precision_feeder took 236 ms` | relay tasks at priority 5 on either core preempt ESPHome's loop (priority 1, core 1) for a handshake's 0.5 s of CPU | every TLS task runs on core 0; afterwards one 51 ms warning in a full load run, and the disc's motor-loop gap unchanged (54 ms) |
| 4 sessions too tight | lowest free heap 31 KB with 4 held, 22 to 25 KB with ten requests at once | a session costs about 20 KB here (M0's scratch relay: 15); the real build has 19 KB less free at idle than M0's "NimBLE trimmed" | `max_sessions` 3 by default. Also: the relay's connection to port 80 is given at most 5 s and no write blocks longer than the idle timeout; lwIP's TCP connections go from 16 to 28 |
| An ACME certificate dropped on a directory change | changing the directory served the self-signed certificate at once, before the new directory had even failed | the certificate had to match the directory set now to be served | one for the name in use stays until the new directory delivers or it expires |
| Vague errors | an untrusted ACME server read "directory: no answer"; an oversize upload "request" | | "its certificate does not check against the trust store", "not reachable", "name not found"; "too large" (the web app's word for it) |

**HTTPS with the fixes** (lowest free heap in KB; idle about 122 KB with the probe, BLE on, no client):

| Load | Lowest in the phase | Lowest at any instant | Result |
|---|---|---|---|
| 1 session | 101 | 101 | handshake 0.4 s |
| 3 sessions | 54 | 51 | all answered |
| 4 sessions (cap 3) | 51 | 43 | the fourth waits its turn (22 s), then answered |
| 10 requests at once | 31 to 35 | 25 | all 10 answered |
| 100 handshakes in a row, twice | 88 | | all answered, 0.4 to 0.8 s; no leak (117 KB after, steady) |
| resumption | | | resumed, 0.1 s |

An RSA-2048 certificate works too (ECDHE-RSA), at about 0.8 s a handshake and about 8 KB lower heap points.

**Port 80 and HTTPS Only:** `/` and `/index.html` answer 302 to `https://precision-feeder-disc.local/`; the owner
checked Home Assistant's "Visit" link lands there. With HTTPS Only on, everything else on port 80 answers 403
`{"error":"https"}`; nothing answered 200 on port 80 during a restart. Not tried: the access point's exemption.

**Uploads** (scratch CA: root, intermediate, leaves; signed and, with a settings password, masked): an EC chain with
its intermediate, served after a restart and verified by curl against the root alone, the card's fingerprint matching
OpenSSL's; an RSA-2048 chain with a PKCS#8 key; refused with the reason: a key that does not match, an encrypted key, a
certificate that does not parse, a chain over 6 KB, a chain that does not lead to the trust store, and an unsigned
upload with a password set (403). A trust store bundle of root and intermediate. Remove and a new self-signed
certificate (new fingerprint after the restart).

**MQTT over TLS** (Mosquitto on the Mac, three TLS listeners and a plain one): with TLS on and the plain port set, the
broker logged "appears to be using TLS to connect to a non-TLS listener", never a plain CONNECT. Against the CA (an IP
name in the broker's certificate): connected, the schedule's topics published. The same CA with a certificate for
another name: refused. Pin on first contact against a self-signed broker: pinned (the fingerprint OpenSSL shows) and
connected; a different certificate on the same broker afterwards: refused; Forget, then a restart: the new one pinned.
A leaf sent without its CA: "not pinned yet: the broker sends no CA certificate to pin; upload its CA instead". The
feeder's MQTT settings were put back afterwards.

**ACME against the owner's step-ca** (the firmware's client this time, not M0's scratch one): with the trust store
holding another CA, refused; with the step-ca root, a certificate 30 s after Renew now (24 h, renewal planned 16 h in),
served at once with its intermediate and verified by curl against the root alone. Renew now with a session open: that
session kept the old certificate and its events for 90 s; new connections got the new one. A dead directory: the
certificate in use stayed (2.4.4), "not reachable". Not seen: a certificate expiring by itself, a renewal at 16 h.

**Feeding went on:** profile A's 18:00 event fed its portion on time in the middle of the run, between image swaps
and HTTPS load. At the end the rig was given the 2.4.4 release image, and the ACME directory, the trust store and the
settings password were cleared again; the Bluetooth key made for the run stays for the BLE tests.

**BLE end to end (2.4.4, 2026-10-24).** At first the bench Mac refused to connect, "Peer removed pairing
information": it still held its pairing with 2.3.0's Bluedroid, which NimBLE does not have (expected,
docs/PROTOCOL.md). Once the owner removed that pairing on the Mac, the Python client (bleak, macOS):

| Check | Result |
|---|---|
| `selftest` | 13/13 with "BLE Can Change Guard Limits" on for the run; with it off (the default) the two Config write checks were answered NOT_AUTHORIZED detail 7, as they should be, and counted as failures: the selftest now reports them as skipped |
| 100 cycles of connect, AUTH, Status, disconnect | 100/100, median 1.87 s, longest 2.80 s; MTU 256 |
| one signed SET_PROFILE written 200 times | every answer the same (OK) |
| 200 new commands back to back | all OK, 13.6 s |
| one FEED of 1 portion, its exact bytes written 50 more times | the same answer each time; the tray went from 9 portions left to 8, one portion |
| free heap (Free Heap sensor, every 10 s) | 122.9 to 123.4 KB through the cycles and the storm, largest block 64 KB; 123,352 bytes before, during and after a client held the link for 75 s (NimBLE takes its connection's memory at start) |

The Mac did not pair (bonds=0 in BLE Info): macOS pairs only when an attribute asks for encryption, and the feeder's
do not (access control is the protocol 2 key). So Forget BLE Bonds is still to check with a client that bonds. The
built-in browser could not open the feeder's address; the owner had the app open over HTTPS in their own browser
throughout. Afterwards the guard permission was off again; the Bluetooth key made for the run stays.
