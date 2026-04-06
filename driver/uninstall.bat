@echo off
REM ============================================================================
REM DroidScreen Virtual Display Driver - Uninstallation Script
REM
REM Removes the DroidScreen virtual display driver from the system.
REM Must be run as Administrator.
REM ============================================================================

setlocal enabledelayedexpansion

echo.
echo ========================================
echo  DroidScreen Virtual Display Driver
echo  Uninstallation Script
echo ========================================
echo.

REM Check for administrator privileges
net session >nul 2>&1
if %errorlevel% neq 0 (
    echo ERROR: This script must be run as Administrator.
    echo Right-click and select "Run as administrator".
    echo.
    pause
    exit /b 1
)

REM Try devcon first
where devcon >nul 2>&1
if %errorlevel% equ 0 (
    echo Removing device with devcon...
    devcon remove Root\DroidScreenDriver
    echo.
)

REM Find and remove the driver from the driver store
echo Searching for DroidScreen driver in driver store...
for /f "tokens=1,2*" %%a in ('pnputil /enum-drivers ^| findstr /i "DroidScreen"') do (
    echo Found: %%a %%b %%c
)

echo.
echo Enumerating OEM drivers to find DroidScreen...
for /f "tokens=1 delims=:" %%a in ('pnputil /enum-drivers ^| findstr /i "oem"') do (
    set OEM_INF=%%a
    set OEM_INF=!OEM_INF: =!
    pnputil /enum-drivers /driver:!OEM_INF! 2>nul | findstr /i "DroidScreen" >nul
    if !errorlevel! equ 0 (
        echo Found DroidScreen driver: !OEM_INF!
        echo Removing...
        pnputil /delete-driver !OEM_INF! /uninstall /force
        echo.
    )
)

REM Clean up staging directory
set STAGE_DIR=%~dp0stage
if exist "%STAGE_DIR%" (
    echo Cleaning up staging directory...
    rmdir /s /q "%STAGE_DIR%"
)

echo.
echo ========================================
echo  Uninstallation Complete
echo ========================================
echo.
echo The DroidScreen virtual display has been removed.
echo You may need to restart your computer for all changes to take effect.
echo.
echo To disable test signing:
echo   bcdedit /set testsigning off
echo.

pause
exit /b 0
