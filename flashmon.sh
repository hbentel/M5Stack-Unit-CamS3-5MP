#!/usr/bin/env bash
# Build, flash, and immediately open monitor — the most common workflow.
# Usage: ./flashmon.sh [port]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# Use our fixed Python 3.14 environment!
source "$SCRIPT_DIR/idf_env.sh"

cd "$SCRIPT_DIR"

# Set target if needed
if [ ! -f "build/config/sdkconfig.h" ]; then
    echo "Setting target to esp32s3..."
    idf.py set-target esp32s3
fi

PORT_ARG=""
if [ -n "${1:-}" ]; then
    PORT_ARG="-p $1"
else
    DETECTED=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1 || true)
    if [ -n "$DETECTED" ]; then
        echo "Auto-detected port: $DETECTED"
        PORT_ARG="-p $DETECTED"
    fi
fi

echo "Build + Flash + Monitor..."
idf.py $PORT_ARG build flash monitor
