"""The command line: python precision_feeder_ble.py ADDRESS COMMAND [ARGS] (see that file for examples).

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import argparse
import asyncio
import os
import struct
import sys
import time

from .client import FeederClient
from .protocol import *  # noqa: F401,F403
from .protocol import _FULL_DAYS  # noqa: F401
from .selftest import selftest


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
                names = list(CONFIG_KEYS) if args.config_key in (None, "all") else [args.config_key]
                for name in names:
                    print(f"{name} = {await feeder.read_config(CONFIG_KEYS[name])}")
                return
            if args.config_key not in CONFIG_KEYS or args.value is None:
                raise SystemExit(f"usage: config set KEY VALUE, KEY one of {', '.join(CONFIG_KEYS)}")
            resp = await feeder.write_record(CONFIG_UUID, config_set(CONFIG_KEYS[args.config_key], args.value))
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
        print(f"{resp.result_text} ({resp.detail_text})")
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
    # Not "key": that name is the BLE key (--key), which main() sets after parsing.
    p.add_argument("config_key", nargs="?", metavar="KEY")
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

