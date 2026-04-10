/*
 * DroidScreen macOS - Touch and pen injection via CGEvent
 *
 * Touch events (finger) are mapped to mouse events.
 *   - Single finger: mouse move / click / drag.
 *   - Two fingers:   scroll gesture via CGEventCreateScrollWheelEvent2.
 *
 * Pen events (stylus) are mapped to CGEvent mouse events with tablet
 * subtype and fields (pressure, tilt, rotation), emulating a Wacom
 * tablet — the same approach used by Sidecar and OpenTabletDriver.
 *
 * Requires Accessibility permission in System Settings.
 */

#pragma once

#include "droidscreen/touch_injector.h"

#include <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace droidscreen {

class MacTouchInjector : public TouchInjector {
public:
    MacTouchInjector();
    ~MacTouchInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;

    /// Set the target display.  map_to_screen() will query
    /// CGDisplayBounds at event time for correct coordinates
    /// regardless of HiDPI, scaling, or resolution changes.
    void set_display_id(CGDirectDisplayID did);

    bool inject_touch(uint8_t action, uint8_t ptr_id,
                      uint16_t x, uint16_t y, uint16_t pressure,
                      uint16_t touch_major, uint16_t touch_minor,
                      uint16_t orientation) override;
    bool inject_pen(uint8_t action, uint8_t ptr_id,
                    uint8_t tool_type, uint32_t buttons,
                    uint16_t x, uint16_t y, uint16_t pressure,
                    uint16_t distance, uint16_t tilt,
                    uint16_t rotation) override;
    void shutdown() override;

private:
    // ------- Wacom-compatible constants (for Adobe interop) -------
    // Capability mask tells apps which tablet fields are valid.
    // Derived from Wacom SDK Wacom.h kTransducer*BitMask values.
    static constexpr int64_t kCapabilityMask =
        0x001 /* deviceId */ | 0x002 /* absX */ | 0x004 /* absY */ |
        0x040 /* buttons */ | 0x080 /* tiltX */ | 0x100 /* tiltY */ |
        0x400 /* pressure */;  // = 0x5C7

    static constexpr int64_t kVendorPointerType = 0x0802; // General Stylus
    static constexpr int64_t kDeviceId = 0xD50D;          // "DroidScreen"

    static constexpr int kProximityRefreshMs = 200;

    // ------- Display -------
    CGDirectDisplayID display_id_ = 0;

    // ------- Multi-touch state (trackpad-like) -------
    //  - 1 finger  → move cursor.  Tap (short touch) = click.
    //  - 2 fingers → scroll gesture (natural scrolling).
    //  No mouse-down on touch-down: behaves like a Mac trackpad.
    static constexpr int kMaxFingers = 2;

    struct FingerSlot {
        bool active = false;
        uint8_t ptr_id = 0;
        uint16_t x = 0;       // last fractional X
        uint16_t y = 0;       // last fractional Y
        uint16_t prev_x = 0;  // previous fractional X (for delta)
        uint16_t prev_y = 0;
        bool has_prev = false; // whether prev_x/y are valid
    };

    FingerSlot fingers_[kMaxFingers];
    int active_finger_count_ = 0;

    // Tap & drag detection (trackpad-like):
    //  - 1-finger short touch + little movement = left click.
    //  - 1-finger touch + move past threshold   = drag.
    //  - 2-finger short touch + little movement  = right click (context menu).
    //  - 2-finger touch + move                   = scroll.
    bool tap_candidate_ = false;             // finger might still be a tap
    bool dragging_ = false;                  // finger moved past threshold → drag mode
    uint16_t tap_start_x_ = 0;
    uint16_t tap_start_y_ = 0;
    bool ever_had_two_fingers_ = false;      // two fingers seen during this gesture
    bool two_finger_tap_candidate_ = false;  // two-finger tap might be a right-click
    bool two_finger_scrolled_ = false;       // any scroll happened → not a tap

    // ------- Scroll momentum (trackpad inertia) -------
    double scroll_vel_x_ = 0.0;   // current velocity in pts/tick
    double scroll_vel_y_ = 0.0;
    std::thread momentum_thread_;
    std::condition_variable momentum_cv_;
    bool momentum_active_ = false;
    bool stop_momentum_ = false;

    void start_momentum_locked();
    void stop_momentum_locked();
    void momentum_loop();
    void post_scroll_event(int32_t dy, int32_t dx);

    // ------- Pen state -------
    bool pen_in_proximity_ = false;
    bool pen_down_ = false;
    uint8_t pen_tool_type_ = 0;
    CGPoint last_pen_pos_ = {0, 0};
    std::chrono::steady_clock::time_point last_proximity_time_;

    // ------- Shared -------
    CGEventSourceRef event_source_ = nullptr;
    std::mutex mutex_;

    CGPoint map_to_screen(uint16_t x_frac, uint16_t y_frac) const;

    void post_proximity_event(bool entering, uint8_t tool_type);
    void post_tablet_mouse_event(CGEventType type, CGPoint pos,
                                 double pressure, double tilt_x,
                                 double tilt_y, double rotation,
                                 int buttons);

    FingerSlot* find_finger(uint8_t ptr_id);
    FingerSlot* alloc_finger(uint8_t ptr_id);
    void free_finger(uint8_t ptr_id);

    void release_all_touch_locked();
    void release_all_pen_locked();
};

} // namespace droidscreen
