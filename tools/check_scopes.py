#!/usr/bin/env python3
"""Checks who can see what (firmware 2.2.0, docs/plans/2.2.0-scopes.md): in a build's resolved configuration (the
output of `esphome config`), the entities Home Assistant and MQTT see, the ones not `internal`, must be exactly the
slim set below, and no internal entity may have its own MQTT topic (ESPHome would then publish it anyway).

  esphome config build.yaml > resolved.yaml && python3 tools/check_scopes.py resolved.yaml [--list]

tools/build_bins.sh --config runs it on the prebuilt binaries and tools/check.sh on the examples.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import sys

# Operate: what Home Assistant and MQTT may see (D22). "<domain>/<name>". Everything else is web app only (internal).
SLIM = {
    "button/Feed One Portion",
    "button/Feed Selected Portions",
    "number/Manual Portions",
    "button/Tray Reload",
    "button/Stop",
    "button/Hold Scheduled Feeding",
    "number/Hold Duration",
    "button/Cancel Hold",
    "switch/Scheduler",
    "select/Active Profile",
    "sensor/Portions Remaining",
    "text_sensor/Tray State",
    "text_sensor/Next Feed",
    "text_sensor/Last Feed",
    "text_sensor/Last Result",
    "text_sensor/Recent Feeds",
    "text_sensor/Today's Feeds",
    "text_sensor/Faults",
    "binary_sensor/Needs Food",
    "text_sensor/Firmware Version",
}


def load(path: str) -> dict:
    """An `esphome config` output, its tags (!lambda, !secret, ...) read as plain values. PyYAML comes with ESPHome;
    it is imported here only, so the tests need nothing."""
    import yaml

    class Loader(yaml.SafeLoader):
        pass

    def any_tag(loader, suffix, node):
        if isinstance(node, yaml.ScalarNode):
            return loader.construct_scalar(node)
        if isinstance(node, yaml.SequenceNode):
            return loader.construct_sequence(node)
        return loader.construct_mapping(node)

    Loader.add_multi_constructor("!", any_tag)
    with open(path) as f:
        return yaml.load(f, Loader=Loader)


def _named(domain: str, item: dict):
    if isinstance(item.get("name"), str):
        yield domain, item["name"], item
    for sub in item.values():  # entities nested in a platform item: the debug sensors' `free:`, wifi_info's
        if isinstance(sub, dict):  # `ip_address:` and its `address_1:` ...
            yield from _named(domain, sub)


def entities(config: dict):
    """(domain, name, item) for every named entity in a resolved configuration."""
    for domain, value in config.items():
        if not isinstance(value, list):
            continue
        for item in value:
            if isinstance(item, dict):
                yield from _named(domain, item)


def problems(config: dict) -> list[str]:
    out = []
    visible = set()
    for domain, name, item in entities(config):
        key = f"{domain}/{name}"
        internal = item.get("internal") is True
        if not internal:
            visible.add(key)
            if key not in SLIM:
                out.append(f"{key} is visible to Home Assistant and MQTT but not in the slim set: make it internal")
        elif item.get("state_topic") or item.get("command_topic"):
            out.append(f"{key} is internal but has its own MQTT topic, so ESPHome would publish it")
    # A build with the feeder's controls and status (every example and binary) has the whole slim set.
    if "text_sensor/Tray State" in visible:
        out += [f"{key} is in the slim set but not visible" for key in sorted(SLIM - visible)]
    return out


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    config = load(sys.argv[1])
    if "--list" in sys.argv[2:]:
        for domain, name, item in sorted(entities(config)):
            print(f"{'internal' if item.get('internal') is True else 'VISIBLE '}  {domain}/{name}")
        return 0
    found = problems(config)
    for p in found:
        print(f"{sys.argv[1]}: {p}")
    return 1 if found else 0


if __name__ == "__main__":
    sys.exit(main())
