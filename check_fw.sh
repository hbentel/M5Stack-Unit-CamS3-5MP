#!/bin/bash
# check_fw.sh — compare running firmware on device against local build

DEVICE_IP="${1:-192.168.50.44}"
ELF="$(dirname "$0")/build/unitcams3_firmware.elf"

echo "Device: http://$DEVICE_IP"
echo

# Device health
HEALTH=$(curl -s --max-time 5 "http://$DEVICE_IP/health")
if [ -z "$HEALTH" ]; then
    echo "ERROR: No response from device"
    exit 1
fi

DEVICE_SHA=$(echo "$HEALTH" | python3 -c "import sys,json; print(json.load(sys.stdin)['app_sha256'])" 2>/dev/null)
VERSION=$(echo "$HEALTH"    | python3 -c "import sys,json; print(json.load(sys.stdin).get('version','unknown'))" 2>/dev/null)
RESET=$(echo "$HEALTH"      | python3 -c "import sys,json; print(json.load(sys.stdin).get('reset_reason','?'))" 2>/dev/null)
UPTIME=$(echo "$HEALTH"     | python3 -c "import sys,json; print(json.load(sys.stdin).get('uptime_s','?'))" 2>/dev/null)

echo "Version:      $VERSION"
echo "Reset reason: $RESET"
echo "Uptime:       ${UPTIME}s"
echo "Device SHA:   $DEVICE_SHA"

# Local build
if [ -f "$ELF" ]; then
    BUILD_SHA=$(shasum -a 256 "$ELF" | cut -c1-64)
    echo "Build SHA:    $BUILD_SHA"
    echo
    if [ "$BUILD_SHA" = "$DEVICE_SHA" ]; then
        echo "✓ MATCH — device is running the current build"
    else
        echo "✗ MISMATCH — device is NOT running the current build"
    fi
else
    echo "Build SHA:    (no build found at $ELF)"
fi
