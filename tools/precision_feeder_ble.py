#!/usr/bin/env python3
"""Reference client for the Precision Feeder BLE protocol (docs/PROTOCOL.md).

The packing and parsing functions are pure Python and need nothing installed. The
FeederClient class and the command line need `bleak` (pip install bleak).

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.

Protocol 2 needs the feeder's shared key (`ble_key` in its secrets.yaml): pass it with
--key, or set PRECISION_FEEDER_BLE_KEY. Both take the base64 text as in secrets.yaml.

Examples:
    python precision_feeder_ble.py scan
    export PRECISION_FEEDER_BLE_KEY='...base64 from secrets.yaml...'
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF status
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF selftest
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF config get all
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF profile A show
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF profile A event 1 on clock 08:00 1 all
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF feed 1
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF set-time
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF hold forever
"""

from __future__ import annotations

import argparse
import asyncio
import base64
import hashlib
import hmac
import os
import secrets
import struct
import time
from dataclasses import dataclass

PROTOCOL_VERSION = 1

# UUIDs: the project's base with the attribute number in the low 16 bits.
_BASE = "02dd1dd9-ba75-4bbc-bdea-306d2101{:04x}"
SERVICE_UUID = _BASE.format(0x0001)
INFO_UUID = _BASE.format(0x0002)
COMMAND_UUID = _BASE.format(0x0003)
RESPONSE_UUID = _BASE.format(0x0004)
STATUS_UUID = _BASE.format(0x0005)
PROFILE_UUID = _BASE.format(0x0006)
CONFIG_UUID = _BASE.format(0x0007)
CALIBRATION_UUID = _BASE.format(0x0008)
EVENT_LOG_UUID = _BASE.format(0x0009)
AUTH_UUID = _BASE.format(0x000A)

PROTOCOL_VERSION = 2

# Channels: the low byte of each writable characteristic's UUID, mixed into the write tags.
CHANNEL_COMMAND = 0x03
CHANNEL_PROFILE = 0x06
CHANNEL_CONFIG = 0x07
CHANNEL_CALIBRATION = 0x08

# Command opcodes.
OP_FEED = 0x01
OP_TRAY_RELOAD = 0x02
OP_SET_PROFILE = 0x03
OP_HOLD = 0x04
OP_STOP = 0x05
OP_SET_TIME = 0x06
OP_SET_LOCATION = 0x07
OP_CALIBRATE = 0x08
OP_JOG = 0x09
OP_SAVE_CALIBRATION = 0x0A
OP_SET_POSITION = 0x0B
OP_SET_TIMEZONE = 0x0C  # reserved; the device answers UNSUPPORTED
OP_AUTH = 0x0D  # proof[16]; the only write that carries no tag
OP_PROFILE_WRITE = 0x80  # opcodes that appear in responses to Profile/Config/Calibration writes
OP_CONFIG_WRITE = 0x81
OP_CALIBRATION_WRITE = 0x82

HOLD_FOREVER = 0xFFFFFFFF

RESULTS = {
    0: "ok",
    1: "busy",
    2: "too soon",
    3: "daily cap reached",
    4: "tray empty",
    5: "position unknown",
    6: "calibrating",
    7: "hold or reload pause",
    8: "invalid argument",
    9: "duplicate",
    10: "clock not set",
    11: "too many portions for one command",
    12: "clock from BLE ignored (network time is fresh)",
    13: "clock from BLE disabled",
    14: "unsupported",
    15: "calibration mode is off",
    16: "not authorized",
}
AUTH_DETAILS = {
    0: "not authenticated (send AUTH first)",
    1: "bad tag (wrong key?)",
    2: "stale sequence number",
    3: "wrong key (AUTH refused; the nonce has changed)",
    4: "too soon after a failed AUTH",
    5: "write too short for a tag",
}
SOURCES = {0: "schedule", 1: "ESPHome", 2: "ble", 3: "button", 4: "one-shot"}
TRAY_STATES = {0: "loaded", 1: "empty", 2: "unknown", 3: "calibrating", 4: "moving"}
TIME_SOURCES = {0: "none", 1: "BLE", 2: "NTP", 3: "Home Assistant"}

