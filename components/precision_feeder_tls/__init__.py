"""Precision Feeder TLS: HTTPS for the web app, the trust store, and MQTT over TLS (firmware 2.4.1).

HTTPS is a relay: TLS on port 443, plain bytes to ESPHome's own web server on 127.0.0.1:80, so every handler (the
app, /events, the settings guard, the firmware upload) works as it is. The feeder's identity is a self-signed
certificate made on the device, or a certificate and key uploaded from the web app. The trust store holds one CA
certificate, which MQTT checks its broker against; or MQTT pins the broker's certificate on first contact (D36).
Decisions and measurements: docs/plans/https-tls.md, DESIGN.md section 9.7.

Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
Designed and written with the assistance of an AI (Anthropic's Claude);
reviewed by a human, but it may still contain mistakes.
"""

import esphome.codegen as cg
from esphome.components import esp32, precision_feeder, socket
from esphome.components.esp32 import add_idf_sdkconfig_option
import esphome.config_validation as cv
from esphome.const import CONF_ID

CODEOWNERS = []
DEPENDENCIES = ["esp32", "precision_feeder", "web_server_base"]
AUTO_LOAD = ["socket"]

precision_feeder_ns = cg.esphome_ns.namespace("precision_feeder")
PrecisionFeederTls = precision_feeder_ns.class_("PrecisionFeederTls", cg.Component)

CONF_FEEDER = "feeder"
CONF_MAX_SESSIONS = "max_sessions"
CONF_IDLE_TIMEOUT = "idle_timeout"
CONF_PORT = "port"

CONFIG_SCHEMA = cv.Schema(
    {
        cv.GenerateID(): cv.declare_id(PrecisionFeederTls),
        cv.GenerateID(CONF_FEEDER): cv.use_id(precision_feeder.PrecisionFeeder),
        # HTTPS sessions at once. Each takes about 15 KB of heap on top of the plain connection it relays (docs/BENCH.md);
        # 4 leave 65 KB or more free with BLE on NimBLE. A connection beyond waits for a free session.
        cv.Optional(CONF_MAX_SESSIONS, default=4): cv.int_range(min=1, max=6),
        # A session with no traffic either way for this long is closed, so idle browser connections give their slot
        # back. An open page's event stream sends something every 10 s, so it stays.
        cv.Optional(CONF_IDLE_TIMEOUT, default="20s"): cv.All(
            cv.positive_time_period_seconds, cv.Range(min=cv.TimePeriod(seconds=12), max=cv.TimePeriod(seconds=300))
        ),
        cv.Optional(CONF_PORT, default=443): cv.port,
    }
).extend(cv.COMPONENT_SCHEMA)


def _sockets(config):
    # Each session: the browser's socket and ours to 127.0.0.1:80 (ESPHome's server counts its own end); the
    # listener; and one for MQTT's first contact with a broker (pinning).
    socket.consume_sockets(2 * config[CONF_MAX_SESSIONS], "precision_feeder_tls")(config)
    socket.consume_sockets(1, "precision_feeder_tls", socket.SocketType.TCP_LISTEN)(config)
    socket.consume_sockets(1, "precision_feeder_tls_pin")(config)
    return config


FINAL_VALIDATE_SCHEMA = _sockets


async def to_code(config):
    var = cg.new_Pvariable(config[CONF_ID])
    await cg.register_component(var, config)
    cg.add(var.set_feeder(await cg.get_variable(config[CONF_FEEDER])))
    cg.add(var.set_max_sessions(config[CONF_MAX_SESSIONS]))
    cg.add(var.set_idle_timeout_s(config[CONF_IDLE_TIMEOUT].total_seconds))
    cg.add(var.set_port(config[CONF_PORT]))
    cg.add_define("USE_PRECISION_FEEDER_TLS")

    # mbedTLS: the server side, the peer's chain kept after a handshake (pinning), session tickets (a returning
    # browser resumes in about 0.1 s instead of 0.3 to 0.6 s). TLS 1.2 only (TLS 1.3 stays off, ESPHome's default):
    # 41 KB less flash and about 13 KB less heap per session (docs/plans/https-tls.md, E1).
    esp32.request_tls()
    esp32.require_mbedtls_tls_server()
    esp32.require_mbedtls_peer_cert()
    esp32.require_mbedtls_tls_extras(
        ["CONFIG_MBEDTLS_SERVER_SSL_SESSION_TICKETS", "CONFIG_MBEDTLS_CLIENT_SSL_SESSION_TICKETS"]
    )
    add_idf_sdkconfig_option("CONFIG_ESP_TLS_SERVER_SESSION_TICKETS", True)
    # Lean buffers, measured on the board: 16 KB in (browsers do not negotiate smaller records), 4 KB out, allocated
    # only while needed.
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_ASYMMETRIC_CONTENT_LEN", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_IN_CONTENT_LEN", 16384)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_SSL_OUT_CONTENT_LEN", 4096)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_BUFFER", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_FREE_CONFIG_DATA", True)
    add_idf_sdkconfig_option("CONFIG_MBEDTLS_DYNAMIC_FREE_CA_CERT", True)
