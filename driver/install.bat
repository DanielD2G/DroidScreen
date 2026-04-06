@echo off
REM ============================================================================
REM DroidScreen Virtual Display Driver - Installation Script
REM
REM This script installs the DroidScreen IddCx virtual display driver.
REM It must be run as Administrator.
REM
REM Prerequisites:
REM   - Built DroidScreenDriver.dll (from the DroidScreenDriver.vcxproj)
REM   - Windows 10 version 2004 or later
REM   - Administrator privileges
REM
REM The script will:
REM   1. Enable test signing (requires reboot if not already enabled)
REM   2. Create a self-signed test certificate
REM   3. Create a catalog file and sign the driver
REM   4. Install the driver using devcon or pnputil
REM ============================================================================

setlocal enabledelayedexpansion

echo.
echo ========================================
echo  DroidScreen Virtual Display Driver
echo  Installation Script
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

REM Configuration
set DRIVER_DIR=%~dp0DroidScreenDriver
set BUILD_DIR=%DRIVER_DIR%\x64\Release
set CERT_NAME=DroidScreenTestCert
set CERT_STORE=PrivateCertStore
set INF_FILE=%DRIVER_DIR%\DroidScreenDriver.inf

REM Check if the driver DLL exists
if not exist "%BUILD_DIR%\DroidScreenDriver.dll" (
    echo ERROR: DroidScreenDriver.dll not found at:
    echo   %BUILD_DIR%\DroidScreenDriver.dll
    echo.
    echo Please build the driver first:
    echo   1. Open DroidScreenDriver.vcxproj in Visual Studio
    echo   2. Select Release ^| x64
    echo   3. Build the solution
    echo.
    echo Or from a Developer Command Prompt:
    echo   cd %DRIVER_DIR%
    echo   msbuild DroidScreenDriver.vcxproj /p:Configuration=Release /p:Platform=x64
    echo.
    pause
    exit /b 1
)

REM ============================================================================
REM Step 1: Enable test signing
REM ============================================================================

echo [Step 1/5] Checking test signing status...

bcdedit /enum {current} | findstr /i "testsigning.*Yes" >nul 2>&1
if %errorlevel% neq 0 (
    echo Enabling test signing mode...
    bcdedit /set testsigning on
    if !errorlevel! neq 0 (
        echo ERROR: Failed to enable test signing.
        echo If Secure Boot is enabled, you may need to disable it in BIOS first.
        pause
        exit /b 1
    )
    echo.
    echo *** IMPORTANT: A reboot is required for test signing to take effect. ***
    echo *** Please reboot and run this script again after reboot.            ***
    echo.
    set /p REBOOT="Reboot now? (Y/N): "
    if /i "!REBOOT!"=="Y" (
        shutdown /r /t 5 /c "Rebooting to enable test signing for DroidScreen driver"
        exit /b 0
    )
    echo Skipping reboot. Please reboot manually before using the driver.
    echo.
)

echo Test signing is enabled.
echo.

REM ============================================================================
REM Step 2: Create test certificate
REM ============================================================================

echo [Step 2/5] Creating self-signed test certificate...

REM Check if certificate already exists
certutil -store %CERT_STORE% %CERT_NAME% >nul 2>&1
if %errorlevel% equ 0 (
    echo Test certificate already exists, skipping creation.
) else (
    REM Create a self-signed certificate for driver signing
    makecert -r -pe -ss %CERT_STORE% -n "CN=%CERT_NAME%" -eku 1.3.6.1.5.5.7.3.3 "%CERT_NAME%.cer"
    if !errorlevel! neq 0 (
        echo WARNING: makecert failed. Trying PowerShell method...
        powershell -Command "New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=%CERT_NAME%' -CertStoreLocation 'Cert:\CurrentUser\My' -FriendlyName '%CERT_NAME%'"
        if !errorlevel! neq 0 (
            echo ERROR: Failed to create test certificate.
            pause
            exit /b 1
        )
    )
    echo Test certificate created.
)
echo.

REM ============================================================================
REM Step 3: Create catalog file
REM ============================================================================

echo [Step 3/5] Creating catalog file...

REM Copy INF and DLL to a staging directory
set STAGE_DIR=%~dp0stage
if exist "%STAGE_DIR%" rmdir /s /q "%STAGE_DIR%"
mkdir "%STAGE_DIR%"

copy /y "%BUILD_DIR%\DroidScreenDriver.dll" "%STAGE_DIR%\" >nul
copy /y "%INF_FILE%" "%STAGE_DIR%\" >nul

REM Create the catalog file from the INF
inf2cat /driver:"%STAGE_DIR%" /os:10_x64 /verbose
if %errorlevel% neq 0 (
    echo WARNING: inf2cat failed. The driver may still install without a catalog.
    echo If you see signing errors, ensure WDK tools are in your PATH.
    echo.
)
echo.

REM ============================================================================
REM Step 4: Sign the driver
REM ============================================================================

echo [Step 4/5] Signing the driver...

REM Sign the catalog file
if exist "%STAGE_DIR%\DroidScreenDriver.cat" (
    signtool sign /s %CERT_STORE% /n %CERT_NAME% /t http://timestamp.digicert.com "%STAGE_DIR%\DroidScreenDriver.cat"
    if !errorlevel! neq 0 (
        echo WARNING: Catalog signing failed. Trying without timestamp...
        signtool sign /s %CERT_STORE% /n %CERT_NAME% "%STAGE_DIR%\DroidScreenDriver.cat"
    )
)

REM Also sign the DLL directly
signtool sign /s %CERT_STORE% /n %CERT_NAME% /t http://timestamp.digicert.com "%STAGE_DIR%\DroidScreenDriver.dll"
if %errorlevel% neq 0 (
    echo WARNING: DLL signing failed. Trying without timestamp...
    signtool sign /s %CERT_STORE% /n %CERT_NAME% "%STAGE_DIR%\DroidScreenDriver.dll"
)
echo.

REM ============================================================================
REM Step 5: Install the driver
REM ============================================================================

echo [Step 5/5] Installing the driver...

REM Try devcon first (from WDK), then fall back to pnputil
where devcon >nul 2>&1
if %errorlevel% equ 0 (
    echo Using devcon to install...
    devcon install "%STAGE_DIR%\DroidScreenDriver.inf" Root\DroidScreenDriver
    if !errorlevel! neq 0 (
        echo devcon install failed, trying pnputil...
        goto :try_pnputil
    )
    goto :install_done
)

:try_pnputil
echo Using pnputil to install...

REM Add the driver to the driver store
pnputil /add-driver "%STAGE_DIR%\DroidScreenDriver.inf" /install
if %errorlevel% neq 0 (
    echo.
    echo WARNING: pnputil installation may have failed.
    echo.
    echo You can try manual installation:
    echo   1. Open Device Manager
    echo   2. Action ^> Add legacy hardware
    echo   3. Install manually from: %STAGE_DIR%
    echo   4. Select "Display adapters" category
    echo   5. Choose "DroidScreen Virtual Display"
    echo.
)

:install_done
echo.
echo ========================================
echo  Installation Complete
echo ========================================
echo.
echo The DroidScreen virtual display should now appear in:
echo   - Display Settings (Win+P or Settings ^> Display)
echo   - Device Manager under Display adapters
echo.
echo To UNINSTALL the driver:
echo   pnputil /delete-driver oem*.inf /uninstall /force
echo   (find the correct oem*.inf with: pnputil /enum-drivers)
echo.
echo To DISABLE test signing later:
echo   bcdedit /set testsigning off
echo.

pause
exit /b 0
