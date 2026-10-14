"""Precision Feeder: tray control, feed guard and on-board scheduler.

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import esphome.codegen as cg
from esphome.components import output, time as time_
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_OUTPUT, CONF_TIME_ID

CODEOWNERS = []
DEPENDENCIES = []
AUTO_LOAD = ["output"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeeder = precision_feeder_ns.class_("PrecisionFeeder", cg.Component)

CONF_SWEEP = "sweep_degrees"
CONF_MIN_PULSE = "min_pulse_width"
CONF_MAX_PULSE = "max_pulse_width"
CONF_INVERT = "invert"
CONF_STEP_TIME = "step_time"
CONF_WIGGLE = "wiggle"
CONF_DETACH_DELAY = "detach_delay"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeeder),
        # A float output driving the servo. Use the ledc platform at 50 Hz.
        cv.Required(CONF_OUTPUT): cv.use_id(output.FloatOutput),
        # The clock the scheduler follows (sntp, homeassistant, ...).
        cv.Required(CONF_TIME_ID): cv.use_id(time_.RealTimeClock),
        # Angle the tray's 7 slots span (7 x 25 degrees on the Rev2 print); the pitch is
        # sweep / 7, so the servo turns 6 pitches (150) from position 0 to 6. Original: 175.
        cv.Optional(CONF_SWEEP, default=175.0): cv.float_range(min=90.0, max=180.0),
        # Servo pulse range that maps to 0 and 180 degrees. 544 to 2400 us is what the
        # original firmware drives (the ESP32Servo library's default), with an SG92R.
        # These are only the starting values: the range can be tuned at run time (Home
        # Assistant "Servo Min/Max Pulse", or BLE), and the saved values then win.
        cv.Optional(CONF_MIN_PULSE, default="544us"): cv.All(
            cv.positive_time_period_microseconds,
            cv.Range(min=cv.TimePeriod(microseconds=300), max=cv.TimePeriod(microseconds=1500)),
        ),
        cv.Optional(CONF_MAX_PULSE, default="2400us"): cv.All(
            cv.positive_time_period_microseconds,
            cv.Range(min=cv.TimePeriod(microseconds=1500), max=cv.TimePeriod(microseconds=3000)),
        ),
        # Mirror the angle (180 - angle) if your servo is mounted the other way round.
        cv.Optional(CONF_INVERT, default=False): cv.boolean,
        # How long each of the three tray moves of a feed takes before the next one starts.
        cv.Optional(CONF_STEP_TIME, default="1s"): cv.positive_time_period_milliseconds,
        # Move forward, back and forward again (shakes the food loose) instead of one move.
        cv.Optional(CONF_WIGGLE, default=True): cv.boolean,
        # Release the servo (no PWM pulses) this long after the last move, to stop jitter and heat.
        cv.Optional(CONF_DETACH_DELAY, default="1s"): cv.positive_time_period_milliseconds,
    }
).extend(cv.COMPONENT_SCHEMA)


def _pulse_span(config):
    span = config[CONF_MAX_PULSE].total_microseconds - config[CONF_MIN_PULSE].total_microseconds
    if span < 500:
        raise cv.Invalid(f"{CONF_MAX_PULSE} must be at least 500 us above {CONF_MIN_PULSE}")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _pulse_span)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    out = await cg.get_variable(config[CONF_OUTPUT])
    cg.add(var.set_output(out))
    clock = await cg.get_variable(config[CONF_TIME_ID])
    cg.add(var.set_time(clock))

    cg.add(var.set_sweep(config[CONF_SWEEP]))
    cg.add(
        var.set_default_pulse_range(
            config[CONF_MIN_PULSE].total_microseconds,
            config[CONF_MAX_PULSE].total_microseconds,
        )
    )
    cg.add(var.set_invert(config[CONF_INVERT]))
    cg.add(var.set_step_time(config[CONF_STEP_TIME].total_milliseconds))
    cg.add(var.set_wiggle(config[CONF_WIGGLE]))
    cg.add(var.set_detach_delay(config[CONF_DETACH_DELAY].total_milliseconds))
