"""Precision Feeder: MQTT's own topics on the device (firmware 2.2.0, docs/MQTT.md).

The topics' logic is plain C++ in components/precision_feeder/feeder_mqtt.* (host-tested, tests/test_mqtt.cpp). This
component subscribes to "<prefix>/feeder/#" with ESPHome's MQTT client once the topic prefix is known (at boot), hands
each message to it, and publishes the state topics it returns, retained. packages/mqtt.yaml sets it up.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import esphome.codegen as cg
from esphome.components import precision_feeder
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["precision_feeder", "mqtt"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeederMqtt = precision_feeder_ns.class_("PrecisionFeederMqtt", cg.Component)

CONF_FEEDER = "feeder"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeederMqtt),
        cv.GenerateID(CONF_FEEDER): cv.use_id(precision_feeder.PrecisionFeeder),
    }
).extend(cv.COMPONENT_SCHEMA)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    feeder = await cg.get_variable(config[CONF_FEEDER])
    cg.add(var.set_feeder(feeder))