# Status flag bits.
FLAGS = {
    0: "time_valid",
    1: "hold",
    2: "calibrating",
    3: "moving",
    4: "scheduler_enabled",
    5: "reload_pause",
    6: "solar_fault",
    7: "oversubscribed",
    8: "time_stale",
    9: "position_unknown",
    10: "tray_empty",
    11: "schedule_over_cap",
    12: "needs_food",
}

# Config keys.
CONFIG_KEYS = {
    "min_gap_s": 1,
    "portion_gap_s": 2,
    "reload_pause_min": 3,
    "refill_interval_days": 4,
    "daily_cap": 5,
    "max_portions_cmd": 6,
    "scheduler_enabled": 7,
    "latitude_e5": 8,
    "longitude_e5": 9,
    "min_pulse_us": 10,
    "max_pulse_us": 11,
}
QUERY_FLAG = 0x80
QUERY_HEADER = 0xFF

ANCHORS = {"clock": 0, "sunrise": 1, "sunset": 2}
WEEKDAYS = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]


# ---- authentication (protocol 2) ----------------------------------------------------------------------

KEY_SIZE = 32
NONCE_SIZE = 16
PROOF_SIZE = 16
TAG_SIZE = 8


def parse_key(text: str) -> bytes:
    """The shared key as written in secrets.yaml (base64 of 32 bytes)."""
    key = base64.b64decode(text.strip().strip('"'), validate=True)
    if len(key) != KEY_SIZE:
        raise ValueError(f"the BLE key must be {KEY_SIZE} bytes, got {len(key)}")
    return key


def auth_proof(key: bytes, nonce: bytes) -> bytes:
    """What AUTH carries: HMAC-SHA256(key, "PF2 auth" || nonce), first 16 bytes."""
    return hmac.new(key, b"PF2 auth" + nonce, hashlib.sha256).digest()[:PROOF_SIZE]


def session_key(key: bytes, nonce: bytes) -> bytes:
    """The key for this connection's tags: HMAC-SHA256(key, "PF2 session" || nonce)."""
    return hmac.new(key, b"PF2 session" + nonce, hashlib.sha256).digest()


def write_tag(session: bytes, channel: int, body: bytes) -> bytes:
    """Tag of an authenticated write: HMAC-SHA256(session, channel || body), first 8 bytes."""
    return hmac.new(session, bytes([channel]) + body, hashlib.sha256).digest()[:TAG_SIZE]


def signed(session: bytes, channel: int, body: bytes) -> bytes:
    """An authenticated write: the body (sequence number first) followed by its tag."""
    return body + write_tag(session, channel, body)


# ---- packing ------------------------------------------------------------------------------------------


def command(seq: int, opcode: int, args: bytes = b"") -> bytes:
    """A Command characteristic write: seq u8, opcode u8, arguments."""
    return struct.pack("<BB", seq & 0xFF, opcode) + args


def feed_args(portions: int) -> bytes:
    return struct.pack("<B", portions)


def hold_args(minutes: int) -> bytes:
    """minutes: 0 cancels, HOLD_FOREVER (0xFFFFFFFF) holds until cancelled."""
    return struct.pack("<I", minutes)


def set_time_args(epoch: int, utc_offset_min: int = 0) -> bytes:
    return struct.pack("<Ih", epoch, utc_offset_min)


def set_location_args(latitude: float, longitude: float) -> bytes:
    return struct.pack("<ii", round(latitude * 1e5), round(longitude * 1e5))


def jog_args(position: int, delta_tenths: int) -> bytes:
    return struct.pack("<Bh", position, delta_tenths)


_FULL_DAYS = ("sunday", "monday", "tuesday", "wednesday", "thursday", "friday", "saturday")


def day_index(name: str) -> int:
    """0 = Sunday. Three letters or the full name, plural allowed ("mon", "monday", "mondays"), as the firmware."""
    word = name.strip().lower()
    for i, (short, full) in enumerate(zip(WEEKDAYS, _FULL_DAYS)):
        if word in (short, full, full + "s"):
            return i
    raise ValueError(f"unknown day {name!r} (use mon, monday, ...)")


