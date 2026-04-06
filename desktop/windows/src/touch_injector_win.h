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
#include <condition_variable>
#include <mutex>
#include <thread>

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
                uint16_t x, uint16_t y, uint16_t pressure,
                uint16_t touch_major, uint16_t touch_minor,
                uint16_t orientation) override;
    void shutdown() override;

    /// Per-pointer tracking state.
    struct PointerState {
        bool     present        = false;
        bool     in_range       = false;
        bool     in_contact     = false;
        bool     edge_update    = false;
        bool     edge_down      = false;
        bool     edge_up        = false;
        bool     edge_canceled  = false;
        int32_t  pixel_x        = 0;
        int32_t  pixel_y        = 0;
        uint16_t pressure       = 0;
        uint16_t touch_major    = 0;
        uint16_t touch_minor    = 0;
        uint16_t orientation    = 0xFFFFu;
    };

private:
    /// Maximum number of simultaneous touch contacts.
    static constexpr uint32_t kMaxContacts = 10;

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

    std::mutex mutex_;
    std::condition_variable repeat_cv_;
    std::thread repeat_thread_;
    bool stop_repeat_ = false;
    bool repeat_reset_pending_ = false;

    bool inject_current_state_locked();
    bool has_repeatable_touches_locked() const;
    void clear_edge_flags_locked();
    uint32_t pointer_flags_locked(const PointerState& ptr) const;
    void repeat_loop();
};

} // namespace droidscreen
