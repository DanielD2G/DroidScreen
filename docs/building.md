# Building DroidScreen

DroidScreen has three platform targets: a **macOS** desktop app, a **Windows** desktop app, and an **Android** tablet app. Each can be built independently.

---

## Prerequisites (all platforms)

| Tool | Version | Notes |
|------|---------|-------|
| Git | any | Clone the repo |
| CMake | 3.20+ | macOS & Windows desktop builds |
| ADB | any | For installing the Android APK and USB forwarding |

---

## macOS (arm64)

### Requirements

- macOS 12.3+ (Monterey or later)
- Xcode Command Line Tools (`xcode-select --install`)
- No external dependencies — all frameworks are system-provided

### Build

```bash
# From the repo root:
cmake -B build-macos -S . -DCMAKE_BUILD_TYPE=Release
cmake --build build-macos --parallel
```

The output is a `.app` bundle at:
```
build-macos/desktop/macos/droidscreen_desktop.app
```

### Install

```bash
cp -R build-macos/desktop/macos/droidscreen_desktop.app /Applications/DroidScreen.app
```

### Frameworks used (linked automatically)

AppKit, ScreenCaptureKit, VideoToolbox, CoreMedia, CoreVideo, CoreGraphics, Foundation

---

## Windows (x64)

### Requirements

- Windows 10 or later
- Visual Studio 2022 (with C++ desktop workload and C++/WinRT support)
- CMake 3.20+
- FFmpeg development libraries (avcodec, avutil, swscale)

### Getting FFmpeg

Download prebuilt FFmpeg shared libraries from [BtbN/FFmpeg-Builds](https://github.com/BtbN/FFmpeg-Builds/releases):

```powershell
# Download and extract (example with FFmpeg 7.1 GPL shared):
Invoke-WebRequest -Uri "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n7.1-latest-win64-gpl-shared-7.1.zip" -OutFile ffmpeg.zip
Expand-Archive ffmpeg.zip -DestinationPath ffmpeg
# The extracted folder contains include/ and lib/ directories.
```

### Build

```powershell
# From the repo root (adjust FFMPEG_ROOT to your extracted path):
cmake -B build -S . -G "Visual Studio 17 2022" -A x64 `
    -DFFMPEG_ROOT="C:\path\to\ffmpeg-n7.1-latest-win64-gpl-shared-7.1"

cmake --build build --config Release --parallel
```

The output executable is at:
```
build\desktop\windows\Release\droidscreen_desktop.exe
```

### Runtime dependencies

The following DLLs must be in the same directory as the `.exe` (or on PATH):
- `avcodec-*.dll`, `avutil-*.dll`, `swscale-*.dll`, `swresample-*.dll`

Copy them from the FFmpeg `bin/` directory.

### Installer (optional)

An NSIS installer script is provided at `installer/windows/DroidScreen.nsi`. To build it:

```powershell
# Install NSIS: https://nsis.sourceforge.io/
# Stage files into a staging/ directory, then:
makensis /DVERSION=1.0.0 /DSTAGING_DIR=staging installer\windows\DroidScreen.nsi
```

### Libraries linked

d3d11, dxgi, ws2_32, windowsapp (WinRT), shell32, comctl32, advapi32, setupapi, cfgmgr32

### Additional driver

DroidScreen uses the [Parsec Virtual Display Driver](https://github.com/nomi-san/parsec-vdd/releases) to create a virtual monitor on Windows. The installer will prompt to download it if not already installed.

---

## Android

### Requirements

- Java Development Kit 17 (OpenJDK 17 recommended)
- Android SDK (API 34)
- Android NDK (installed automatically by Gradle)
- Gradle (bundled via `gradlew` wrapper)

On macOS with Homebrew:
```bash
brew install openjdk@17
export JAVA_HOME=/opt/homebrew/opt/openjdk@17   # add to ~/.zshrc
```

On Linux:
```bash
sudo apt install openjdk-17-jdk
```

### Build

```bash
cd android
./gradlew assembleDebug
```

The output APK is at:
```
android/app/build/outputs/apk/debug/app-debug.apk
```

### Install on device

Connect the Android tablet via USB and enable USB debugging, then:

```bash
cd android
./gradlew installDebug
```

Or manually:
```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

### Build configuration

| Setting | Value |
|---------|-------|
| Min SDK | 26 (Android 8.0) |
| Target SDK | 34 (Android 14) |
| ABI targets | arm64-v8a, x86_64 |
| Native CMake | 3.22.1 |
| NDK libraries | mediandk (MediaCodec), android, log |

---

## CI / GitHub Actions

The repository includes a GitHub Actions workflow at `.github/workflows/build-release.yml` that:

1. Detects which platforms have changes since the last release tag
2. Builds only the affected platforms
3. Packages artifacts:
   - **macOS**: `DroidScreen-macOS-arm64-v{VERSION}.zip`
   - **Windows**: `DroidScreen-Windows-x64-v{VERSION}-Setup.exe` (NSIS installer)
   - **Android**: `DroidScreen-Android-v{VERSION}.apk`
4. Creates a GitHub Release with all platform artifacts

Builds are triggered on pushes to `main`.

---

## Project structure

```
DroidScreen/
  protocol/              # Shared C protocol library (handshake, framing, touch)
  desktop/
    common/              # Shared C++ desktop library (pipeline, server, rate controller)
    macos/               # macOS app (Objective-C++, ScreenCaptureKit, VideoToolbox)
    windows/             # Windows app (C++/WinRT, WGC, FFmpeg, Parsec VDD)
  android/               # Android app (Kotlin + JNI C++, MediaCodec)
  installer/windows/     # NSIS installer script
  .github/workflows/     # CI build pipeline
```
