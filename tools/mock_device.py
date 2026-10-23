#!/usr/bin/env python3
"""A fake Precision Feeder for building and checking the web app without hardware.

It speaks the same HTTP as ESPHome's web_server (version 3, ESPHome 2026.9.1): the index page that loads /0.css and
/0.js, the /events server-sent-event stream (a "ping" with the device config, every entity with full detail, then
state changes, log lines and a ping every 10 s), GET /<domain>/<name> and POST /<domain>/<name>/<action>. The
entities are the ones the packages in packages/ define, with plausible values and a little behaviour (feeding moves
the tray, Tray Reload refills it, the editor entities follow Edit Profile / Edit Event). It is a stand-in for the
transport, not a second implementation of the feeder: the guard here is deliberately crude, and anything it gets
wrong is the device's word that counts.

    python3 tools/mock_device.py                     http://localhost:8080 (and [::1]:8080)
    python3 tools/mock_device.py --port 8000 --no-ble --no-calibration --no-diagnostics
    python3 tools/mock_device.py --auth user:pass    HTTP basic authentication, as web_server's auth: block
    python3 tools/mock_device.py --unknown           start with the tray position unknown
    python3 tools/mock_device.py --mechanism n20     the direct-drive disc (packages/mechanism_n20.yaml) instead of the servo

Standard library only. The app is served from web/dist/ (app.js, app.css) when it exists.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import hmac
import os
import json
import math
import pathlib
import queue
import re
import socket
import socketserver
import threading
import time
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
DIST = ROOT / "web" / "dist"

# The index page ESPHome builds for js_include/css_include with empty js_url/css_url (web_server/__init__.py).
INDEX = ("<!DOCTYPE html><html><head><meta charset=UTF-8><link rel=icon href=data:>"
         "<link rel=stylesheet href=/0.css></head><body><script type=module src=/0.js></script>"
         "<esp-app></esp-app></body></html>")

CONFIG, DIAGNOSTIC = 1, 2  # entity_category as the web server sends it
DAY_NAMES = ["sun", "mon", "tue", "wed", "thu", "fri", "sat"]
DAY_ABBR = ["Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"]


class Entity:
    def __init__(self, domain: str, name: str, value=None, *, icon: str = "", category: int = 0, uom: str = "",
                 decimals: int = 0, **detail):
        self.domain, self.name, self.value = domain, name, value
        self.icon, self.category, self.uom, self.decimals, self.detail = icon, category, uom, decimals, detail

    @property
    def id(self) -> str:
        return f"{self.domain}/{self.name}"

    def json(self, full: bool) -> dict:
        d: dict = {"id": self.id}
        if full:
            d.update(domain=self.domain, name=self.name, icon=self.icon, entity_category=self.category)
            if self.detail.get("disabled"):
                d["is_disabled_by_default"] = True
        if self.domain == "button":
            return d
        if self.domain in ("sensor", "number"):
            v = self.value
            if v is None or (isinstance(v, float) and math.isnan(v)):
                d["value"], d["state"] = ("NaN" if self.domain == "number" else None), "NA"
            else:
                text = f"{v:.{self.decimals}f}"
                d["value"] = text if self.domain == "number" else round(float(v), self.decimals)
                d["state"] = f"{text} {self.uom}".strip()
            if full and self.uom:
                d["uom"] = self.uom
            if full and self.domain == "number":
                d.update({k: f"{self.detail[k]:.{self.decimals}f}" for k in ("min_value", "max_value", "step")},
                         mode=1)
        elif self.domain in ("binary_sensor", "switch"):
            d["value"], d["state"] = bool(self.value), "ON" if self.value else "OFF"
            if full and self.domain == "switch":
                d["assumed_state"] = False
        elif self.domain == "select":
            d["value"] = d["state"] = self.value
            if full:
                d["option"] = list(self.detail["options"])
        elif self.domain == "text":
            d["value"] = d["state"] = self.value
            d.update(min_length=0, max_length=self.detail["max_length"], pattern="")
            if full:
                d["mode"] = 0
        else:  # text_sensor
            d["value"] = d["state"] = self.value
        return d


# As feeder_core.h: the pins not in PINS_REFUSED, and the board presets (servo, button, LED).
PIN_OPTIONS = [f"GPIO{p}" for p in range(49)
               if p not in (0, 3, 19, 20, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 45, 46)]
BOARD_PRESETS = [("Waveshare ESP32-S3-Zero", ("GPIO1", "GPIO2", "GPIO21")),
                 ("ESP32-S3-DevKitC-1", ("GPIO4", "GPIO5", "GPIO38"))]


class Feeder:
    """The device: its entities and a crude imitation of what the firmware does with them."""

    def __init__(self, args):
        self.lock = threading.RLock()
        self.clients: list[queue.Queue] = []
        self.started = time.monotonic()
        self.position = 2
        self.disc = getattr(args, "mechanism", "servo") == "n20"
        self.capacity = 14 if self.disc else 6
        self.unknown = args.unknown
        self.calibrating = False
        self.history: list[tuple[float, int, str]] = []  # (epoch, portions, source), newest first
        self.last_feed_mono = -1e9
        self.hold_until: float | None = None
        self.edit_profile, self.edit_event = 0, 0
        self.cal_pos, self.offsets = 0, [0.0] * 7
        # 3 profiles x 4 events: [enabled, anchor, offset_min, portions, days mask (bit0 = Sunday)]
        self.profiles = [
            {"every_n": 1, "fast": 0b0000001,
             "ev": [[1, 0, 510, 1, 0x7E], [1, 0, 1050, 1, 0x7E], [0, 2, -30, 1, 0x7F], [0, 0, 720, 1, 0x7F]]},
            {"every_n": 2, "fast": 0,
             "ev": [[1, 0, 600, 1, 0x7F], [0, 0, 720, 1, 0x7F], [0, 0, 720, 1, 0x7F], [0, 0, 720, 1, 0x7F]]},
            {"every_n": 1, "fast": 0,
             "ev": [[1, 1, 30, 1, 0x7F], [1, 0, 720, 1, 0x7F], [1, 2, -60, 1, 0x7F], [0, 0, 720, 1, 0x7F]]},
        ]
        self.oneshots: list[tuple[float, int]] = []
        self.build_pins = ["GPIO1", "GPIO2", "GPIO21"]  # servo, button, LED: as packages/core.yaml
        self.saved_pins: list[str | None] = [None, None, None]  # None: the build's
        self.pins_in_use = list(self.build_pins)
        # Where the keys come from, as /feeder/auth reports them: like a prebuilt binary, the API key is provisioned
        # at run time (none yet) and BLE has the build's key.
        self.keys = {"api": "none", "ble": "off" if args.no_ble else "build"}
        # The network settings (DESIGN.md section 9.3), as GET /feeder/network reports them. Never a password.
        self.network = {"name": "precision-feeder", "name_saved": "", "name_build": "precision-feeder",
                        "ap": "build", "wifi": True, "setup": not getattr(args, "set_up", False),
                        "forget": True}  # a blank build whose network was set on the device (D30)
        self.network_boot = ("", "build")  # what the feeder started with: a change waits for a restart
        self.entities: dict[str, Entity] = {}
        self._build(args)

    # ---- entity table (names exactly as in packages/*.yaml) ----

    def _add(self, *a, **k):
        e = Entity(*a, **k)
        self.entities[e.id] = e

    def _build(self, args):
        a = self._add
        # status.yaml
        a("sensor", "Tray Position", icon="mdi:rotate-right")
        a("sensor", "Portions Remaining", icon="mdi:fish")
        a("sensor", "Portions Fed Today", icon="mdi:counter")
        a("sensor", "Runway", icon="mdi:calendar-clock", uom="d")
        a("sensor", "Minutes Since Clock Sync", 3, icon="mdi:clock-time-four-outline", category=DIAGNOSTIC, uom="min")
        a("sensor", "Capacity", self.capacity, icon="mdi:tray-full", category=DIAGNOSTIC)
        a("text_sensor", "Mechanism", "n20-direct" if self.disc else "servo", icon="mdi:cog-transfer", category=DIAGNOSTIC)
        for name, icon in [("Tray State", "mdi:state-machine"), ("Next Feed", "mdi:clock-outline"),
                           ("Hold", "mdi:pause-circle-outline"), ("Last Feed", "mdi:history"),
                           ("Last Result", "mdi:message-text-outline"), ("Recent Feeds", "mdi:format-list-bulleted"),
                           ("Active Schedule", "mdi:calendar-text"), ("One-shot Feeds", "mdi:calendar-star"),
                           ("Tray Empty On", "mdi:calendar-end"), ("Faults", "mdi:alert-circle-outline")]:
            a("text_sensor", name, "", icon=icon)
        self.entities["text_sensor/Last Result"].value = "none yet"
        a("text_sensor", "Time Source", "NTP", icon="mdi:clock-check-outline", category=DIAGNOSTIC)
        a("text_sensor", "Firmware Version", "1.1.0", category=DIAGNOSTIC)
        a("binary_sensor", "Clock Valid", True, category=DIAGNOSTIC)
        a("binary_sensor", "Needs Food", False, icon="mdi:fish-off")
        a("binary_sensor", "Schedule Over Capacity", False, icon="mdi:alert-outline")
        a("text_sensor", "Today's Feeds", "", icon="mdi:calendar-today")
        a("text_sensor", "Feed Guard", "", icon="mdi:shield-check-outline")
        a("text_sensor", "Device Time", "", icon="mdi:clock-digital", category=DIAGNOSTIC)
        # controls.yaml
        for name in ["Feed One Portion", "Feed Selected Portions", "Tray Reload", "Stop", "Hold Scheduled Feeding",
                     "Cancel Hold"]:
            a("button", name)
        a("switch", "Scheduler", True, icon="mdi:calendar-check")
        a("select", "Active Profile", "A", icon="mdi:calendar-multiple", options=["A", "B", "C"])
        a("number", "Manual Portions", 1, icon="mdi:numeric", min_value=1, max_value=6, step=1)
        a("number", "Hold Duration", 24, icon="mdi:timer-sand", uom="h", min_value=0, max_value=100000, step=1)
        a("number", "Minimum Gap Between Feeds", 60, category=CONFIG, uom="s", min_value=10, max_value=3600, step=1)
        a("number", "Gap Between Portions", 3, category=CONFIG, uom="s", min_value=1, max_value=60, step=1)
        a("number", "Daily Portion Cap", 4, category=CONFIG, icon="mdi:shield-check", min_value=1, max_value=6, step=1)
        a("number", "Portions Counted (24 h)", 0, category=DIAGNOSTIC, icon="mdi:counter", min_value=0, max_value=24,
          step=1)
        a("number", "Max Portions Per Command", 1, category=CONFIG, icon="mdi:shield-check", min_value=1, max_value=6,
          step=1)
        a("number", "Reload Pause", 10, category=CONFIG, uom="min", min_value=0, max_value=60, step=1)
        a("number", "Refill Interval", 3, category=CONFIG, uom="d", min_value=1, max_value=30, step=1)
        a("number", "Latitude", 0.0, category=CONFIG, uom="°", decimals=4, min_value=-90, max_value=90, step=0.0001)
        a("number", "Longitude", 0.0, category=CONFIG, uom="°", decimals=4, min_value=-180, max_value=180,
          step=0.0001)
        a("text", "Timezone", "", icon="mdi:map-clock-outline", category=CONFIG, max_length=63)
        # schedule_editor.yaml
        a("text_sensor", "Edited Profile", "", icon="mdi:calendar-edit")
        a("text_sensor", "Schedule Data", "", icon="mdi:code-braces", category=DIAGNOSTIC, disabled=True)
        a("select", "Edit Profile", "A", icon="mdi:pencil", options=["A", "B", "C"])
        a("select", "Edit Event", "1", icon="mdi:pencil", options=["1", "2", "3", "4"])
        a("select", "Event Anchor", "clock", icon="mdi:weather-sunset", options=["clock", "sunrise", "sunset"])
        a("switch", "Event Enabled", True, icon="mdi:check-circle-outline")
        a("text", "Event Time", "", icon="mdi:clock-edit-outline", max_length=8)
        a("text", "Event Days", "", icon="mdi:calendar-week", max_length=40)
        a("text", "Profile Fast Days", "", icon="mdi:calendar-remove", max_length=40)
        a("text", "Add One-shot Feed", "", icon="mdi:calendar-plus", max_length=20)
        a("number", "Event Portions", 1, icon="mdi:fish", min_value=1, max_value=6, step=1)
        a("number", "Profile Every N Days", 1, icon="mdi:calendar-sync", min_value=1, max_value=14, step=1)
        a("number", "One-shot Portions", 1, icon="mdi:fish", min_value=1, max_value=6, step=1)
        a("button", "Restart Cycle Today")
        a("button", "Clear One-shot Feeds")
        if self.disc:  # mechanism_n20.yaml
            a("switch", "Calibration Mode", False, icon="mdi:tune")
            for name, value, lo, hi, step, uom in [("Ticks Per Turn", 8344, 1000, 100000, 1, "ticks"),
                                                   ("Drive Power", 100, 20, 100, 1, "%"),
                                                   ("Braking Distance", 160, 10, 2000, 1, "ticks"),
                                                   ("Wiggle Amplitude", 2.0, 0.1, 5, 0.1, "°"),
                                                   ("Wiggle Cycles", 4, 1, 10, 1, None),
                                                   ("Slot Trim", 0.0, -12, 12, 0.1, "°"),
                                                   ("Calibration Turns", 1, 1, 10, 1, None),
                                                   ("Disc Slots", 15, 3, 60, 1, None)]:
                a("number", name, value, category=CONFIG, uom=uom, min_value=lo, max_value=hi, step=step)
            a("switch", "Reverse Motor", False, category=CONFIG)
            a("switch", "Reverse Encoder", False, category=CONFIG)
            for name in ["Start", "Yes", "No", "Nudge 0.1°", "Nudge 1°", "Overshot", "Zero", "Run Turns", "Measured",
                         "Next Slot", "Trim Here"]:
                a("button", f"Calibration {name}")
            a("text_sensor", "Calibration Step", "Calibration mode is off")
            a("text_sensor", "Motor Fault", "none", category=DIAGNOSTIC)
            a("sensor", "Encoder Ticks", 1113, category=DIAGNOSTIC)
            a("sensor", "Motor Loop Max Gap", 1.2, category=DIAGNOSTIC, uom="ms")
            a("number", "Declared Tray Position", 0, icon="mdi:numeric", min_value=0, max_value=14, step=1)
            a("button", "Declare Tray Position")
            for name, pin in [("Motor IN1 Pin", "GPIO1"), ("Motor IN2 Pin", "GPIO2"), ("Motor STBY Pin", "GPIO6"),
                              ("Encoder A Pin", "GPIO4"), ("Encoder B Pin", "GPIO5")]:
                a("select", name, pin, icon="mdi:pin", category=CONFIG, options=PIN_OPTIONS)
        elif not args.no_calibration:  # calibration.yaml
            a("switch", "Calibration Mode", False, icon="mdi:tune")
            a("select", "Calibration Position", "0", icon="mdi:numeric", options=[str(i) for i in range(7)])
            a("number", "Calibration Offset", 0.0, icon="mdi:angle-acute", uom="°", decimals=1, min_value=-15,
              max_value=15, step=0.1)
            a("number", "Servo Min Pulse", 544, icon="mdi:pulse", category=CONFIG, uom="µs", min_value=300,
              max_value=1500, step=1)
            a("number", "Servo Max Pulse", 2400, icon="mdi:pulse", category=CONFIG, uom="µs", min_value=1500,
              max_value=3000, step=1)
            a("number", "Declared Tray Position", 0, icon="mdi:numeric", min_value=0, max_value=6, step=1)
            a("button", "Declare Tray Position")
        # hardware.yaml, and the pin selects of button.yaml and status_led.yaml
        if getattr(args, "dual", False):  # drive_select.yaml: a firmware with both drives (D17)
            a("select", "Drive", "Build Default", icon="mdi:cog-transfer-outline", category=CONFIG,
              options=["Build Default", "Servo", "Direct-drive disc"])
        if not self.disc or getattr(args, "dual", False):  # the disc-only build leaves hardware.yaml out
            a("select", "Board", "Build Default", icon="mdi:developer-board", category=CONFIG,
              options=["Build Default"] + [name for name, _ in BOARD_PRESETS] + ["Custom"])
            a("select", "Servo Pin", "GPIO1", icon="mdi:pin", category=CONFIG, options=PIN_OPTIONS)
        a("select", "Button Pin", "GPIO2", icon="mdi:gesture-tap-button", category=CONFIG, options=["none"] + PIN_OPTIONS)
        a("select", "LED Pin", "GPIO21", icon="mdi:led-outline", category=CONFIG, options=["none"] + PIN_OPTIONS)
        a("text_sensor", "Pins In Use", "", icon="mdi:pin-outline", category=DIAGNOSTIC)
        a("binary_sensor", "Restart Needed", False, icon="mdi:restart-alert", category=DIAGNOSTIC)
        a("button", "Restart", icon="mdi:restart", category=CONFIG)
        if not args.no_diagnostics:  # diagnostics.yaml (ESPHome's debug sensors)
            a("sensor", "Free Heap", 121344, uom="B")
            a("sensor", "Largest Free Heap Block", 65536, uom="B")
            a("sensor", "Heap Fragmentation", 9, uom="%")
            a("sensor", "Loop Time", 12, uom="ms")
        if not args.no_ble:  # ble_gatt.yaml: a simulated client that connected and fed once
            a("text_sensor", "BLE Info", "", icon="mdi:bluetooth-settings", category=DIAGNOSTIC, disabled=True)
            a("switch", "BLE Can Change Guard Limits", True, icon="mdi:shield-lock-outline", category=CONFIG)
            a("switch", "BLE Can Change Calibration", True, icon="mdi:tune-variant", category=CONFIG)
            a("text_sensor", "BLE Session", "", icon="mdi:bluetooth-connect", category=DIAGNOSTIC, disabled=True)
            self.ble_activity = [
                {"id": 1, "t": 5, "kind": "connect"},
                {"id": 2, "t": 5, "kind": "auth", "name": "AUTH", "seq": 1, "result": "ok", "detail": 0},
                {"id": 3, "t": 6, "kind": "command", "name": "SET_TIME", "seq": 2, "result": "clock from BLE ignored "
                 "(network time is fresh)", "detail": 0},
                {"id": 4, "t": 9, "kind": "record", "name": "CONFIG_WRITE", "seq": 3, "result": "ok", "detail": 5},
            ]
        if not args.no_network:  # network_info.yaml (ESPHome's wifi_info and wifi_signal)
            a("text_sensor", "Wi-Fi Network", "example-network", category=DIAGNOSTIC)
            a("text_sensor", "IP Address", "192.168.1.40", category=DIAGNOSTIC)
            a("text_sensor", "IP Address 2", "fe80::1234:56ff:fe78:9abc", category=DIAGNOSTIC)
            a("text_sensor", "IP Address 3", "fd00::1234:56ff:fe78:9abc", category=DIAGNOSTIC)
            a("text_sensor", "IP Address 4", "", category=DIAGNOSTIC)
            a("sensor", "Wi-Fi Signal", -58, uom="dBm")
        self.refresh(publish=False)

    # ---- helpers ----

    def uptime(self) -> int:
        return int(time.monotonic() - self.started)

    def get(self, domain: str, name: str):
        return self.entities[f"{domain}/{name}"].value

    def set(self, domain: str, name: str, value, publish: bool = True):
        e = self.entities.get(f"{domain}/{name}")
        if e is None or e.value == value:
            return
        e.value = value
        if publish:
            self.broadcast("state", json.dumps(e.json(False)))

    def fmt(self, epoch: float) -> str:
        t = time.localtime(epoch)
        return f"{DAY_ABBR[(t.tm_wday + 1) % 7]} {time.strftime('%Y-%m-%d %H:%M', t)}"

    @staticmethod
    def days_text(mask: int) -> str:
        mask &= 0x7F
        if mask == 0x7F:
            return "all"
        if mask == 0:
            return "none"
        return ",".join(DAY_NAMES[i] for i in range(7) if mask & (1 << i))

    @staticmethod
    def parse_days(text: str) -> int | None:
        text = text.strip().lower()
        words = {"all": 0x7F, "none": 0, "weekdays": 0x3E, "weekend": 0x41}
        if text in words:
            return words[text]
        mask = 0
        for part in filter(None, (p.strip() for p in text.split(","))):
            if part[:3] not in DAY_NAMES:
                return None
            mask |= 1 << DAY_NAMES.index(part[:3])
        return mask

    @staticmethod
    def event_time_text(ev) -> str:
        if ev[1] == 0:
            return f"{ev[2] // 60:02d}:{ev[2] % 60:02d}"
        a = abs(ev[2])
        return f"{'-' if ev[2] < 0 else '+'}{a // 60:02d}:{a % 60:02d}"

    def summary(self, p: int) -> str:
        prof = self.profiles[p]
        parts = []
        for ev in sorted((e for e in prof["ev"] if e[0]), key=lambda e: e[2] + (0, 360, 1080)[e[1]]):
            t = self.event_time_text(ev)
            if ev[1]:
                t = ("sunrise", "sunset")[ev[1] - 1] + t
            parts.append(f"{t} x{ev[3]}" + ("" if ev[4] == 0x7F else f" ({self.days_text(ev[4])})"))
        out = f"{'ABC'[p]}: " + (", ".join(parts) or "no events")
        if prof["every_n"] > 1:
            out += f"; every {prof['every_n']} days"
        if prof["fast"]:
            out += f"; fast: {self.days_text(prof['fast'])}"
        if not self.get("switch", "Scheduler"):
            out += " (scheduler off)"
        return out

    def remaining(self) -> int:
        return self.capacity - self.position

    def fed_24h(self) -> int:
        return sum(n for t, n, _ in self.history if t > time.time() - 86400)

    def refresh(self, publish: bool = True):
        """Recompute every derived entity, publishing only what changed (as the firmware's 1 s interval does)."""
        def s(domain, name, value):
            self.set(domain, name, value, publish)

        if self.hold_until is not None and self.hold_until != math.inf and time.time() > self.hold_until:
            self.hold_until = None
        state = ("calibrating" if self.calibrating else "unknown" if self.unknown
                 else "empty" if self.position >= self.capacity else "loaded")
        s("sensor", "Tray Position", self.position)
        s("sensor", "Portions Remaining", self.remaining())
        midnight = time.mktime(time.localtime()[:3] + (0, 0, 0, 0, 0, -1))
        s("sensor", "Portions Fed Today", sum(n for t, n, _ in self.history if t >= midnight))
        s("sensor", "Runway", None if self.position >= self.capacity else float(self.remaining() // 2))
        s("text_sensor", "Tray State", state)
        nxt = time.time() + 3 * 3600
        s("text_sensor", "Next Feed", self.fmt(nxt - nxt % 60))
        s("text_sensor", "Hold", "none" if self.hold_until is None else "until cancelled"
          if self.hold_until == math.inf else "until " + self.fmt(self.hold_until))
        if self.history:
            t, _, src = self.history[0]
            s("text_sensor", "Last Feed", f"{self.fmt(t)} ({src})")
        else:
            s("text_sensor", "Last Feed", "never")
        s("text_sensor", "Recent Feeds", "; ".join(
            f"{time.strftime('%m-%d %H:%M', time.localtime(t))} x{n} {src}" for t, n, src in self.history[:6])
          or "none")
        s("text_sensor", "Active Schedule", self.summary("ABC".index(self.get("select", "Active Profile"))))
        s("text_sensor", "One-shot Feeds", "; ".join(f"{self.fmt(t)} x{n}" for t, n in self.oneshots) or "none")
        s("text_sensor", "Tray Empty On", "empty now" if self.position >= self.capacity else self.fmt(time.time() + 2 * 86400))
        faults = []
        if self.unknown:
            faults.append("tray position unknown")
        if self.position >= self.capacity:
            faults.append("tray empty")
        s("text_sensor", "Faults", "; ".join(faults) or "none")
        s("binary_sensor", "Needs Food", self.position >= self.capacity)
        s("number", "Portions Counted (24 h)", self.fed_24h())
        guard = self.guard(1)
        s("text_sensor", "Feed Guard", "ready" if guard == "ok" else guard)
        off = time.localtime().tm_gmtoff // 60
        s("text_sensor", "Device Time", f"{self.fmt(time.time())} {'-' if off < 0 else '+'}{abs(off) // 60:02d}:{abs(off) % 60:02d}")
        s("text_sensor", "Today's Feeds", self.todays_feeds())
        if "text_sensor/BLE Session" in self.entities:
            up = self.uptime()
            s("text_sensor", "BLE Session", f"connected=1;auth=1;peer=4A:21;mtu=185;since=5;last={max(9, up - up % 5)};"
              f"conns=1;wr=4;wrb=76;rd={up // 5};rdb={up // 5 * 20};nt={up // 2};ntb={up // 2 * 20};authok=1;"
              "authfail=0;refused=0;invalid=0")
        s("text_sensor", "Schedule Data", "1" + "".join(
            f"|{p['every_n']},{p['fast']:02x}" + "".join(
                f",{e[0]}{'crs'[e[1]]}{e[2]}/{e[3]}/{e[4]:02x}" for e in p["ev"]) for p in self.profiles))
        # the schedule editor follows Edit Profile / Edit Event
        p = self.profiles[self.edit_profile]
        ev = p["ev"][self.edit_event]
        active = "ABC".index(self.get("select", "Active Profile")) == self.edit_profile
        s("text_sensor", "Edited Profile", self.summary(self.edit_profile) + (" [active]" if active else " [not active]"))
        s("select", "Edit Profile", "ABC"[self.edit_profile])
        s("select", "Edit Event", str(self.edit_event + 1))
        s("select", "Event Anchor", ("clock", "sunrise", "sunset")[ev[1]])
        s("switch", "Event Enabled", bool(ev[0]))
        s("text", "Event Time", self.event_time_text(ev))
        s("text", "Event Days", self.days_text(ev[4]))
        s("text", "Profile Fast Days", self.days_text(p["fast"]))
        s("number", "Event Portions", ev[3])
        s("number", "Profile Every N Days", p["every_n"])
        pins = self.effective_pins()
        presets = [name for name, p in BOARD_PRESETS if list(p) == pins]
        s("select", "Board", "Build Default" if self.saved_pins == [None, None, None] else
          presets[0] if presets else "Custom")
        s("select", "Servo Pin", pins[0])
        s("select", "Button Pin", pins[1])
        s("select", "LED Pin", pins[2])
        s("text_sensor", "Pins In Use", "servo {}, button {}, LED {}".format(*self.pins_in_use))
        s("binary_sensor", "Restart Needed", pins != self.pins_in_use or self.network_restart())
        if "text_sensor/BLE Info" in self.entities:
            s("text_sensor", "BLE Info", "service=02dd1dd9-ba75-4bbc-bdea-306d21010001;protocol=2;auth=hmac-sha256;"
              f"time=fallback_only;stale_h=24;key={self.keys['ble']};"
              f"guard={int(self.get('switch', 'BLE Can Change Guard Limits'))};"
              f"calibration={int(self.get('switch', 'BLE Can Change Calibration'))}")
        if "switch/Calibration Mode" in self.entities:
            s("switch", "Calibration Mode", self.calibrating)
        if "select/Calibration Position" in self.entities:
            s("select", "Calibration Position", str(self.cal_pos))
            s("number", "Calibration Offset", self.offsets[self.cal_pos])
        if self.disc and not self.calibrating:
            s("text_sensor", "Calibration Step", "Calibration mode is off")

    def network_restart(self) -> bool:
        return (self.network["name_saved"], self.network["ap"]) != self.network_boot

    def effective_pins(self) -> list[str]:
        return [saved or build for saved, build in zip(self.saved_pins, self.build_pins)]

    def log(self, level: str, tag: str, msg: str):
        colour = {"E": "1;31", "W": "0;33", "I": "0;32", "D": "0;36"}.get(level, "0")
        self.broadcast("log", f"\033[{colour}m[{level}][{tag}]: {msg}\033[0m")

    def broadcast(self, event: str, data: str):
        for q in list(self.clients):
            q.put((event, data))

    # ---- actions ----

    def guard(self, portions: int) -> str:
        if portions > self.get("number", "Max Portions Per Command"):
            return "too many portions for one command"
        if self.calibrating:
            return "calibrating"
        if self.unknown:
            return "position unknown"
        if self.position >= self.capacity:
            return "tray empty"
        if time.monotonic() - self.last_feed_mono < self.get("number", "Minimum Gap Between Feeds"):
            return "too soon"
        if self.fed_24h() + portions > self.get("number", "Daily Portion Cap"):
            return "daily cap reached"
        return "ok"

    def todays_feeds(self) -> str:
        """A rough Today's Feeds: clock events of the active profile only (sun events at 06:00 / 18:00)."""
        p = self.profiles["ABC".index(self.get("select", "Active Profile"))]
        wday = (time.localtime().tm_wday + 1) % 7
        if not self.get("switch", "Scheduler"):
            return "none (scheduler off)"
        if p["fast"] & (1 << wday):
            return "none (fast day)"
        now = time.localtime()
        minute = now.tm_hour * 60 + now.tm_min
        items = sorted((e[2] + (0, 360, 1080)[e[1]], e[3]) for e in p["ev"] if e[0] and e[4] & (1 << wday))
        out, nxt = [], True
        for at, n in items:
            fed = any(src == "schedule" for _, _, src in self.history)
            status = ("fed" if fed else "skipped") if at < minute else "due" if at == minute else "later"
            if status == "later" and nxt:
                status, nxt = "next", False
            out.append(f"{at // 60:02d}:{at % 60:02d} x{n} {status}")
        return "; ".join(out) or "none"

    def feed(self, portions: int):
        result = self.guard(portions)
        if result == "ok":
            portions = min(portions, self.remaining())
            self.position += portions
            self.last_feed_mono = time.monotonic()
            self.history.insert(0, (time.time(), portions, "ESPHome"))
            result = "ok"
            self.log("I", "precision_feeder", f"Feed x{portions} (ESPHome): ok")
        if result != "ok":
            self.log("W", "precision_feeder", f"Feed refused (ESPHome): {result}")
        self.set("text_sensor", "Last Result", result)

    def action(self, domain: str, name: str, action: str, params: dict) -> bool:
        key = f"{domain}/{name}"
        if key not in self.entities:
            return False
        value = params.get("value", params.get("option", ""))
        ev = self.profiles[self.edit_profile]["ev"][self.edit_event]
        if domain == "button" and action == "press":
            if name == "Feed One Portion":
                self.feed(1)
            elif name == "Feed Selected Portions":
                self.feed(int(self.get("number", "Manual Portions")))
            elif self.disc and name.startswith("Calibration "):
                # A plausible next step only: the device's own flow is mechanism_n20.cpp (tests/test_n20.cpp).
                step = {"Calibration Start": "Step 1 of 5: did the disc just turn the way it should when it feeds? "
                                             "Press Yes or No.",
                        "Calibration Yes": "Step 2 of 5: centre one empty slot exactly over the hole with Nudge "
                                           "(forward only; too far? Overshot backs up), then press Zero.",
                        "Calibration Zero": "Step 3 of 5: press Run turns. The disc makes 1 full turn(s) and stops "
                                            "close to the same slot.",
                        "Calibration Run Turns": "Step 3 of 5: nudge until that same slot is centred over the hole "
                                                 "again (too far? Overshot), then press Measured.",
                        "Calibration Measured": "Step 4 of 5: 8351 ticks per turn. Press Next slot: the disc moves "
                                                "one slot exactly as a feed does, then you check it.",
                        "Calibration Next Slot": "Step 4 of 5, slot 1 of 15: centred over the hole? Yes: Next slot. "
                                                 "No: nudge it to the centre (too far? Overshot), then press Trim here.",
                        "Calibration Trim Here": "Step 4 of 5: trim set to +0.8 degrees; every slot moves with it. "
                                                 "Press Next slot to check the next one."}.get(name)
                if self.calibrating and step:
                    self.set("text_sensor", "Calibration Step", step)
            elif name == "Tray Reload":
                self.position, self.unknown = 0, False
                self.log("I", "precision_feeder", "Tray reloaded: position 0, scheduled feeding paused")
            elif name == "Stop":
                self.log("I", "precision_feeder", "Stop")
            elif name == "Hold Scheduled Feeding":
                h = self.get("number", "Hold Duration")
                self.hold_until = math.inf if h == 0 else time.time() + h * 3600
            elif name == "Cancel Hold":
                self.hold_until = None
            elif name == "Clear One-shot Feeds":
                self.oneshots.clear()
            elif name == "Restart":
                self.log("I", "precision_feeder", "Restarting (Restart button)")
                self.pins_in_use = self.effective_pins()
                self.network_boot = (self.network["name_saved"], self.network["ap"])
                self.network["name"] = self.network["name_saved"] or self.network["name_build"]
                self.started = time.monotonic()
            elif name == "Declare Tray Position":
                self.position, self.unknown = int(self.get("number", "Declared Tray Position")), False
        elif domain == "switch" and action in ("turn_on", "turn_off", "toggle"):
            on = {"turn_on": True, "turn_off": False}.get(action, not self.get("switch", name))
            if name == "Calibration Mode":
                self.calibrating = on
            elif name == "Event Enabled":
                ev[0] = int(on)
            else:
                self.set("switch", name, on)
        elif domain == "select" and action == "set":
            if value not in self.entities[key].detail["options"]:
                return True  # ignored, as on the device
            if name == "Edit Profile":
                self.edit_profile = "ABC".index(value)
            elif name == "Edit Event":
                self.edit_event = int(value) - 1
            elif name == "Event Anchor":
                ev[1] = ("clock", "sunrise", "sunset").index(value)
            elif name == "Calibration Position":
                self.cal_pos = int(value)
            elif name == "Board":
                presets = dict(BOARD_PRESETS)
                if value == "Build Default":
                    self.saved_pins = [None, None, None]
                elif value in presets:
                    self.saved_pins = list(presets[value])
            elif name in ("Servo Pin", "Button Pin", "LED Pin"):
                role = ("Servo Pin", "Button Pin", "LED Pin").index(name)
                after = self.effective_pins()
                after[role] = value
                taken = [p for p in after if p != "none"]
                if len(taken) != len(set(taken)):
                    self.log("W", "precision_feeder", f"{value} refused: another part already uses it")
                else:
                    self.saved_pins[role] = value
                    self.log("I", "precision_feeder", f"Pin set to {value}; restart to apply")
            else:
                self.set("select", name, value)
        elif domain == "number" and action == "set":
            e = self.entities[key]
            try:
                x = float(value)
            except ValueError:
                return True
            if not e.detail["min_value"] <= x <= e.detail["max_value"]:
                return True  # out of range: ignored, as on the device
            if name == "Event Portions":
                ev[3] = int(x)
            elif name == "Profile Every N Days":
                self.profiles[self.edit_profile]["every_n"] = int(x)
            elif name == "Calibration Offset":
                self.offsets[self.cal_pos] = round(x, 1)
            elif name == "Portions Counted (24 h)":  # as set_portions_24h: an adjustment, or forget the oldest
                diff = int(x) - self.fed_24h()
                if diff > 0:
                    self.history.insert(0, (time.time(), diff, "adjustment"))
                while diff < 0 and self.history:
                    t, n, src = self.history.pop()
                    diff += n
                self.log("W", "precision_feeder", f"24 h count set by hand to {int(x)}")
            else:
                self.set("number", name, round(x, e.decimals) if e.decimals else int(x))
        elif domain == "text" and action == "set":
            if name == "Event Time":
                self.set_event_time(ev, value)
            elif name in ("Event Days", "Profile Fast Days"):
                mask = self.parse_days(value)
                if mask is None:
                    self.log("W", "schedule_editor", f"Days '{value}' not understood")
                elif name == "Event Days":
                    ev[4] = mask
                else:
                    self.profiles[self.edit_profile]["fast"] = mask
            elif name == "Timezone":
                # The device parses it (tz_rules.h); the mock only keeps it, and shows the clock as before.
                self.set("text", "Timezone", value.strip())
                self.log("I", "precision_feeder", f"Timezone set to {value.strip() or 'the build' + chr(39) + 's'}")
            elif name == "Add One-shot Feed" and value:
                try:
                    t = time.mktime(time.strptime(value, "%Y-%m-%d %H:%M"))
                    self.oneshots.append((t, int(self.get("number", "One-shot Portions"))))
                except ValueError:
                    self.log("W", "schedule_editor", f"One-shot '{value}' rejected")
        else:
            return False
        self.refresh()
        return True

    def set_event_time(self, ev, text: str):
        text = text.strip()
        try:
            sign = -1 if text.startswith("-") else 1
            h, _, m = text.lstrip("+-").partition(":")
            minutes = int(h) * 60 + int(m or 0)
            if ev[1] == 0 and text[0] not in "+-" and 0 <= minutes < 1440:
                ev[2] = minutes
            elif ev[1] != 0 and minutes <= 720:
                ev[2] = sign * minutes
            else:
                raise ValueError
        except (ValueError, IndexError):
            self.log("W", "schedule_editor", f"Event time '{text}' not understood")


def protected_entities() -> list[str]:
    """The settings guard's list, read from packages/web_server.yaml (the device's own list)."""
    names, inside = [], False
    for line in (ROOT / "packages/web_server.yaml").read_text().splitlines():
        if line.strip() == "protected_entities:":
            inside = True
        elif inside and line.strip().startswith("- "):
            names.append(line.strip()[2:])
        elif inside and line.strip() and not line.strip().startswith("#"):
            inside = False
    return names


class SettingsPassword:
    """The device's settings password (FeederCore, feeder_password.cpp): one-time challenges and HMAC answers."""

    ITERATIONS = 20000

    def __init__(self):
        self.salt = bytes(16)
        self.key: bytes | None = None
        self.nonce: bytes | None = None
        self.protected = protected_entities()

    def challenge(self, keys: dict) -> dict:
        self.nonce = os.urandom(16)
        return {"scheme": 1 if self.key else 0, "iterations": self.ITERATIONS, "salt": self.salt.hex(),
                "nonce": self.nonce.hex(), "wait": 0, "keys": keys, "protected": self.protected}

    def set_key(self, header: str | None, path: str, key_hex: str) -> bytes | None | bool:
        """POST /feeder/keys/...: the unmasked key, None to clear, or False if refused."""
        nonce = bytes.fromhex(header[:32]) if header else bytes(16)
        current = self.key
        if not self.check(header, f"{path}\n{key_hex}"):
            return False
        if not key_hex:
            return None
        mask = hmac.new(current, nonce + b"new key", hashlib.sha256).digest() if current else bytes(32)
        return bytes(a ^ b for a, b in zip(bytes.fromhex(key_hex), mask))

    def unmask_text(self, nonce: bytes, masked_hex: str) -> str:
        """A secret text sent masked (FeederCore::password_unmask_text): two HMAC blocks, none without a password."""
        masked = bytes.fromhex(masked_hex)
        mask = (hmac.new(self.key, nonce + b"new secret 1", hashlib.sha256).digest() +
                hmac.new(self.key, nonce + b"new secret 2", hashlib.sha256).digest()) if self.key else bytes(64)
        return bytes(a ^ b for a, b in zip(masked, mask)).decode("latin-1")

    def check(self, header: str | None, message: str) -> bool:
        if self.key is None:
            return True
        nonce, self.nonce = self.nonce, None
        if not header or nonce is None or header[:32] != nonce.hex():
            return False
        tag = hmac.new(self.key, nonce + message.encode(), hashlib.sha256).hexdigest()
        return hmac.compare_digest(tag, header[32:].lower())

    def change(self, header: str | None, salt_hex: str, key_hex: str) -> bool:
        nonce = bytes.fromhex(header[:32]) if header else bytes(16)
        current = self.key
        if not self.check(header, f"/feeder/auth/password\n{salt_hex}{key_hex}"):
            return False
        if not key_hex:
            self.key = None
            return True
        mask = hmac.new(current, nonce + b"new key", hashlib.sha256).digest() if current else bytes(32)
        self.salt = bytes.fromhex(salt_hex)
        self.key = bytes(a ^ b for a, b in zip(bytes.fromhex(key_hex), mask))
        return True


def make_handler(feeder: Feeder, auth: str | None, ping_s: float):
    password = SettingsPassword()

    expected = "Basic " + base64.b64encode(auth.encode()).decode() if auth else None

    class Handler(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, fmt, *args):  # quieter than the default
            pass

        def _authorized(self) -> bool:
            if expected is None or self.headers.get("Authorization") == expected:
                return True
            self.send_response(401)
            self.send_header("WWW-Authenticate", 'Basic realm="Login Required"')
            self.send_header("Content-Length", "0")
            self.end_headers()
            return False

        def _send(self, code: int, body: bytes = b"", ctype: str = "text/plain"):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def _parts(self):
            url = urllib.parse.urlsplit(self.path)
            parts = [urllib.parse.unquote(p) for p in url.path.split("/") if p]
            params = {k: v[0] for k, v in urllib.parse.parse_qs(url.query).items()}
            return url.path, parts, params

        def do_GET(self):
            if not self._authorized():
                return
            path, parts, params = self._parts()
            if path == "/":
                return self._send(200, INDEX.encode(), "text/html")
            if path in ("/0.js", "/0.css"):
                f = DIST / ("app.js" if path == "/0.js" else "app.css")
                body = f.read_bytes() if f.exists() else b"/* web/dist is not built yet */"
                return self._send(200, body, "text/javascript" if path.endswith("js") else "text/css")
            if path == "/events":
                return self._events()
            if path == "/feeder/auth":
                with feeder.lock:
                    return self._send(200, json.dumps(password.challenge(feeder.keys)).encode(), "application/json")
            if path == "/feeder/network":
                with feeder.lock:
                    body = dict(feeder.network, restart=feeder.network_restart())
                return self._send(200, json.dumps(body).encode(), "application/json")
            if path == "/feeder/ble" and hasattr(feeder, "ble_activity"):
                with feeder.lock:
                    body = json.dumps({"entries": feeder.ble_activity}).encode()
                return self._send(200, body, "application/json")
            if len(parts) == 2:
                with feeder.lock:
                    e = feeder.entities.get("/".join(parts))
                    if e is not None:
                        body = json.dumps(e.json(params.get("detail") == "all")).encode()
                        return self._send(200, body, "application/json")
            self._send(404)

        def do_POST(self):
            if not self._authorized():
                return
            path, parts, params = self._parts()
            length = int(self.headers.get("Content-Length") or 0)
            if length:
                params.update({k: v[0] for k, v in urllib.parse.parse_qs(self.rfile.read(length).decode()).items()})
            header = self.headers.get("X-Feeder-Auth")
            refused = (403, b'{"error":"refused","wait":0}', "application/json")
            with feeder.lock:
                if path == "/feeder/auth/check":
                    return self._send(200, b"{}", "application/json") if password.check(header, path + "\n") \
                        else self._send(*refused)
                if path in ("/feeder/keys/api", "/feeder/keys/ble"):
                    kind = path.rsplit("/", 1)[1]
                    key = password.set_key(header, path, params.get("key", ""))
                    if key is False:
                        return self._send(*refused)
                    if feeder.keys[kind] in ("off", "build" if kind == "api" else "-"):
                        return self._send(409, b'{"error":"build"}', "application/json")
                    if key is not None and (len(key) != 32 or not any(key)):  # clearing: both keys (2.3.0)
                        return self._send(400, b'{"error":"format"}', "application/json")
                    feeder.keys[kind] = "device" if key else "none"
                    feeder.log("W", "precision_feeder", f"{kind.upper()} key {'set' if key else 'cleared'} from the web app")
                    return self._send(200, b"{}", "application/json")
                if path == "/feeder/setup":
                    done = params.get("done", "")
                    if not password.check(header, f"{path}\n{done}"):
                        return self._send(*refused)
                    if done not in ("0", "1"):
                        return self._send(400, b'{"error":"format"}', "application/json")
                    feeder.network["setup"] = done == "0"
                    feeder.log("I", "precision_feeder", "Setup finished" if done == "1" else "Setup offered again")
                    return self._send(200, b"{}", "application/json")
                if path.startswith("/feeder/network/"):
                    return self._send(*self._network(path, params, header, refused))
                if path == "/feeder/auth/password":
                    ok = password.change(header, params.get("salt", ""), params.get("key", ""))
                    return self._send(200, b"{}", "application/json") if ok else self._send(*refused)
                if len(parts) == 3 and "/".join(parts[:2]) in password.protected:
                    value = params.get("value", params.get("option", ""))
                    if not password.check(header, "/" + "/".join(parts) + "\n" + value):
                        return self._send(*refused)
            if len(parts) == 3:
                with feeder.lock:
                    if feeder.action(parts[0], parts[1], parts[2], params):
                        return self._send(200)
            self._send(404)

        def _network(self, path: str, params: dict, header: str | None, refused: tuple) -> tuple:
            """POST /feeder/network/name|ap|wifi|forget, as the device's settings guard checks them."""
            bad = (400, b'{"error":"format"}', "application/json")
            kind = path.rsplit("/", 1)[1]
            name, ssid_hex, pw_hex = params.get("name", ""), params.get("ssid", ""), params.get("password", "")
            nonce = bytes.fromhex(header[:32]) if header else bytes(16)
            signed = {"name": name, "ap": pw_hex, "wifi": ssid_hex + pw_hex, "forget": ""}.get(kind)
            if signed is None:
                return 404, b"", "text/plain"
            if not password.check(header, f"{path}\n{signed}"):
                return refused
            if kind == "forget":
                if not feeder.network["forget"]:
                    return 409, b'{"error":"build"}', "application/json"
                feeder.network["forget"] = False
                feeder.log("W", "precision_feeder", "Saved Wi-Fi network forgotten: restarting into the setup access point")
                return 200, b"{}", "application/json"
            try:
                secret = password.unmask_text(nonce, pw_hex)
            except ValueError:
                return bad
            if kind == "name":
                if name and not re.fullmatch(r"[a-z0-9]([a-z0-9-]{0,29}[a-z0-9])?", name):
                    return bad
                feeder.network["name_saved"] = name
                feeder.log("I", "precision_feeder", f"Name set to {name or 'the build' + chr(39) + 's'} (after a restart)")
            elif kind == "ap":
                if secret and not re.fullmatch(r"[\x20-\x7e]{8,63}", secret):
                    return bad
                feeder.network["ap"] = "device" if secret else "build"
                feeder.log("I", "precision_feeder", "Access-point password changed (after a restart)")
            else:
                try:
                    ssid = bytes.fromhex(ssid_hex)
                except ValueError:
                    return bad
                if not 1 <= len(ssid) <= 32 or (secret and not (re.fullmatch(r"[\x20-\x7e]{8,63}", secret) or
                                                                re.fullmatch(r"[0-9a-fA-F]{64}", secret))):
                    return bad
                feeder.log("W", "precision_feeder", "Wi-Fi network set from the web app: joining it now")
            return 200, b"{}", "application/json"

        def _events(self):
            q: queue.Queue = queue.Queue()
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            with feeder.lock:
                config = {"title": "Precision Feeder", "comment": "", "ota": False, "log": True, "lang": "en",
                          "uptime": feeder.uptime()}
                q.put(("ping", json.dumps(config)))
                for e in feeder.entities.values():
                    q.put(("state", json.dumps(e.json(True))))
                feeder.clients.append(q)
            try:
                while True:
                    try:
                        event, data = q.get(timeout=ping_s)
                    except queue.Empty:
                        event, data = "ping", json.dumps({"uptime": feeder.uptime()})
                    self.wfile.write(f"event: {event}\ndata: {data}\n\n".encode())
                    self.wfile.flush()
            except OSError:
                pass
            finally:
                with feeder.lock:
                    feeder.clients.remove(q)

    return Handler


class Server(ThreadingHTTPServer):
    daemon_threads = True

    def server_bind(self):
        # HTTPServer.server_bind looks the host name up (getfqdn), which can hang with no DNS; skip it.
        socketserver.TCPServer.server_bind(self)
        self.server_name, self.server_port = str(self.server_address[0]), self.server_address[1]


class DualStackServer(Server):
    """Listens on IPv6 and, where the system allows it, IPv4 on the same socket."""

    address_family = socket.AF_INET6

    def server_bind(self):
        try:
            self.socket.setsockopt(socket.IPPROTO_IPV6, socket.IPV6_V6ONLY, 0)
        except OSError:
            pass
        super().server_bind()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="::", help="address to listen on (default: all, IPv6 and IPv4)")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--set-up", action="store_true", help="a feeder already set up (no setup wizard)")
    ap.add_argument("--auth", metavar="USER:PASS", help="require HTTP basic authentication")
    ap.add_argument("--no-calibration", action="store_true", help="as a build without the calibration package")
    ap.add_argument("--no-diagnostics", action="store_true", help="as a build without the diagnostics package")
    ap.add_argument("--no-network", action="store_true", help="as a build without the network_info package")
    ap.add_argument("--no-ble", action="store_true", help="as a build without the ble_gatt package")
    ap.add_argument("--unknown", action="store_true", help="start with the tray position unknown")
    ap.add_argument("--dual", action="store_true", help="a firmware with both drives: Drive, and the board and pins")
    ap.add_argument("--mechanism", choices=["servo", "n20"], default="servo",
                    help="the servo fan tray (default) or the direct-drive disc (mechanism_n20.yaml)")
    ap.add_argument("--ping", type=float, default=10.0, help="seconds between keep-alive pings (default 10)")
    args = ap.parse_args()

    feeder = Feeder(args)
    server_cls = DualStackServer if ":" in args.host else Server
    server = server_cls((args.host, args.port), make_handler(feeder, args.auth, args.ping))

    def tick():  # the firmware's 1-second interval
        while True:
            time.sleep(1)
            with feeder.lock:
                feeder.refresh()

    threading.Thread(target=tick, daemon=True).start()
    print(f"Mock Precision Feeder on http://localhost:{args.port}/ (Ctrl+C to stop)", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
