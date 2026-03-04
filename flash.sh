#!/usr/bin/env bash
# Flash firmware to the UnitCamS3-5MP.
# Usage: ./flash.sh [port]
#   port: optional, e.g. /dev/cu.usbmodem1101 (macOS) or /dev/ttyACM0 (Linux)
#         auto-detected if not specified

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Use our fixed Python 3.14 environment!
source "$SCRIPT_DIR/idf_env.sh"

cd "$SCRIPT_DIR"

# Check firmware exists
if [ ! -d "build" ]; then
    echo "ERROR: No build directory. Run ./build.sh first."
    exit 1
fi

# Refuse to flash a v6 build with the v5 toolchain — the cmake caches are
# version-specific and idf.py flash will fail with a picolibc error.
V5_IDF="$HOME/esp/esp-idf"
CACHED_IDF=$(grep "^IDF_PATH:INTERNAL=" build/CMakeCache.txt 2>/dev/null | cut -d= -f2- || true)
if [ -n "$CACHED_IDF" ] && [ "$CACHED_IDF" != "$V5_IDF" ]; then
    echo "ERROR: build/ was compiled with a different IDF ($CACHED_IDF)."
    echo "To flash a v6 build, activate the v6 environment and run:"
    echo "  export IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf6.0_py3.11_env"
    echo "  source ~/esp/esp-idf-v6/export.sh > /tmp/v6export.log 2>&1"
    echo "  PORT=\$(ls /dev/cu.usbmodem* /dev/ttyACM* 2>/dev/null | head -1)"
    echo "  idf.py -p \"\$PORT\" flash"
    exit 1
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
    else
        echo "No port specified and none auto-detected."
        echo "Plug in the board and try: ./flash.sh /dev/cu.usbmodemXXXX  (macOS)"
        echo "                       or: ./flash.sh /dev/ttyACM0           (Linux)"
        exit 1
    fi
fi

echo "Flashing..."
idf.py $PORT_ARG flash

echo ""
echo "Flash complete."
