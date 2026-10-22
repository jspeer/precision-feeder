#!/usr/bin/env python3
"""Reference client for the Precision Feeder BLE protocol (docs/PROTOCOL.md).

The packing and parsing functions are pure Python and need nothing installed. The
FeederClient class and the command line need `bleak` (pip install bleak).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.

Protocol 2 needs the feeder's shared key (`ble_key` in its secrets.yaml): pass it with
--key, or set PRECISION_FEEDER_BLE_KEY. Both take the base64 text as in secrets.yaml.

Examples:
    python precision_feeder_ble.py scan
    export PRECISION_FEEDER_BLE_KEY='...base64 from secrets.yaml...'
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF status
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF selftest
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF config get all
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF profile A show
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF profile A event 1 on clock 08:00 1 all
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF feed 1
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF set-time
    python precision_feeder_ble.py AA:BB:CC:DD:EE:FF hold forever
"""

# The code lives in the precision_feeder_client package next to this file; this script is the entry point, and
# "import precision_feeder_ble" still gives the whole protocol (and FeederClient, selftest, main).
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from precision_feeder_client import *  # noqa: E402,F401,F403
from precision_feeder_client.protocol import _FULL_DAYS  # noqa: E402,F401
from precision_feeder_client.cli import main  # noqa: E402,F401


def __getattr__(name):
    import precision_feeder_client

    return getattr(precision_feeder_client, name)


if __name__ == "__main__":
    main()
