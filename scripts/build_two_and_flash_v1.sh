#!/usr/bin/env bash
# Sysbuild to `build/` (v1) and `build_v2/` (v2), then program `build/merged.hex`
#
#   ./scripts/build_two_and_flash_v1.sh
#   PRISTINE=1 ./scripts/build_two_and_flash_v1.sh
#   SKIP_FLASH=1 ./scripts/build_two_and_flash_v1.sh
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO_ROOT"
echo "Repository: $REPO_ROOT"

if [[ -z "${ZEPHYR_BASE:-}" ]]; then
  for z in "/c/ncs/v3.2.4/zephyr" "C:/ncs/v3.2.4/zephyr" "$HOME/ncs/v3.2.4/zephyr"; do
    if [[ -d "$z" ]]; then export ZEPHYR_BASE="$z"; echo "ZEPHYR_BASE=$ZEPHYR_BASE (auto-set)"; break; fi
  done
else
  echo "ZEPHYR_BASE=$ZEPHYR_BASE"
fi
if [[ -d "/c/ncs/toolchains/fd21892d0f/opt/zephyr-sdk" ]]; then
  export ZEPHYR_SDK_INSTALL_DIR="/c/ncs/toolchains/fd21892d0f/opt/zephyr-sdk"
  echo "ZEPHYR_SDK_INSTALL_DIR=$ZEPHYR_SDK_INSTALL_DIR (auto-set)"
fi

run_west() {
  if command -v west &>/dev/null; then
    west "$@"
  elif [[ -f "/c/ncs/toolchains/fd21892d0f/opt/bin/python" ]]; then
    /c/ncs/toolchains/fd21892d0f/opt/bin/python -m west "$@"
  else
    echo "error: put west on PATH, or install NCS under /c/ncs" >&2
    exit 1
  fi
}

if [[ ! -f app/keys/mcuboot.pem ]]; then
  echo "error: run scripts/setup_dev_keys.sh first" >&2
  exit 1
fi

BOARD="${BOARD:-nrf52840dk/nrf52840}"
V1_DIR="${V1_DIR:-build}"
V2_DIR="${V2_DIR:-build_v2}"
P_EXTRA=()
if [[ -n "${PRISTINE:-}" ]]; then P_EXTRA=(-p always); fi

echo ""
echo "=== [1/2] v1 -> $V1_DIR ==="
run_west build --sysbuild -b "$BOARD" -d "$V1_DIR" "${P_EXTRA[@]:-}" app -- \
  -DAPP_FIRMWARE_VERSION=1.0.0 -DAPP_BLE_NAME=Dax_BLE_v1

echo ""
echo "=== [2/2] v2 -> $V2_DIR ==="
run_west build --sysbuild -b "$BOARD" -d "$V2_DIR" "${P_EXTRA[@]:-}" app -- \
  -DAPP_FIRMWARE_VERSION=2.0.0 -DAPP_BLE_NAME=Dax_BLE_v2

S1="$V1_DIR/app/zephyr/zephyr.signed.bin"
S2="$V2_DIR/app/zephyr/zephyr.signed.bin"
MG="$V1_DIR/merged.hex"
echo ""
echo "v1 signed: $S1  |  v2 signed: $S2  |  merged: $MG"

if [[ -n "${SKIP_FLASH:-}" ]]; then echo "SKIP_FLASH: done"; exit 0; fi
if [[ ! -f "$MG" ]]; then echo "error: missing $MG" >&2; exit 1; fi
if ! command -v nrfjprog &>/dev/null; then echo "error: nrfjprog not in PATH" >&2; exit 1; fi

echo ""
echo "=== Flash v1 (merged) ==="
nrfjprog --program "$MG" -f nrf52 --sectorerase --verify --reset
echo "Done. OTA: $S2"
