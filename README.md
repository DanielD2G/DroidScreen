# DroidScreen

Turn an Android tablet into a wired USB secondary display for macOS and Windows.

**Performance target:** <80ms end-to-end latency, 60fps, 1080p.

## Architecture

```
┌─────────────────┐    USB (adb reverse)    ┌──────────────────┐
│  Desktop App    │ ──── H.264 stream ────> │  Android App     │
│                 │ <─── touch events ───── │                  │
│  Screen Capture │    TCP + TCP_NODELAY    │  HW Decode       │
│  HW Encode      │                         │  SurfaceView     │
└─────────────────┘                         └──────────────────┘
```

**Pipeline (zero-copy GPU):**
- **macOS:** ScreenCaptureKit → VideoToolbox H.264 → USB → MediaCodec → SurfaceView
- **Windows:** WGC → HLSL BGRA→NV12 → NVENC → USB → MediaCodec → SurfaceView

## Quick Start

### Prerequisites

| Component | macOS | Windows |
|-----------|-------|---------|
| **Build tools** | Xcode + CMake | Visual Studio 2022 + CMake |
| **Android tools** | `brew install android-platform-tools` | [Platform Tools](https://developer.android.com/tools/releases/platform-tools) |
| **Android app** | Android Studio or SDK+NDK+JDK 17 | Same |

### 1. Build Desktop App

**macOS:**
```bash
./scripts/build_macos.sh
# Binary: build-macos/desktop/macos/droidscreen_desktop
```

**Windows (Developer Command Prompt):**
```cmd
scripts\build_windows.bat
REM Binary: build-windows\desktop\windows\Release\droidscreen_desktop.exe
```

### 2. Build Android App

Open `android/` in Android Studio → Build → Run on device.

Or from command line:
```bash
cd android
# Create local.properties with sdk.dir=/path/to/android/sdk
./gradlew assembleDebug
adb install -r app/build/outputs/apk/debug/app-debug.apk
```

### 3. Run

```bash
# One-command launch (macOS):
./scripts/run.sh

# Or manually:
adb reverse tcp:38271 tcp:38271     # Setup USB tunnel
# Start DroidScreen app on tablet
./build-macos/desktop/macos/droidscreen_desktop --port 38271 --fps 60 --bitrate 15000
```

**Options:**
```
--port PORT       TCP port (default: 38271)
--fps FPS         Target frame rate (default: 60)
--bitrate KBPS    H.264 bitrate in kbps (default: 15000)
--display INDEX   Which display to capture (default: 0)
```

## Step-by-Step First Run

1. **Enable USB Debugging** on your Android tablet (Settings → Developer Options)
2. **Connect** tablet to computer via USB cable
3. **Accept** the USB debugging prompt on the tablet
4. **Install** the Android app: `adb install app-debug.apk`
5. **Open** DroidScreen on the tablet — it shows "Waiting for connection..."
6. **Run** `adb reverse tcp:38271 tcp:38271`
7. **Run** the desktop app — it connects and starts streaming

## Project Structure

```
DroidScreen/
├── protocol/          # C wire protocol library (shared)
├── desktop/
│   ├── common/        # Cross-platform pipeline, TCP, rate control
│   ├── macos/         # ScreenCaptureKit + VideoToolbox
│   ├── windows/       # WGC + NVENC + HLSL shaders
│   └── shaders/       # BGRA→NV12 pixel shaders
├── android/           # Kotlin + NDK (MediaCodec decoder)
├── driver/            # IddCx virtual display driver (Phase 3)
└── scripts/           # Build and run helpers
```

## Technical Details

| Layer | macOS | Windows | Android |
|-------|-------|---------|---------|
| **Capture** | ScreenCaptureKit | Windows.Graphics.Capture | — |
| **Color** | — (BGRA→VT) | HLSL shader BGRA→NV12 | — |
| **Encode** | VideoToolbox H.264 | NVENC direct / FFmpeg fallback | — |
| **Transport** | TCP + TCP_NODELAY via `adb reverse` | Same | TCP server |
| **Decode** | — | — | AMediaCodec (HW) |
| **Render** | — | — | SurfaceView (zero-copy) |
| **Touch** | — (not supported) | SyntheticPointerInput | MotionEvent capture |

## Troubleshooting

- **"No device found":** Check USB cable, enable USB debugging, run `adb devices`
- **Black screen on tablet:** Grant screen recording permission (macOS), check firewall
- **High latency:** Reduce bitrate (`--bitrate 8000`), try USB 3.0 port
- **NVENC not found (Windows):** Falls back to software encoder. Install latest NVIDIA drivers for HW encoding
- **macOS permission:** System Settings → Privacy → Screen Recording → allow DroidScreen
