/*
 * DroidScreen Windows - Parsec VDD virtual display implementation
 *
 * Creates a virtual display using the Parsec VDD driver's DeviceIoControl
 * API. The driver is a signed IddCx driver (no test signing required).
 *
 * Flow:
 *   1. QueryDeviceStatus  — verify driver is installed
 *   2. OpenDeviceHandle   — get a HANDLE to the VDD device
 *   3. VddAddDisplay      — hotplug a virtual monitor
 *   4. Start ping thread  — VddUpdate every 90 ms to keep display alive
 *   5. Wait for Windows to register the new display (~1.5 s)
 *   6. ChangeDisplaySettingsEx — set resolution and refresh rate
 *   7. EnumDisplayDevices / EnumDisplayMonitors — locate HMONITOR
 */

#include "virtual_display_win.h"
#include "parsec-vdd.h"

#include <cstdio>
#include <chrono>
#include <cstring>

using namespace std::chrono_literals;

namespace droidscreen {

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

VirtualDisplayWin::VirtualDisplayWin() = default;

VirtualDisplayWin::~VirtualDisplayWin() {
    destroy();
}

// ---------------------------------------------------------------------------
// Driver status check
// ---------------------------------------------------------------------------

bool VirtualDisplayWin::is_driver_installed() {
    parsec_vdd::DeviceStatus status =
        parsec_vdd::QueryDeviceStatus(
            &parsec_vdd::VDD_CLASS_GUID,
            parsec_vdd::VDD_HARDWARE_ID);

    switch (status) {
    case parsec_vdd::DEVICE_OK:
        return true;
    case parsec_vdd::DEVICE_NOT_INSTALLED:
        last_error_ = "Parsec VDD driver is not installed. "
                      "Download from: https://github.com/nomi-san/parsec-vdd/releases";
        return false;
    case parsec_vdd::DEVICE_DISABLED:
        last_error_ = "Parsec VDD driver is disabled in Device Manager.";
        return false;
    case parsec_vdd::DEVICE_RESTART_REQUIRED:
        last_error_ = "Parsec VDD driver requires a PC restart to function.";
        return false;
    case parsec_vdd::DEVICE_DISABLED_SERVICE:
        last_error_ = "Parsec VDD driver service is disabled.";
        return false;
    case parsec_vdd::DEVICE_DRIVER_ERROR:
        last_error_ = "Parsec VDD driver encountered an error.";
        return false;
    default:
        last_error_ = "Parsec VDD driver status unknown (code "
                      + std::to_string(static_cast<int>(status)) + ").";
        return false;
    }
}

// ---------------------------------------------------------------------------
// Create virtual display
// ---------------------------------------------------------------------------

bool VirtualDisplayWin::create(uint32_t width, uint32_t height, uint32_t fps) {
    if (active_) {
        last_error_ = "Virtual display already active.";
        return false;
    }

    // 1. Check driver status.
    if (!is_driver_installed()) {
        return false; // last_error_ already set
    }

    // 2. Write the desired resolution to the Parsec VDD registry.
    //    The driver reads custom modes from HKLM\SOFTWARE\Parsec\vdd\{0-4}
    //    at display plug time. Writing BEFORE VddAddDisplay guarantees
    //    the mode is available even if it's not a default preset.
    {
        HKEY key = nullptr;
        LONG rc = RegCreateKeyExA(HKEY_LOCAL_MACHINE,
                                   "SOFTWARE\\Parsec\\vdd\\0",
                                   0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr);
        if (rc == ERROR_SUCCESS) {
            DWORD w = width, h = height, hz = fps;
            RegSetValueExA(key, "width",  0, REG_DWORD, (BYTE*)&w,  sizeof(DWORD));
            RegSetValueExA(key, "height", 0, REG_DWORD, (BYTE*)&h,  sizeof(DWORD));
            RegSetValueExA(key, "hz",     0, REG_DWORD, (BYTE*)&hz, sizeof(DWORD));
            RegCloseKey(key);
            fprintf(stderr, "[vdd] Wrote custom mode %ux%u@%u to registry\n",
                    width, height, fps);
        } else {
            // Non-fatal: mode may already be a default preset.
            fprintf(stderr, "[vdd] Could not write registry (rc=%ld, "
                    "may need admin) — relying on default presets\n", rc);
        }
    }

    // 3. Open device handle.
    device_handle_ = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device_handle_ == nullptr || device_handle_ == INVALID_HANDLE_VALUE) {
        last_error_ = "Failed to open Parsec VDD device handle.";
        device_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    fprintf(stderr, "[vdd] Device handle opened: %p\n", device_handle_);

    // 4. Add a virtual display.
    display_index_ = parsec_vdd::VddAddDisplay(device_handle_);
    if (display_index_ < 0) {
        last_error_ = "VddAddDisplay failed (returned "
                      + std::to_string(display_index_) + ").";
        parsec_vdd::CloseDeviceHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    fprintf(stderr, "[vdd] Virtual display added, index=%d\n", display_index_);

    // 5. Start the keep-alive ping thread.
    ping_running_.store(true);
    ping_thread_ = std::thread(&VirtualDisplayWin::ping_loop, this);

    // 6. Poll until the display is registered and the requested mode
    //    appears in the enumeration. The driver populates modes
    //    asynchronously after VddAddDisplay.
    fprintf(stderr, "[vdd] Waiting for display modes to populate...\n");
    {
        std::wstring parsec_dev;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        bool mode_found = false;

        while (std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(300));

            // Find the Parsec device name.
            DISPLAY_DEVICEW dd = {};
            dd.cb = sizeof(dd);
            parsec_dev.clear();
            for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
                if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;
                std::wstring devStr(dd.DeviceString);
                if (devStr.find(L"Parsec") != std::wstring::npos) {
                    parsec_dev = dd.DeviceName;
                    break;
                }
                // Check child device.
                DISPLAY_DEVICEW child = {};
                child.cb = sizeof(child);
                if (EnumDisplayDevicesW(dd.DeviceName, 0, &child, 0)) {
                    std::wstring childStr(child.DeviceString);
                    if (childStr.find(L"Parsec") != std::wstring::npos) {
                        parsec_dev = dd.DeviceName;
                        break;
                    }
                }
            }

            if (parsec_dev.empty()) continue;

            // Check if the requested mode is in the enumeration.
            DEVMODEW enumDm = {};
            enumDm.dmSize = sizeof(enumDm);
            for (DWORD m = 0; EnumDisplaySettingsW(parsec_dev.c_str(), m, &enumDm); m++) {
                if (enumDm.dmPelsWidth == width && enumDm.dmPelsHeight == height) {
                    mode_found = true;
                    break;
                }
            }

            if (mode_found) {
                fprintf(stderr, "[vdd] Mode %ux%u found in display enumeration\n",
                        width, height);
                break;
            }
        }

        if (!mode_found) {
            fprintf(stderr, "[vdd] Warning: mode %ux%u not found after polling "
                    "(will try ChangeDisplaySettings anyway)\n", width, height);
        }
    }

