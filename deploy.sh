#!/usr/bin/env bash
# Compile and flash the console.
#
#   ./deploy.sh [/dev/ttyACM0]
#   ./deploy.sh --compile-only
#
# Requires arduino-cli with the esp32 core, and the libraries listed in the
# README already in ~/Arduino/libraries.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# PSRAM=opi because this is the R8 part with octal PSRAM; USBMode=hwcdc and
# CDCOnBoot=cdc because the board has no UART bridge - the ESP32-S3's native
# USB is the only console, and without CDCOnBoot the Serial output goes
# nowhere.
FQBN="esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,CDCOnBoot=cdc,USBMode=hwcdc,CPUFreq=240,DebugLevel=error"

if [[ ! -f "$HERE/config.h" ]]; then
  echo "error: config.h missing. Copy config.example.h to config.h and fill it in." >&2
  exit 1
fi

echo "compiling..."
arduino-cli compile --fqbn "$FQBN" --build-path "$HERE/build" "$HERE"

if [[ "${1:-}" == "--compile-only" ]]; then
  echo "compiled (not flashed)"
  exit 0
fi

PORT="${1:-/dev/ttyACM0}"
if [[ ! -e "$PORT" ]]; then
  echo "error: $PORT not found. Is the board plugged in?" >&2
  exit 1
fi

echo "flashing $PORT..."
arduino-cli upload -p "$PORT" --fqbn "$FQBN" --input-dir "$HERE/build" "$HERE"

cat <<EOF

Flashed. To watch it boot:

    arduino-cli monitor -p $PORT -c baudrate=115200

It prints the listening URL. Then point the hooks at it:

    ./install-hooks.sh <ip>
EOF
