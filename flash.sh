#!/usr/bin/env bash
# Flash firmware to the UnitCamS3-5MP.
# Usage: ./flash.sh [port]
#   port: optional, e.g. /dev/cu.usbmodem1101
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

PORT_ARG=""
if [ -n "${1:-}" ]; then
    PORT_ARG="-p $1"
else
    # Auto-detect ESP32-S3 native USB
    DETECTED=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    if [ -n "$DETECTED" ]; then
        echo "Auto-detected port: $DETECTED"
        PORT_ARG="-p $DETECTED"
    else
        echo "No port specified and none auto-detected."
        echo "Plug in the board and try: ./flash.sh /dev/cu.usbmodemXXXX"
        exit 1
    fi
fi

echo "Flashing..."
idf.py $PORT_ARG flash

echo ""
echo "Flash complete."
