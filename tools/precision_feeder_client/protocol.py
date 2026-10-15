"""The Precision Feeder BLE protocol in pure Python: constants, packing, parsing, authentication.

Needs nothing outside the standard library. Byte for byte as docs/PROTOCOL.md (protocol 2); the firmware is
checked against the same golden vectors (tools/test_precision_feeder_ble.py, tests/test_protocol.cpp). Reuse this
module for other clients, such as a PLC bridge.

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import struct
import time
from dataclasses import dataclass


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


RECORD_OPCODES = {PROFILE_UUID: OP_PROFILE_WRITE, CONFIG_UUID: OP_CONFIG_WRITE, CALIBRATION_UUID: OP_CALIBRATION_WRITE}
RECORD_CHANNELS = {PROFILE_UUID: CHANNEL_PROFILE, CONFIG_UUID: CHANNEL_CONFIG, CALIBRATION_UUID: CHANNEL_CALIBRATION}


def next_seq(seq: int) -> int:
    """Next command sequence number, 1..255. 0 is never used for commands: the device answers
    Profile/Config/Calibration writes with seq 0."""
    return seq % 255 + 1


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

