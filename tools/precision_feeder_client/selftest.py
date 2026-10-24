"""selftest: checks a feeder's BLE protocol without changing anything (never feeds, never touches a hold).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import asyncio
import struct

from .client import FeederClient
from .protocol import (
    CALIBRATION_SLOTS, CHANNEL_COMMAND, CHANNEL_CONFIG, CONFIG_KEYS, CONFIG_UUID, OP_SET_PROFILE, OP_SET_TIMEZONE,
    PROTOCOL_VERSION,
    command, config_set, signed,
)


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
    check("Status readable", status.tray_position < info.positions, status.describe().splitlines()[0])
    values = {}
    for name, key in CONFIG_KEYS.items():
        values[name] = await feeder.read_config(key)
    check("every Config key readable", len(values) == len(CONFIG_KEYS), ", ".join(f"{k}={v}" for k, v in values.items()))
    r = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS["daily_cap"], values["daily_cap"]))
    if r.result == 16 and r.detail == 7:
        # The feeder's "BLE Can Change Guard Limits" is off (the default): guard limits are refused before they are
        # checked, as they should be, so the two Config write checks cannot run.
        check("Config writes refused: BLE may not change guard limits (skipped two checks)", True, "detail 7")
    else:
        check("Config write (same value back) answered with its seq", r.ok and r.seq != 0, r.result_text)
        r = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS["daily_cap"], 0))
        check("out-of-range Config value refused", r.result == 8, r.result_text)
    header = await feeder.read_profile_header(status.active_profile)
    check("Profile header readable", 1 <= header["every_n"] <= 14, str(header))
    if info.has_calibration:
        offsets = [await feeder.read_calibration(i) for i in range(CALIBRATION_SLOTS)]
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

