@echo off
REM Build DroidScreen desktop app for Windows
REM Requires: Visual Studio 2022 with C++ Desktop workload, Windows SDK 10.0.19041+

setlocal
set PROJECT_DIR=%~dp0..
set BUILD_DIR=%PROJECT_DIR%\build-windows

echo === DroidScreen Windows Build ===
echo Project: %PROJECT_DIR%
echo Build:   %BUILD_DIR%
echo.

REM Configure with CMake (Visual Studio generator)
cmake -B "%BUILD_DIR%" -S "%PROJECT_DIR%" ^
    -G "Visual Studio 17 2022" ^
    -A x64 ^
    -DCMAKE_SYSTEM_VERSION=10.0

if %ERRORLEVEL% NEQ 0 (
    echo CMake configure failed!
    exit /b 1
)

REM Build Release
cmake --build "%BUILD_DIR%" --config Release --parallel

if %ERRORLEVEL% NEQ 0 (
    echo Build failed!
    exit /b 1
)

echo.
echo === Build complete ===
echo Binary: %BUILD_DIR%\desktop\windows\Release\droidscreen_desktop.exe
echo.
echo To run:
echo   1. Connect Android device via USB
echo   2. Enable USB debugging on Android
echo   3. Install ^& run DroidScreen Android app
echo   4. Run: adb reverse tcp:38271 tcp:38271
echo   5. Run: %BUILD_DIR%\desktop\windows\Release\droidscreen_desktop.exe
