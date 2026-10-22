"""Precision Feeder: the direct-drive disc mechanism on the device (DRV8833 + GA12-N20 with encoder).

The mechanism's logic is plain C++ in components/precision_feeder/mechanism_n20.* (host-tested, tests/test_n20.cpp).
This component only connects it to the chip: it claims its five GPIOs itself through ESP-IDF (LEDC for the bridge
inputs, PCNT for the encoder, a GPIO for STBY), so the YAML claims none of them, and runs the motion every millisecond
from an esp_timer. It registers the mechanism with the feeder before the feeder starts, and reserves its pins so the
button and the LED can never be moved onto them. packages/mechanism_n20.yaml sets it up.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import esphome.codegen as cg
from esphome.components import precision_feeder
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_OUTPUT
from esphome.core import CORE

CODEOWNERS = []
DEPENDENCIES = ["precision_feeder"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
N20Driver = precision_feeder_ns.class_("N20Driver", cg.Component)

CONF_FEEDER = "feeder"
CONF_SLOTS = "slots"
CONF_IN1 = "in1_pin"
CONF_IN2 = "in2_pin"
CONF_STANDBY = "standby_pin"
CONF_ENCODER_A = "encoder_a_pin"
CONF_ENCODER_B = "encoder_b_pin"
CONF_PWM_FREQUENCY = "pwm_frequency"
CONF_BUILD_DEFAULT = "build_default"
PINS = (CONF_IN1, CONF_IN2, CONF_STANDBY, CONF_ENCODER_A, CONF_ENCODER_B)

# The motion's timings and limits: YAML-only (D10). Defaults are the bench sketch's (v7); see N20Timing in
# mechanism_n20.h for what each does.
TIMING = {
    "start_time": ("start_ms", "2ms"),
    "first_brake": ("first_brake_ms", "100ms"),
    "creep_brake": ("creep_brake_ms", "15ms"),
    "final_brake": ("final_brake_ms", "100ms"),
    "settle_time": ("settle_ms", "100ms"),
    "wiggle_first_brake": ("wiggle_first_brake_ms", "10ms"),
    "wiggle_creep_brake": ("wiggle_creep_brake_ms", "8ms"),
    "wiggle_turn_brake": ("wiggle_turn_brake_ms", "20ms"),
    "stall_time": ("stall_ms", "500ms"),
    "drive_stall_time": ("drive_stall_ms", "50ms"),
    "timeout_base": ("timeout_base_ms", "3s"),
    "watchdog": ("watchdog_ms", "25ms"),
    "direction_test_time": ("direction_test_ms", "120ms"),
}
PULSES = {
    "pulse_start": ("pulse_start_ms", 3, 1, 50),
    "pulse_min": ("pulse_min_ms", 2, 1, 50),
    "pulse_max": ("pulse_max_ms", 12, 1, 50),
    "wiggle_pulse_floor": ("wiggle_pulse_floor_ms", 5, 1, 50),
}
TICKS = {
    "pulse_far_ticks": ("pulse_far_ticks", 6, 1, 1000),
    "wiggle_far_ticks": ("wiggle_far_ticks", 20, 1, 1000),
    "reverse_fault_ticks": ("reverse_fault_ticks", 32, 1, 10000),
}


def _pins_distinct(config):
    pins = [config[p] for p in PINS]
    if len(set(pins)) != len(pins):
        raise cv.Invalid(f"the five pins must all differ, got {dict((p, config[p]) for p in PINS)}")
    return config


CONFIG_SCHEMA = cv.All(
    cv.Schema(
        {
            cv.GenerateID(): cv.declare_id(N20Driver),
            cv.GenerateID(CONF_FEEDER): cv.use_id(precision_feeder.PrecisionFeeder),
            # Slots on the disc, all open, one always over the chute: a load holds slots - 1 portions. The bench
            # print has 15 (24 degrees apart). Set packages/mechanism_n20.yaml's tray_capacity to slots - 1.
            cv.Optional(CONF_SLOTS, default=15): cv.int_range(min=3, max=60),
            # With the servo in the build too: whether the disc is the drive until one is chosen on the device.
            cv.Optional(CONF_BUILD_DEFAULT, default=True): cv.boolean,
            # GPIO numbers. Not claimed as ESPHome pins: this component sets them up itself (a pin on the
            # ESP32-S3 that check_pin() refuses fails at boot).
            cv.Required(CONF_IN1): precision_feeder.gpio_number,
            cv.Required(CONF_IN2): precision_feeder.gpio_number,
            cv.Required(CONF_STANDBY): precision_feeder.gpio_number,
            cv.Required(CONF_ENCODER_A): precision_feeder.gpio_number,
            cv.Required(CONF_ENCODER_B): precision_feeder.gpio_number,
            # The bridge's PWM: above hearing; the DRV8833 takes up to 50 kHz.
            cv.Optional(CONF_PWM_FREQUENCY, default="20kHz"): cv.All(
                cv.frequency, cv.Range(min=1000, max=50000)
            ),
            **{
                cv.Optional(key, default=default): cv.All(
                    cv.positive_time_period_milliseconds, cv.Range(max=cv.TimePeriod(seconds=60))
                )
                for key, (_, default) in TIMING.items()
            },
            **{cv.Optional(key, default=d): cv.int_range(min=lo, max=hi) for key, (_, d, lo, hi) in PULSES.items()},
            **{cv.Optional(key, default=d): cv.int_range(min=lo, max=hi) for key, (_, d, lo, hi) in TICKS.items()},
        }
    ).extend(cv.COMPONENT_SCHEMA),
    _pins_distinct,
)


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID], config[CONF_SLOTS])
    await cg.register_component(var, config)
    feeder = await cg.get_variable(config[CONF_FEEDER])
    cg.add(
        var.set_pins(
            config[CONF_IN1], config[CONF_IN2], config[CONF_STANDBY], config[CONF_ENCODER_A], config[CONF_ENCODER_B]
        )
    )
    cg.add(var.set_pwm_frequency(int(config[CONF_PWM_FREQUENCY])))
    for key, (field, _) in TIMING.items():
        cg.add(var.set_timing_ms(field, config[key].total_milliseconds))
    for key, (field, _, _, _) in {**PULSES, **TICKS}.items():
        cg.add(var.set_timing_ms(field, config[key]))
    # Emitted before App.setup(): the feeder's own setup (core_start) then already finds this mechanism. A build whose
    # feeder also has the servo's output carries both: the drive is chosen on the device (D17).
    with_servo = CONF_OUTPUT in CORE.config.get("precision_feeder", {})
    cg.add(var.attach(feeder, with_servo, config[CONF_BUILD_DEFAULT]))
