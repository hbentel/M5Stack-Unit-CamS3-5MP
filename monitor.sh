#!/usr/bin/env bash
# Open serial monitor for the UnitCamS3-5MP.
# Usage: ./monitor.sh [port]
#   port: optional, e.g. /dev/cu.usbmodem1101
# Exit with Ctrl+]
#
# Works for both IDF v5 and v6 builds — idf.py monitor decodes crash addresses
# from the ELF file's DWARF info, so the IDF version used here does not matter.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "$SCRIPT_DIR/idf_env.sh"

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
