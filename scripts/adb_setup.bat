@echo off
REM DroidScreen - ADB setup script (Windows)
REM
REM Sets up adb reverse port forwarding so the desktop app can connect
REM to the Android device's TCP server through USB.
REM
REM Usage: adb_setup.bat [PORT]

setlocal

set PORT=%1
if "%PORT%"=="" set PORT=38271

echo [adb_setup] Checking for connected devices...
adb devices -l
if errorlevel 1 (
    echo [adb_setup] ERROR: adb not found or no devices connected.
    exit /b 1
)

echo [adb_setup] Setting up reverse port forwarding on port %PORT%...
adb reverse tcp:%PORT% tcp:%PORT%
if errorlevel 1 (
    echo [adb_setup] ERROR: adb reverse failed.
    exit /b 1
)

echo [adb_setup] Verifying...
adb reverse --list

echo.
echo [adb_setup] Done. The Android app's TCP server on port %PORT%
echo             is now reachable at 127.0.0.1:%PORT% on this machine.
echo.
echo To remove: adb reverse --remove tcp:%PORT%
echo To remove all: adb reverse --remove-all

endlocal
