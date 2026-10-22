#!/usr/bin/env bash
# Builds the prebuilt binaries (firmware/*.yaml) as GitHub Actions does for a release, or only validates them.
#
#   tools/build_bins.sh --config          validate both variants (tools/check.sh runs this)
#   tools/build_bins.sh <out dir>         compile both variants and copy the images to <out dir>:
#                                         precision-feeder-<variant>.factory.bin (first flash, from address 0) and
#                                         precision-feeder-<variant>.ota.bin (the Firmware page's upload)
#
# The configs read the packages from a clone named precision-feeder/ next to them, as examples/ do, so they are
# built in that layout in a temporary folder. Every !secret the packages read is removed by the configs; the
# throwaway secrets below only let the YAML load, and none ends up in an image.
#
# Part of the Precision Feeder ESPHome firmware (MIT licence, see LICENSE).
# Designed and written with the assistance of an AI (Anthropic's Claude);
# reviewed by a human, but it may still contain mistakes.
set -euo pipefail
cd "$(dirname "$0")/.."

MODE="${1:?usage: tools/build_bins.sh --config | <out dir>}"
ESPHOME=""
if [ -x .venv/bin/esphome ]; then ESPHOME=.venv/bin/esphome; elif command -v esphome >/dev/null; then ESPHOME=esphome; fi
[ -n "$ESPHOME" ] || { echo "ESPHome not found"; exit 1; }

WORK="${BUILD_BINS_WORK:-$(mktemp -d)}"  # BUILD_BINS_WORK keeps the build folder (and its cache) between runs
mkdir -p "$WORK"
ln -sfn "$(pwd)" "$WORK/precision-feeder"
API_KEY="$(head -c 32 /dev/urandom | base64)"
BLE_KEY="$(head -c 32 /dev/urandom | base64)"
cat > "$WORK/secrets.yaml" <<SECRETS
wifi_ssid: "unused"
wifi_password: "unused-password"
fallback_ap_password: "unused-password"
api_encryption_key: "$API_KEY"
ble_key: "$BLE_KEY"
SECRETS

for variant in full no-ble; do
  cp "firmware/$variant.yaml" "$WORK/precision-feeder-$variant.yaml"
  if [ "$MODE" = "--config" ]; then
    echo "== esphome config firmware/$variant.yaml"
    "$ESPHOME" config "$WORK/precision-feeder-$variant.yaml" >/dev/null
    # Nothing secret may reach the image: the throwaway values must all have been removed.
    if "$ESPHOME" config "$WORK/precision-feeder-$variant.yaml" --show-secrets 2>/dev/null |
        grep -qF -e "unused" -e "$API_KEY" -e "$BLE_KEY"; then
      echo "firmware/$variant.yaml still uses a secret"
      exit 1
    fi
    continue
  fi
  echo "== esphome compile firmware/$variant.yaml"
  touch "$WORK/.started-$variant"
  # ESPHome's own exit status decides; the filter only prints the sizes. A failed build fails the release.
  if ! "$ESPHOME" compile "$WORK/precision-feeder-$variant.yaml" > "$WORK/compile-$variant.log" 2>&1; then
    echo "compiling $variant failed:"
    tail -40 "$WORK/compile-$variant.log"
    exit 1
  fi
  grep -E "^(RAM|Flash):" "$WORK/compile-$variant.log" || true
  if grep -qiE "warning:|error:" "$WORK/compile-$variant.log"; then
    echo "compiler warnings or errors in $variant:"
    grep -iE "warning:|error:" "$WORK/compile-$variant.log"
    exit 1
  fi
  # Both variants have the device name precision-feeder, so they share a build folder: take the image just made.
  build=$(find "$WORK/.esphome/build" -name firmware.factory.bin -newer "$WORK/.started-$variant" | head -1)
  [ -n "$build" ] || { echo "no factory image for $variant"; exit 1; }
  mkdir -p "$MODE"
  cp "$build" "$MODE/precision-feeder-$variant.factory.bin"
  cp "$(dirname "$build")/firmware.ota.bin" "$MODE/precision-feeder-$variant.ota.bin"
  grep -E "^Flash:" "$WORK/compile-$variant.log" | tail -1 > "$MODE/precision-feeder-$variant.size.txt" || true
done
