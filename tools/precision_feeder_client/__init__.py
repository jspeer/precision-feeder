"""Python reference client for the Precision Feeder BLE protocol (docs/PROTOCOL.md).

  protocol  pure Python: constants, packing, parsing, authentication (no dependencies)
  client    FeederClient over BLE (needs bleak)
  selftest  checks a feeder's protocol without changing anything
  cli       the command line (tools/precision_feeder_ble.py)

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

from .protocol import *  # noqa: F401,F403


def __getattr__(name):
    # The BLE parts import bleak only when used, so the pure protocol works without it installed.
    if name == "FeederClient":
        from .client import FeederClient

        return FeederClient
    if name == "selftest":
        from .selftest import selftest

        return selftest
    raise AttributeError(name)
