#!/usr/bin/env bash
#
# DroidScreen - ADB setup script (macOS/Linux)
#
# Sets up adb reverse port forwarding so the desktop app can connect
# to the Android device's TCP server through USB.
#
# Usage: ./adb_setup.sh [PORT]

set -euo pipefail

PORT="${1:-38271}"

echo "[adb_setup] Checking for connected devices..."
adb devices -l

echo "[adb_setup] Setting up reverse port forwarding on port $PORT..."
adb reverse tcp:"$PORT" tcp:"$PORT"

echo "[adb_setup] Verifying..."
adb reverse --list

echo "[adb_setup] Done. The Android app's TCP server on port $PORT"
echo "            is now reachable at 127.0.0.1:$PORT on this machine."
echo ""
echo "To remove: adb reverse --remove tcp:$PORT"
echo "To remove all: adb reverse --remove-all"
