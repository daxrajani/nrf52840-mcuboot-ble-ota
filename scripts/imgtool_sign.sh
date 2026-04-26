#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY="${KEY:-$ROOT/app/keys/mcuboot.pem}"
VERSION="${VERSION:-1.0.0}"
SLOT_SIZE="${SLOT_SIZE:-0x60000}"
IN="${1:?usage: $0 <zephyr.bin> <out.signed.bin>}"
OUT="${2:?usage: $0 <zephyr.bin> <out.signed.bin>}"
test -f "$KEY" || { echo "missing key: $KEY" >&2; exit 1; }
test -f "$IN" || { echo "missing input: $IN" >&2; exit 1; }
imgtool sign --key "$KEY" --header-size 0x200 --align 4 --version "$VERSION" --slot-size "$SLOT_SIZE" "$IN" "$OUT"
echo "Signed: $OUT"
