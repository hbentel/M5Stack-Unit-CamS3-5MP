#!/usr/bin/env bash
# Build the UnitCamS3-5MP firmware.
# Usage: ./build.sh [clean]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/idf_env.sh"

cd "$SCRIPT_DIR"

if [ "${1:-}" = "clean" ]; then
    echo "Cleaning build..."
    rm -rf build sdkconfig
fi

# If the existing cmake cache was built with a different IDF (e.g. v6), the
# bootloader subproject cache will point to the wrong IDF_PATH. Detect and clean.
V5_IDF="$HOME/esp/esp-idf"
CACHED_IDF=$(grep "^IDF_PATH:INTERNAL=" build/CMakeCache.txt 2>/dev/null | cut -d= -f2- || true)
if [ -z "$CACHED_IDF" ] || [ "$CACHED_IDF" != "$V5_IDF" ]; then
    rm -rf build sdkconfig
    echo "Setting target to esp32s3..."
    idf.py set-target esp32s3
fi

echo "Building..."
idf.py build

echo ""
echo "Build complete. Firmware at: build/unitcams3_firmware.bin"
