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

    // 2. Open device handle.
    device_handle_ = parsec_vdd::OpenDeviceHandle(&parsec_vdd::VDD_ADAPTER_GUID);
    if (device_handle_ == nullptr || device_handle_ == INVALID_HANDLE_VALUE) {
        last_error_ = "Failed to open Parsec VDD device handle.";
        device_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    fprintf(stderr, "[vdd] Device handle opened: %p\n", device_handle_);

    // 3. Add a virtual display.
    display_index_ = parsec_vdd::VddAddDisplay(device_handle_);
    if (display_index_ < 0) {
        last_error_ = "VddAddDisplay failed (returned "
                      + std::to_string(display_index_) + ").";
        parsec_vdd::CloseDeviceHandle(device_handle_);
        device_handle_ = INVALID_HANDLE_VALUE;
        return false;
    }

    fprintf(stderr, "[vdd] Virtual display added, index=%d\n", display_index_);

    // 4. Start the keep-alive ping thread.
    ping_running_.store(true);
    ping_thread_ = std::thread(&VirtualDisplayWin::ping_loop, this);

    // 5. Wait for Windows to register the new display.
    //    The driver notifies the OS asynchronously; it typically takes
    //    1-2 seconds for the display to appear in the enumeration.
    fprintf(stderr, "[vdd] Waiting for display to appear...\n");
    std::this_thread::sleep_for(2000ms);

    // 6. Configure the display resolution and refresh rate.
    if (!configure_display(width, height, fps)) {
        // Non-fatal: the display exists but resolution may be default.
        fprintf(stderr, "[vdd] Warning: could not set resolution %ux%u@%u\n",
                width, height, fps);
    }

    // Give Windows a moment after the mode change.
    std::this_thread::sleep_for(500ms);

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

        // Get current display settings.
        DEVMODEW dm = {};
        dm.dmSize = sizeof(dm);
        if (!EnumDisplaySettingsW(dd.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
            // Try registry settings.
            EnumDisplaySettingsW(dd.DeviceName, ENUM_REGISTRY_SETTINGS, &dm);
        }

        // Set the desired resolution and refresh rate.
        dm.dmPelsWidth = width;
        dm.dmPelsHeight = height;
        dm.dmDisplayFrequency = fps;
        dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;

        LONG result = ChangeDisplaySettingsExW(
            dd.DeviceName, &dm, nullptr,
            CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);

        if (result != DISP_CHANGE_SUCCESSFUL) {
            fprintf(stderr, "[vdd] ChangeDisplaySettingsEx failed: %ld\n", result);
            // Try without specifying refresh rate.
            dm.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT;
            result = ChangeDisplaySettingsExW(
                dd.DeviceName, &dm, nullptr,
                CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
        }

        if (result == DISP_CHANGE_SUCCESSFUL) {
            // Apply the change globally.
            ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
            fprintf(stderr, "[vdd] Display resolution set to %ux%u@%u\n",
                    width, height, fps);
            return true;
        } else {
            fprintf(stderr, "[vdd] ChangeDisplaySettingsEx final attempt "
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

struct MonitorSearchCtx {
    HMONITOR result;
};

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

    // Get the desktop coordinates for this display device.
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(parsec_device_name.c_str(),
                              ENUM_CURRENT_SETTINGS, &dm)) {
        fprintf(stderr, "[vdd] Could not get display settings for Parsec device\n");
        return nullptr;
    }

    // Use MonitorFromPoint with a point inside this display's area.
    POINT pt;
    pt.x = dm.dmPosition.x + static_cast<LONG>(dm.dmPelsWidth / 2);
    pt.y = dm.dmPosition.y + static_cast<LONG>(dm.dmPelsHeight / 2);

    HMONITOR hmon = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if (hmon) {
        fprintf(stderr, "[vdd] Found HMONITOR=%p at (%ld, %ld)\n",
                hmon, pt.x, pt.y);
    } else {
        fprintf(stderr, "[vdd] MonitorFromPoint returned null for (%ld, %ld)\n",
                pt.x, pt.y);
    }

    return hmon;
}

} // namespace droidscreen
