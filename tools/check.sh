#!/usr/bin/env bash
# Run every check this repository has: the host tests (C++ and Python) and, if ESPHome is available, a config
# validation of each example. The same script runs in CI (.github/workflows/ci.yml) and on your machine.
#
#   tools/check.sh              host tests, plus "esphome config" if esphome is on PATH or in .venv
#   tools/check.sh --compile    also compile the examples (slow; downloads the toolchain the first time)
#
# Part of the Precision Feeder ESPHome firmware (public domain, The Unlicense).
# Designed and written with the assistance of an AI (Anthropic's Claude);
# reviewed by a human, but it may still contain mistakes.
set -euo pipefail
cd "$(dirname "$0")/.."
CXX="${CXX:-c++}"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT

echo "== C++ host tests"
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder \
  tests/test_core.cpp components/precision_feeder/feeder_*.cpp -o "$OUT/test_core"
"$OUT/test_core"
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder tests/test_logic.cpp -o "$OUT/test_logic"
"$OUT/test_logic" | tail -1
"$CXX" -O2 -std=c++17 -Wall -Wextra -Werror -Icomponents/precision_feeder_ble tests/test_protocol.cpp -o "$OUT/test_protocol"
"$OUT/test_protocol"

echo "== Python client tests"
(cd tools && python3 -m unittest -q test_precision_feeder_ble)

ESPHOME=""
if [ -x .venv/bin/esphome ]; then ESPHOME=.venv/bin/esphome; elif command -v esphome >/dev/null; then ESPHOME=esphome; fi
if [ -z "$ESPHOME" ]; then
  echo "== ESPHome not found: skipping config validation"
  exit 0
fi
if [ ! -f examples/secrets.yaml ]; then
  # Throwaway values, only so the examples validate (CI has no secrets).
  echo "== examples/secrets.yaml missing: writing throwaway values for validation"
  cat > examples/secrets.yaml <<SECRETS
wifi_ssid: "ci"
wifi_password: "ci-password"
fallback_ap_password: "ci-fallback-password"
api_encryption_key: "$(head -c 32 /dev/urandom | base64)"
ble_key: "$(head -c 32 /dev/urandom | base64)"
SECRETS
fi
for f in examples/basic.yaml examples/ble.yaml; do
  echo "== esphome config $f"
  "$ESPHOME" config "$f" >/dev/null
done
if [ "${1:-}" = "--compile" ]; then
  for f in examples/basic.yaml examples/ble.yaml; do
    echo "== esphome compile $f"
    "$ESPHOME" compile "$f" 2>&1 | tee "$OUT/compile.log" | grep -E "^(RAM|Flash):" || true
    if grep -q "warning:" "$OUT/compile.log"; then
      echo "compiler warnings in $f:"
      grep "warning:" "$OUT/compile.log"
      exit 1
    fi
  done
fi
echo "== all checks passed"
