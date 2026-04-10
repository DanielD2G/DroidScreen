#!/bin/bash
# Run DroidScreen end-to-end
# Usage: ./scripts/run.sh [--port PORT] [--fps FPS] [--bitrate KBPS]
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
PORT=${PORT:-38271}

# Detect platform
case "$(uname -s)" in
    Darwin) PLATFORM="macos" ;;
    MINGW*|MSYS*|CYGWIN*) PLATFORM="windows" ;;
    *) echo "Unsupported platform: $(uname -s)"; exit 1 ;;
esac

BINARY="$PROJECT_DIR/build-$PLATFORM/desktop/$PLATFORM/droidscreen_desktop"

if [ ! -f "$BINARY" ]; then
    echo "Binary not found at: $BINARY"
    echo "Building first..."
    "$SCRIPT_DIR/build_${PLATFORM}.sh"
fi

# Check ADB
if ! command -v adb &>/dev/null; then
    echo "ERROR: adb not found in PATH"
    echo ""
    echo "Install Android platform-tools:"
    echo "  macOS:   brew install android-platform-tools"
    echo "  Windows: Download from https://developer.android.com/tools/releases/platform-tools"
    exit 1
fi

# Check device connected
DEVICE_COUNT=$(adb devices | grep -c "device$" || true)
if [ "$DEVICE_COUNT" -eq 0 ]; then
    echo "ERROR: No Android device connected via USB"
    echo ""
    echo "Checklist:"
    echo "  1. Connect tablet via USB cable"
    echo "  2. Enable Developer Options on tablet"
    echo "  3. Enable USB Debugging in Developer Options"
    echo "  4. Accept the USB debugging prompt on the tablet"
    echo ""
    echo "Then run this script again."
    exit 1
fi

echo "=== DroidScreen ==="
echo "Device found. Setting up ADB reverse tunnel..."

# Setup ADB reverse tunnel
adb reverse tcp:$PORT tcp:$PORT
echo "ADB reverse tunnel: localhost:$PORT -> device:$PORT"

# Cleanup on exit
cleanup() {
    echo ""
    echo "Cleaning up..."
    adb reverse --remove tcp:$PORT 2>/dev/null || true
    echo "Done."
}
trap cleanup EXIT

echo ""
echo "Make sure the DroidScreen Android app is running on the tablet."
echo "Starting desktop capture..."
echo ""

# Run the desktop app (pass all CLI args through)
"$BINARY" --port $PORT "$@"
