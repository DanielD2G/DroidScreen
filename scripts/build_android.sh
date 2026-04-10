#!/bin/bash
# Build DroidScreen Android app
# Requires: Android SDK, NDK 26+, Java 17+
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(dirname "$SCRIPT_DIR")"
ANDROID_DIR="$PROJECT_DIR/android"

echo "=== DroidScreen Android Build ==="

# Check prerequisites
if [ -z "$ANDROID_HOME" ] && [ -z "$ANDROID_SDK_ROOT" ]; then
    # Try common locations
    if [ -d "$HOME/Library/Android/sdk" ]; then
        export ANDROID_HOME="$HOME/Library/Android/sdk"
    elif [ -d "$HOME/Android/Sdk" ]; then
        export ANDROID_HOME="$HOME/Android/Sdk"
    else
        echo "ERROR: ANDROID_HOME not set and Android SDK not found"
        echo "Install Android Studio or set ANDROID_HOME"
        exit 1
    fi
fi

echo "ANDROID_HOME: ${ANDROID_HOME:-$ANDROID_SDK_ROOT}"

# Check for Java
if ! command -v java &>/dev/null; then
    echo "ERROR: Java not found. Install JDK 17+"
    echo "  macOS: brew install openjdk@17"
    echo "  Linux: sudo apt install openjdk-17-jdk"
    exit 1
fi

echo "Java: $(java -version 2>&1 | head -1)"

cd "$ANDROID_DIR"

# Create local.properties if missing
if [ ! -f local.properties ]; then
    echo "sdk.dir=${ANDROID_HOME:-$ANDROID_SDK_ROOT}" > local.properties
    echo "Created local.properties"
fi

# Build with Gradle wrapper
if [ -f gradlew ]; then
    chmod +x gradlew
    ./gradlew assembleDebug
else
    echo "ERROR: Gradle wrapper not found. Run from Android Studio first,"
    echo "  or create it with: gradle wrapper --gradle-version 8.5"
    exit 1
fi

APK_PATH="app/build/outputs/apk/debug/app-debug.apk"

echo ""
echo "=== Build complete ==="
echo "APK: $ANDROID_DIR/$APK_PATH"
echo ""
echo "To install on connected device:"
echo "  adb install -r $ANDROID_DIR/$APK_PATH"
