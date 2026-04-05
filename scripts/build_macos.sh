#!/bin/bash
# Build DroidScreen desktop app for macOS
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-macos"

echo "=== DroidScreen macOS Build ==="
echo "Project: $PROJECT_DIR"
echo "Build:   $BUILD_DIR"
echo ""

# Configure
cmake -B "$BUILD_DIR" -S "$PROJECT_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.3

# Build
cmake --build "$BUILD_DIR" --parallel

echo ""
echo "=== Build complete ==="
echo "Binary: $BUILD_DIR/desktop/macos/droidscreen_desktop"
echo ""
echo "To run:"
echo "  1. Connect Android device via USB"
echo "  2. Enable USB debugging on Android"
echo "  3. Install & run DroidScreen Android app"
echo "  4. Run: adb reverse tcp:38271 tcp:38271"
echo "  5. Run: $BUILD_DIR/desktop/macos/droidscreen_desktop"