    // 7. Configure the display resolution and refresh rate.
    if (!configure_display(width, height, fps)) {
        // Non-fatal: the display exists but resolution may be default.
        fprintf(stderr, "[vdd] Warning: could not set resolution %ux%u@%u\n",
                width, height, fps);
    }

    // Give Windows a moment after the mode change.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // 7. Find the HMONITOR.
    monitor_ = find_parsec_monitor();
    if (!monitor_) {
        last_error_ = "Virtual display was created but HMONITOR could not "
                      "be located. The display may not be recognized yet.";
        // Clean up.
        destroy();
        return false;
    }

    width_ = width;
    height_ = height;
    fps_ = fps;
    active_ = true;

    fprintf(stderr, "[vdd] Virtual display ready: %ux%u@%u, HMONITOR=%p\n",
            width, height, fps, monitor_);
    return true;
}

// ---------------------------------------------------------------------------
// Destroy virtual display
// ---------------------------------------------------------------------------

void VirtualDisplayWin::destroy() {
    if (!active_ && device_handle_ == INVALID_HANDLE_VALUE) return;

    fprintf(stderr, "[vdd] Destroying virtual display...\n");

    // Stop the ping thread first.
    if (ping_running_.exchange(false)) {
        if (ping_thread_.joinable()) {
            ping_thread_.join();
        }
    }

    // Remove the display.
    if (display_index_ >= 0 && device_handle_ != INVALID_HANDLE_VALUE) {
        parsec_vdd::VddRemoveDisplay(device_handle_, display_index_);
        fprintf(stderr, "[vdd] Display index %d removed\n", display_index_);
    }

    // Close the device handle.
    if (device_handle_ != INVALID_HANDLE_VALUE) {
        parsec_vdd::CloseDeviceHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
    }

    display_index_ = -1;
    width_ = 0;
    height_ = 0;
    fps_ = 0;
    active_ = false;
    monitor_ = nullptr;

    fprintf(stderr, "[vdd] Virtual display destroyed\n");
}

