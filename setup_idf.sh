#!/usr/bin/env bash
# Install ESP-IDF v5.3.2 (latest 5.3.x LTS) for the UnitCamS3-5MP project.
# Run once: ./setup_idf.sh
# After install, all other scripts source ESP-IDF automatically.

set -euo pipefail

IDF_DIR="$HOME/esp/esp-idf"
IDF_TAG="v5.3.2"

echo "=== ESP-IDF Setup ==="
echo "Target: $IDF_TAG"
echo "Install dir: $IDF_DIR"
echo ""

# Prerequisites check
if ! command -v git &>/dev/null; then
    echo "ERROR: git is required. Install with: brew install git"
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required. Install with: brew install python3"
    exit 1
fi

# Note: cmake/ninja are bundled by ESP-IDF's install.sh, no need for brew.

# Clone ESP-IDF
if [ -d "$IDF_DIR" ]; then
    echo "ESP-IDF directory already exists at $IDF_DIR"
    cd "$IDF_DIR"
    CURRENT_TAG=$(git describe --tags --exact-match 2>/dev/null || echo "unknown")
    if [ "$CURRENT_TAG" != "$IDF_TAG" ]; then
        echo "Current tag: $CURRENT_TAG, switching to $IDF_TAG..."
        git fetch --tags
        git checkout "$IDF_TAG"
        git submodule update --init --recursive
    else
        echo "Already on $IDF_TAG"
    fi
else
    echo "Cloning ESP-IDF $IDF_TAG..."
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone --branch "$IDF_TAG" --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

# Install toolchains (ESP32-S3 only to save space)
echo ""
echo "Installing ESP32-S3 toolchain..."
cd "$IDF_DIR"
./install.sh esp32s3

echo ""
echo "=== ESP-IDF $IDF_TAG installed successfully ==="
echo ""
echo "To use manually:"
echo "  source $IDF_DIR/export.sh"
echo ""
echo "The build/flash/monitor scripts will source it automatically."
