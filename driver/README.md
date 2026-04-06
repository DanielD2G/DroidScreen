# DroidScreen Virtual Display Driver

IddCx (Indirect Display Driver) for Windows that creates a virtual display for DroidScreen. The desktop application captures this display via Windows.Graphics.Capture (WGC) and streams it to an Android tablet over USB.

## Architecture

The driver is a UMDF v2 (User-Mode Driver Framework) DLL that uses the IddCx (Indirect Display CX) class extension. It does **not** run in kernel mode.

```
Windows Compositor
       |
       v
IddCx Framework  <-->  DroidScreenDriver.dll (this driver)
       |
       v
Virtual Display  <-->  DroidScreen Desktop App (captures via WGC)
       |
       v
USB stream to Android tablet
```

The driver's job is simply to make a virtual display **exist** in Windows. The actual frame capture and streaming is handled by the desktop application.

### Source Files

| File | Purpose |
|------|---------|
| `Driver.h` | Common includes, trace macros, WDF context structures |
| `Driver.cpp` | WDF entry point, IddCx adapter/monitor callbacks |
| `IndirectMonitor.h/cpp` | Display modes, EDID generation, monitor lifecycle |
| `SwapChainProcessor.h/cpp` | Drains compositor frames to keep the pipeline moving |
| `DroidScreenDriver.inf` | Driver installation information |
| `DroidScreenDriver.def` | DLL export definitions |
| `DroidScreenDriver.vcxproj` | MSBuild project (WDK) |

## Prerequisites

- **Windows 10 version 2004** or later (IddCx 1.4+)
- **Visual Studio 2022** with "Desktop development with C++" workload
- **Windows 11 SDK** (10.0.22621.0 or later)
- **Windows Driver Kit (WDK)** matching the SDK version
- **WDK Visual Studio Extension** (installed with WDK)

### Installing the WDK

1. Download from: https://learn.microsoft.com/en-us/windows-hardware/drivers/download-the-wdk
2. Install the Windows SDK first, then the WDK
3. The WDK installer will offer to install the VS extension — accept it

## Building

### Visual Studio (recommended)

1. Open `DroidScreenDriver/DroidScreenDriver.vcxproj` in Visual Studio 2022
2. Select **x64** platform and **Release** (or Debug) configuration
3. Build > Build Solution (Ctrl+Shift+B)

### Command Line

From a **Developer Command Prompt for VS 2022**:

```cmd
cd DroidScreenDriver
msbuild DroidScreenDriver.vcxproj /p:Configuration=Release /p:Platform=x64
```

Output will be in `DroidScreenDriver/x64/Release/`.

## Installation

### Quick Install

Run `install.bat` as Administrator. It will:

1. Enable test signing mode (reboot required on first run)
2. Create a self-signed test certificate
3. Sign the driver with the test certificate
4. Install the driver via devcon/pnputil

### Manual Install

1. **Enable test signing** (run as Administrator):
   ```cmd
   bcdedit /set testsigning on
   ```
   Reboot after this.

2. **Create and install a test certificate**:
   ```cmd
   makecert -r -pe -ss PrivateCertStore -n "CN=DroidScreenTestCert" DroidScreenTestCert.cer
   ```

3. **Sign the driver**:
   ```cmd
   signtool sign /s PrivateCertStore /n DroidScreenTestCert DroidScreenDriver.dll
   ```

4. **Install with devcon** (from WDK tools):
   ```cmd
   devcon install DroidScreenDriver.inf Root\DroidScreenDriver
   ```

   Or with **pnputil**:
   ```cmd
   pnputil /add-driver DroidScreenDriver.inf /install
   ```

5. The virtual display should appear in Display Settings.

### Uninstall

Run `uninstall.bat` as Administrator, or manually:

```cmd
REM Find the driver
pnputil /enum-drivers | findstr DroidScreen

REM Remove it (replace oemXX.inf with the actual filename)
pnputil /delete-driver oemXX.inf /uninstall /force

REM Optionally disable test signing
bcdedit /set testsigning off
```

## Supported Display Modes

| Resolution | Aspect Ratio | Use Case |
|-----------|-------------|----------|
| 2560x1600 | 16:10 | Tablet native (preferred) |
| 1920x1200 | 16:10 | Reduced resolution |
| 1280x800 | 16:10 | Low bandwidth |
| 1920x1080 | 16:9 | Standard Full HD |
| 3840x2160 | 16:9 | 4K option |

The default/preferred mode is 2560x1600@60Hz, matching common Android tablet resolutions.

## Troubleshooting

### Driver fails to install
- Ensure test signing is enabled and you have rebooted
- Check that the driver is properly signed (even with a test cert)
- Look in Event Viewer > Windows Logs > System for driver errors

### Virtual display does not appear
- Open Device Manager and check for the "DroidScreen Virtual Display" under Display adapters
- If it shows a yellow warning icon, right-click > Properties to see the error
- Ensure your Windows version is 2004 or later (run `winver`)

### Test signing watermark on desktop
This is normal when test signing is enabled. It appears in the bottom-right corner. To remove it, you would need a production (EV) code signing certificate.

## Development Notes

### IddCx Flow

```
DriverEntry
  └─> WdfDriverCreate
        └─> EvtDeviceAdd
              └─> IddCxAdapterInitAsync
                    └─> EvtAdapterInitFinished
                          └─> IddCxMonitorCreate
                                └─> IddCxMonitorArrival
                                      └─> EvtMonitorAssignSwapChain
                                            └─> SwapChainProcessor (thread)
```

### Adding New Resolutions

Edit `s_SupportedModes` in `IndirectMonitor.cpp`. Each entry is `{ Width, Height, RefreshNumerator, RefreshDenominator }`.

### Debug Logging

In Debug builds, the driver outputs trace messages via `DbgPrintEx`. View them with:
- **DebugView** (Sysinternals) — enable "Capture Kernel" output
- **WinDbg** attached to the system
