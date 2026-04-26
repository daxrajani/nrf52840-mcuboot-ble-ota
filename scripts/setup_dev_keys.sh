#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
KEY_DIR="$ROOT/app/keys"
KEY_PATH="$KEY_DIR/mcuboot.pem"

mkdir -p "$KEY_DIR"

if [[ -f "$KEY_PATH" ]]; then
  echo "Key already exists: $KEY_PATH"
  exit 0
fi

if command -v imgtool >/dev/null 2>&1; then
  imgtool keygen -k "$KEY_PATH" -t ecdsa-p256
  echo "Created $KEY_PATH"
  exit 0
fi

if command -v openssl >/dev/null 2>&1; then
  openssl ecparam -name prime256v1 -genkey -noout -out "$KEY_PATH"
  echo "Created $KEY_PATH (OpenSSL)"
  exit 0
fi

echo "Neither imgtool nor openssl found. Install Zephyr SDK / nRF toolchain." >&2
exit 1
