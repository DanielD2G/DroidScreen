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

#include <cstdint>
#include <mutex>

namespace droidscreen {

class MacMouseInjector : public MouseInjector {
public:
    MacMouseInjector();
    ~MacMouseInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;

    /// Set the target display.  map_to_screen() will query
    /// CGDisplayBounds at event time for correct coordinates
    /// regardless of HiDPI, scaling, or resolution changes.
    void set_display_id(CGDirectDisplayID did);

    bool inject_mouse(uint8_t action, uint8_t buttons,
                      uint16_t x, uint16_t y) override;
    void shutdown() override;

private:
    CGDirectDisplayID display_id_ = 0;

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
