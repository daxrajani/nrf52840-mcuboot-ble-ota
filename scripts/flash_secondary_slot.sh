#!/usr/bin/env bash
set -euo pipefail

IMAGE="${1:?usage: $0 <signed-image.bin> <secondary-slot-base-addr>}"
ADDR="${2:?usage: $0 <signed-image.bin> <secondary-slot-base-addr>}"

test -f "$IMAGE" || { echo "Image file does not exist: $IMAGE" >&2; exit 1; }

echo "Flashing secondary slot image..."
echo "  image: $IMAGE"
echo "  addr : $ADDR"

nrfjprog --program "$IMAGE" --sectorerase --verify -f nrf52 --addr "$ADDR"
echo "Done. Reset board and observe MCUboot swap logs."
