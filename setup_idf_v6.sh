#!/usr/bin/env bash
# Install ESP-IDF v6 (latest stable v6 release) for the UnitCamS3-5MP project.
# Run once: bash setup_idf_v6.sh
# After install, use bash build_v6.sh to build with v6.

set -euo pipefail

IDF_DIR="$HOME/esp/esp-idf-v6"
IDF_BRANCH="release/v6.0"

echo "=== ESP-IDF v6 Setup ==="
echo "Branch: $IDF_BRANCH"
echo "Install dir: $IDF_DIR"
echo ""

# Prerequisites check
if ! command -v git &>/dev/null; then
    echo "ERROR: git is required. Install with: brew install git  (macOS) or apt install git  (Linux)"
    exit 1
fi

if ! command -v python3 &>/dev/null; then
    echo "ERROR: python3 is required. Install with: brew install python3  (macOS) or apt install python3  (Linux)"
    exit 1
fi

# Clone or update ESP-IDF v6
if [ -d "$IDF_DIR" ]; then
    echo "ESP-IDF v6 directory already exists at $IDF_DIR"
    cd "$IDF_DIR"
    CURRENT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
    if [ "$CURRENT_BRANCH" != "${IDF_BRANCH#release/}" ] && [ "$CURRENT_BRANCH" != "v6.0" ]; then
        echo "Updating submodules..."
        git submodule update --init --recursive
    else
        echo "Already on $IDF_BRANCH — pulling latest..."
        git pull --ff-only
        git submodule update --init --recursive
    fi
else
    echo "Cloning ESP-IDF $IDF_BRANCH..."
    mkdir -p "$(dirname "$IDF_DIR")"
    git clone --branch "$IDF_BRANCH" --recursive https://github.com/espressif/esp-idf.git "$IDF_DIR"
fi

# Install toolchains (ESP32-S3 only to save space)
echo ""
echo "Installing ESP32-S3 toolchain..."
cd "$IDF_DIR"
./install.sh esp32s3

echo ""
echo "=== ESP-IDF v6 installed successfully ==="
echo ""
echo "To build with v6:"
echo "  bash build_v6.sh"
echo ""
echo "To use manually:"
echo "  source $IDF_DIR/export.sh"
