/*
 * DroidScreen Windows - Parsec VDD virtual display wrapper
 *
 * Uses the Parsec Virtual Display Driver (parsec-vdd) to create and
 * manage a virtual display at runtime. This replaces the custom IddCx
 * driver with a signed, open-source driver that requires no test signing.
 *
 * The Parsec VDD driver must be installed separately. At startup,
 * this class checks for driver availability and reports an actionable
 * error if the driver is missing.
 *
 * Key behavior:
 *   - create() opens the VDD device, adds a virtual display, starts a
 *     keep-alive ping thread, sets the resolution, and locates the
 *     HMONITOR for WGC capture targeting.
 *   - destroy() tears everything down in reverse order.
 *   - The ping thread calls VddUpdate every 90 ms (must be <100 ms).
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <atomic>
#include <thread>
#include <string>

namespace droidscreen {

class VirtualDisplayWin {
public:
    VirtualDisplayWin();
    ~VirtualDisplayWin();

    /// Check if the Parsec VDD driver is installed and ready.
    bool is_driver_installed();

    /// Create a virtual display with the given resolution and refresh rate.
    /// Returns true on success. After success, monitor_handle() is valid.
    bool create(uint32_t width, uint32_t height, uint32_t fps);

    /// Destroy the virtual display and release all resources.
    void destroy();

    /// Get the HMONITOR of the created virtual display (for WGC capture).
    HMONITOR monitor_handle() const { return monitor_; }

    /// Whether a virtual display is currently active.
    bool is_active() const { return active_; }

    /// Width of the virtual display.
    uint32_t width() const { return width_; }

    /// Height of the virtual display.
    uint32_t height() const { return height_; }

    /// Human-readable status message (for logging / user feedback).
    const std::string& last_error() const { return last_error_; }

private:
    HANDLE device_handle_ = INVALID_HANDLE_VALUE;
    int display_index_ = -1;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint32_t fps_ = 0;
    bool active_ = false;
    HMONITOR monitor_ = nullptr;
    std::string last_error_;

    // Keep-alive thread — calls VddUpdate every 90 ms.
    std::thread ping_thread_;
    std::atomic<bool> ping_running_{false};

    void ping_loop();

    /// After VddAddDisplay, enumerate displays to find the Parsec VDD
    /// monitor and set its resolution via ChangeDisplaySettingsEx.
    bool configure_display(uint32_t width, uint32_t height, uint32_t fps);

    /// Find the HMONITOR corresponding to the Parsec virtual display.
    HMONITOR find_parsec_monitor();
};

} // namespace droidscreen