def days_mask(*days: str) -> int:
    """days_mask("mon", "wed") -> bit mask with bit0 = Sunday."""
    mask = 0
    for d in days:
        mask |= 1 << day_index(d)
    return mask


def profile_header(profile: int, every_n: int, fast_mask: int, anchor_day: int = 0) -> bytes:
    """anchor_day: days since 1970-01-01 that count as cycle day 0; 0 starts the cycle today."""
    return struct.pack("<BBBBH", 0x00, profile, every_n, fast_mask, anchor_day)


def profile_event(
    profile: int, event: int, enabled: bool, anchor: str, offset_min: int, portions: int, days: int = 0x7F
) -> bytes:
    """offset_min: minutes after midnight for 'clock'; signed minutes from the sun for 'sunrise'/'sunset'."""
    flags = (1 if enabled else 0) | (ANCHORS[anchor] << 1)
    return struct.pack("<BBBBhBB", 0x01, profile, event, flags, offset_min, portions, days)


def profile_query(profile: int, event: int | None) -> bytes:
    """Ask for a record: event None asks for the profile header. Read the characteristic afterwards."""
    return struct.pack("<BBB", 0x02, profile, QUERY_HEADER if event is None else event)


def config_set(key: int, value: int) -> bytes:
    return struct.pack("<Bi", key, value)


def config_query(key: int) -> bytes:
    return struct.pack("<B", key | QUERY_FLAG)


def calibration_set(index: int, tenths: int) -> bytes:
    return struct.pack("<Bh", index, tenths)


def calibration_query(index: int) -> bytes:
    return struct.pack("<B", index | QUERY_FLAG)


# ---- parsing ------------------------------------------------------------------------------------------


@dataclass
class Response:
    seq: int
    opcode: int
    result: int
    detail: int

    @property
    def ok(self) -> bool:
        return self.result == 0

    @property
    def result_text(self) -> str:
        return RESULTS.get(self.result, f"unknown ({self.result})")


def parse_response(data: bytes) -> Response:
    if len(data) != 4:
        raise ValueError(f"response must be 4 bytes, got {len(data)}")
    return Response(*struct.unpack("<BBBB", data))


@dataclass
class Status:
    tray_position: int
    remaining: int
    tray_state: int
    active_profile: int
    flags: int
    last_feed_epoch: int
    last_feed_source: int
    feeds_today: int
    next_event_epoch: int
    runway_days: int
    time_source: int
    time_age_min: int

    @property
    def clock_text(self) -> str:
        if self.time_source == 0:
            # Source 0 with a valid clock: the RTC kept it across a soft reset; no source has set it since boot.
            return "kept across restart" if self.flags & 1 else "not set"
        return f"{TIME_SOURCES.get(self.time_source, '?')}, {self.time_age_min} min ago"

    @property
    def flag_names(self) -> list[str]:
        return [name for bit, name in FLAGS.items() if self.flags & (1 << bit)]

    def describe(self) -> str:
        def when(epoch: int) -> str:
            return time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(epoch)) if epoch else "never/unknown"

        runway = "more than 30 days" if self.runway_days == 255 else f"{self.runway_days} days"
        return "\n".join(
            [
                f"tray:          position {self.tray_position}, {self.remaining} portions left, "
                f"{TRAY_STATES.get(self.tray_state, '?')}",
                f"profile:       {'ABC'[self.active_profile] if self.active_profile < 3 else '?'}",
                f"flags:         {', '.join(self.flag_names) or 'none'}",
                f"last feed:     {when(self.last_feed_epoch)} ({SOURCES.get(self.last_feed_source, '?')})",
                f"feeds today:   {self.feeds_today}",
                f"next event:    {when(self.next_event_epoch)}",
                f"runway:        {runway}",
                f"clock:         {self.clock_text}",
            ]
        )


