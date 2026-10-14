"""Precision Feeder BLE control plane: the protocol behind the GATT table.

Include this component (and an esp32_ble_server with the characteristics) only for
builds that need BLE; builds without it carry none of its code.

Part of the Precision Feeder ESPHome firmware (MIT licence).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import esphome.codegen as cg
from esphome.components import esp32_ble_server, precision_feeder
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
)

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeederBle),
        cv.GenerateID(CONF_FEEDER): cv.use_id(precision_feeder.PrecisionFeeder),
        cv.Required(CONF_SERVER): cv.use_id(esp32_ble_server.BLEServer),
        # The GATT characteristics, declared by id in the esp32_ble_server config.
        **{
            cv.Required(name): cv.use_id(esp32_ble_server.BLECharacteristic)
            for name in CHARACTERISTICS
        },
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

    cg.add(var.set_time_policy(config[CONF_TIME_POLICY]))
    cg.add(var.set_network_time_stale_ms(config[CONF_NTP_STALE].total_milliseconds))
