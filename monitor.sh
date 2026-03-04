#!/usr/bin/env bash
# Open serial monitor for the UnitCamS3-5MP.
# Usage: ./monitor.sh [port]
#   port: optional, e.g. /dev/cu.usbmodem1101
# Exit with Ctrl+]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ERROR: ESP-IDF not found at $IDF_DIR"
    echo "Run ./setup_idf.sh first."
    exit 1
fi

# Use native arm64 Python on Apple Silicon
if [ -x /usr/bin/python3 ]; then
    export PATH="/usr/bin:$PATH"
fi

source "$IDF_DIR/export.sh" 2>/dev/null

cd "$SCRIPT_DIR"

PORT_ARG=""
if [ -n "${1:-}" ]; then
    PORT_ARG="-p $1"
else
    DETECTED=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    if [ -n "$DETECTED" ]; then
        echo "Auto-detected port: $DETECTED"
        PORT_ARG="-p $DETECTED"
    else
        echo "No port specified and none auto-detected."
        echo "Plug in the board and try: ./monitor.sh /dev/cu.usbmodemXXXX"
        exit 1
    fi
fi

echo "Starting monitor (Ctrl+] to exit)..."
idf.py $PORT_ARG monitor