def parse_status(data: bytes) -> Status:
    if len(data) != 20:
        raise ValueError(f"status must be 20 bytes, got {len(data)}")
    return Status(*struct.unpack("<BBBBHIBBIBBH", data))


@dataclass
class EventLog:
    epoch: int
    source: int
    portions: int
    result: int


def parse_event_log(data: bytes) -> EventLog:
    if len(data) != 7:
        raise ValueError(f"event log entry must be 7 bytes, got {len(data)}")
    return EventLog(*struct.unpack("<IBBB", data))


@dataclass
class Info:
    protocol_version: int
    capabilities: int
    positions: int
    profiles: int
    events_per_profile: int


def parse_info(data: bytes) -> Info:
    if len(data) != 6:
        raise ValueError(f"info must be 6 bytes, got {len(data)}")
    version, caps, positions, profiles, events = struct.unpack("<BHBBB", data)
    return Info(version, caps, positions, profiles, events)


def parse_profile_header(data: bytes) -> dict:
    tag, profile, every_n, fast_mask, anchor_day = struct.unpack("<BBBBH", data)
    if tag != 0x00:
        raise ValueError("not a profile header")
    return {"profile": profile, "every_n": every_n, "fast_mask": fast_mask, "anchor_day": anchor_day}


def parse_profile_event(data: bytes) -> dict:
    tag, profile, event, flags, offset_min, portions, days = struct.unpack("<BBBBhBB", data)
    if tag != 0x01:
        raise ValueError("not a profile event")
    return {
        "profile": profile,
        "event": event,
        "enabled": bool(flags & 1),
        "anchor": {v: k for k, v in ANCHORS.items()}[(flags >> 1) & 3],
        "offset_min": offset_min,
        "portions": portions,
        "days": days,
    }


def parse_config(data: bytes) -> tuple[int, int]:
    key, value = struct.unpack("<Bi", data)
    return key, value


def parse_calibration(data: bytes) -> tuple[int, int]:
    index, tenths = struct.unpack("<Bh", data)
    return index, tenths


# ---- BLE client (needs bleak) -------------------------------------------------------------------------

# Writes to these characteristics are answered on Response with seq 0 and this opcode.
RECORD_OPCODES = {PROFILE_UUID: OP_PROFILE_WRITE, CONFIG_UUID: OP_CONFIG_WRITE, CALIBRATION_UUID: OP_CALIBRATION_WRITE}
RECORD_CHANNELS = {PROFILE_UUID: CHANNEL_PROFILE, CONFIG_UUID: CHANNEL_CONFIG, CALIBRATION_UUID: CHANNEL_CALIBRATION}


def next_seq(seq: int) -> int:
    """Next command sequence number, 1..255. 0 is never used for commands: the device answers
    Profile/Config/Calibration writes with seq 0."""
    return seq % 255 + 1



