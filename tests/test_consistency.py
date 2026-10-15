"""Checks that the limits repeated outside C++ agree with FeederCore's (components/precision_feeder/feeder_core.h).

FeederCore's setters are the single source of truth for every setting's range. The YAML entities repeat the ranges
as min/max for Home Assistant's input boxes, and the component schema repeats the pulse limits; this test fails if
any of them drift. Run from the repository root:  python3 -m unittest tests/test_consistency.py

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import argparse
import importlib.util
import pathlib
import re
import unittest

ROOT = pathlib.Path(__file__).resolve().parent.parent
CORE_H = (ROOT / "components/precision_feeder/feeder_core.h").read_text()


def core_constants() -> dict[str, int]:
    found = {}
    for line in CORE_H.splitlines():
        m = re.match(r"\s*constexpr \w+ (.*);", line)
        if m:
            for name, value in re.findall(r"(\w+) = (-?\d+)", m.group(1)):
                found[name] = int(value)
    return found


def yaml_entities() -> dict[str, dict[str, float]]:
    """name -> {min_value, max_value} for every entity in packages/*.yaml that has them."""
    entities = {}
    for path in (ROOT / "packages").glob("*.yaml"):
        current = None
        for line in path.read_text().splitlines():
            m = re.match(r'\s+name: "(.*)"', line)
            if m:
                current = entities.setdefault(m.group(1), {})
                continue
            m = re.match(r"\s+(min_value|max_value): (-?[\d.]+)", line)
            if m and current is not None:
                current[m.group(1)] = float(m.group(2))
    return entities


class Consistency(unittest.TestCase):
    def setUp(self):
        self.c = core_constants()
        self.e = yaml_entities()

    def check(self, entity, lo, hi, scale=1.0):
        self.assertIn(entity, self.e, f"entity {entity!r} not found in packages/")
        got = (self.e[entity].get("min_value"), self.e[entity].get("max_value"))
        self.assertEqual(got, (lo / scale, hi / scale), f"{entity}: YAML min/max {got} != feeder_core.h {lo}..{hi}")

    def test_guard_settings(self):
        c = self.c
        self.check("Minimum Gap Between Feeds", c["MIN_GAP_S_LO"], c["MIN_GAP_S_HI"])
        self.check("Gap Between Portions", c["PORTION_GAP_S_LO"], c["PORTION_GAP_S_HI"])
        self.check("Reload Pause", c["RELOAD_PAUSE_MIN_LO"], c["RELOAD_PAUSE_MIN_HI"])
        self.check("Refill Interval", c["REFILL_DAYS_LO"], c["REFILL_DAYS_HI"])
        self.check("Daily Portion Cap", c["DAILY_CAP_LO"], c["DAILY_CAP_HI"])
        self.check("Max Portions Per Command", c["PORTIONS_CMD_LO"], c["PORTIONS_CMD_HI"])

    def test_calibration(self):
        c = self.c
        self.check("Servo Min Pulse", c["PULSE_MIN_LO"], c["PULSE_MIN_HI"])
        self.check("Servo Max Pulse", c["PULSE_MAX_LO"], c["PULSE_MAX_HI"])
        self.check("Calibration Offset", c["CAL_OFFSET_LO"], c["CAL_OFFSET_HI"], scale=10.0)

    def test_location(self):
        self.check("Latitude", -90, 90)
        self.check("Longitude", -180, 180)
        self.assertIn("latitude >= -90.0f && latitude <= 90.0f", CORE_H)
        self.assertIn("longitude >= -180.0f && longitude <= 180.0f", CORE_H)

    def test_component_schema_pulse_limits(self):
        schema = (ROOT / "components/precision_feeder/__init__.py").read_text()
        ranges = [(int(a), int(b)) for a, b in re.findall(
            r"cv\.Range\(min=cv\.TimePeriod\(microseconds=(\d+)\), max=cv\.TimePeriod\(microseconds=(\d+)\)\)", schema)]
        c = self.c
        self.assertEqual(ranges, [(c["PULSE_MIN_LO"], c["PULSE_MIN_HI"]), (c["PULSE_MAX_LO"], c["PULSE_MAX_HI"])])
        self.assertIn(f"if span < {c['PULSE_SPAN_MIN']}:", schema)

    def test_mock_device_entities(self):
        """tools/mock_device.py imitates the device: every entity it serves must exist under that name."""
        spec = importlib.util.spec_from_file_location("mock_device", ROOT / "tools/mock_device.py")
        mock = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mock)
        args = argparse.Namespace(unknown=False, no_calibration=False, no_diagnostics=False, no_network=False)
        served = [e.name for e in mock.Feeder(args).entities.values()]
        self.assertGreater(len(served), 50)
        for name in served:
            self.assertIn(name, self.e, f"mock_device.py serves {name!r}, which no package defines")


if __name__ == "__main__":
    unittest.main()