// ---------------------------------------------------------------------------
// Keep-alive ping loop
// ---------------------------------------------------------------------------

void VirtualDisplayWin::ping_loop() {
    fprintf(stderr, "[vdd] Ping thread started\n");
    while (ping_running_.load()) {
        parsec_vdd::VddUpdate(device_handle_);
        std::this_thread::sleep_for(90ms);
    }
    fprintf(stderr, "[vdd] Ping thread stopped\n");
}

// ---------------------------------------------------------------------------
// Configure display resolution via ChangeDisplaySettingsEx
// ---------------------------------------------------------------------------

bool VirtualDisplayWin::configure_display(uint32_t width, uint32_t height,
                                           uint32_t fps) {
    // Enumerate display devices to find the Parsec VDD adapter.
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);

    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        // Skip inactive devices.
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;

        // Check if this is a Parsec virtual display.
        // The device string typically contains "ParsecVDA" or "Parsec".
        bool is_parsec = false;
        {
            // Check the adapter device string.
            std::wstring devStr(dd.DeviceString);
            if (devStr.find(L"Parsec") != std::wstring::npos) {
                is_parsec = true;
            }
        }

        if (!is_parsec) {
            // Also check child display device.
            DISPLAY_DEVICEW child = {};
            child.cb = sizeof(child);
            if (EnumDisplayDevicesW(dd.DeviceName, 0, &child, 0)) {
                std::wstring childStr(child.DeviceString);
                if (childStr.find(L"Parsec") != std::wstring::npos) {
                    is_parsec = true;
                }
            }
        }

        if (!is_parsec) continue;

        fprintf(stderr, "[vdd] Found Parsec display device: %ls\n", dd.DeviceName);

        // Enumerate all available modes for this display.
        fprintf(stderr, "[vdd] Available modes:\n");
        DEVMODEW enumDm = {};
        enumDm.dmSize = sizeof(enumDm);
        uint32_t best_w = 0, best_h = 0, best_hz = 0;
        uint64_t best_diff = UINT64_MAX;

        for (DWORD m = 0; EnumDisplaySettingsW(dd.DeviceName, m, &enumDm); m++) {
            // Only log unique resolutions (skip duplicate refresh rates).
            if (m < 30 || (enumDm.dmPelsWidth == width && enumDm.dmPelsHeight == height)) {
                fprintf(stderr, "[vdd]   %lux%lu@%lu\n",
                        enumDm.dmPelsWidth, enumDm.dmPelsHeight,
                        enumDm.dmDisplayFrequency);
            }

            // Find the closest matching mode.
            uint64_t diff = (uint64_t)abs((int)enumDm.dmPelsWidth - (int)width) * 1000
                          + (uint64_t)abs((int)enumDm.dmPelsHeight - (int)height) * 1000
                          + (uint64_t)abs((int)enumDm.dmDisplayFrequency - (int)fps);
            if (diff < best_diff) {
                best_diff = diff;
                best_w = enumDm.dmPelsWidth;
                best_h = enumDm.dmPelsHeight;
                best_hz = enumDm.dmDisplayFrequency;
            }
        }

        // Try the exact requested resolution first.
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        dm.dmPelsWidth = width;
        dm.dmPelsHeight = height;
        dm.dmDisplayFrequency = fps;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

        LONG result = ChangeDisplaySettingsExW(
            dd.DeviceName, &dm, nullptr,
            CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);

        if (result != DISP_CHANGE_SUCCESSFUL) {
            fprintf(stderr, "[vdd] Exact resolution %ux%u@%u not supported "
                    "(error %ld)\n", width, height, fps, result);

            // Try without refresh rate.
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
            result = ChangeDisplaySettingsExW(
                dd.DeviceName, &dm, nullptr,
                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        }

        if (result != DISP_CHANGE_SUCCESSFUL && best_w > 0) {
            // Fall back to the closest available mode.
            fprintf(stderr, "[vdd] Falling back to closest mode: %ux%u@%u\n",
                    best_w, best_h, best_hz);
            dm.dmPelsWidth = best_w;
            dm.dmPelsHeight = best_h;
            dm.dmDisplayFrequency = best_hz;
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
            result = ChangeDisplaySettingsExW(
                dd.DeviceName, &dm, nullptr,
                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        }

        if (result == DISP_CHANGE_SUCCESSFUL) {
            // Apply the change globally.
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            fprintf(stderr, "[vdd] Display resolution set to %lux%lu@%lu\n",
                    dm.dmPelsWidth, dm.dmPelsHeight, dm.dmDisplayFrequency);
            return true;
        } else {
            fprintf(stderr, "[vdd] ChangeDisplaySettingsEx all attempts "
                    "failed: %ld\n", result);
            return false;
        }
    }

    fprintf(stderr, "[vdd] Could not find Parsec display device for "
            "resolution configuration\n");
    return false;
}

