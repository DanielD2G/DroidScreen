# DroidScreen — Build Guide

## Prerequisites

| Tool | Version | Install |
|------|---------|---------|
| Xcode Command Line Tools | 15+ | `xcode-select --install` |
| CMake | 3.20+ | `brew install cmake` |
| Java JDK | 17 | `brew install openjdk@17` |
| Android SDK | 34 | Via Android Studio or `sdkmanager` |
| Android NDK | 26.x | `sdkmanager "ndk;26.1.10909125"` |
| ADB | latest | `brew install android-platform-tools` |

## Quick Start

```bash
# Build everything and run
./scripts/run.sh
```

This builds both desktop and Android (if needed), sets up the USB tunnel, and launches.

---

## Android App

### Build

```bash
# Option A: Use the build script
./scripts/build_android.sh

# Option B: Manual (if JAVA_HOME/ANDROID_HOME need explicit paths)
export JAVA_HOME="/opt/homebrew/opt/openjdk@17"
export ANDROID_HOME="$HOME/Library/Android/sdk"
cd android && ./gradlew assembleDebug
```

The APK is at: `android/app/build/outputs/apk/debug/app-debug.apk`

### Install on device

```bash
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
```

### Launch

```bash
adb shell am start -n com.droidscreen.app/.MainActivity
```

---

## macOS Desktop App

### Build

```bash
./scripts/build_macos.sh
```

By default this runs CMake + build + **ad-hoc code signing** automatically. The signed `.app` bundle is at:

```
build-macos/desktop/macos/droidscreen_desktop.app
```

If you want macOS permissions such as **Screen Recording** to survive rebuilds,
build with a real signing identity:

```bash
export DROIDSCREEN_CODESIGN_IDENTITY="Your Code Signing Identity"
./scripts/build_macos.sh
```

For this machine, the recommended persistent setup is to store that identity in
an untracked local config file that all agents can reuse:

```bash
mkdir -p ~/.config/droidscreen
cat > ~/.config/droidscreen/build.env <<'EOF'
export DROIDSCREEN_CODESIGN_IDENTITY="Your Code Signing Identity"
EOF
chmod 600 ~/.config/droidscreen/build.env
```

`build_macos.sh` loads `~/.config/droidscreen/build.env` automatically.

Ad-hoc signing (`codesign -s -`) is convenient for local builds, but it does
**not** provide a stable TCC identity across content changes.

### Install to /Applications

```bash
# IMPORTANT: Remove old app first, then copy — plain cp -R may not overwrite the binary
rm -rf /Applications/DroidScreen.app
cp -R build-macos/desktop/macos/droidscreen_desktop.app /Applications/DroidScreen.app
```

### macOS Permissions (Screen Recording)

The first time you run the app, macOS will ask for **Screen Recording** permission.

**To avoid re-granting permissions after every build:**

1. Build the app with a **real signing identity**.
   Ad-hoc signing is not enough to preserve Screen Recording permission across rebuilds.
   Set `DROIDSCREEN_CODESIGN_IDENTITY` before running `./scripts/build_macos.sh`.

2. **Always use `rm -rf` + `cp -R`** to install to `/Applications/` — never `cp -R`
   alone, because it may partially overwrite the bundle and confuse macOS's
   permission cache.

3. If macOS still asks for permissions after a rebuild:
   - Open **System Settings → Privacy & Security → Screen Recording**
   - Toggle DroidScreen OFF then ON again
   - Restart the app

4. The entitlements file is at `desktop/macos/DroidScreen.entitlements`.

If you do not have a signing identity yet, create or use one in Keychain Access
and then export its exact name into `DROIDSCREEN_CODESIGN_IDENTITY`.

### Launch

```bash
open /Applications/DroidScreen.app
# — or —
open build-macos/desktop/macos/droidscreen_desktop.app
```

---

## Full Workflow (build both + run)

```bash
# 1. Build Android
export JAVA_HOME="/opt/homebrew/opt/openjdk@17"
export ANDROID_HOME="$HOME/Library/Android/sdk"
cd android && ./gradlew assembleDebug && cd ..

# 2. Build macOS
./scripts/build_macos.sh

# 3. Install
adb install -r android/app/build/outputs/apk/debug/app-debug.apk
rm -rf /Applications/DroidScreen.app
cp -R build-macos/desktop/macos/droidscreen_desktop.app /Applications/DroidScreen.app

# 4. Launch Android app
adb shell am force-stop com.droidscreen.app
adb shell am start -n com.droidscreen.app/.MainActivity

# 5. Launch desktop app
open /Applications/DroidScreen.app
```

The desktop app auto-detects the USB device and connects. No manual `adb reverse` needed.

---

## Windows Desktop App

### Build

```bash
scripts\build_windows.bat
```

### Permissions

Windows may show a **SmartScreen** warning on first run. Click "More info → Run anyway".
No recurring permission prompts — the app is not signed so SmartScreen only warns once.

---

## Troubleshooting

| Problem | Fix |
|---------|-----|
| `bind() failed: Address already in use` | `adb reverse --remove-all && adb forward --remove-all`, then restart Android app |
| Screen Recording denied after rebuild | Rebuild with a real signing identity via `DROIDSCREEN_CODESIGN_IDENTITY`, then reinstall with `rm -rf` + `cp -R` |
| `JAVA_HOME not set` | `export JAVA_HOME="/opt/homebrew/opt/openjdk@17"` |
| `ANDROID_HOME not set` | `export ANDROID_HOME="$HOME/Library/Android/sdk"` |
| Desktop connects but no video | Grant Screen Recording permission in System Settings |
| `CMake Error: MediaRemote` | The private framework link needs macOS 12.3+. Check `desktop/macos/CMakeLists.txt` |
