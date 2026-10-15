"""Tests for the reference client's packing and parsing.

The byte vectors are the same golden vectors used by tests/test_protocol.cpp (the firmware side),
so the client and the firmware are checked against the same bytes. No Bluetooth needed.

Run:  python3 -m unittest test_precision_feeder_ble   (from this directory)

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import unittest

import precision_feeder_ble as pf


def b(*values: int) -> bytes:
    return bytes(values)


class GoldenVectors(unittest.TestCase):
    def test_status_parses(self):
        data = b(0x02, 0x04, 0x00, 0x01, 0x11, 0x00, 0x80, 0x47, 0xED, 0x68, 0x02, 0x01, 0x50, 0xFE, 0xED, 0x68, 0x03, 0x02, 0x7D, 0x00)
        s = pf.parse_status(data)
        self.assertEqual((s.tray_position, s.remaining, s.tray_state, s.active_profile), (2, 4, 0, 1))
        self.assertEqual(s.flags, 0x11)
        self.assertEqual(s.flag_names, ["time_valid", "scheduler_enabled"])
        self.assertEqual((s.last_feed_epoch, s.last_feed_source, s.feeds_today), (1760380800, 2, 1))
        self.assertEqual(s.next_event_epoch, 1760427600)
        self.assertEqual((s.runway_days, s.time_source, s.time_age_min), (3, 2, 125))
        self.assertIn("NTP", s.describe())
        over = pf.parse_status(data[:4] + b(0x11, 0x08) + data[6:])
        self.assertIn("schedule_over_cap", over.flag_names)
        food = pf.parse_status(data[:4] + b(0x11, 0x10) + data[6:])
        self.assertIn("needs_food", food.flag_names)
        kept = pf.parse_status(data[:17] + b(0x00) + data[18:])  # source 0, clock valid
        self.assertIn("kept across restart", kept.describe())
        unset = pf.parse_status(data[:4] + b(0x10, 0x00) + data[6:17] + b(0x00) + data[18:])  # source 0, not valid
        self.assertIn("clock:         not set", unset.describe())

    def test_status_wrong_size(self):
        with self.assertRaises(ValueError):
            pf.parse_status(b(0) * 19)

    def test_profile_header(self):
        data = b(0x00, 0x01, 0x02, 0x01, 0x03, 0x51)
        self.assertEqual(pf.profile_header(1, 2, 0x01, 20739), data)
        self.assertEqual(pf.parse_profile_header(data), {"profile": 1, "every_n": 2, "fast_mask": 1, "anchor_day": 20739})

    def test_profile_event_sunset_plus_three_hours(self):
        data = b(0x01, 0x00, 0x02, 0x05, 0xB4, 0x00, 0x02, 0x3E)
        self.assertEqual(pf.profile_event(0, 2, True, "sunset", 180, 2, pf.days_mask("mon", "tue", "wed", "thu", "fri")), data)
        ev = pf.parse_profile_event(data)
        self.assertEqual((ev["anchor"], ev["offset_min"], ev["portions"], ev["days"], ev["enabled"]), ("sunset", 180, 2, 0x3E, True))

    def test_profile_event_clock_0800(self):
        data = b(0x01, 0x02, 0x03, 0x01, 0xE0, 0x01, 0x01, 0x7F)
        self.assertEqual(pf.profile_event(2, 3, True, "clock", 480, 1), data)

    def test_profile_event_sunrise_minus_30(self):
        data = b(0x01, 0x01, 0x00, 0x02, 0xE2, 0xFF, 0x01, 0x7F)
        self.assertEqual(pf.profile_event(1, 0, False, "sunrise", -30, 1), data)
        self.assertEqual(pf.parse_profile_event(data)["offset_min"], -30)

    def test_event_log(self):
        e = pf.parse_event_log(b(0x80, 0x47, 0xED, 0x68, 0x02, 0x01, 0x00))
        self.assertEqual((e.epoch, e.source, e.portions, e.result), (1760380800, 2, 1, 0))

    def test_commands(self):
        self.assertEqual(pf.command(5, pf.OP_FEED, pf.feed_args(1)), b(0x05, 0x01, 0x01))
        self.assertEqual(pf.command(6, pf.OP_HOLD, pf.hold_args(pf.HOLD_FOREVER)), b(0x06, 0x04, 0xFF, 0xFF, 0xFF, 0xFF))
        self.assertEqual(pf.command(7, pf.OP_SET_TIME, pf.set_time_args(1760380800, -360)), b(0x07, 0x06, 0x80, 0x47, 0xED, 0x68, 0x98, 0xFE))
        self.assertEqual(
            pf.command(8, pf.OP_SET_LOCATION, pf.set_location_args(39.7684, -86.1581)),
            b(0x08, 0x07, 0x88, 0xAE, 0x3C, 0x00, 0x7E, 0x88, 0x7C, 0xFF),
        )

    def test_sequence_number_wraps(self):
        self.assertEqual(pf.command(256 + 5, pf.OP_STOP)[0], 5)

    def test_response(self):
        r = pf.parse_response(b(0x05, 0x01, 0x00, 0x01))
        self.assertEqual((r.seq, r.opcode, r.result, r.detail), (5, 1, 0, 1))
        self.assertTrue(r.ok)
        self.assertEqual(pf.parse_response(b(1, 6, 12, 0)).result_text, "clock from BLE ignored (network time is fresh)")
        self.assertEqual(pf.parse_response(b(1, 6, 99, 0)).result_text, "unknown (99)")

    def test_config_and_calibration(self):
        self.assertEqual(pf.config_set(pf.CONFIG_KEYS["daily_cap"], 3), b(0x05, 0x03, 0x00, 0x00, 0x00))
        self.assertEqual(pf.config_query(pf.CONFIG_KEYS["latitude_e5"]), b(0x88))
        self.assertEqual(pf.config_set(pf.CONFIG_KEYS["max_pulse_us"], 2770), b(0x0B, 0xD2, 0x0A, 0x00, 0x00))
        self.assertEqual(pf.calibration_set(2, -12), b(0x02, 0xF4, 0xFF))
        self.assertEqual(pf.calibration_query(2), b(0x82))
        self.assertEqual(pf.parse_config(b(0x05, 0x03, 0, 0, 0)), (5, 3))
        self.assertEqual(pf.parse_calibration(b(0x02, 0xF4, 0xFF)), (2, -12))

    def test_profile_query(self):
        self.assertEqual(pf.profile_query(1, None), b(0x02, 0x01, 0xFF))
        self.assertEqual(pf.profile_query(1, 3), b(0x02, 0x01, 0x03))

    def test_info(self):
        i = pf.parse_info(b(2, 0x7F, 0x00, 7, 3, 4))
        self.assertEqual((i.protocol_version, i.capabilities, i.positions, i.profiles, i.events_per_profile), (2, 0x7F, 7, 3, 4))

    def test_authentication_golden_vectors(self):
        # The same vectors as tests/test_protocol.cpp, so the firmware and the client agree byte for byte.
        key = bytes(range(32))
        nonce = bytes(range(0xA0, 0xB0))
        self.assertEqual(pf.auth_proof(key, nonce).hex(), "fae372f2c6d3c9d6ee0e4974574d6c7f")
        ks = pf.session_key(key, nonce)
        self.assertEqual(ks.hex(), "0c3122b4b9cc9402f417944dda88312c46479e89d230c4865069355aadf391c3")
        self.assertEqual(pf.write_tag(ks, pf.CHANNEL_COMMAND, b(5, 0x01, 0x01)).hex(), "2a8f789b5ed334f6")
        self.assertEqual(pf.write_tag(ks, pf.CHANNEL_CONFIG, b(6, 0x05, 0x03, 0, 0, 0)).hex(), "c479cee3220fce8b")
        self.assertEqual(pf.signed(ks, pf.CHANNEL_COMMAND, b(5, 0x01, 0x01)), b(5, 1, 1) + bytes.fromhex("2a8f789b5ed334f6"))

    def test_parse_key(self):
        import base64

        self.assertEqual(pf.parse_key(base64.b64encode(bytes(range(32))).decode()), bytes(range(32)))
        with self.assertRaises(ValueError):
            pf.parse_key(base64.b64encode(b"short").decode())

    def test_writes_fit_the_payload(self):
        # The longest command (SET_LOCATION) and record (a profile event) still fit 20 bytes with seq and tag.
        self.assertLessEqual(len(pf.command(1, pf.OP_SET_LOCATION, pf.set_location_args(1, 1))) + pf.TAG_SIZE, 20)
        self.assertLessEqual(1 + len(pf.profile_event(0, 0, True, "clock", 0, 1)) + pf.TAG_SIZE, 20)
        self.assertLessEqual(len(pf.command(1, pf.OP_AUTH, bytes(pf.PROOF_SIZE))), 20)

    def test_uuids_follow_the_base(self):
        self.assertEqual(pf.SERVICE_UUID, "02dd1dd9-ba75-4bbc-bdea-306d21010001")
        self.assertEqual(pf.EVENT_LOG_UUID, "02dd1dd9-ba75-4bbc-bdea-306d21010009")

    def test_sequence_numbers_skip_zero(self):
        self.assertEqual(pf.next_seq(254), 255)
        self.assertEqual(pf.next_seq(255), 1)
        self.assertEqual(pf.next_seq(0), 1)
        seen = set()
        seq = 1
        for _ in range(600):
            seq = pf.next_seq(seq)
            seen.add(seq)
        self.assertEqual(seen, set(range(1, 256)))

    def test_record_write_opcodes(self):
        self.assertEqual(pf.RECORD_OPCODES[pf.PROFILE_UUID], 0x80)
        self.assertEqual(pf.RECORD_OPCODES[pf.CONFIG_UUID], 0x81)
        self.assertEqual(pf.RECORD_OPCODES[pf.CALIBRATION_UUID], 0x82)

    def test_days_mask(self):
        self.assertEqual(pf.days_mask("sun"), 0x01)
        self.assertEqual(pf.days_mask("Sat"), 0x40)
        self.assertEqual(pf.days_mask("mon", "wed", "fri"), 0b0101010)


class YamlAgreesWithClient(unittest.TestCase):
    """The UUIDs in the YAML GATT table must match the client's."""

    def test_uuids_match_ble_gatt_yaml(self):
        import pathlib
        import re

        text = (pathlib.Path(__file__).resolve().parent.parent / "packages" / "ble_gatt.yaml").read_text()
        found = set(re.findall(r"uuid: (02dd1dd9-ba75-4bbc-bdea-306d2101[0-9a-f]{4})", text))
        wanted = {
            pf.SERVICE_UUID,
            pf.INFO_UUID,
            pf.COMMAND_UUID,
            pf.RESPONSE_UUID,
            pf.STATUS_UUID,
            pf.PROFILE_UUID,
            pf.CONFIG_UUID,
            pf.CALIBRATION_UUID,
            pf.EVENT_LOG_UUID,
            pf.AUTH_UUID,
        }
        self.assertEqual(found, wanted)


if __name__ == "__main__":
    unittest.main()