// ---------------------------------------------------------------------------
// Find the HMONITOR for the Parsec virtual display
// ---------------------------------------------------------------------------

struct MonitorMatchCtx {
    std::wstring target_device;
    HMONITOR     result;
};

static BOOL CALLBACK monitor_enum_callback(HMONITOR hmon, HDC /*hdc*/,
                                            LPRECT /*rc*/, LPARAM lparam) {
    auto* ctx = reinterpret_cast<MonitorMatchCtx*>(lparam);

    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (GetMonitorInfoW(hmon, &mi)) {
        if (ctx->target_device == mi.szDevice) {
            ctx->result = hmon;
            return FALSE; // Stop enumeration.
        }
    }
    return TRUE; // Continue.
}

HMONITOR VirtualDisplayWin::find_parsec_monitor() {
    // Enumerate display devices to find the Parsec VDD device name.
    DISPLAY_DEVICEW dd = {};
    dd.cb = sizeof(dd);
    std::wstring parsec_device_name;

    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++) {
        if (!(dd.StateFlags & DISPLAY_DEVICE_ACTIVE)) continue;

        bool is_parsec = false;
        {
            std::wstring devStr(dd.DeviceString);
            if (devStr.find(L"Parsec") != std::wstring::npos) {
                is_parsec = true;
            }
        }

        if (!is_parsec) {
            DISPLAY_DEVICEW child = {};
            child.cb = sizeof(child);
            if (EnumDisplayDevicesW(dd.DeviceName, 0, &child, 0)) {
                std::wstring childStr(child.DeviceString);
                if (childStr.find(L"Parsec") != std::wstring::npos) {
                    is_parsec = true;
                }
            }
        }

        if (is_parsec) {
            parsec_device_name = dd.DeviceName;
            fprintf(stderr, "[vdd] Parsec monitor device name: %ls\n",
                    parsec_device_name.c_str());
            break;
        }
    }

    if (parsec_device_name.empty()) {
        fprintf(stderr, "[vdd] Could not find Parsec display in device list\n");
        return nullptr;
    }

    // Method 1: EnumDisplayMonitors — matches by device name (most reliable).
    MonitorMatchCtx ctx;
    ctx.target_device = parsec_device_name;
    ctx.result = nullptr;

    EnumDisplayMonitors(nullptr, nullptr, monitor_enum_callback,
                        reinterpret_cast<LPARAM>(&ctx));

    if (ctx.result) {
        fprintf(stderr, "[vdd] Found HMONITOR=%p via EnumDisplayMonitors\n",
                ctx.result);
        return ctx.result;
    }

    // Method 2: Fallback — use MonitorFromPoint with display coordinates.
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (EnumDisplaySettingsW(parsec_device_name.c_str(),
                              ENUM_CURRENT_SETTINGS, &dm)) {
        POINT pt;
        pt.x = dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth / 2);
        pt.y = dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight / 2);

        HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
        if (hmon) {
            fprintf(stderr, "[vdd] Found HMONITOR=%p via MonitorFromPoint "
                    "at (%ld, %ld)\n", hmon, pt.x, pt.y);
            return hmon;
        }
        fprintf(stderr, "[vdd] MonitorFromPoint returned null for (%ld, %ld)\n",
                pt.x, pt.y);
    }

    // Method 3: Last resort — try all monitors and pick the last one
    // (the virtual display is typically the most recently added).
    HMONITOR last_monitor = nullptr;
    EnumDisplayMonitors(nullptr, nullptr,
        [](HMONITOR hmon, HDC, LPRECT, LPARAM lp) -> BOOL {
            *reinterpret_cast<HMONITOR*>(lp) = hmon;
            return TRUE; // Continue — we want the last one.
        }, reinterpret_cast<LPARAM>(&last_monitor));

    if (last_monitor) {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        GetMonitorInfoW(last_monitor, &mi);
        fprintf(stderr, "[vdd] Using last monitor as fallback: %ls, "
                "HMONITOR=%p\n", mi.szDevice, last_monitor);
    }

    return last_monitor;
}

} // namespace droidscreen
