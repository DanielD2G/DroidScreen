/*
 * DroidScreen macOS - CGVirtualDisplay wrapper
 *
 * Uses Apple's private CGVirtualDisplay API to create a virtual
 * monitor that appears in System Settings as a real display.
 * ScreenCaptureKit can then capture it.
 */

#pragma once

#include <cstdint>

namespace droidscreen {

class VirtualDisplay {
public:
    VirtualDisplay();
    ~VirtualDisplay();

    /// Create a virtual display with the given resolution.
    /// @param width  Pixel width.
    /// @param height Pixel height.
    /// @param fps    Refresh rate in Hz.
    /// @return true on success.
    bool create(uint32_t width, uint32_t height, uint32_t fps = 60);

    /// Destroy the virtual display.
    void destroy();

    /// The CGDirectDisplayID of the virtual display (for SCK filtering).
    uint32_t display_id() const { return display_id_; }

    /// Whether the display is currently active.
    bool is_active() const { return active_; }

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    void* display_ = nullptr;   // CGVirtualDisplay*
    void* descriptor_ = nullptr; // CGVirtualDisplayDescriptor*
    void* settings_ = nullptr;   // CGVirtualDisplaySettings*
    uint32_t display_id_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    bool active_ = false;
};

} // namespace droidscreen
