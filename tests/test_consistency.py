"""Checks that the limits repeated outside C++ agree with FeederCore's (components/precision_feeder/feeder_core.h).

FeederCore's setters are the single source of truth for every setting's range. The YAML entities repeat the ranges
as min/max for Home Assistant's input boxes, and the component schema repeats the pulse limits; this test fails if
any of them drift. Run from the repository root:  python3 -m unittest tests/test_consistency.py

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
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
SERVO_H = (ROOT / "components/precision_feeder/mechanism_servo.h").read_text()


def mechanism_substitutions(package: str) -> dict[str, str]:
    """The substitutions a mechanism package sets (its tray_capacity, ...)."""
    found = {}
    for line in (ROOT / "packages" / package).read_text().splitlines():
        m = re.match(r'  (\w+): "?([^"#]*?)"?\s*(#.*)?$', line)
        if m:
            found[m.group(1)] = m.group(2)
    return found


def core_constants() -> dict[str, int]:
    found = {}
    for line in (CORE_H + SERVO_H).splitlines():
        m = re.match(r"\s*constexpr \w+ (.*);", line)
        if m:
            for name, value in re.findall(r"(\w+) = (-?\d+)", m.group(1)):
                found[name] = int(value)
    return found


def yaml_entities() -> dict[str, dict[str, float]]:
    """name -> {min_value, max_value} for every entity in packages/*.yaml that has them. ${tray_capacity} is read as
    the servo mechanism's value (packages/mechanism_servo.yaml), the mechanism main builds with."""
    capacity = mechanism_substitutions("mechanism_servo.yaml")["tray_capacity"]
    entities = {}
    for path in (ROOT / "packages").glob("*.yaml"):
        current = None
        for line in path.read_text().splitlines():
            m = re.match(r'\s+name: "(.*)"', line)
            if m:
                current = entities.setdefault(m.group(1), {})
                continue
            line = line.replace("${tray_capacity}", capacity)
            m = re.match(r"\s+(min_value|max_value): (-?[\d.]+)", line)
            if m and current is not None:
                current[m.group(1)] = float(m.group(2))
    return entities


def entity_domains() -> dict[str, str]:
    """name -> the top-level YAML key it is defined under (number, switch, ...), for every package entity."""
    domains = {}
    for path in (ROOT / "packages").glob("*.yaml"):
        domain = None
        for line in path.read_text().splitlines():
            m = re.match(r"(\w+):", line)
            if m:
                domain = m.group(1)
            m = re.match(r'\s+name: "(.*)"', line)
            if m and domain:
                domains[m.group(1)] = domain
    return domains


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
        # The portion limits go up to the mechanism's capacity, which the YAML repeats as ${tray_capacity}.
        self.check("Daily Portion Cap", c["DAILY_CAP_LO"], c["SERVO_CAPACITY"])
        self.check("Max Portions Per Command", c["PORTIONS_CMD_LO"], c["SERVO_CAPACITY"])
        self.check("Manual Portions", 1, c["SERVO_CAPACITY"])
        self.check("Event Portions", c["EVENT_PORTIONS_LO"], c["SERVO_CAPACITY"])
        self.check("One-shot Portions", 1, c["SERVO_CAPACITY"])
        self.check("Declared Tray Position", 0, c["SERVO_CAPACITY"])

    def test_tray_capacity(self):
        """Each mechanism package's tray_capacity is that mechanism's capacity, and every portion limit uses it."""
        self.assertEqual(int(mechanism_substitutions("mechanism_servo.yaml")["tray_capacity"]), self.c["SERVO_CAPACITY"])
        for path in (ROOT / "packages").glob("*.yaml"):
            for n, line in enumerate(path.read_text().splitlines(), 1):
                self.assertNotRegex(line, r"max_value: 6$", f"{path.name}:{n}: a literal 6; use ${{tray_capacity}}")

    def test_calibration(self):
        c = self.c
        self.check("Servo Min Pulse", c["PULSE_MIN_LO"], c["PULSE_MIN_HI"])
        self.check("Servo Max Pulse", c["PULSE_MAX_LO"], c["PULSE_MAX_HI"])
        self.check("Calibration Offset", c["CAL_OFFSET_LO"], c["CAL_OFFSET_HI"], scale=10.0)

    def test_n20_mechanism(self):
        """The direct-drive disc (packages/mechanism_n20.yaml): its capacity is its slots - 1, each tuning entity's
        YAML limits are its table's (mechanism_n20.cpp, N20_PARAMS) and every protected entity it lists exists."""
        sub = mechanism_substitutions("mechanism_n20.yaml")
        header = (ROOT / "components/precision_feeder/mechanism_n20.h").read_text()
        lo, hi = map(int, re.search(r"SLOTS_MIN = (\d+), SLOTS_MAX = (\d+);", header).groups())
        # The input boxes go up to the most portions any disc may hold; the feeder holds each to its own.
        self.assertEqual(int(sub["tray_capacity"]), hi - 1)
        self.assertTrue(lo <= int(sub["disc_slots"]) <= hi)
        source = (ROOT / "components/precision_feeder/mechanism_n20.cpp").read_text()
        source = source.replace("N20Mechanism::SLOTS_MIN", str(lo)).replace("N20Mechanism::SLOTS_MAX", str(hi))
        table = re.findall(r'\{"(\w+)", "([^"]+)", "[^"]*", (-?\d+), (-?\d+), (-?\d+), (\d+)\}', source)
        self.assertEqual(len(table), 10, "N20_PARAMS not found")
        yaml = (ROOT / "packages/mechanism_n20.yaml").read_text()
        for key, name, lo, hi, _default, scale in table:
            if name.startswith("Reverse"):
                self.assertIn(f'name: "{name}"', yaml, f"{name}: a switch in mechanism_n20.yaml")
                continue
            block = yaml.split(f'name: "{name}"', 1)
            self.assertEqual(len(block), 2, f"{name} ({key}) has no entity in mechanism_n20.yaml")
            got = [float(x) for x in re.findall(r"(?:min|max)_value: (-?[\d.]+)", block[1])[:2]]
            self.assertEqual(got, [int(lo) / int(scale), int(hi) / int(scale)], f"{name}: YAML {got} != table {lo}..{hi}")
        listed = re.findall(r"^    - (\w+)/(.+)$", yaml.split("protected_entities:", 1)[1].split("\n\n", 1)[0], re.M)
        self.assertGreater(len(listed), 10)
        for domain, name in listed:
            self.assertIn(name, self.e, f"protected {domain}/{name} not defined")

    def test_forget_wifi_key(self):
        """Forget Wi-Fi (2.3.0, D30) erases ESPHome's saved network by its fixed NVS key, which is ESPHome's, not ours:
        fail here, not on a feeder, if an ESPHome upgrade changes it. Skipped where ESPHome is not installed."""
        ours = re.search(r'WIFI_SAVED_NVS_KEY = "(\d+)"', (ROOT / "components/precision_feeder/precision_feeder.cpp").read_text())
        self.assertIsNotNone(ours, "WIFI_SAVED_NVS_KEY not found")
        candidates = list((ROOT / ".venv").glob("lib/python3*/site-packages/esphome/components/wifi/wifi_component.cpp"))
        spec = importlib.util.find_spec("esphome")
        if spec is not None and spec.submodule_search_locations:
            candidates += [pathlib.Path(p) / "components/wifi/wifi_component.cpp" for p in spec.submodule_search_locations]
        sources = [c for c in candidates if c.exists()]
        if not sources:
            self.skipTest("ESPHome is not installed here")
        text = sources[0].read_text()
        m = re.search(r"has_sta\(\) \? App\.get_config_version_hash\(\) : (\d+)UL", text)
        self.assertIsNotNone(m, f"{sources[0]}: the saved-network hash for a build with no YAML network has moved")
        self.assertEqual(m.group(1), ours.group(1), "ESPHome's saved-network hash changed: update WIFI_SAVED_NVS_KEY")

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

    def test_web_app_entities(self):
        """web/src/entities.js names every entity the web app uses; each must exist in packages/, and no other
        file of the app may name an entity directly (so the table is the whole contract)."""
        domains = "sensor|binary_sensor|text_sensor|button|switch|select|number|text"
        table = (ROOT / "web/src/entities.js").read_text()
        names = [m[1] for m in re.findall(rf"""(['"])(?:{domains})/(.+?)\1""", table)]
        self.assertGreater(len(names), 60)
        for name in names:
            self.assertIn(name, self.e, f"web/src/entities.js uses {name!r}, which no package defines")
        for path in (ROOT / "web/src").rglob("*.js"):
            if path.name == "entities.js":
                continue
            literal = re.search(rf"""['"`](?:{domains})/[A-Z]""", path.read_text())
            self.assertIsNone(literal, f"{path.relative_to(ROOT)} names an entity directly; use E.<name> from entities.js")

    def test_mock_device_entities(self):
        """tools/mock_device.py imitates the device: every entity it serves must exist under that name."""
        spec = importlib.util.spec_from_file_location("mock_device", ROOT / "tools/mock_device.py")
        mock = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mock)
        args = argparse.Namespace(unknown=False, no_calibration=False, no_diagnostics=False, no_network=False, no_ble=False)
        served = [e.name for e in mock.Feeder(args).entities.values()]
        self.assertGreater(len(served), 50)
        for name in served:
            self.assertIn(name, self.e, f"mock_device.py serves {name!r}, which no package defines")


    def test_protected_entities(self):
        """The settings guard's list names real entities, and covers the guard limits and the calibration."""
        spec = importlib.util.spec_from_file_location("mock_device", ROOT / "tools/mock_device.py")
        mock = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mock)
        listed = mock.protected_entities()
        self.assertGreater(len(listed), 5)
        for entry in listed:
            domain, name = entry.split("/", 1)
            self.assertIn(name, self.e, f"protected entity {entry!r} not found in packages/")
            self.assertEqual(entity_domains().get(name), domain, entry)
        names = {entry.split("/", 1)[1] for entry in listed}
        for guard in ("Minimum Gap Between Feeds", "Gap Between Portions", "Daily Portion Cap",
                      "Portions Counted (24 h)", "Max Portions Per Command"):
            self.assertIn(guard, names, f"guard limit {guard!r} is not protected")
        calibration = re.findall(r'name: "(.*)"', (ROOT / "packages/calibration.yaml").read_text())
        for name in calibration:
            if not name.startswith("Declare"):
                self.assertIn(name, names, f"calibration entity {name!r} is not protected")

    def test_pin_options(self):
        """The pin selects offer exactly the GPIOs FeederCore accepts (all but PINS_REFUSED), plus "none" for the
        button and the LED; the board presets use only accepted pins."""
        refused = {int(v) for v in re.search(r"PINS_REFUSED\[\] = \{(.*?)\};", CORE_H).group(1).split(",")}
        gpio_max = self.c["GPIO_MAX"]
        accepted = [f"GPIO{p}" for p in range(gpio_max + 1) if p not in refused]
        for package, name, extra in (("hardware", "Servo Pin", []), ("button", "Button Pin", ["none"]),
                                     ("status_led", "LED Pin", ["none"]), ("mechanism_n20", "Motor IN1 Pin", []),
                                     ("mechanism_n20", "Motor IN2 Pin", []), ("mechanism_n20", "Motor STBY Pin", []),
                                     ("mechanism_n20", "Encoder A Pin", []), ("mechanism_n20", "Encoder B Pin", [])):
            text = (ROOT / f"packages/{package}.yaml").read_text()
            block = text[text.index(f'name: "{name}"'):]
            options = re.findall(r'"([^"]+)"', re.search(r"options: \[(.*?)\]", block, re.S).group(1))
            self.assertEqual(options, extra + accepted, f"{name} options")
        for servo, button, led in re.findall(r'\{Board::\w+, "[^"]+", \{(\d+), (\d+), (\d+)\}\}', CORE_H):
            for pin in (servo, button, led):
                self.assertNotIn(int(pin), refused, "board preset uses a refused pin")
        hardware = (ROOT / "packages/hardware.yaml").read_text()
        boards = re.findall(r'"([^"]+)"', re.search(r'name: "Board".*?options: \[(.*?)\]', hardware, re.S).group(1))
        presets = re.findall(r'\{Board::\w+, "([^"]+)"', CORE_H)
        self.assertEqual(boards, ["Build Default"] + presets + ["Custom"], "Board options")

    def test_pins_protected(self):
        spec = importlib.util.spec_from_file_location("mock_device", ROOT / "tools/mock_device.py")
        mock = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mock)
        listed = mock.protected_entities()
        for entry in ("select/Board", "select/Servo Pin", "select/Button Pin", "select/LED Pin"):
            self.assertIn(entry, listed)

    def test_timezone_length(self):
        """The Timezone entity takes as many characters as the device's parser accepts."""
        tz_h = (ROOT / "components/precision_feeder/tz_rules.h").read_text()
        limit = int(re.search(r"TZ_TEXT_MAX = (\d+);", tz_h).group(1))
        controls = (ROOT / "packages/controls.yaml").read_text()
        block = controls[controls.index('name: "Timezone"'):]
        self.assertEqual(int(re.search(r"max_length: (\d+)", block).group(1)), limit)

    def test_ntp_servers_length(self):
        """The NTP Servers entity takes as many characters as the Device record holds, and is not protected."""
        core_h = (ROOT / "components/precision_feeder/feeder_core.h").read_text()
        size = int(re.search(r"NTP_SERVERS_SIZE = (\d+);", core_h).group(1))
        controls = (ROOT / "packages/controls.yaml").read_text()
        block = controls[controls.index('name: "NTP Servers"'):]
        self.assertEqual(int(re.search(r"max_length: (\d+)", block).group(1)), size - 1)
        self.assertNotIn("text/NTP Servers", (ROOT / "packages/web_server.yaml").read_text())

    def test_timezone_golden(self):
        """tests/tz_golden.h still matches ESPHome's reference parser (skipped without aioesphomeapi)."""
        try:
            from aioesphomeapi.posix_tz import parse_posix_tz
        except ImportError:
            self.skipTest("aioesphomeapi not installed")
        rows = re.findall(r'\{"(.*?)", (-?\d+), (-?\d+), \{(.*?)\}, \{(.*?)\}\}',
                          (ROOT / "tests/tz_golden.h").read_text())
        self.assertGreater(len(rows), 80)
        for text, std, dst, start, end in rows:
            p = parse_posix_tz(text)
            rule = lambda r: ", ".join(str(v) for v in (r.time_seconds, r.day, int(r.type), r.month, r.week, r.day_of_week))
            self.assertEqual((int(std), int(dst), rule(p.dst_start), rule(p.dst_end)),
                             (p.std_offset_seconds, p.dst_offset_seconds, start, end), text)


if __name__ == "__main__":
    unittest.main()
