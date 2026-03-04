#!/usr/bin/env bash
# Build, flash, and immediately open monitor — the most common workflow.
# Usage: ./flashmon.sh [port]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Use our fixed Python 3.14 environment!
source "$SCRIPT_DIR/idf_env.sh"

cd "$SCRIPT_DIR"

# If the existing cmake cache was built with a different IDF (e.g. v6), clean
# before rebuilding with v5 — mismatched caches cause picolibc errors.
V5_IDF="$HOME/esp/esp-idf"
CACHED_IDF=$(grep "^IDF_PATH:INTERNAL=" build/CMakeCache.txt 2>/dev/null | cut -d= -f2- || true)
if [ -n "$CACHED_IDF" ] && [ "$CACHED_IDF" != "$V5_IDF" ]; then
    echo "Detected v6 build artifacts — cleaning before v5 build..."
    rm -rf build sdkconfig
fi

if [ ! -f "build/config/sdkconfig.h" ]; then
    echo "Setting target to esp32s3..."
    idf.py set-target esp32s3
fi

PORT_ARG=""
if [ -n "${1:-}" ]; then
    PORT_ARG="-p $1"
else
    # Auto-detect: macOS native USB, Linux CDC-ACM, Linux UART-bridge
    DETECTED=$(ls /dev/cu.usbmodem* /dev/ttyACM* /dev/ttyUSB* 2>/dev/null | head -1 || true)
    if [ -n "$DETECTED" ]; then
        echo "Auto-detected port: $DETECTED"
        PORT_ARG="-p $DETECTED"
    fi
fi

echo "Build + Flash + Monitor..."
idf.py $PORT_ARG build flash monitor
