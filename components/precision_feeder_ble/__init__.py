"""Precision Feeder BLE control plane: the GATT server and the protocol behind it.

Since 2.4.0 it runs on ESP-IDF's NimBLE host, with its own GATT table (gatt_table.h), instead of ESPHome's
esp32_ble and esp32_ble_server (Bluedroid). Include it only for builds that need BLE; builds without it carry none
of its code. It cannot share a build with esp32_ble or anything built on it (the two stacks cannot both be linked).

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import base64

import esphome.codegen as cg
from esphome.components import precision_feeder
from esphome.components.esp32 import add_idf_sdkconfig_option, request_bluetooth
import esphome.config_validation as cv
from esphome.const import CONF_ID, CONF_NAME
import esphome.final_validate as fv

CODEOWNERS = []
DEPENDENCIES = ["esp32", "precision_feeder"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeederBle = precision_feeder_ns.class_("PrecisionFeederBle", cg.Component)
TimePolicy = precision_feeder_ns.namespace("proto").enum("TimePolicy", is_class=True)

CONF_FEEDER = "feeder"
CONF_MANUFACTURER = "manufacturer"
CONF_MODEL = "model"
CONF_FIRMWARE_VERSION = "firmware_version"
CONF_KEY = "key"
CONF_TIME_POLICY = "time_policy"
CONF_NTP_STALE = "network_time_stale_after"

# ESPHome's Bluetooth components: each needs Bluedroid (esp32_ble), which cannot be linked next to NimBLE.
BLUEDROID_COMPONENTS = (
    "esp32_ble",
    "esp32_ble_server",
    "esp32_ble_tracker",
    "esp32_ble_beacon",
    "ble_client",
    "bluetooth_proxy",
    "esp32_improv",
)

# NimBLE trimmed to what one GATT server with one client needs (docs/plans/nimble.md, B9; measured on the board in
# docs/BENCH.md). ESP-IDF's defaults (3 connections, large buffer pools, BLE 5 features) cost as much heap as
# Bluedroid; these save about 33 KB of heap against the Bluedroid build of 2.3.x.
NIMBLE_SDKCONFIG = {
    "CONFIG_BT_BLUEDROID_ENABLED": False,
    "CONFIG_BT_NIMBLE_ENABLED": True,
    # A peripheral only: no central role, no scanning.
    "CONFIG_BT_NIMBLE_ROLE_CENTRAL": False,
    "CONFIG_BT_NIMBLE_ROLE_OBSERVER": False,
    "CONFIG_BT_NIMBLE_MAX_CONNECTIONS": 1,
    "CONFIG_BT_NIMBLE_MAX_CCCDS": 4,
    "CONFIG_BT_NIMBLE_GATT_MAX_PROCS": 1,
    "CONFIG_BT_NIMBLE_ATT_MAX_PREP_ENTRIES": 8,
    "CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT": 8,
    "CONFIG_BT_NIMBLE_MSYS_2_BLOCK_COUNT": 8,
    "CONFIG_BT_NIMBLE_TRANSPORT_ACL_FROM_LL_COUNT": 8,
    "CONFIG_BT_NIMBLE_TRANSPORT_EVT_COUNT": 12,
    "CONFIG_BT_NIMBLE_TRANSPORT_EVT_DISCARD_COUNT": 4,
    "CONFIG_BT_NIMBLE_WHITELIST_SIZE": 2,
    "CONFIG_BT_NIMBLE_50_FEATURE_SUPPORT": False,
    "CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT": False,
    "CONFIG_BT_NIMBLE_LOG_LEVEL_WARNING": True,
    # Bonds survive a restart, at most 3 (the oldest makes room), as on Bluedroid.
    "CONFIG_BT_NIMBLE_NVS_PERSIST": True,
    "CONFIG_BT_NIMBLE_MAX_BONDS": 3,
    # The Device Information Service carries the manufacturer, model and firmware version, as before.
    "CONFIG_BT_NIMBLE_SVC_DIS_MANUFACTURER_NAME": True,
    "CONFIG_BT_NIMBLE_SVC_DIS_FIRMWARE_REVISION": True,
    "CONFIG_BT_NIMBLE_SVC_DIS_IEEE": False,
    "CONFIG_BT_NIMBLE_SVC_DIS_UDI": False,
    # The controller: one link, two activities (advertising and the connection), no scanning.
    "CONFIG_BT_CTRL_BLE_MAX_ACT": 2,
    "CONFIG_BT_CTRL_BLE_SCAN_DUPL": False,
    "CONFIG_BT_CTRL_DTM_ENABLE": False,
    "CONFIG_BT_CTRL_BLE_MASTER": False,
    "CONFIG_BT_CTRL_BLE_SCAN": False,
}
# NimBLE's example services, none used.
for _svc in ("PROX", "ANS", "CTS", "HTP", "IPSS", "TPS", "IAS", "LLS", "SPS", "HR", "BAS"):
    NIMBLE_SDKCONFIG[f"CONFIG_BT_NIMBLE_{_svc}_SERVICE"] = False

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
        # Shown to BLE scanners. At most 20 characters; at most 13 with name_add_mac_suffix, which adds "-" and the
        # last 6 hex digits of the MAC (as ESPHome's esp32_ble did). Without it, the device's name.
        cv.Optional(CONF_NAME): cv.string_strict,
        # The Device Information Service.
        cv.Optional(CONF_MANUFACTURER, default="Precision Feeder"): cv.string_strict,
        cv.Optional(CONF_MODEL, default="Precision Feeder Rev2"): cv.string_strict,
        cv.Optional(CONF_FIRMWARE_VERSION, default=""): cv.string,
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


def _final_validate(config):
    full = fv.full_config.get()
    for other in BLUEDROID_COMPONENTS:
        if other in full:
            raise cv.Invalid(
                f"precision_feeder_ble runs its own Bluetooth stack (NimBLE) and cannot share a build with "
                f"'{other}' (ESPHome's Bluetooth, Bluedroid). Remove '{other}'; since 2.4.0 packages/ble_gatt.yaml "
                f"no longer needs esp32_ble or esp32_ble_server."
            )
    if (name := config.get(CONF_NAME)) is not None:
        limit = 13 if full["esphome"].get("name_add_mac_suffix") else 20
        if len(name) > limit:
            raise cv.Invalid(f"BLE name '{name}' is too long: at most {limit} characters")
    return config


FINAL_VALIDATE_SCHEMA = _final_validate


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)

    cg.add(var.set_feeder(await cg.get_variable(config[CONF_FEEDER])))
    if CONF_NAME in config:
        cg.add(var.set_name(config[CONF_NAME]))
    cg.add(var.set_device_info(config[CONF_MANUFACTURER], config[CONF_MODEL], config[CONF_FIRMWARE_VERSION]))

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

    request_bluetooth()
    for option, value in NIMBLE_SDKCONFIG.items():
        add_idf_sdkconfig_option(option, value)
