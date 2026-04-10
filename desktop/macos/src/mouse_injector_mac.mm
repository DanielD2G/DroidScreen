/*
 * DroidScreen macOS - Mouse injection implementation
 *
 * Uses CGEventCreateMouseEvent / CGEventPost to inject cursor and
 * mouse button events on the virtual display.
 *
 * Coordinates are mapped by querying CGDisplayBounds at event time
 * so they are always correct regardless of HiDPI, resolution changes,
 * or display rearrangement.
 */

#import "mouse_injector_mac.h"

extern "C" {
#include "droidscreen/touch.h"
}

#include <algorithm>
#include <cstdio>

static constexpr uint8_t kTouchDown       = 0;
static constexpr uint8_t kTouchMove       = 1;
static constexpr uint8_t kTouchUp         = 2;
static constexpr uint8_t kTouchCancel     = 3;
static constexpr uint8_t kTouchHover      = 4;
static constexpr uint8_t kTouchHoverLeave = 5;
static constexpr uint8_t kTouchButtonOnly = 6;
static constexpr uint8_t kTouchCancelAll  = 7;

static constexpr uint8_t kButtonLeft   = 1 << 0;
static constexpr uint8_t kButtonRight  = 1 << 1;
static constexpr uint8_t kButtonMiddle = 1 << 2;

namespace droidscreen {

MacMouseInjector::MacMouseInjector() = default;

MacMouseInjector::~MacMouseInjector() {
    shutdown();
}

bool MacMouseInjector::init(uint32_t /*w*/, uint32_t /*h*/,
                            int32_t /*ox*/, int32_t /*oy*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    current_buttons_ = 0;
    last_button_ = kCGMouseButtonLeft;

    if (event_source_) {
        CFRelease(event_source_);
    }
    event_source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (!event_source_) {
        fprintf(stderr, "[mouse] CGEventSourceCreate failed\n");
        return false;
    }

    fprintf(stderr, "[mouse] injector initialized (display_id=%u)\n", display_id_);
    return true;
}

void MacMouseInjector::set_display_id(CGDirectDisplayID did) {
    std::lock_guard<std::mutex> lock(mutex_);
    display_id_ = did;

    CGRect bounds = CGDisplayBounds(did);
    fprintf(stderr, "[mouse] display %u bounds: origin=(%.0f,%.0f) size=%.0fx%.0f\n",
            did, bounds.origin.x, bounds.origin.y,
            bounds.size.width, bounds.size.height);
}

CGPoint MacMouseInjector::map_to_screen(uint16_t x_frac, uint16_t y_frac) const {
    // Query display bounds LIVE so we are always correct regardless of
    // HiDPI mode, user-selected resolution, or display rearrangement.
    CGRect bounds = CGDisplayBounds(display_id_);

    double screen_x = bounds.origin.x +
                      (static_cast<double>(x_frac) / 65535.0) * bounds.size.width;
    double screen_y = bounds.origin.y +
                      (static_cast<double>(y_frac) / 65535.0) * bounds.size.height;

    return CGPointMake(screen_x, screen_y);
}

void MacMouseInjector::post_mouse_event(CGEventType type, CGPoint pos,
                                        CGMouseButton button) const {
    CGEventRef event = CGEventCreateMouseEvent(event_source_, type, pos, button);
    if (!event) return;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

bool MacMouseInjector::inject_mouse(uint8_t action, uint8_t buttons,
                                    uint16_t x, uint16_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!event_source_) return false;

    CGPoint pos = map_to_screen(x, y);

    // Determine desired button state based on action.
    uint8_t desired = buttons;
    if (action == kTouchUp || action == kTouchCancel ||
        action == kTouchHoverLeave || action == kTouchCancelAll) {
        desired = 0;
    }

    // 1. Emit movement event.
    if (action != kTouchButtonOnly && action != kTouchHoverLeave) {
        CGEventType move_type;
        if (current_buttons_ & kButtonLeft) {
            move_type = kCGEventLeftMouseDragged;
        } else if (current_buttons_ & kButtonRight) {
            move_type = kCGEventRightMouseDragged;
        } else if (current_buttons_ & kButtonMiddle) {
            move_type = kCGEventOtherMouseDragged;
        } else {
            move_type = kCGEventMouseMoved;
        }
        post_mouse_event(move_type, pos, last_button_);
        last_pos_ = pos;
    }

    // 2. Emit button transitions.
    uint8_t changed = current_buttons_ ^ desired;

    if (changed & kButtonLeft) {
        bool down = desired & kButtonLeft;
        post_mouse_event(down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp,
                         last_pos_, kCGMouseButtonLeft);
        if (down) last_button_ = kCGMouseButtonLeft;
    }
    if (changed & kButtonRight) {
        bool down = desired & kButtonRight;
        post_mouse_event(down ? kCGEventRightMouseDown : kCGEventRightMouseUp,
                         last_pos_, kCGMouseButtonRight);
        if (down) last_button_ = kCGMouseButtonRight;
    }
    if (changed & kButtonMiddle) {
        bool down = desired & kButtonMiddle;
        post_mouse_event(down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp,
                         last_pos_, kCGMouseButtonCenter);
        if (down) last_button_ = kCGMouseButtonCenter;
    }

    current_buttons_ = desired;
    return true;
}

void MacMouseInjector::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (current_buttons_ && event_source_) {
        if (current_buttons_ & kButtonLeft)
            post_mouse_event(kCGEventLeftMouseUp, last_pos_, kCGMouseButtonLeft);
        if (current_buttons_ & kButtonRight)
            post_mouse_event(kCGEventRightMouseUp, last_pos_, kCGMouseButtonRight);
        if (current_buttons_ & kButtonMiddle)
            post_mouse_event(kCGEventOtherMouseUp, last_pos_, kCGMouseButtonCenter);
        current_buttons_ = 0;
    }

    if (event_source_) {
        CFRelease(event_source_);
        event_source_ = nullptr;
        fprintf(stderr, "[mouse] injector shut down\n");
    }
}

} // namespace droidscreen
