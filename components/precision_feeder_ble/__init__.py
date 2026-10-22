"""Precision Feeder BLE control plane: the protocol behind the GATT table.

Include this component (and an esp32_ble_server with the characteristics) only for
builds that need BLE; builds without it carry none of its code.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import base64

import esphome.codegen as cg
from esphome.components import esp32_ble, esp32_ble_server, precision_feeder
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32_ble_server", "precision_feeder"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeederBle = precision_feeder_ns.class_("PrecisionFeederBle", cg.Component)
TimePolicy = precision_feeder_ns.namespace("proto").enum("TimePolicy", is_class=True)

CONF_FEEDER = "feeder"
CONF_SERVER = "server"
CONF_INFO = "info"
CONF_COMMAND = "command"
CONF_RESPONSE = "response"
CONF_STATUS = "status"
CONF_PROFILE = "profile"
CONF_CONFIG = "config"
CONF_CALIBRATION = "calibration"
CONF_EVENT_LOG = "event_log"
CONF_AUTH = "auth"
CONF_KEY = "key"
CONF_TIME_POLICY = "time_policy"
CONF_NTP_STALE = "network_time_stale_after"

CHARACTERISTICS = (
    CONF_INFO,
    CONF_COMMAND,
    CONF_RESPONSE,
    CONF_STATUS,
    CONF_PROFILE,
    CONF_CONFIG,
    CONF_CALIBRATION,
    CONF_EVENT_LOG,
    CONF_AUTH,
)

KEY_SIZE = 32


def validate_key(value):
    """The shared BLE key: 32 random bytes, base64 encoded (openssl rand -base64 32)."""
    value = cv.string_strict(value)
    try:
        raw = base64.b64decode(value, validate=True)
    except ValueError as err:
        raise cv.Invalid(f"BLE key is not valid base64: {err}") from err
    if len(raw) != KEY_SIZE:
        raise cv.Invalid(f"BLE key must be {KEY_SIZE} bytes (base64 of 32 random bytes), got {len(raw)}")
    if not any(raw):
        raise cv.Invalid("BLE key must not be all zeros: anyone could authenticate with it")
    return value

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeederBle),
        cv.GenerateID(CONF_FEEDER): cv.use_id(precision_feeder.PrecisionFeeder),
        cv.GenerateID(esp32_ble.CONF_BLE_ID): cv.use_id(esp32_ble.ESP32BLE),
        cv.Required(CONF_SERVER): cv.use_id(esp32_ble_server.BLEServer),
        # The GATT characteristics, declared by id in the esp32_ble_server config.
        **{
            cv.Required(name): cv.use_id(esp32_ble_server.BLECharacteristic)
            for name in CHARACTERISTICS
        },
        # The shared secret a client must prove it knows before any write is accepted (protocol 2).
        # 32 random bytes, base64: `openssl rand -base64 32`. Keep it in secrets.yaml.
        # Optional since 2.0.0: a key generated or entered in the web app is saved on the feeder and wins over
        # this one. With neither, every AUTH is refused (no BLE client can do anything) until one is set.
        cv.Optional(CONF_KEY): validate_key,
        # What to do with a clock pushed over BLE (SET_TIME):
        #   fallback_only: accept it only if NTP / Home Assistant has not set the clock recently
        #   always:        accept any plausible value
        #   off:           refuse
        cv.Optional(CONF_TIME_POLICY, default="fallback_only"): cv.enum(
            {
                "fallback_only": TimePolicy.FALLBACK_ONLY,
                "always": TimePolicy.ALWAYS,
                "off": TimePolicy.OFF,
            },
            lower=True,
        ),
        # How old the last NTP / Home Assistant sync may be before a BLE clock is accepted
        # under the fallback_only policy.
        cv.Optional(CONF_NTP_STALE, default="24h"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_feeder(await cg.get_variable(config[CONF_FEEDER])))
    cg.add(var.set_server(await cg.get_variable(config[CONF_SERVER])))
    for name in CHARACTERISTICS:
        char = await cg.get_variable(config[name])
        cg.add(getattr(var, f"set_{name}")(char))

    # The build's key lives in FeederCore, next to one saved on the device (all zeros: none).
    feeder = await cg.get_variable(config[CONF_FEEDER])
    key = base64.b64decode(config[CONF_KEY]) if CONF_KEY in config else bytes(KEY_SIZE)
    cg.add(
        feeder.set_build_ble_key(
            cg.RawExpression("std::array<uint8_t, 32>{" + ", ".join(str(b) for b in key) + "}.data()")
        )
    )
    cg.add(var.set_time_policy(config[CONF_TIME_POLICY]))
    cg.add(var.set_network_time_stale_ms(config[CONF_NTP_STALE].total_milliseconds))

    # Connection details for diagnostics (peer address, MTU), from the GATT server's events.
    esp32_ble.register_gatts_event_handler(await cg.get_variable(config[esp32_ble.CONF_BLE_ID]), var)
