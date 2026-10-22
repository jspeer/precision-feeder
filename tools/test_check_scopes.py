"""Tests for tools/check_scopes.py: it catches an entity visible outside the slim set, a slim one hidden, and an
internal one with its own MQTT topic. Run from tools/:  python3 -m unittest test_check_scopes

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import unittest

import check_scopes


def slim_config(**extra):
    """A resolved config with the whole slim set visible, plus `extra` domains."""
    config = {}
    for key in check_scopes.SLIM:
        domain, name = key.split("/", 1)
        config.setdefault(domain, []).append({"platform": "template", "name": name})
    for domain, items in extra.items():
        config.setdefault(domain, []).extend(items)
    return config


class CheckScopes(unittest.TestCase):
    def test_slim_set_passes(self):
        config = slim_config(number=[{"platform": "template", "name": "Daily Portion Cap", "internal": True}])
        self.assertEqual(check_scopes.problems(config), [])

    def test_visible_component_caught(self):
        config = slim_config(select=[{"platform": "template", "name": "Servo Pin"}])
        self.assertTrue(any("select/Servo Pin" in p for p in check_scopes.problems(config)))

    def test_nested_entity_caught(self):
        config = slim_config(text_sensor=[{"platform": "wifi_info",
                                           "ip_address": {"name": "IP Address", "internal": True,
                                                          "address_1": {"name": "IP Address 2"}}}])
        self.assertTrue(any("text_sensor/IP Address 2" in p for p in check_scopes.problems(config)))

    def test_hidden_slim_entity_caught(self):
        config = slim_config()
        for item in config["button"]:
            if item["name"] == "Feed One Portion":
                item["internal"] = True
        self.assertTrue(any("button/Feed One Portion is in the slim set" in p for p in check_scopes.problems(config)))

    def test_internal_with_mqtt_topic_caught(self):
        config = slim_config(number=[{"platform": "template", "name": "Daily Portion Cap", "internal": True,
                                      "command_topic": "feeder/cap/set"}])
        self.assertTrue(any("own MQTT topic" in p for p in check_scopes.problems(config)))


if __name__ == "__main__":
    unittest.main()