class FeederClient:
    """Talks to one feeder. Commands carry a sequence number and are retried with the SAME number
    if no answer arrives, which is safe: the device replays its earlier answer instead of acting twice."""

    def __init__(self, address: str, key: bytes, retries: int = 2, timeout: float = 5.0):
        from bleak import BleakClient  # imported here so the pure functions work without bleak

        self._client = BleakClient(address)
        self._key = key
        self._session = b""
        self._retries = retries
        self._timeout = timeout
        self._seq = next_seq(secrets.randbelow(255))  # every write uses the next one; the session starts at AUTH
        # Answers are matched on (seq, opcode): seq alone is not unique (record writes all answer with seq 0).
        self._waiting: dict[tuple[int, int], asyncio.Future] = {}
        self.events: list[EventLog] = []

    async def __aenter__(self) -> "FeederClient":
        await self._client.connect()
        await self._client.start_notify(RESPONSE_UUID, self._on_response)
        await self._client.start_notify(EVENT_LOG_UUID, lambda _c, d: self.events.append(parse_event_log(bytes(d))))
        await self.authenticate()
        return self

    async def authenticate(self) -> None:
        """Read the challenge and prove the key. Every write after this is signed."""
        nonce = bytes(await self._client.read_gatt_char(AUTH_UUID))
        resp = await self._send(OP_AUTH, command(self._next(), OP_AUTH, auth_proof(self._key, nonce)))
        if not resp.ok:
            raise PermissionError(f"BLE authentication refused: {AUTH_DETAILS.get(resp.detail, resp.detail)}")
        self._session = session_key(self._key, nonce)

    async def __aexit__(self, *exc) -> None:
        await self._client.disconnect()

    def _on_response(self, _char, data: bytearray) -> None:
        resp = parse_response(bytes(data))
        fut = self._waiting.get((resp.seq, resp.opcode))
        if fut is not None and not fut.done():
            fut.set_result(resp)

    def _next(self) -> int:
        self._seq = next_seq(self._seq)
        return self._seq

    async def _send(self, opcode: int, data: bytes, uuid: str = COMMAND_UUID) -> Response:
        """Write `data` (sequence number first) and wait for the answer with the same (seq, opcode). A lost
        answer is retried with the SAME bytes: the device replays its answer instead of acting twice."""
        key = (data[0], opcode)
        loop = asyncio.get_running_loop()
        for attempt in range(self._retries + 1):
            fut = loop.create_future()
            self._waiting[key] = fut
            await self._client.write_gatt_char(uuid, data, response=True)
            try:
                return await asyncio.wait_for(fut, self._timeout)
            except asyncio.TimeoutError:
                continue
            finally:
                self._waiting.pop(key, None)
        raise TimeoutError(f"no answer to opcode 0x{opcode:02X} after {self._retries + 1} tries")

    async def command(self, opcode: int, args: bytes = b"") -> Response:
        body = command(self._next(), opcode, args)
        return await self._send(opcode, signed(self._session, CHANNEL_COMMAND, body))

    async def info(self) -> Info:
        return parse_info(bytes(await self._client.read_gatt_char(INFO_UUID)))

    async def status(self) -> Status:
        return parse_status(bytes(await self._client.read_gatt_char(STATUS_UUID)))

    async def write_record(self, uuid: str, data: bytes) -> Response:
        """Write a Profile/Config/Calibration record (or query) and wait for the device's Response."""
        body = bytes([self._next()]) + data
        return await self._send(RECORD_OPCODES[uuid], signed(self._session, RECORD_CHANNELS[uuid], body), uuid)

    async def read_profile_header(self, profile: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, None))
        return parse_profile_header(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_profile_event(self, profile: int, event: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, event))
        return parse_profile_event(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_config(self, key: int) -> int:
        await self.write_record(CONFIG_UUID, config_query(key))
        return parse_config(bytes(await self._client.read_gatt_char(CONFIG_UUID)))[1]

    async def read_calibration(self, index: int) -> int:
        """A position's offset, tenths of a degree."""
        await self.write_record(CALIBRATION_UUID, calibration_query(index))
        return parse_calibration(bytes(await self._client.read_gatt_char(CALIBRATION_UUID)))[1]


def parse_event_time(anchor: str, text: str) -> int:
    """'08:30' for a clock event; '+01:30', '-00:45' or '+90' (minutes) for sunrise/sunset."""
    sign = -1 if text.startswith("-") else 1
    body = text.lstrip("+-")
    minutes = int(body.split(":")[0]) * 60 + int(body.split(":")[1]) if ":" in body else int(body)
    if anchor == "clock":
        if text[0] in "+-" or not 0 <= minutes <= 1439:
            raise ValueError("a clock time is HH:MM, 00:00 to 23:59")
        return minutes
    if minutes > 720:
        raise ValueError("a sunrise/sunset offset is at most 12 hours")
    return sign * minutes


def parse_day_list(text: str) -> int:
    """'all', 'none', 'weekdays', 'weekend' or day names ('mon,wed,fri')."""
    words = {"all": 0x7F, "daily": 0x7F, "everyday": 0x7F, "none": 0, "off": 0, "weekdays": 0x3E, "weekend": 0x41,
             "weekends": 0x41}
    mask = 0
    for word in text.replace(" ", ",").lower().split(","):
        if word:
            mask |= words[word] if word in words else days_mask(word)
    return mask


def describe_event(ev: dict) -> str:
    if ev["anchor"] == "clock":
        when = f"{ev['offset_min'] // 60:02d}:{ev['offset_min'] % 60:02d}"
    else:
        m = abs(ev["offset_min"])
        when = f"{ev['anchor']}{'-' if ev['offset_min'] < 0 else '+'}{m // 60:02d}:{m % 60:02d}"
    days = "all" if ev["days"] == 0x7F else ",".join(d for i, d in enumerate(WEEKDAYS) if ev["days"] >> i & 1) or "none"
    return f"event {ev['event'] + 1}: {'on ' if ev['enabled'] else 'off'} {when} x{ev['portions']} ({days})"


async def selftest(feeder: FeederClient) -> list[tuple[str, bool, str]]:
    """Check the protocol against a real feeder without changing anything: reads, writes of the same values back,
    re-selecting the active profile (the retry test), and refusals of stale, tampered and misdirected writes. It never
    feeds and never touches a hold."""
    results = []

    def check(name, ok, detail=""):
        results.append((name, bool(ok), detail))

    info = await feeder.info()
    check("Info: protocol 2 with authentication", info.protocol_version == PROTOCOL_VERSION and info.capabilities >> 6 & 1,
          str(info))
    await asyncio.sleep(1.0)  # Status refreshes at most twice a second
    status = await feeder.status()
    check("Status readable", status.tray_position <= 6, status.describe().splitlines()[0])
    values = {}
    for name, key in CONFIG_KEYS.items():
        values[name] = await feeder.read_config(key)
    check("every Config key readable", len(values) == len(CONFIG_KEYS), ", ".join(f"{k}={v}" for k, v in values.items()))
    r = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS["daily_cap"], values["daily_cap"]))
    check("Config write (same value back) answered with its seq", r.ok and r.seq != 0, r.result_text)
    r = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS["daily_cap"], 0))
    check("out-of-range Config value refused", r.result == 8, r.result_text)
    header = await feeder.read_profile_header(status.active_profile)
    check("Profile header readable", 1 <= header["every_n"] <= 14, str(header))
    offsets = [await feeder.read_calibration(i) for i in range(7)]
    check("Calibration readable", all(-150 <= o <= 150 for o in offsets), str(offsets))
    r = await feeder.command(OP_SET_TIMEZONE)
    check("reserved opcode answered UNSUPPORTED", r.result == 14, r.result_text)
    # Re-selecting the profile that is already active changes nothing, so it is safe to send, retry and replay.
    same = struct.pack("<B", status.active_profile)
    body = command(feeder._next(), OP_SET_PROFILE, same)
    data = signed(feeder._session, CHANNEL_COMMAND, body)
    first = await feeder._send(OP_SET_PROFILE, data)
    again = await feeder._send(OP_SET_PROFILE, data)
    check("a command, and its exact retry gets the same answer",
          first.ok and (again.result, again.detail) == (first.result, first.detail))
    # A different command under the sequence number just used: correctly signed, but not new.
    stale = signed(feeder._session, CHANNEL_COMMAND, command(body[0], OP_SET_TIMEZONE))
    r = await feeder._send(OP_SET_TIMEZONE, stale)
    check("a new write with an old sequence number is refused", r.result == 16 and r.detail == 2, f"detail {r.detail}")
    tampered = bytearray(signed(feeder._session, CHANNEL_COMMAND, command(feeder._next(), OP_SET_PROFILE, same)))
    tampered[2] ^= 1
    r = await feeder._send(OP_SET_PROFILE, bytes(tampered))
    check("a tampered write is refused", r.result == 16 and r.detail == 1, f"detail {r.detail}")
    moved = signed(feeder._session, CHANNEL_CONFIG, command(feeder._next(), OP_SET_PROFILE, same))
    r = await feeder._send(OP_SET_PROFILE, moved)
    check("a write signed for another characteristic is refused", r.result == 16 and r.detail == 1, f"detail {r.detail}")
    r = await feeder.command(OP_SET_PROFILE, same)
    check("commands still work afterwards", r.ok, r.result_text)
    after = await feeder.status()
    check("nothing changed on the feeder (profile, hold)",
          after.active_profile == status.active_profile and (after.flags & 2) == (status.flags & 2))
    return results


