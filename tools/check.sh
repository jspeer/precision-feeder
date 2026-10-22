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
# The direct-drive disc (mechanism_n20.*) on a simulated motor, against the owner's bench sketch (tests/reference/).
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder -Itests -Itests/reference tests/test_n20.cpp \
  components/precision_feeder/feeder_*.cpp components/precision_feeder/mechanism_*.cpp -o "$OUT/test_n20"
"$OUT/test_n20"
"$OUT/test_ble_engine"

echo "== Python tests (client, and limits consistency)"
(cd tools && python3 -m unittest -q test_precision_feeder_ble test_make_release)
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
awk '/^  web_ui: !include/ { next } { print } /^  disc: !include/ { print "  ble_gatt: !include precision-feeder/packages/ble_gatt.yaml" }' \
  examples/disc.yaml > "$CONFIG/headless.yaml"
grep -q "ble_gatt" "$CONFIG/headless.yaml"
for f in basic.yaml ble.yaml disc.yaml ble_key_on_device.yaml headless.yaml; do
  echo "== esphome config examples/$f (as a copy in a config folder)"
  "$ESPHOME" config "$CONFIG/$f" >/dev/null
done
if [ "${1:-}" = "--compile" ]; then
  for f in basic.yaml ble.yaml disc.yaml ble_key_on_device.yaml headless.yaml; do
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
