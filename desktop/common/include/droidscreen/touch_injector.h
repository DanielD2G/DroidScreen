/*
 * DroidScreen Desktop - Touch injection interface
 *
 * Translates touch events received from the Android device into
 * local input events. NullTouchInjector is a no-op for platforms
 * that do not support or need touch injection (e.g., macOS).
 */

#pragma once

#include <cstdint>

namespace droidscreen {

class TouchInjector {
public:
    virtual ~TouchInjector() = default;

    /// Initialize the injector for a surface of the given dimensions.
    /// @param w Surface width in pixels.
    /// @param h Surface height in pixels.
    /// @param ox Horizontal offset of the surface on the display.
    /// @param oy Vertical offset of the surface on the display.
    virtual bool init(uint32_t w, uint32_t h,
                      int32_t ox = 0, int32_t oy = 0) = 0;

    /// Inject a single touch event.
    /// @param action Touch action (down/move/up/cancel).
    /// @param ptr_id Pointer ID for multi-touch.
    /// @param x Fractional X coordinate (0..65535).
    /// @param y Fractional Y coordinate (0..65535).
    /// @param pressure Touch pressure (0..65535).
    virtual bool inject(uint8_t action, uint8_t ptr_id,
                        uint16_t x, uint16_t y, uint16_t pressure) = 0;

    /// Shut down the injector and release resources.
    virtual void shutdown() = 0;
};

/// No-op touch injector for platforms that don't need touch injection.
class NullTouchInjector : public TouchInjector {
public:
    bool init(uint32_t, uint32_t, int32_t, int32_t) override { return true; }
    bool inject(uint8_t, uint8_t, uint16_t, uint16_t, uint16_t) override { return true; }
    void shutdown() override {}
};

} // namespace droidscreen