# ---- command line -------------------------------------------------------------------------------------


async def _scan() -> None:
    from bleak import BleakScanner

    print("Scanning for 8 seconds...")
    for device in await BleakScanner.discover(timeout=8.0, service_uuids=[SERVICE_UUID]):
        print(f"{device.address}  {device.name}")


async def _run(args: argparse.Namespace) -> None:
    key_text = args.key or os.environ.get("PRECISION_FEEDER_BLE_KEY")
    if not key_text:
        raise SystemExit("the feeder's BLE key is needed: --key or PRECISION_FEEDER_BLE_KEY (ble_key in secrets.yaml)")
    async with FeederClient(args.address, parse_key(key_text)) as feeder:
        cmd = args.command
        if cmd == "status":
            print((await feeder.status()).describe())
            return
        if cmd == "info":
            print(await feeder.info())
            return
        if cmd == "feed":
            resp = await feeder.command(OP_FEED, feed_args(args.portions))
        elif cmd == "reload":
            resp = await feeder.command(OP_TRAY_RELOAD)
        elif cmd == "stop":
            resp = await feeder.command(OP_STOP)
        elif cmd == "set-profile":
            resp = await feeder.command(OP_SET_PROFILE, struct.pack("<B", "ABC".index(args.profile.upper())))
        elif cmd == "hold":
            if args.duration == "forever":
                minutes = HOLD_FOREVER
            elif args.duration == "cancel":
                minutes = 0
            else:
                minutes = int(float(args.duration) * 60)  # hours
            resp = await feeder.command(OP_HOLD, hold_args(minutes))
        elif cmd == "set-time":
            offset = -(time.altzone if time.localtime().tm_isdst > 0 else time.timezone) // 60
            resp = await feeder.command(OP_SET_TIME, set_time_args(int(time.time()), offset))
        elif cmd == "location":
            resp = await feeder.command(OP_SET_LOCATION, set_location_args(args.latitude, args.longitude))
        elif cmd == "calibrate":
            resp = await feeder.command(OP_CALIBRATE, struct.pack("<B", 1 if args.state == "on" else 0))
        elif cmd == "jog":
            resp = await feeder.command(OP_JOG, jog_args(args.position, args.tenths))
        elif cmd == "save-calibration":
            resp = await feeder.command(OP_SAVE_CALIBRATION)
        elif cmd == "set-position":
            resp = await feeder.command(OP_SET_POSITION, struct.pack("<B", args.position))
        elif cmd == "config":
            if args.action == "get":
                names = list(CONFIG_KEYS) if args.key in (None, "all") else [args.key]
                for name in names:
                    print(f"{name} = {await feeder.read_config(CONFIG_KEYS[name])}")
                return
            if args.key not in CONFIG_KEYS or args.value is None:
                raise SystemExit(f"usage: config set KEY VALUE, KEY one of {', '.join(CONFIG_KEYS)}")
            resp = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS[args.key], args.value))
        elif cmd == "profile":
            p = "ABC".index(args.profile.upper())
            if args.action == "show":
                h = await feeder.read_profile_header(p)
                print(f"profile {'ABC'[p]}: every {h['every_n']} day(s), fast days mask 0x{h['fast_mask']:02X}, "
                      f"cycle anchor day {h['anchor_day']}")
                for e in range(4):
                    print("  " + describe_event(await feeder.read_profile_event(p, e)))
                return
            if args.action == "event":
                ev = args.values
                if len(ev) != 6:
                    raise SystemExit("usage: profile A event N on|off clock|sunrise|sunset TIME PORTIONS DAYS")
                n, state, anchor, when, portions, days = ev
                record = profile_event(p, int(n) - 1, state == "on", anchor, parse_event_time(anchor, when),
                                       int(portions), parse_day_list(days))
            else:  # header
                if len(args.values) != 2:
                    raise SystemExit("usage: profile A header EVERY_N FAST_DAYS (fast days: none, sun, ...)")
                record = profile_header(p, int(args.values[0]), parse_day_list(args.values[1]))
            resp = await feeder.write_record(PROFILE_UUID, record)
        elif cmd == "calibration":
            if args.action == "get":
                for i in range(7):
                    print(f"position {i}: {await feeder.read_calibration(i) / 10:+.1f} degrees")
                return
            if args.position is None or args.tenths is None:
                raise SystemExit("usage: calibration set POSITION TENTHS")
            resp = await feeder.write_record(CALIBRATION_UUID, calibration_set(args.position, args.tenths))
        elif cmd == "selftest":
            results = await selftest(feeder)
            for name, ok, detail in results:
                print(f"{'PASS' if ok else 'FAIL'}  {name}  {detail}")
            failed = sum(1 for _, ok, _ in results if not ok)
            print(f"{len(results) - failed}/{len(results)} passed")
            if failed:
                raise SystemExit(1)
            return
        else:
            raise SystemExit(f"unknown command {cmd}")
        print(f"{resp.result_text} (detail {resp.detail})")
        if not resp.ok:
            raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Precision Feeder BLE reference client")
    parser.add_argument("--key", help="the feeder's ble_key (base64); or set PRECISION_FEEDER_BLE_KEY")
    sub = parser.add_subparsers(dest="command", required=True)
    sub.add_parser("scan")

    def add(name, **kw):
        return sub.add_parser(name, **kw)

    add("status")
    add("info")
    add("feed").add_argument("portions", type=int, nargs="?", default=1)
    add("reload")
    add("stop")
    add("set-profile").add_argument("profile", choices=["A", "B", "C", "a", "b", "c"])
    add("hold").add_argument("duration", help="hours, 'forever' or 'cancel'")
    add("set-time")
    p = add("location")
    p.add_argument("latitude", type=float)
    p.add_argument("longitude", type=float)
    add("calibrate").add_argument("state", choices=["on", "off"])
    p = add("jog")
    p.add_argument("position", type=int)
    p.add_argument("tenths", type=int, help="tenths of a degree, negative allowed")
    add("save-calibration")
    add("set-position").add_argument("position", type=int)
    p = add("config", help="config get [KEY|all] | config set KEY VALUE")
    p.add_argument("action", choices=["get", "set"])
    p.add_argument("key", nargs="?")
    p.add_argument("value", type=int, nargs="?")
    p = add("profile", help="profile A show | profile A header EVERY_N FAST_DAYS | profile A event N on|off ANCHOR TIME PORTIONS DAYS")
    p.add_argument("profile", choices=["A", "B", "C", "a", "b", "c"])
    p.add_argument("action", choices=["show", "header", "event"])
    p.add_argument("values", nargs="*")
    p = add("calibration", help="calibration get | calibration set POSITION TENTHS")
    p.add_argument("action", choices=["get", "set"])
    p.add_argument("position", type=int, nargs="?")
    p.add_argument("tenths", type=int, nargs="?")
    add("selftest", help="check the protocol against this feeder; never feeds")

    # The address comes first on the command line ("ADDRESS status"), then the subcommand.
    import sys

    argv = sys.argv[1:]
    key = None
    if "--key" in argv:  # may come anywhere; take it out before splitting off the address
        i = argv.index("--key")
        if i + 1 >= len(argv):
            parser.error("--key needs a value")
        key = argv[i + 1]
        del argv[i : i + 2]
    if argv and argv[0] == "scan":
        asyncio.run(_scan())
        return
    if len(argv) < 2:
        parser.error("usage: precision_feeder_ble.py ADDRESS COMMAND [ARGS]  |  precision_feeder_ble.py scan")
    address, rest = argv[0], argv[1:]
    args = parser.parse_args(rest)
    args.address = address
    args.key = key
    try:
        asyncio.run(_run(args))
    except ValueError as err:  # bad input (a day name, a time, a key): say what, without a traceback
        raise SystemExit(f"error: {err}") from None


if __name__ == "__main__":
    main()
