#!/usr/bin/env bash
# Run every check this repository has: the host tests (C++ and Python) and, if ESPHome is available, a config
# validation of each example. The same script runs in CI (.github/workflows/ci.yml) and on your machine.
#
#   tools/check.sh              host tests, plus "esphome config" if esphome is on PATH or in .venv (the examples,
#                               ble.yaml with the api_key_on_device package, and the prebuilt binaries' firmware/)
#   tools/check.sh --compile    also compile the examples (slow; downloads the toolchain the first time)
#
# Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
# Designed and written with the assistance of an AI (Anthropic's Claude);
# reviewed by a human, but it may still contain mistakes.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== C++ host tests"
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder \
  tests/test_core.cpp components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp -o "$OUT/test_core"
"$OUT/test_core"
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder tests/test_logic.cpp -o "$OUT/test_logic"
"$OUT/test_logic" | tail -1
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder_ble tests/test_protocol.cpp -o "$OUT/test_protocol"
"$OUT/test_protocol"
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder -Icomponents/precision_feeder_ble -Itests \
  tests/test_ble_engine.cpp components/precision_feeder_ble/engine.cpp components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp \
  -o "$OUT/test_ble_engine"
# The NimBLE adapter's hand-off between tasks and its GATT table (gatt_link.h, gatt_table.h; 2.4.0).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -pthread -Icomponents/precision_feeder_ble tests/test_ble_link.cpp \
  -o "$OUT/test_ble_link"
"$OUT/test_ble_link"
# The direct-drive disc (mechanism_n20.*) on a simulated motor, against the owner's bench sketch (tests/reference/).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder -Itests -Itests/reference tests/test_n20.cpp \
  components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp -o "$OUT/test_n20"
"$OUT/test_n20"
"$OUT/test_ble_engine"
# MQTT's own topics (feeder_mqtt.*, 2.2.0).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder -Itests tests/test_mqtt.cpp \
  components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp -o "$OUT/test_mqtt"
"$OUT/test_mqtt"
# HTTPS and MQTT over TLS: what port 80 does, how MQTT connects, what to pin (tls_policy.h, 2.4.1).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder_tls tests/test_tls.cpp -o "$OUT/test_tls"
"$OUT/test_tls"
# The ACME client against a fake server: requests, JWS, retries, renewal (acme.h, 2.4.2).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder_tls -Icomponents/precision_feeder \
  tests/test_acme.cpp -o "$OUT/test_acme"
"$OUT/test_acme"

echo "== Python tests (client, and limits consistency)"
(cd tools && python3 -m unittest -q test_precision_feeder_ble test_make_release test_check_scopes)
python3 -m unittest -q tests/test_consistency.py

echo "== Web app (web/dist built from web/src, parser tests)"
python3 tools/build_web.py --check
if command -v node >/dev/null; then
  node --check web/dist/app.js
  node --test tests/test_web.mjs >/dev/null
else
  echo "Node not found: skipping the web app's syntax check and parser tests"
fi

ESPHOME=""
if [ -x .venv/bin/esphome ]; then ESPHOME=.venv/bin/esphome; elif command -v esphome >/dev/null; then ESPHOME=esphome; fi
if [ -z "$ESPHOME" ]; then
  echo "== ESPHome not found: skipping config validation"
  exit 0
fi
# The examples are meant to sit in an ESPHome config folder next to a clone of this repository named
# precision-feeder/ (see docs/BUILDING.md), so validate them in exactly that layout, in a temporary folder.
CONFIG="$OUT/config"
mkdir -p "$CONFIG"
ln -s "$(pwd)" "$CONFIG/precision-feeder"
cp examples/basic.yaml examples/ble.yaml examples/disc.yaml "$CONFIG/"
if [ -f examples/secrets.yaml ]; then
  cp examples/secrets.yaml "$CONFIG/secrets.yaml"
else
  # Throwaway values, only so the examples validate (CI has no secrets).
  echo "== no examples/secrets.yaml: using throwaway values for validation"
  cat > "$CONFIG/secrets.yaml" <<SECRETS
wifi_ssid: "ci"
wifi_password: "ci-password"
fallback_ap_password: "ci-fallback-password"
api_encryption_key: "$(head -c 32 /dev/urandom | base64)"
ble_key: "$(head -c 32 /dev/urandom | base64)"
SECRETS
fi
# The BLE example with the API key provisioned on the device (packages/api_key_on_device.yaml), as the prebuilt
# binaries are built.
awk '{ print } /^  ble_gatt: !include/ { print "  api_key: !include precision-feeder/packages/api_key_on_device.yaml" }' \
  examples/ble.yaml > "$CONFIG/ble_key_on_device.yaml"
grep -q "api_key_on_device" "$CONFIG/ble_key_on_device.yaml"
# A headless build (BUILDING.md): the disc example without the web app, run over BLE.
awk '/^  (web_ui|tls): !include/ { next } { print } /^  disc: !include/ { print "  ble_gatt: !include precision-feeder/packages/ble_gatt.yaml" }' \
  examples/disc.yaml > "$CONFIG/headless.yaml"
grep -q "ble_gatt" "$CONFIG/headless.yaml"
if grep -q "^  tls: !include" "$CONFIG/headless.yaml"; then echo "headless.yaml still has tls"; exit 1; fi
# MQTT over TLS and ACME (mqtt.yaml, mqtt_tls.yaml and acme.yaml, as the prebuilt binaries have them): the basic
# example with all three.
sed -E 's/^  # (mqtt|mqtt_tls|acme): !include/  \1: !include/' examples/basic.yaml > "$CONFIG/mqtt_tls.yaml"
grep -q "^  mqtt_tls: !include" "$CONFIG/mqtt_tls.yaml"
grep -q "^  acme: !include" "$CONFIG/mqtt_tls.yaml"
# Python with PyYAML for tools/check_scopes.py: ESPHome's own, else the system's.
PY="$(dirname "$ESPHOME")/python"
[ -x "$PY" ] || PY=python3
for f in basic.yaml ble.yaml disc.yaml ble_key_on_device.yaml headless.yaml mqtt_tls.yaml; do
  echo "== esphome config examples/$f (as a copy in a config folder), and who sees what"
  "$ESPHOME" config "$CONFIG/$f" > "$OUT/resolved.yaml"
  "$PY" tools/check_scopes.py "$OUT/resolved.yaml"
done
if [ "${1:-}" = "--compile" ]; then
  for f in basic.yaml ble.yaml disc.yaml ble_key_on_device.yaml headless.yaml mqtt_tls.yaml; do
    echo "== esphome compile examples/$f"
    # ESPHome's own exit status decides (the filter below only prints the sizes); a failed build fails the check.
    if ! "$ESPHOME" compile "$CONFIG/$f" > "$OUT/compile.log" 2>&1; then
      echo "compiling $f failed:"
      tail -40 "$OUT/compile.log"
      exit 1
    fi
    grep -E "^(RAM|Flash):" "$OUT/compile.log" || true
    if grep -qiE "warning:|error:" "$OUT/compile.log"; then
      echo "compiler warnings or errors in $f:"
      grep -iE "warning:|error:" "$OUT/compile.log"
      exit 1
    fi
  done
fi
# The prebuilt binaries' configs (firmware/), and that no secret reaches them.
tools/build_bins.sh --config
echo "== all checks passed"
