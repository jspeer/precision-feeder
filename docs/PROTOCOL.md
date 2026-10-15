# Precision Feeder BLE protocol

Protocol version **1**. This document is authoritative; the firmware
(`components/precision_feeder_ble/protocol.h`) and the reference client
(`tools/precision_feeder_ble.py`) are checked against the same byte vectors.

*Written with the assistance of an AI (Anthropic's Claude); verify against the
source if something looks wrong.*

## Overview

The feeder is a BLE peripheral with one custom GATT service. A client (a PLC
bridge, a phone app, a script) connects, optionally subscribes to notifications,
and talks to it through eight characteristics. One client at a time.

- All multi-byte values are **little-endian**. All records fit the default 23-byte
  ATT MTU (20 bytes of payload), so no MTU negotiation is needed.
- The feeder's schedule and guard always run on the device. BLE can *edit* the
  schedule and *request* feeds; it is never required for feeding to continue.
- Every feed request, from any source, passes the same guard on the device
  (minimum gap, daily cap, tray state, hold). BLE cannot bypass it.
- Pairing is "Just Works" (no authentication); see DESIGN.md section 10.

## UUIDs

Base `02dd1dd9-ba75-4bbc-bdea-306d2101xxxx`; `xxxx` is:

| xxxx | Characteristic | Properties |
|---|---|---|
| 0001 | *Service* | advertised |
| 0002 | Info | read |
| 0003 | Command | write |
| 0004 | Response | notify, read |
| 0005 | Status | read, notify |
| 0006 | Profile | read, write |
| 0007 | Config | read, write |
| 0008 | Calibration | read, write |
| 0009 | Event log | notify |

## Info (read)

`protocol u8, capabilities u16, positions u8, profiles u8, events_per_profile u8`

Currently `01 | caps | 07 | 03 | 04`. Capability bits: 0 solar anchors,
1 clock push (`SET_TIME`) available, 2 profile editing, 3 calibration,
4 hold-until-cancelled, 5 servo pulse range settable (Config keys 10 and 11). If your client sees a protocol version it does not know,
do not send commands.

## Command (write) and Response (notify)

A command is `seq u8, opcode u8, arguments...`. The device answers on **Response**
with `seq u8, opcode u8, result u8, detail u8` (and also keeps the last answer
readable, in case a notification was missed).

| Opcode | Name | Arguments | `detail` in the response |
|---|---|---|---|
| 0x01 | FEED | `portions u8` | portions that will be dispensed (clamped to what remains); remaining portions if refused |
| 0x02 | TRAY_RELOAD | none | remaining portions (6) |
| 0x03 | SET_PROFILE | `index u8` (0 to 2) | index |
| 0x04 | HOLD | `minutes u32`: 0 cancels, 0xFFFFFFFF until cancelled | |
| 0x05 | STOP | none | |
| 0x06 | SET_TIME | `epoch u32` (UTC), `utc_offset_min i16` | |
| 0x07 | SET_LOCATION | `latitude i32`, `longitude i32` (units of 1e-5 degree) | |
| 0x08 | CALIBRATE | `on u8` (0 or 1) | |
| 0x09 | JOG | `position u8`, `delta i16` (tenths of a degree) | |
| 0x0A | SAVE_CALIBRATION | none | |
| 0x0B | SET_POSITION | `position u8` (0 to 6) | remaining portions |
| 0x0C | SET_TIMEZONE | reserved: always answered UNSUPPORTED | |

Argument lengths are exact; a wrong length is INVALID_ARG.

### Results

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | done |
| 1 | BUSY | the tray is moving |
| 2 | TOO_SOON | inside the minimum gap after the last feed |
| 3 | DAILY_CAP | would exceed the rolling 24 h portion cap |
| 4 | EMPTY | no portions remain |
| 5 | UNKNOWN_POSITION | tray position not trusted (after a power loss mid-feed, or STOP); reload the tray or SET_POSITION |
| 6 | CALIBRATING | calibration mode is on |
| 7 | HOLD | scheduled feeding is on hold or in the reload pause |
| 8 | INVALID_ARG | wrong length or out of range |
| 9 | DUPLICATE | reserved |
| 10 | TIME_INVALID | the clock is not set (hold with a duration needs it) |
| 11 | PORTION_LIMIT | more portions than the per-command limit |
| 12 | IGNORED_NTP_ACTIVE | SET_TIME refused: network time is fresh |
| 13 | BLE_TIME_DISABLED | SET_TIME refused: policy is `off` |
| 14 | UNSUPPORTED | unknown or reserved opcode |
| 15 | NOT_CALIBRATING | JOG needs calibration mode |

### Retries are safe

Commands are **idempotent**. If you do not get an answer, send the *same bytes*
again: same `seq`, opcode and arguments. The device remembers its answers to the
last 8 commands for 30 seconds and replays the stored answer for an exact repeat
without acting a second time, so a retried FEED never feeds twice. A command that
reuses a `seq` with different contents is treated as new. Use a fresh `seq` (1 to
255; avoid 0, which the device uses for its answers to record writes) for each new
command, and match answers on `seq` *and* opcode.

### Setting the clock

`SET_TIME` exists for when the feeder cannot reach NTP: it is in WiFi fallback-AP
mode, or power came back while the internet was down. NTP stays the primary
source. The device's `time_policy` decides:

| Policy | Behaviour |
|---|---|
| `fallback_only` (default) | accepted only if NTP / Home Assistant has not set the clock within the last 24 h (configurable); otherwise IGNORED_NTP_ACTIVE |
| `always` | any plausible value (not before 2019) is accepted |
| `off` | refused with BLE_TIME_DISABLED |

So a client can push the time on every connection without risk: with the default
policy it is ignored whenever the device already has good network time. The
device's timezone is fixed at build time; `utc_offset_min` is accepted for
forward compatibility and not used. The device does not remember the clock across
a power loss, so a PLC should push the time after every connect.

## Status (read, notify)

Sent when it changes, at most twice a second, and immediately after a connection
or a feed request. 20 bytes:

| Offset | Field | Type |
|---|---|---|
| 0 | tray position (0 to 6) | u8 |
| 1 | portions remaining | u8 |
| 2 | tray state: 0 loaded, 1 empty, 2 unknown, 3 calibrating, 4 moving | u8 |
| 3 | active profile (0 to 2) | u8 |
| 4 | flags | u16 |
| 6 | last feed epoch (0 = never or clock was unset) | u32 |
| 10 | last feed source: 0 schedule, 1 home assistant, 2 ble, 3 button, 4 one-shot | u8 |
| 11 | portions fed today | u8 |
| 12 | next scheduled feed epoch (0 = none) | u32 |
| 16 | runway in days (255 = more than 30) | u8 |
| 17 | clock source: 0 none, 1 BLE, 2 NTP, 3 Home Assistant (0 with flag 0 set: the clock was kept across a restart, see below) | u8 |
| 18 | minutes since the clock was last set (saturating) | u16 |

Clock source 0 with flag bit 0 (clock valid) set means the clock survived a soft
restart (OTA update, crash, reflash: the chip's RTC keeps counting) but no source has
set it since. The time is good; `SET_TIME` is accepted under `fallback_only` until
NTP or Home Assistant syncs (firmware 0.3.1 and later; before that, such a clock was
wrongly reported as NTP).

Flags: bit 0 clock valid, 1 hold, 2 calibrating, 3 moving, 4 scheduler enabled,
5 reload pause, 6 sunrise/sunset unavailable, 7 schedule needs more portions than
remain before the next refill, 8 clock not re-synchronised for over a week,
9 position unknown, 10 tray empty, 11 the active profile feeds more portions on its
busiest day than the daily cap allows (added in firmware 0.3.2; 0 before), 12 needs
food: the tray is empty, or the next scheduled or one-shot feed needs more portions
than remain (added in 0.3.3; never set while the position is unknown).

## Profile (read, write)

Three profiles, four events each. **Writes** apply immediately and are answered on
Response with `seq 0`, opcode `0x80`, `result`, and `detail` = the profile index.

- Header `00, profile, every_n, fast_mask, anchor_day u16` (6 bytes).
  `every_n` 1 to 14; `fast_mask` bit 0 = Sunday ... bit 6 = Saturday, the days
  with no feeding; `anchor_day` is days since 1970-01-01 that count as cycle day
  0, or **0 to start the cycle today**.
- Event `01, profile, event, flags, offset i16, portions, days` (8 bytes).
  `flags` bit 0 enabled, bits 1 to 2 anchor (0 clock, 1 sunrise, 2 sunset).
  `offset` is minutes after midnight (0 to 1439) for a clock event, or signed
  minutes from the sun (-720 to +720). `portions` 1 to 6; `days` is a weekday
  mask like `fast_mask`.
- Query `02, profile, event` (3 bytes; event `FF` means the header): selects which
  record the next **read** returns, in the formats above.

## Config (read, write)

Set: `key u8, value i32` (5 bytes). Query: write `key | 0x80` (1 byte), then read to
get `key, value`. Answered on Response (`seq 0`, opcode `0x81`, detail = key).

| Key | Setting | Range |
|---|---|---|
| 1 | minimum seconds between feeds | 10 to 3600 |
| 2 | seconds between portions of one feed | 1 to 60 |
| 3 | reload pause, minutes | 0 to 60 |
| 4 | refill interval, days | 1 to 30 |
| 5 | rolling 24 h portion cap | 1 to 6 |
| 6 | max portions per command | 1 to 6 |
| 7 | scheduler enabled | 0 or 1 |
| 8 | latitude, 1e-5 degree | -9,000,000 to 9,000,000 |
| 9 | longitude, 1e-5 degree | -18,000,000 to 18,000,000 |
| 10 | servo pulse for 0 degrees, µs | 300 to 1500 |
| 11 | servo pulse for 180 degrees, µs | 1500 to 3000 |

Keys 10 and 11 were added in firmware 0.3.0 (an addition within protocol 1; older
firmware answers them INVALID_ARG). The two must stay at least 500 µs apart: a write
that would break that is refused with INVALID_ARG. In calibration mode the servo moves
to the selected position again after a change.

## Calibration (read, write)

Set: `index u8, offset i16` (3 bytes; tenths of a degree, -150 to +150). Query:
write `index | 0x80`, then read. Answered on Response (`seq 0`, opcode `0x82`).
The servo only moves when calibration mode is on (CALIBRATE 1); JOG nudges a
position and moves the servo there. Turning calibration off, or SAVE_CALIBRATION,
writes the offsets to flash.

## Event log (notify)

One notification per feed request, accepted or refused, from any source:
`epoch u32 (0 = clock unset), source u8, portions u8, result u8` (7 bytes).
`result` uses the table above. This is how a client learns the outcome of a
*scheduled* feed it did not ask for. A scheduled or one-shot feed that the device
has to wait for (busy, too soon, on hold) is retried quietly during its one-minute
window and reported once: when it is fed, or with the last refusal when the window
closes.

## Example bytes

| What | Bytes |
|---|---|
| FEED 1 portion, seq 5 | `05 01 01` |
| ...answered OK, 1 portion | `05 01 00 01` |
| HOLD until cancelled, seq 6 | `06 04 FF FF FF FF` |
| SET_TIME 1760380800, offset -360, seq 7 | `07 06 80 47 ED 68 98 FE` |
| SET_LOCATION 39.76840, -86.15810, seq 8 | `08 07 88 AE 3C 00 7E 88 7C FF` |
| Profile header: profile 1, every 2 days, Sundays off, anchor 20739 | `00 01 02 01 03 51` |
| Profile event: profile 0, event 2, enabled, sunset +180 min, 2 portions, weekdays | `01 00 02 05 B4 00 02 3E` |
| Profile event: profile 1, event 0, disabled, sunrise -30 min, 1 portion, every day | `01 01 00 02 E2 FF 01 7F` |
| Config: daily cap = 3 | `05 03 00 00 00` |
| Config query: latitude | `88` |
| Config: servo max pulse = 2770 µs | `0B D2 0A 00 00` |
| Calibration: position 2 = -1.2 degrees | `02 F4 FF` |
| Event log: a BLE feed of 1 portion at 1760380800, accepted | `80 47 ED 68 02 01 00` |

## Changing the protocol

Never change the meaning or size of an existing field within a protocol version.
Additions go in new opcodes, new config keys or new capability bits. If a field
must change, bump the protocol version in Info and update this document, the
firmware and the client together.
