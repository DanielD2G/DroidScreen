/*
 * DroidScreen macOS - Mouse injection via CGEvent
 *
 * Translates mouse events received from the Android device into
 * macOS cursor events using CGEventCreateMouseEvent + CGEventPost.
 * Requires Accessibility permission in System Settings.
 */

#pragma once

#include "droidscreen/mouse_injector.h"

#include <CoreGraphics/CoreGraphics.h>

#include <atomic>
#include <cstdint>
#include <mutex>

namespace droidscreen {

class MacMouseInjector : public MouseInjector {
public:
    MacMouseInjector();
    ~MacMouseInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;

    /// Set the target display.  map_to_screen() uses its CGDisplayBounds,
    /// cached and refreshed after every display reconfiguration (HiDPI,
    /// scaling, resolution or arrangement changes).
    void set_display_id(CGDirectDisplayID did);

    bool inject_mouse(uint8_t action, uint8_t buttons,
                      uint16_t x, uint16_t y) override;
    void shutdown() override;

    /// Types text (as Unicode keyboard events) or presses a special key into
    /// whatever app has keyboard focus. Needs Accessibility permission.
    bool inject_key(uint8_t kind, uint8_t modifiers, uint16_t key,
                    const char* text, size_t text_len) override;

private:
    CGDirectDisplayID display_id_ = 0;

    // CGDisplayBounds of display_id_, read under mutex_. The reconfiguration
    // callback only clears bounds_valid_; the next event re-reads the bounds.
    mutable CGRect bounds_cache_ = CGRectZero;
    mutable std::atomic<bool> bounds_valid_{false};
    bool reconfig_registered_ = false;
    static void display_reconfigured(CGDirectDisplayID display,
                                     CGDisplayChangeSummaryFlags flags,
                                     void* user_info);

    uint8_t current_buttons_ = 0;
    CGMouseButton last_button_ = kCGMouseButtonLeft;
    CGPoint last_pos_ = {0, 0};

    CGEventSourceRef event_source_ = nullptr;
    std::mutex mutex_;

    CGPoint map_to_screen(uint16_t x_frac, uint16_t y_frac) const;
    void post_mouse_event(CGEventType type, CGPoint pos,
                          CGMouseButton button) const;
};

} // namespace droidscreen
