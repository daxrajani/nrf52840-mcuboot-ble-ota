#!/usr/bin/env bash
set -euo pipefail

VARIANT="${1:-v1}"
BOARD="${2:-nrf52840dk/nrf52840}"

case "$VARIANT" in
  v1)
    BUILD_DIR="build_v1"
    VERSION="1.0.0"
    NAME="Dax_BLE_v1"
    CRASH="OFF"
    ;;
  v2)
    BUILD_DIR="build_v2"
    VERSION="2.0.0"
    NAME="Dax_BLE_v2"
    CRASH="OFF"
    ;;
  v3-crash)
    BUILD_DIR="build_v3_crash"
    VERSION="3.0.0"
    NAME="Dax_BLE_v3"
    CRASH="ON"
    ;;
  *)
    echo "Usage: $0 [v1|v2|v3-crash] [board]" >&2
    exit 1
    ;;
esac

echo "Building $VARIANT on $BOARD ..."
west build --sysbuild -b "$BOARD" -d "$BUILD_DIR" app -- \
  -DAPP_FIRMWARE_VERSION="$VERSION" \
  -DAPP_BLE_NAME="$NAME" \
  -DCRASH_DEMO="$CRASH"

SIGNED="$(pwd)/$BUILD_DIR/app/zephyr/zephyr.signed.bin"
if [[ -f "$SIGNED" ]]; then
  echo "Signed image: $SIGNED"
else
  echo "Warning: signed image not found at expected location: $SIGNED" >&2
fi
