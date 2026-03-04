#!/usr/bin/env bash
# Build the UnitCamS3-5MP firmware with ESP-IDF v6.
# Usage: bash build_v6.sh [clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
V6_IDF="$HOME/esp/esp-idf-v6"
V6_VENV="$HOME/.espressif/python_env/idf6.0_py3.11_env"

if [ ! -f "$V6_IDF/export.sh" ]; then
    echo "ERROR: ESP-IDF v6 not found at $V6_IDF"
    exit 1
fi

echo "Activating ESP-IDF v6..."
export IDF_PYTHON_ENV_PATH="$V6_VENV"
# shellcheck source=/dev/null
source "$V6_IDF/export.sh" > /tmp/v6export.log 2>&1

cd "$SCRIPT_DIR"

if [ "${1:-}" = "clean" ]; then
    echo "Cleaning build..."
    rm -rf build sdkconfig
fi

# If the existing cmake cache was built with a different IDF (e.g. v5), the
# bootloader subproject cache will point to the wrong IDF_PATH and ninja will
# refuse to build. Detect this and clean automatically.
CACHED_IDF=$(grep "^IDF_PATH:INTERNAL=" build/CMakeCache.txt 2>/dev/null | cut -d= -f2- || true)
if [ -z "$CACHED_IDF" ] || [ "$CACHED_IDF" != "$V6_IDF" ]; then
    rm -rf build sdkconfig
    echo "Setting target to esp32s3..."
    idf.py set-target esp32s3
fi

echo "Building..."
idf.py build

echo ""
echo "Build complete. Firmware at: build/unitcams3_firmware.bin"
