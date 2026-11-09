/*
 * DroidScreen macOS - Touch and pen injection via CGEvent
 *
 * Touch events (finger) are mapped to touchscreen-style input:
 *   - 1 finger tap:            left click (double/triple tap = multi-click).
 *   - 1 finger swipe:          continuous scroll that follows the finger.
 *   - 1 finger hold + move:    left-button drag (select, move windows...).
 *   - 1 finger hold + release: right click.
 *   - 2 finger swipe:          continuous scroll (centroid).
 *   - 2 finger tap:            right click.
 *   - 3+ fingers:              gesture cancelled.
 *
 * Scrolls are posted as continuous pixel scroll events with scroll
 * phases, followed by momentum events tagged with momentum phases, so
 * apps treat them like trackpad scrolls (rubber-banding, etc.).
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

    /// Set the target display.  map_to_screen() uses its CGDisplayBounds,
    /// cached and refreshed after every display reconfiguration (HiDPI,
    /// scaling, resolution or arrangement changes).
    void set_display_id(CGDirectDisplayID did);

    bool inject_touch(uint8_t action, uint8_t ptr_id,
                      uint16_t x, uint16_t y, uint16_t pressure,
                      uint16_t touch_major, uint16_t touch_minor,
                      uint16_t orientation, uint32_t timestamp_ms) override;
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

    // CGDisplayBounds of display_id_, read under mutex_. The reconfiguration
    // callback only clears bounds_valid_; the next event re-reads the bounds.
    mutable CGRect bounds_cache_ = CGRectZero;
    mutable std::atomic<bool> bounds_valid_{false};
    bool reconfig_registered_ = false;
    static void display_reconfigured(CGDirectDisplayID display,
                                     CGDisplayChangeSummaryFlags flags,
                                     void* user_info);

    // ------- Finger tracking -------
    // More slots than gestures use, so a 3rd finger is detected and
    // cancels the gesture instead of being silently dropped.
    static constexpr int kMaxFingers = 10;

    struct FingerSlot {
        bool active = false;
        uint8_t ptr_id = 0;
        CGPoint pos = {0, 0};  // last position, global display points
    };

    FingerSlot fingers_[kMaxFingers];
    int active_finger_count_ = 0;

    // ------- Gesture state machine -------
    enum class Gesture {
        Idle,          // no fingers down
        OnePending,    // 1 finger, not yet classified (tap / scroll / drag)
        OneScroll,     // 1 finger scrolling
        OneDrag,       // 1 finger long-press drag (left button held)
        TwoPending,    // 2 fingers, not yet classified (tap / scroll)
        TwoScroll,     // 2 fingers scrolling (centroid)
        TwoReleasing,  // 2-finger tap candidate, one finger already lifted
        Dead,          // gesture cancelled; ignore until all fingers lift
    };

    Gesture gesture_ = Gesture::Idle;
    uint8_t primary_id_ = 0;        // finger that started the gesture
    CGPoint start_pos_ = {0, 0};    // primary finger down position
    uint32_t start_ts_ = 0;         // gesture start (Android ms)
    bool suppress_tap_ = false;     // touch-down stopped momentum
    bool two_tap_ok_ = false;       // 2-finger gesture may still be a tap
    CGPoint two_offset_ = {0, 0};   // 2-finger tap movement so far
    CGPoint two_anchor_ = {0, 0};   // 2-finger centroid at touch-down

    // Multi-click tracking (double / triple tap).
    int click_count_ = 0;
    double double_click_ms_ = 500.0;  // system double-click interval
    uint32_t last_click_ts_ = 0;
    CGPoint last_click_pos_ = {0, 0};

    // ------- Scroll -------
    enum class AxisLock { None, X, Y };
    CGPoint scroll_anchor_ = {0, 0};  // location scroll events target
    AxisLock axis_lock_ = AxisLock::None;
    double scroll_rem_x_ = 0.0;       // sub-pixel remainder
    double scroll_rem_y_ = 0.0;
    double scroll_pos_x_ = 0.0;       // cumulative scroll (for velocity)
    double scroll_pos_y_ = 0.0;

    struct ScrollSample {
        uint32_t ts;
        double x;
        double y;
    };
    static constexpr int kMaxScrollSamples = 32;
    ScrollSample scroll_samples_[kMaxScrollSamples];
    int scroll_sample_count_ = 0;
    int scroll_sample_head_ = 0;

    void begin_scroll_locked(CGPoint anchor, CGPoint offset, uint32_t ts);
    void scroll_by_locked(double dx, double dy, uint32_t ts);
    enum class ScrollEnd { Cancelled, Ended, Momentum };
    void end_scroll_locked(uint32_t ts, ScrollEnd how);
    void post_scroll_locked(double dx, double dy, uint32_t phase,
                            uint32_t momentum_phase);

    // ------- Momentum (trackpad inertia) -------
    // A single worker thread lives for the injector's lifetime and runs
    // momentum on demand.  Events are posted under mutex_ so phase
    // ordering (Begin → Continue → End) can never interleave.
    double momentum_vx_ = 0.0;  // pts/s
    double momentum_vy_ = 0.0;
    bool momentum_active_ = false;
    bool momentum_began_ = false;
    bool worker_stop_ = false;
    std::thread momentum_thread_;
    std::condition_variable momentum_cv_;

    void start_momentum_locked(double vx, double vy);
    void stop_momentum_locked();
    void momentum_loop();

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

    void post_mouse(CGEventType type, CGPoint pos, CGMouseButton button,
                    int click_state);
    void click_locked(CGPoint pos, uint32_t down_ts);
    void right_click_locked(CGPoint pos);
    void start_drag_locked(CGPoint to);
    void cancel_gesture_locked(uint32_t ts);

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
