"""Precision Feeder: tray control, feed guard and on-board scheduler.

Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import re

import esphome.codegen as cg
from esphome.components import output, time as time_
from esphome.components.esphome.ota import ESPHomeOTAComponent
from esphome.components.esp32.gpio import ESP32InternalGPIOPin
import esphome.config_validation as cv
from esphome.const import CONF_ENCRYPTION, CONF_ID, CONF_KEY, CONF_OUTPUT, CONF_PASSWORD, CONF_TIME_ID
import esphome.final_validate as fv

CODEOWNERS = []
DEPENDENCIES = []
AUTO_LOAD = ["output"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeeder = precision_feeder_ns.class_("PrecisionFeeder", cg.Component)
PinLoader = precision_feeder_ns.class_("PinLoader", cg.Component)

CONF_SWEEP = "sweep_degrees"
CONF_MIN_PULSE = "min_pulse_width"
CONF_MAX_PULSE = "max_pulse_width"
CONF_INVERT = "invert"
CONF_STEP_TIME = "step_time"
CONF_WIGGLE = "wiggle"
CONF_DETACH_DELAY = "detach_delay"
CONF_WEB_SERVER_ID = "web_server_id"
CONF_PROTECTED = "protected_entities"
CONF_SERVO_PIN_ID = "servo_pin_id"
CONF_BUTTON_ID = "button_id"
CONF_BUTTON_PIN_ID = "button_pin_id"
CONF_STATUS_LED_ID = "status_led_id"
CONF_STATUS_LED_OUTPUT_ID = "status_led_output_id"
CONF_STATUS_LED_PIN = "status_led_pin"
CONF_PIN_LOADER_ID = "pin_loader_id"
CONF_OTA_ID = "ota_id"

WebServer = cg.esphome_ns.namespace("web_server").class_("WebServer")


def gpio_number(value):
    """A GPIO number, as 21 or "GPIO21". Not claimed as a pin: the LED's own driver already claims it."""
    if isinstance(value, str):
        m = re.fullmatch(r"(?:GPIO)?(\d+)", value.strip(), re.IGNORECASE)
        if not m:
            raise cv.Invalid(f"expected a GPIO number such as GPIO21, got {value!r}")
        value = int(m.group(1))
    return cv.int_range(min=0, max=48)(value)


def protected_entity(value):
    """"<domain>/<name>" as the web server addresses an entity, for example "number/Daily Portion Cap"."""
    value = cv.string_strict(value)
    if not re.fullmatch(r'[a-z_]+/[^/"\\]+', value):
        raise cv.Invalid(f"expected <domain>/<entity name> (no quotes, backslashes or extra slashes), got {value!r}")
    return value


CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeeder),
        # A float output driving the servo (ledc at 50 Hz; packages/mechanism_servo.yaml sets it). Left out only by a
        # build whose mechanism is another component that registers itself with the feeder (AGENTS.md, "Adding a
        # mechanism"); a build with neither fails at boot with an error in the log.
        cv.Optional(CONF_OUTPUT): cv.use_id(output.FloatOutput),
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
        # With the web server: entities ("<domain>/<name>") whose changes through the web server need the settings
        # password when one is set (DESIGN.md section 10.1). Home Assistant and BLE are not affected.
        cv.OnlyWith(CONF_WEB_SERVER_ID, "web_server"): cv.use_id(WebServer),
        cv.Optional(CONF_PROTECTED, default=[]): cv.ensure_list(protected_entity),
        # Runtime pins (DESIGN.md section 2.1): the pins of the servo output, the button and the status LED, so the
        # pins saved on the device can replace the YAML's before those drivers start. Set by the packages (core.yaml,
        # button.yaml, status_led.yaml); leave them out and every pin stays as the YAML says.
        cv.GenerateID(CONF_PIN_LOADER_ID): cv.declare_id(PinLoader),
        cv.Optional(CONF_SERVO_PIN_ID): cv.use_id(ESP32InternalGPIOPin),
        cv.Inclusive(CONF_BUTTON_ID, "button"): cv.use_id(cg.Component),
        cv.Inclusive(CONF_BUTTON_PIN_ID, "button"): cv.use_id(ESP32InternalGPIOPin),
        cv.Inclusive(CONF_STATUS_LED_ID, "status_led"): cv.use_id(cg.Component),
        cv.Inclusive(CONF_STATUS_LED_OUTPUT_ID, "status_led"): cv.use_id(cg.Component),
        cv.Inclusive(CONF_STATUS_LED_PIN, "status_led"): gpio_number,
        # An API key provisioned at run time (packages/api_key_on_device.yaml): the ESPHome OTA whose password this
        # component sets to a random value at every boot, so plaintext OTA is refused (encrypted OTA uses the key).
        cv.Optional(CONF_OTA_ID): cv.use_id(ESPHomeOTAComponent),
    }
).extend(cv.COMPONENT_SCHEMA)


def _pulse_span(config):
    span = config[CONF_MAX_PULSE].total_microseconds - config[CONF_MIN_PULSE].total_microseconds
    if span < 500:
        raise cv.Invalid(f"{CONF_MAX_PULSE} must be at least 500 us above {CONF_MIN_PULSE}")
    return config


CONFIG_SCHEMA = cv.All(CONFIG_SCHEMA, _pulse_span)


def _final_validate(config):
    """ota_id only makes sense with a runtime API key and an OTA that compiles its password path."""
    if CONF_OTA_ID not in config:
        return config
    full = fv.full_config.get()
    api = full.get("api") or {}
    encryption = api.get(CONF_ENCRYPTION)
    if encryption is None or CONF_KEY in encryption:
        raise cv.Invalid(f"{CONF_OTA_ID} needs `api: encryption: {{}}` with no key (the key provisioned at run time)")
    for ota in full.get("ota") or []:
        if ota.get(CONF_ID) == config[CONF_OTA_ID]:
            if ota.get(CONF_PASSWORD) != "" or CONF_ENCRYPTION in ota:
                raise cv.Invalid(f"{CONF_OTA_ID}: give that ota `password: \"\"` and no `encryption:`")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    if CONF_OUTPUT in config:
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

    # Runtime pins: the servo's (when the mechanism is the servo), the button's and the LED's. A mechanism that drives
    # its own pins has no servo pin; the button and LED pins still work.
    if CONF_SERVO_PIN_ID in config:
        cg.add(var.set_servo_pin(await cg.get_variable(config[CONF_SERVO_PIN_ID])))
    if CONF_BUTTON_ID in config:
        sensor = await cg.get_variable(config[CONF_BUTTON_ID])
        cg.add(var.set_button(sensor, await cg.get_variable(config[CONF_BUTTON_PIN_ID])))
    if CONF_STATUS_LED_ID in config:
        light = await cg.get_variable(config[CONF_STATUS_LED_ID])
        led = await cg.get_variable(config[CONF_STATUS_LED_OUTPUT_ID])
        cg.add(var.set_status_led(light, led, config[CONF_STATUS_LED_PIN]))
    if CONF_OTA_ID in config:
        cg.add(var.set_ota(await cg.get_variable(config[CONF_OTA_ID])))

    loader = cg.new_Pvariable(config[CONF_PIN_LOADER_ID], var)  # does nothing without runtime pins
    await cg.register_component(loader, {})

    if CONF_WEB_SERVER_ID in config:
        cg.add(var.set_web_server(await cg.get_variable(config[CONF_WEB_SERVER_ID])))
        for entity in config[CONF_PROTECTED]:
            cg.add(var.add_protected(entity))
        # Emitted before App.setup(), so the guard is registered ahead of the web server's own handler.
        cg.add(var.register_settings_guard())
