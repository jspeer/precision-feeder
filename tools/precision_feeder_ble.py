#!/usr/bin/env python3
"""Reference client for the Precision Feeder BLE protocol (docs/PROTOCOL.md).

The packing and parsing functions are pure Python and need nothing installed. The
FeederClient class and the command line need `bleak` (pip install bleak).

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.

Examples:
    python precision_feeder_ble.py scan
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF status
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF feed 1
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF set-time
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF hold forever
"""

from __future__ import annotations

import argparse
import asyncio
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
}
SOURCES = {0: "schedule", 1: "home assistant", 2: "ble", 3: "button", 4: "one-shot"}
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


def days_mask(*days: str) -> int:
    """days_mask("mon", "wed") -> bit mask with bit0 = Sunday."""
    mask = 0
    for d in days:
        mask |= 1 << WEEKDAYS.index(d.lower()[:3])
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


def next_seq(seq: int) -> int:
    """Next command sequence number, 1..255. 0 is never used for commands: the device answers
    Profile/Config/Calibration writes with seq 0."""
    return seq % 255 + 1



class FeederClient:
    """Talks to one feeder. Commands carry a sequence number and are retried with the SAME number
    if no answer arrives, which is safe: the device replays its earlier answer instead of acting twice."""

    def __init__(self, address: str, retries: int = 2, timeout: float = 5.0):
        from bleak import BleakClient  # imported here so the pure functions work without bleak

        self._client = BleakClient(address)
        self._retries = retries
        self._timeout = timeout
        self._seq = next_seq(int(time.time()))  # varies between runs, so a restart rarely reuses recent numbers
        # Answers are matched on (seq, opcode): seq alone is not unique (record writes all answer with seq 0).
        self._waiting: dict[tuple[int, int], asyncio.Future] = {}
        self.events: list[EventLog] = []

    async def __aenter__(self) -> "FeederClient":
        await self._client.connect()
        await self._client.start_notify(RESPONSE_UUID, self._on_response)
        await self._client.start_notify(EVENT_LOG_UUID, lambda _c, d: self.events.append(parse_event_log(bytes(d))))
        return self

    async def __aexit__(self, *exc) -> None:
        await self._client.disconnect()

    def _on_response(self, _char, data: bytearray) -> None:
        resp = parse_response(bytes(data))
        fut = self._waiting.get((resp.seq, resp.opcode))
        if fut is not None and not fut.done():
            fut.set_result(resp)

    async def command(self, opcode: int, args: bytes = b"") -> Response:
        self._seq = next_seq(self._seq)
        seq = self._seq
        key = (seq, opcode)
        loop = asyncio.get_running_loop()
        for attempt in range(self._retries + 1):
            fut = loop.create_future()
            self._waiting[key] = fut
            await self._client.write_gatt_char(COMMAND_UUID, command(seq, opcode, args), response=True)
            try:
                return await asyncio.wait_for(fut, self._timeout)
            except asyncio.TimeoutError:
                continue  # same seq again: a duplicate is answered, not executed twice
            finally:
                self._waiting.pop(key, None)
        raise TimeoutError(f"no answer to opcode 0x{opcode:02X} after {self._retries + 1} tries")

    async def info(self) -> Info:
        return parse_info(bytes(await self._client.read_gatt_char(INFO_UUID)))

    async def status(self) -> Status:
        return parse_status(bytes(await self._client.read_gatt_char(STATUS_UUID)))

    async def write_record(self, uuid: str, data: bytes) -> Response:
        """Write a Profile/Config/Calibration record and wait for the device's Response (seq 0)."""
        key = (0, RECORD_OPCODES[uuid])
        loop = asyncio.get_running_loop()
        fut = loop.create_future()
        self._waiting[key] = fut
        try:
            await self._client.write_gatt_char(uuid, data, response=True)
            return await asyncio.wait_for(fut, self._timeout)
        finally:
            self._waiting.pop(key, None)

    async def read_profile_header(self, profile: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, None))
        return parse_profile_header(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_profile_event(self, profile: int, event: int) -> dict:
        await self.write_record(PROFILE_UUID, profile_query(profile, event))
        return parse_profile_event(bytes(await self._client.read_gatt_char(PROFILE_UUID)))

    async def read_config(self, key: int) -> int:
        await self.write_record(CONFIG_UUID, config_query(key))
        return parse_config(bytes(await self._client.read_gatt_char(CONFIG_UUID)))[1]


# ---- command line -------------------------------------------------------------------------------------


async def _scan() -> None:
    from bleak import BleakScanner

    print("Scanning for 8 seconds...")
    for device in await BleakScanner.discover(timeout=8.0, service_uuids=[SERVICE_UUID]):
        print(f"{device.address}  {device.name}")


async def _run(args: argparse.Namespace) -> None:
    async with FeederClient(args.address) as feeder:
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
        else:
            raise SystemExit(f"unknown command {cmd}")
        print(f"{resp.result_text} (detail {resp.detail})")
        if not resp.ok:
            raise SystemExit(1)


def main() -> None:
    parser = argparse.ArgumentParser(description="Precision Feeder BLE reference client")
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

    # The address comes first on the command line ("ADDRESS status"), then the subcommand.
    import sys

    argv = sys.argv[1:]
    if argv and argv[0] == "scan":
        asyncio.run(_scan())
        return
    if len(argv) < 2:
        parser.error("usage: precision_feeder_ble.py ADDRESS COMMAND [ARGS]  |  precision_feeder_ble.py scan")
    address, rest = argv[0], argv[1:]
    args = parser.parse_args(rest)
    args.address = address
    asyncio.run(_run(args))


if __name__ == "__main__":
    main()
