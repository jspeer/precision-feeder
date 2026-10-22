# Precision Feeder BLE protocol

Protocol version **2** (firmware 0.4.0 and later). This document is
authoritative; the firmware (`components/precision_feeder_ble/protocol.h` and
`crypto.h`) and the reference client (`tools/precision_feeder_ble.py`) are
checked against the same byte vectors.

*Written with the assistance of an AI (Anthropic's Claude); verify against the
source if something looks wrong.* Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense),
inspired by FlyingGyroscope's Precision Feeder Rev2
([original firmware](https://github.com/FlyingGyroscope/PrecisionFeederRev2),
[Printables model](https://www.printables.com/model/1673252-automated-precision-fish-feeder-with-wifi-rev2));
none of the original code is used.

## Overview

The feeder is a BLE peripheral with one custom GATT service. A client (a PLC
bridge, a phone app, a script) connects, **authenticates with a shared key**,
optionally subscribes to notifications, and talks to it through nine
characteristics. One client at a time.

- All multi-byte values are **little-endian**. Every write and record fits the
  default 23-byte ATT MTU (20 bytes of payload), so no MTU negotiation is needed.
- The feeder's schedule and guard always run on the device. BLE can *edit* the
  schedule and *request* feeds; it is never required for feeding to continue.
- Every feed request, from any source, passes the same guard on the device
  (minimum gap, daily cap, tray state, hold). BLE cannot bypass it.
- **Every write is authenticated** (see [Authentication](#authentication)). A
  client that does not know the key can read Info and Auth and nothing else.

### Changes from protocol 1

- New **Auth** characteristic (`000A`) and **AUTH** command (`0x0D`).
- Every write (Command, Profile, Config, Calibration) now ends with an 8-byte
  **tag**. Profile, Config and Calibration writes now **start with a sequence
  number**, and their answers carry it (protocol 1 answered them with `seq 0`).
- New result **16 NOT_AUTHORIZED**, and capability bit 6.
- Before AUTH, Status, Profile, Config, Calibration and Response read as zeros,
  and nothing is notified.
- Record formats, opcodes, arguments, result codes 0 to 15 and the Status layout
  are unchanged.

## UUIDs

Base `02dd1dd9-ba75-4bbc-bdea-306d2101xxxx`; `xxxx` is:

| xxxx | Characteristic | Properties | Before AUTH |
|---|---|---|---|
| 0001 | *Service* | advertised | |
| 0002 | Info | read | readable |
| 0003 | Command | write | only AUTH is accepted |
| 0004 | Response | notify, read | answers to your writes |
| 0005 | Status | read, notify | zeros, not notified |
| 0006 | Profile | read, write | zeros; writes refused |
| 0007 | Config | read, write | zeros; writes refused |
| 0008 | Calibration | read, write | zeros; writes refused |
| 0009 | Event log | notify | not notified |
| 000A | Auth | read | the challenge (16 bytes) |

## Info (read)

`protocol u8, capabilities u16, positions u8, profiles u8, events_per_profile u8`

Currently `02 | caps | 07 | 03 | 04` for the servo tray. Capability bits: 0 solar anchors,
1 clock push (`SET_TIME`) available, 2 profile editing, 3 calibration,
4 hold-until-cancelled, 5 servo pulse range settable (Config keys 10 and 11),
6 writes must be authenticated.

`positions` is the number of tray positions, 0 to `positions - 1`: the portions per load plus one (7 for the servo
tray, so 6 portions). Since firmware 2.1.0 a feeder can have another mechanism (another motor and tray); it reports
its own `positions`, and the ranges that depend on it follow: SET_POSITION, event portions, Config keys 5 and 6 go up
to `positions - 1`. Bits 3 (calibration) and 5 (pulse range) are set only when the mechanism is the servo, whose
seven slots the Calibration characteristic is indexed by; without them, Calibration writes and queries answer
INVALID_ARG and Config keys 10 and 11 UNSUPPORTED. Nothing in the packet layouts changed, so this is still protocol 2. **If your client sees a protocol version it does
not know, do not send commands.**

## Authentication

The feeder and the client share a secret **key**: 32 random bytes, base64 as made by
`openssl rand -base64 32`. The same base64 text goes into the client. On the feeder
it is either `ble_key` in its `secrets.yaml`, or (firmware 2.0.0) a key generated or
entered in its web app and saved on the device, which wins. The prebuilt firmware has
no `ble_key`: make one in the web app (Configuration, Keys). With no key at all, AUTH
is always refused with detail 6; an all-zero key is never accepted.

### 1. Read the challenge

Read **Auth** (`000A`): a random 16-byte **nonce**. The feeder makes a new one for
every connection and after every failed AUTH, from its hardware random number
generator.

### 2. Prove the key: AUTH

Write to Command: `seq u8, 0x0D, proof[16]` (18 bytes, no tag), where

```
proof = HMAC-SHA256(key, "PF2 auth" || nonce)[0..15]
```

(`"PF2 auth"` is the 8 ASCII bytes, no terminator; `||` is concatenation.)
The answer on Response is `seq, 0x0D, result, detail`: OK, or NOT_AUTHORIZED with
a detail code (below). On OK, both sides derive this connection's **session key**:

```
session = HMAC-SHA256(key, "PF2 session" || nonce)      (all 32 bytes)
```

After a failed AUTH the nonce changes (read Auth again), and AUTH is refused for
one second. A disconnect ends the session; the next connection starts again at
step 1. A failed AUTH on an authenticated connection ends its session too, and
Status, Profile, Config, Calibration and Response read as zeros again until the
next successful AUTH (firmware 2.1.2; before, they kept their last values).

A valid AUTH on a connection that is already authenticated (a retry after a lost
answer, or a replay of the same bytes) is answered OK and **changes nothing**: the
session key, the last accepted sequence number and the retry cache stay as they
are. (Firmware 0.4.0 to 0.4.1 reset the sequence number here, which could let a
write already executed in the session run a second time; fixed in 0.5.0.)

### 3. Sign every write

Every write after AUTH, to Command, Profile, Config or Calibration, is

```
seq u8, body..., tag[8]
tag = HMAC-SHA256(session, channel || seq || body...)[0..7]
```

- `channel` is the low byte of the characteristic's UUID: `03` Command,
  `06` Profile, `07` Config, `08` Calibration. A write signed for one
  characteristic is refused by another.
- For Command, `body` is `opcode, arguments...` as in protocol 1.
- For Profile, Config and Calibration, `body` is the record or query exactly as
  in protocol 1 (sections below). Their answers carry this `seq`.
- **`seq` runs 1 to 255 and wraps to 1 (never 0). Each new write must use a
  sequence number after the last accepted one** (AUTH's counts), at most 127
  ahead. Commands and records share one counter: simply use the next number for
  every write.

### Retries are safe

If an answer does not arrive, send the **same bytes** again (same `seq`, body
and tag). The feeder remembers its answers to the last 8 writes for 30 seconds
and replays the stored answer for an exact repeat **without acting again**, so a
retried FEED never feeds twice. A new write that reuses an older `seq` is refused
as stale. Match answers on `seq` and opcode.

### NOT_AUTHORIZED details

| `detail` | Meaning |
|---|---|
| 0 | not authenticated: send AUTH first |
| 1 | bad tag: wrong key, the bytes were changed, or signed for another characteristic |
| 2 | stale sequence number: not after the last accepted one (a retry must repeat the exact bytes) |
| 3 | AUTH refused: wrong proof (wrong key, or an old nonce). The nonce has changed |
| 4 | AUTH refused: within a second of a failed AUTH |
| 5 | too short to carry a sequence number and tag |
| 6 | AUTH refused: no key is set on the feeder (firmware 2.0.0) |
| 7 | authenticated, but the feeder's owner has turned this setting off for BLE (firmware 2.0.0; see below) |

**Clients must accept detail values they do not know** (show the number, treat the
write as refused). New values may be added within protocol 2; 6 and 7 were.

### BLE permissions (firmware 2.0.0)

Two settings on the feeder, both on by default, decide what an authenticated client
may change. Turned off (in the feeder's web app or Home Assistant), these writes are
answered NOT_AUTHORIZED with detail 7. They still use up their sequence number, and
an exact retry gets the same answer from the cache.

| Setting | Refused when off |
|---|---|
| BLE can change guard limits | Config writes to keys 1, 2, 5 and 6 |
| BLE can change calibration | Calibration writes; Config writes to keys 10 and 11; CALIBRATE, JOG, SAVE_CALIBRATION |

Queries (reading a Config or Calibration record), feeding, the schedule, the clock,
hold and SET_POSITION are never refused by them. Info and the protocol version do
not change.

### What this protects, and what it does not

- **Protects:** nobody without the key can feed, change the schedule or
  settings, calibrate, set the clock, or read the feeder's state. Recorded
  traffic cannot be replayed (sequence numbers, and a new nonce every
  connection), and writes cannot be altered or moved to another characteristic.
- **Pairing:** the feeder offers LE Secure Connections with bonding
  (`auth_req_mode: sc_bond`). A client that pairs gets an encrypted link, which
  also hides what is read. With no screen or keypad, pairing is "Just Works", so
  it is not access control by itself; the key is.
- **Does not protect:** the feeder does not prove its own identity to the
  client, and answers and notifications are not signed. Someone with radio
  equipment who can impersonate the feeder could feed a client false status.
  The animals are protected (the feeder only acts on authenticated writes); a
  client that must trust what it reads should pair, and treat the feeder's
  status as informational.
- **Keep the key secret.** Anyone with it has full control (still within the feed
  guard, and the two permissions above). To change it, generate a new one in the
  feeder's web app (firmware 2.0.0; a connected client must authenticate again), or
  update `secrets.yaml` and reflash, then update every client.

## Command (write) and Response (notify)

A command is `seq u8, opcode u8, arguments..., tag[8]` (AUTH has no tag). The
device answers on **Response** with `seq u8, opcode u8, result u8, detail u8` (and
also keeps the last answer readable, in case a notification was missed).

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
| 0x0D | AUTH | `proof[16]`, no tag (see [Authentication](#authentication)) | NOT_AUTHORIZED detail |

Argument lengths are exact (not counting the tag); a wrong length is INVALID_ARG.

### Results

| Code | Name | Meaning |
|---|---|---|
| 0 | OK | done |
| 1 | BUSY | the tray is moving (a feed, or a reload or SET_POSITION until the tray has settled, since firmware 2.1.2) |
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
| 16 | NOT_AUTHORIZED | see [NOT_AUTHORIZED details](#not_authorized-details) |
| 17 | STORAGE_FAILED | the feeder could not save what must be on flash before the tray moves (a feed's intent), so it did not move (firmware 2.1.2). Clients must treat any code they do not know as a refusal |

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
device's timezone comes from the build, Home Assistant or its Timezone entity, never
from BLE; `utc_offset_min` is accepted for forward compatibility and not used. The device does not remember the clock across
a power loss, so a PLC should push the time after every connect.

## Status (read, notify)

Sent when it changes, at most twice a second, and immediately after AUTH or a
feed request; a read may lag a command by up to half a second. Zeros, and no
notifications, until AUTH succeeds. 20 bytes:

| Offset | Field | Type |
|---|---|---|
| 0 | tray position (0 to 6) | u8 |
| 1 | portions remaining | u8 |
| 2 | tray state: 0 loaded, 1 empty, 2 unknown, 3 calibrating, 4 moving | u8 |
| 3 | active profile (0 to 2) | u8 |
| 4 | flags | u16 |
| 6 | last feed epoch (0 = never or clock was unset) | u32 |
| 10 | last feed source: 0 schedule, 1 ESPHome (a feed button pressed in Home Assistant or the web UI; called "home assistant" before firmware 0.3.4), 2 ble, 3 button, 4 one-shot | u8 |
| 11 | portions fed today | u8 |
| 12 | next scheduled feed epoch (0 = none) | u32 |
| 16 | runway in days (255 = more than 30) | u8 |
| 17 | clock source: 0 none, 1 BLE, 2 NTP, 3 Home Assistant (0 with flag 0 set: the clock was kept across a restart, see below) | u8 |
| 18 | minutes since the clock was last set (saturating) | u16 |

Clock source 0 with flag bit 0 (clock valid) set means the clock survived a soft
restart (OTA update, crash, reflash: the chip's RTC keeps counting) but no source has
set it since. The time is good; `SET_TIME` is accepted under `fallback_only` until
NTP or Home Assistant syncs.

Flags: bit 0 clock valid, 1 hold, 2 calibrating, 3 moving, 4 scheduler enabled,
5 reload pause, 6 sunrise/sunset unavailable (no location: sun events are skipped;
or no sunrise/sunset today: they use 06:00 / 18:00), 7 schedule needs more portions
than remain before the next refill, 8 clock not re-synchronised for over a week,
9 position unknown, 10 tray empty, 11 the active profile feeds more portions on its
busiest day than the daily cap allows, 12 needs food: the tray is empty, or the next
scheduled or one-shot feed needs more portions than remain (never set while the
position is unknown).

## Profile (read, write)

Three profiles, four events each. **Writes** are `seq, record, tag` and apply
immediately; they are answered on Response with this `seq`, opcode `0x80`,
`result`, and `detail` = the profile index. The record is one of:

- Header `00, profile, every_n, fast_mask, anchor_day u16` (6 bytes).
  `every_n` 1 to 14; `fast_mask` bit 0 = Sunday ... bit 6 = Saturday, the days
  with no feeding; `anchor_day` is days since 1970-01-01 that count as cycle day
  0, or **0 to start the cycle today**.
- Event `01, profile, event, flags, offset i16, portions, days` (8 bytes).
  `flags` bit 0 enabled, bits 1 to 2 anchor (0 clock, 1 sunrise, 2 sunset).
  `offset` is minutes after midnight (0 to 1439) for a clock event, or signed
  minutes from the sun (-720 to +720). `portions` 1 to `positions - 1` (6 on the servo; firmware 2.1.0 and 2.1.1
  still refused more than 6); `days` is a weekday
  mask like `fast_mask`.
- Query `02, profile, event` (3 bytes; event `FF` means the header): selects which
  record the next **read** returns, in the formats above (no seq, no tag).

## Config (read, write)

Set: `seq, key u8, value i32, tag`. Query: `seq, key | 0x80, tag`, then read to get
`key, value` (5 bytes). Answered on Response (this `seq`, opcode `0x81`, detail =
key).

| Key | Setting | Range |
|---|---|---|
| 1 | minimum seconds between feeds | 10 to 3600 |
| 2 | seconds between portions of one feed | 1 to 60 |
| 3 | reload pause, minutes | 0 to 60 |
| 4 | refill interval, days | 1 to 30 |
| 5 | rolling 24 h portion cap | 1 to 6 (`positions - 1`) |
| 6 | max portions per command | 1 to 6 (`positions - 1`) |
| 7 | scheduler enabled | 0 or 1 |
| 8 | latitude, 1e-5 degree | -9,000,000 to 9,000,000 |
| 9 | longitude, 1e-5 degree | -18,000,000 to 18,000,000 |
| 10 | servo pulse for 0 degrees, µs | 300 to 1500 |
| 11 | servo pulse for 180 degrees, µs | 1500 to 3000 |

Keys 10 and 11 must stay at least 500 µs apart: a write that would break that is
refused with INVALID_ARG. In calibration mode the servo moves to the selected
position again after a change.

## Calibration (read, write)

Set: `seq, index u8, offset i16, tag` (offset in tenths of a degree, -150 to +150).
Query: `seq, index | 0x80, tag`, then read `index, offset` (3 bytes). Answered on
Response (this `seq`, opcode `0x82`). The servo only moves when calibration mode is
on (CALIBRATE 1); JOG nudges a position and moves the servo there. Turning
calibration off, or SAVE_CALIBRATION, writes the offsets to flash.

## Event log (notify)

One notification per feed request, accepted or refused, from any source, while an
authenticated client is connected: `epoch u32 (0 = clock unset), source u8,
portions u8, result u8` (7 bytes). `result` uses the table above. This is how a
client learns the outcome of a *scheduled* feed it did not ask for. A scheduled or
one-shot feed that the device has to wait for (busy, too soon, on hold) is retried
quietly during its one-minute window and reported once: when it is fed, or with
the last refusal when the window closes.

## Diagnostics on the device (firmware 1.1.0)

Nothing on the air changes: no new characteristic, opcode or field, and the
protocol stays at version 2. The device keeps local diagnostics about BLE for its
web app and Home Assistant: counters since boot, the current session (connected,
authenticated, the last two bytes of the client's address, the MTU), and an
activity log of the last 32 connections and writes with their answers. Reads and
Status notifications are only counted, so a client that polls does not fill the
log. They never contain the key, a nonce or a tag. Formats: DESIGN.md section 7
("BLE Info", "BLE Session" and `GET /feeder/ble`). The device log names opcodes
("Command FEED seq 5 -> ok").

## Example bytes

Unsigned bodies (as in protocol 1; add the seq and tag as described above):

| What | Bytes |
|---|---|
| FEED 1 portion, seq 5 (before the tag) | `05 01 01` |
| ...answered OK, 1 portion | `05 01 00 01` |
| HOLD until cancelled, seq 6 | `06 04 FF FF FF FF` |
| SET_TIME 1760380800, offset -360, seq 7 | `07 06 80 47 ED 68 98 FE` |
| SET_LOCATION 39.76840, -86.15810, seq 8 | `08 07 88 AE 3C 00 7E 88 7C FF` |
| Profile header record: profile 1, every 2 days, Sundays off, anchor 20739 | `00 01 02 01 03 51` |
| Profile event record: profile 0, event 2, enabled, sunset +180 min, 2 portions, weekdays | `01 00 02 05 B4 00 02 3E` |
| Profile event record: profile 1, event 0, disabled, sunrise -30 min, 1 portion, every day | `01 01 00 02 E2 FF 01 7F` |
| Config record: daily cap = 3 | `05 03 00 00 00` |
| Config query: latitude | `88` |
| Config record: servo max pulse = 2770 µs | `0B D2 0A 00 00` |
| Calibration record: position 2 = -1.2 degrees | `02 F4 FF` |
| Event log: a BLE feed of 1 portion at 1760380800, accepted | `80 47 ED 68 02 01 00` |

Authentication, with key `00 01 02 ... 1F` (bytes 0 to 31) and nonce
`A0 A1 ... AF`:

| What | Bytes |
|---|---|
| proof | `FA E3 72 F2 C6 D3 C9 D6 EE 0E 49 74 57 4D 6C 7F` |
| AUTH, seq 1 | `01 0D FA E3 72 F2 C6 D3 C9 D6 EE 0E 49 74 57 4D 6C 7F` |
| session key | `0C 31 22 B4 B9 CC 94 02 F4 17 94 4D DA 88 31 2C 46 47 9E 89 D2 30 C4 86 50 69 35 5A AD F3 91 C3` |
| signed FEED 1, seq 5 (Command) | `05 01 01 2A 8F 78 9B 5E D3 34 F6` |
| signed Config write daily cap = 3, seq 6 (Config) | `06 05 03 00 00 00 C4 79 CE E3 22 0F CE 8B` |

## Changing the protocol

Never change the meaning or size of an existing field within a protocol version.
Additions go in new opcodes, new config keys or new capability bits. If a field
must change, bump the protocol version in Info and update this document, the
firmware and the client together.
