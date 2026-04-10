#!/bin/bash
# Build DroidScreen desktop app for macOS
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
BUILD_DIR="$PROJECT_DIR/build-macos"
APP_BUNDLE="$BUILD_DIR/desktop/macos/droidscreen_desktop.app"
ENTITLEMENTS="$PROJECT_DIR/desktop/macos/DroidScreen.entitlements"
CONFIG_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/droidscreen"
CONFIG_FILE="${DROIDSCREEN_CONFIG_FILE:-$CONFIG_DIR/build.env}"

if [ -f "$CONFIG_FILE" ]; then
    # shellcheck disable=SC1090
    . "$CONFIG_FILE"
fi

SIGN_IDENTITY="${DROIDSCREEN_CODESIGN_IDENTITY:--}"

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

# Sign the app bundle.
# Use a real signing identity if you want macOS TCC permissions such as
# Screen Recording to persist across rebuilds. Ad-hoc signing is fine for
# local testing, but its code identity changes whenever the app contents change.
codesign --force --deep --sign "$SIGN_IDENTITY" \
    --entitlements "$ENTITLEMENTS" \
    "$APP_BUNDLE"
echo "Signed: $APP_BUNDLE"

if [ "$SIGN_IDENTITY" = "-" ]; then
    echo "NOTE: Using ad-hoc signing."
    echo "      Screen Recording and other TCC permissions may need to be re-granted"
    echo "      after rebuilds because ad-hoc signatures do not provide a stable identity."
    echo "      To preserve permissions, set DROIDSCREEN_CODESIGN_IDENTITY to a real"
    echo "      certificate name before building."
else
    echo "Signing identity: $SIGN_IDENTITY"
    echo "Config file: $CONFIG_FILE"
fi

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
