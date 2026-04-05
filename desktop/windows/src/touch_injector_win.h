/*
 * DroidScreen Windows - Synthetic touch injector
 *
 * Injects touch events into the Windows input system using the
 * SyntheticPointerDevice API (Windows 10 1809+).
 *
 * Touch coordinates arrive from the Android device as fractional
 * values (0..65535) and are mapped to absolute virtual desktop
 * coordinates for injection.
 */

#pragma once

#include "droidscreen/touch_injector.h"

#include <cstdint>
#include <array>

// Forward-declare Windows types to avoid including windows.h in the header.
struct HSYNTHETICPOINTERDEVICE__;
typedef HSYNTHETICPOINTERDEVICE__* HSYNTHETICPOINTERDEVICE;

namespace droidscreen {

class WinTouchInjector : public TouchInjector {
public:
    WinTouchInjector();
    ~WinTouchInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;
    bool inject(uint8_t action, uint8_t ptr_id,
                uint16_t x, uint16_t y, uint16_t pressure) override;
    void shutdown() override;

private:
    /// Maximum number of simultaneous touch contacts.
    static constexpr uint32_t kMaxContacts = 10;

    /// Per-pointer tracking state.
    struct PointerState {
        bool   active    = false;
        int32_t pixel_x  = 0;
        int32_t pixel_y  = 0;
        uint16_t pressure = 0;
    };

    /// Surface dimensions and offset on the virtual desktop.
    uint32_t surface_w_ = 0;
    uint32_t surface_h_ = 0;
    int32_t  offset_x_  = 0;
    int32_t  offset_y_  = 0;

    /// Virtual desktop metrics (cached at init).
    int32_t vdesk_x_      = 0;
    int32_t vdesk_y_      = 0;
    int32_t vdesk_width_  = 0;
    int32_t vdesk_height_ = 0;

    /// Synthetic pointer device handle.
    HSYNTHETICPOINTERDEVICE device_ = nullptr;

    /// Per-pointer state.
    std::array<PointerState, kMaxContacts> pointers_ = {};
};

} // namespace droidscreen
