/*
 * DroidScreen macOS - Touch and pen injection implementation
 *
 * Touch (finger) → mouse events via CGEvent.
 *   - 1 finger:  mouse cursor (move / click / drag).
 *   - 2 fingers: scroll gesture (CGEventCreateScrollWheelEvent2).
 *                 If a single-finger drag was in progress it is released
 *                 before switching to scroll mode.
 *
 * Pen (stylus) → mouse events with tablet subtype and fields,
 *                 emulating a Wacom tablet for full pressure/tilt
 *                 support in drawing applications.
 *
 * Implementation based on OpenTabletDriver's MacOSVirtualMouse.cs
 * and the Wacom SDK (Wacom.h capability masks).
 *
 * Key rules:
 *   - NEVER reuse a CGEventRef; create → set fields → post → release.
 *   - Set kCGMouseEventSubtype BEFORE setting tablet point fields.
 *   - TiltY is INVERTED on macOS (confirmed by Chromium source).
 *   - Adobe requires proximity events with Wacom-compatible capability
 *     masks and vendor pointer type to recognize pressure.
 *   - Re-send proximity events after >200ms of inactivity.
 */

#import "touch_injector_mac.h"

extern "C" {
#include "droidscreen/touch.h"
}

#include <algorithm>
#include <cmath>
#include <cstdio>

static constexpr uint8_t kTouchDown       = 0;
static constexpr uint8_t kTouchMove       = 1;
static constexpr uint8_t kTouchUp         = 2;
static constexpr uint8_t kTouchCancel     = 3;
static constexpr uint8_t kTouchHover      = 4;
static constexpr uint8_t kTouchHoverLeave = 5;
static constexpr uint8_t kTouchButtonOnly = 6;
static constexpr uint8_t kTouchCancelAll  = 7;

// Scroll: scale factor for converting point-space movement to scroll amount.
static constexpr double kScrollScale = 0.5;

// Momentum: friction per tick (0.95 = gentle deceleration like Mac trackpad).
static constexpr double kMomentumFriction = 0.95;
// Momentum: stop when velocity drops below this (in pts).
static constexpr double kMomentumMinVelocity = 0.5;
// Momentum: tick interval (~60 fps).
static constexpr auto kMomentumInterval = std::chrono::milliseconds(16);
// Velocity smoothing: exponential moving average factor (0..1).
static constexpr double kVelocitySmoothing = 0.3;

// Tap detection: maximum movement (in fractional units) to still count as tap.
// ~1.5% of the screen — generous enough for finger jitter on a touchscreen.
static constexpr int32_t kTapMoveTolerance = 1000;  // out of 65535

namespace droidscreen {

MacTouchInjector::MacTouchInjector() = default;

MacTouchInjector::~MacTouchInjector() {
    shutdown();
}

bool MacTouchInjector::init(uint32_t /*w*/, uint32_t /*h*/,
                            int32_t /*ox*/, int32_t /*oy*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& f : fingers_) {
        f = {};
    }
    active_finger_count_ = 0;
    tap_candidate_ = false;
    ever_had_two_fingers_ = false;
    two_finger_tap_candidate_ = false;
    two_finger_scrolled_ = false;
    scroll_vel_x_ = 0.0;
    scroll_vel_y_ = 0.0;
    momentum_active_ = false;

    pen_in_proximity_ = false;
    pen_down_ = false;
    pen_tool_type_ = DS_TOUCH_TOOL_UNKNOWN;
    last_proximity_time_ = std::chrono::steady_clock::now();

    if (event_source_) {
        CFRelease(event_source_);
    }
    event_source_ = CGEventSourceCreate(kCGEventSourceStatePrivate);
    if (!event_source_) {
        fprintf(stderr, "[touch] CGEventSourceCreate failed\n");
        return false;
    }

    fprintf(stderr, "[touch] injector initialized (display_id=%u)\n", display_id_);
    return true;
}

void MacTouchInjector::set_display_id(CGDirectDisplayID did) {
    std::lock_guard<std::mutex> lock(mutex_);
    display_id_ = did;

    CGRect bounds = CGDisplayBounds(did);
    fprintf(stderr, "[touch] display %u bounds: origin=(%.0f,%.0f) size=%.0fx%.0f\n",
            did, bounds.origin.x, bounds.origin.y,
            bounds.size.width, bounds.size.height);
}

// ---------------------------------------------------------------------------
// Coordinate mapping
// ---------------------------------------------------------------------------

CGPoint MacTouchInjector::map_to_screen(uint16_t x_frac, uint16_t y_frac) const {
    // Query display bounds LIVE so we are always correct regardless of
    // HiDPI mode, user-selected resolution, or display rearrangement.
    CGRect bounds = CGDisplayBounds(display_id_);

    double screen_x = bounds.origin.x +
                      (static_cast<double>(x_frac) / 65535.0) * bounds.size.width;
    double screen_y = bounds.origin.y +
                      (static_cast<double>(y_frac) / 65535.0) * bounds.size.height;

    return CGPointMake(screen_x, screen_y);
}

// ---------------------------------------------------------------------------
// Finger slot management
// ---------------------------------------------------------------------------

MacTouchInjector::FingerSlot* MacTouchInjector::find_finger(uint8_t ptr_id) {
    for (auto& f : fingers_) {
        if (f.active && f.ptr_id == ptr_id) return &f;
    }
    return nullptr;
}

MacTouchInjector::FingerSlot* MacTouchInjector::alloc_finger(uint8_t ptr_id) {
    for (auto& f : fingers_) {
        if (!f.active) {
            f = {};
            f.active = true;
            f.ptr_id = ptr_id;
            active_finger_count_++;
            return &f;
        }
    }
    return nullptr;  // all slots full
}

void MacTouchInjector::free_finger(uint8_t ptr_id) {
    for (auto& f : fingers_) {
        if (f.active && f.ptr_id == ptr_id) {
            f.active = false;
            active_finger_count_--;
            return;
        }
    }
}

// ---------------------------------------------------------------------------
// Scroll momentum (trackpad inertia)
// ---------------------------------------------------------------------------

void MacTouchInjector::post_scroll_event(int32_t dy, int32_t dx) {
    if (!event_source_) return;
    CGEventRef ev = CGEventCreateScrollWheelEvent2(
        event_source_, kCGScrollEventUnitPixel, 2, dy, dx, 0);
    if (ev) {
        CGEventPost(kCGHIDEventTap, ev);
        CFRelease(ev);
    }
}

void MacTouchInjector::stop_momentum_locked() {
    if (!momentum_active_) return;
    momentum_active_ = false;
    momentum_cv_.notify_all();
}

void MacTouchInjector::start_momentum_locked() {
    // Stop any existing momentum first.
    stop_momentum_locked();

    if (std::abs(scroll_vel_x_) < kMomentumMinVelocity &&
        std::abs(scroll_vel_y_) < kMomentumMinVelocity) {
        return;  // too slow, no momentum needed
    }

    momentum_active_ = true;

    // Detach any previous thread that already finished.
    if (momentum_thread_.joinable()) {
        momentum_thread_.join();
    }

    momentum_thread_ = std::thread(&MacTouchInjector::momentum_loop, this);
}

void MacTouchInjector::momentum_loop() {
    std::unique_lock<std::mutex> lock(mutex_);

    double vx = scroll_vel_x_;
    double vy = scroll_vel_y_;

    while (momentum_active_ && !stop_momentum_) {
        vx *= kMomentumFriction;
        vy *= kMomentumFriction;

        if (std::abs(vx) < kMomentumMinVelocity &&
            std::abs(vy) < kMomentumMinVelocity) {
            break;
        }

        int32_t dy = static_cast<int32_t>(std::round(vy));
        int32_t dx = static_cast<int32_t>(std::round(vx));

        if (dx != 0 || dy != 0) {
            // Unlock while posting to avoid holding the lock during CGEventPost.
            lock.unlock();
            post_scroll_event(dy, dx);
            lock.lock();
        }

        // Sleep for one tick, but wake early if momentum is cancelled.
        momentum_cv_.wait_for(lock, kMomentumInterval, [this] {
            return !momentum_active_ || stop_momentum_;
        });
    }

    momentum_active_ = false;
}

// ---------------------------------------------------------------------------
// Proximity events (Wacom-compatible)
// ---------------------------------------------------------------------------

void MacTouchInjector::post_proximity_event(bool entering, uint8_t tool_type) {
    CGEventRef ev = CGEventCreate(event_source_);
    if (!ev) return;

    CGEventSetType(ev, kCGEventTabletProximity);

    // Pointer type: 1 = pen, 3 = eraser (NSPointingDeviceType enum).
    int64_t pointer_type = 1; // pen
    if (tool_type == DS_TOUCH_TOOL_ERASER) {
        pointer_type = 3; // eraser
    }

    CGEventSetIntegerValueField(ev, kCGTabletProximityEventEnterProximity,
                                entering ? 1 : 0);
    CGEventSetIntegerValueField(ev, kCGTabletProximityEventPointerType,
                                pointer_type);
    CGEventSetIntegerValueField(ev, kCGTabletProximityEventCapabilityMask,
                                kCapabilityMask);
    CGEventSetIntegerValueField(ev, kCGTabletProximityEventDeviceID,
                                kDeviceId);
    CGEventSetIntegerValueField(ev, kCGTabletProximityEventVendorPointerType,
                                kVendorPointerType);

    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);

    last_proximity_time_ = std::chrono::steady_clock::now();
}

// ---------------------------------------------------------------------------
// Tablet mouse events (pressure, tilt, rotation)
// ---------------------------------------------------------------------------

void MacTouchInjector::post_tablet_mouse_event(CGEventType type, CGPoint pos,
                                               double pressure,
                                               double tilt_x, double tilt_y,
                                               double rotation,
                                               int buttons) {
    CGEventRef ev = CGEventCreateMouseEvent(event_source_, type, pos,
                                            kCGMouseButtonLeft);
    if (!ev) return;

    // Subtype MUST be set BEFORE tablet fields (CGEvent stores fields
    // in a union keyed by type).
    CGEventSetIntegerValueField(ev, kCGMouseEventSubtype,
                                kCGEventMouseSubtypeTabletPoint);

    // Pressure in both mouse and tablet fields (some apps read one, some the other).
    CGEventSetDoubleValueField(ev, kCGMouseEventPressure, pressure);
    CGEventSetDoubleValueField(ev, kCGTabletEventPointPressure, pressure);

    // Tablet point fields.
    CGEventSetIntegerValueField(ev, kCGTabletEventPointButtons, buttons);
    CGEventSetIntegerValueField(ev, kCGTabletEventDeviceID, kDeviceId);

    // Tilt: macOS range is 0..1 (normalized).  TiltY is INVERTED.
    CGEventSetDoubleValueField(ev, kCGTabletEventTiltX, tilt_x);
    CGEventSetDoubleValueField(ev, kCGTabletEventTiltY, -tilt_y);

    // Rotation in degrees.
    CGEventSetDoubleValueField(ev, kCGTabletEventRotation, rotation);

    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);  // NEVER reuse — fields are a union per event type.
}

// ---------------------------------------------------------------------------
// inject_touch  —  trackpad-like behavior
//
// 1 finger:  Move cursor (no click on touch-down).
//            Short tap with little movement = click (down+up on release).
// 2 fingers: Scroll gesture with natural scrolling (like Mac trackpad).
//            No click is generated.
// ---------------------------------------------------------------------------

bool MacTouchInjector::inject_touch(uint8_t action, uint8_t ptr_id,
                                    uint16_t x, uint16_t y,
                                    uint16_t /*pressure*/,
                                    uint16_t /*touch_major*/,
                                    uint16_t /*touch_minor*/,
                                    uint16_t /*orientation*/) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!event_source_) return false;

    // ---- Cancel-all ----
    if (action == kTouchCancelAll) {
        release_all_touch_locked();
        return true;
    }

    switch (action) {

    // ==================================================================
    //  FINGER DOWN
    // ==================================================================
    case kTouchDown: {
        FingerSlot* slot = find_finger(ptr_id);
        if (slot) break; // already tracked

        slot = alloc_finger(ptr_id);
        if (!slot) break; // all slots full

        slot->x = x;
        slot->y = y;
        slot->has_prev = false;

        if (active_finger_count_ == 1) {
            // First finger — just move cursor, NO click yet.
            // Mark as potential tap candidate.  Cancel any momentum.
            stop_momentum_locked();
            tap_candidate_ = true;
            dragging_ = false;
            tap_start_x_ = x;
            tap_start_y_ = y;
            ever_had_two_fingers_ = false;

            CGPoint pos = map_to_screen(x, y);
            CGEventRef ev = CGEventCreateMouseEvent(
                event_source_, kCGEventMouseMoved, pos, kCGMouseButtonLeft);
            if (ev) {
                CGEventPost(kCGHIDEventTap, ev);
                CFRelease(ev);
            }
        } else if (active_finger_count_ >= 2) {
            // Second finger → potential scroll or right-click.
            stop_momentum_locked();
            tap_candidate_ = false;
            ever_had_two_fingers_ = true;
            two_finger_tap_candidate_ = true;
            two_finger_scrolled_ = false;
            scroll_vel_x_ = 0.0;
            scroll_vel_y_ = 0.0;
        }
        break;
    }

    // ==================================================================
    //  FINGER MOVE
    // ==================================================================
    case kTouchMove: {
        FingerSlot* slot = find_finger(ptr_id);
        if (!slot) break;

        // Save previous position before updating.
        slot->prev_x = slot->x;
        slot->prev_y = slot->y;
        slot->x = x;
        slot->y = y;

        if (active_finger_count_ == 1) {
            // ---- Single finger → move cursor or drag ----
            CGPoint pos = map_to_screen(x, y);

            if (tap_candidate_ && !dragging_) {
                // Still might be a tap — check movement.
                int32_t dx = static_cast<int32_t>(x) - static_cast<int32_t>(tap_start_x_);
                int32_t dy = static_cast<int32_t>(y) - static_cast<int32_t>(tap_start_y_);
                if (dx * dx + dy * dy > kTapMoveTolerance * kTapMoveTolerance) {
                    // Moved too far → enter drag mode.
                    tap_candidate_ = false;
                    dragging_ = true;

                    // Send mouseDown at the START position, then dragged to current.
                    CGPoint start_pos = map_to_screen(tap_start_x_, tap_start_y_);
                    CGEventRef down = CGEventCreateMouseEvent(
                        event_source_, kCGEventLeftMouseDown, start_pos,
                        kCGMouseButtonLeft);
                    if (down) {
                        CGEventPost(kCGHIDEventTap, down);
                        CFRelease(down);
                    }
                }
            }

            if (dragging_) {
                // Drag mode — send mouseDragged.
                CGEventRef ev = CGEventCreateMouseEvent(
                    event_source_, kCGEventLeftMouseDragged, pos,
                    kCGMouseButtonLeft);
                if (ev) {
                    CGEventPost(kCGHIDEventTap, ev);
                    CFRelease(ev);
                }
            } else {
                // Not dragging yet — just move cursor.
                CGEventRef ev = CGEventCreateMouseEvent(
                    event_source_, kCGEventMouseMoved, pos,
                    kCGMouseButtonLeft);
                if (ev) {
                    CGEventPost(kCGHIDEventTap, ev);
                    CFRelease(ev);
                }
            }
        } else if (active_finger_count_ >= 2 && slot->has_prev) {
            // ---- Two+ fingers → scroll gesture (natural scrolling) ----
            int32_t dx_frac = static_cast<int32_t>(x) - static_cast<int32_t>(slot->prev_x);
            int32_t dy_frac = static_cast<int32_t>(y) - static_cast<int32_t>(slot->prev_y);

            CGRect scroll_bounds = CGDisplayBounds(display_id_);
            double dx_pts = (static_cast<double>(dx_frac) * scroll_bounds.size.width) / 65535.0;
            double dy_pts = (static_cast<double>(dy_frac) * scroll_bounds.size.height) / 65535.0;

            // Natural scrolling (like Mac trackpad):
            //   Finger moves DOWN  → content scrolls DOWN  → positive scroll delta.
            //   Finger moves RIGHT → content scrolls RIGHT → positive scroll delta.
            double sx = dx_pts * kScrollScale;
            double sy = dy_pts * kScrollScale;

            // Update velocity with exponential moving average for momentum.
            scroll_vel_x_ = scroll_vel_x_ * (1.0 - kVelocitySmoothing) + sx * kVelocitySmoothing;
            scroll_vel_y_ = scroll_vel_y_ * (1.0 - kVelocitySmoothing) + sy * kVelocitySmoothing;

            int32_t scroll_y = static_cast<int32_t>(sy);
            int32_t scroll_x = static_cast<int32_t>(sx);

            if (scroll_x != 0 || scroll_y != 0) {
                post_scroll_event(scroll_y, scroll_x);
                two_finger_tap_candidate_ = false;
                two_finger_scrolled_ = true;
            }
        }

        slot->has_prev = true;
        break;
    }

    // ==================================================================
    //  FINGER UP / CANCEL
    // ==================================================================
    case kTouchUp:
    case kTouchCancel: {
        FingerSlot* slot = find_finger(ptr_id);
        if (!slot) break;

        bool was_last_finger = (active_finger_count_ == 1);
        free_finger(ptr_id);

        if (was_last_finger) {
            CGPoint pos = map_to_screen(x, y);

            if (dragging_) {
                // End drag — send mouseUp.
                CGEventRef up = CGEventCreateMouseEvent(
                    event_source_, kCGEventLeftMouseUp, pos,
                    kCGMouseButtonLeft);
                if (up) {
                    CGEventPost(kCGHIDEventTap, up);
                    CFRelease(up);
                }
                dragging_ = false;
            } else if (tap_candidate_ && !ever_had_two_fingers_) {
                // Single-finger tap: generate click (mouseDown + mouseUp).
                CGEventRef down = CGEventCreateMouseEvent(
                    event_source_, kCGEventLeftMouseDown, pos,
                    kCGMouseButtonLeft);
                CGEventRef up = CGEventCreateMouseEvent(
                    event_source_, kCGEventLeftMouseUp, pos,
                    kCGMouseButtonLeft);
                if (down) {
                    CGEventPost(kCGHIDEventTap, down);
                    CFRelease(down);
                }
                if (up) {
                    CGEventPost(kCGHIDEventTap, up);
                    CFRelease(up);
                }
            }
        }

        if (active_finger_count_ == 0) {
            // All fingers lifted.
            if (two_finger_tap_candidate_ && !two_finger_scrolled_) {
                // Two-finger tap with no scroll → right-click (context menu).
                CGPoint pos = map_to_screen(x, y);
                CGEventRef down = CGEventCreateMouseEvent(
                    event_source_, kCGEventRightMouseDown, pos,
                    kCGMouseButtonRight);
                CGEventRef up = CGEventCreateMouseEvent(
                    event_source_, kCGEventRightMouseUp, pos,
                    kCGMouseButtonRight);
                if (down) {
                    CGEventPost(kCGHIDEventTap, down);
                    CFRelease(down);
                }
                if (up) {
                    CGEventPost(kCGHIDEventTap, up);
                    CFRelease(up);
                }
            } else if (ever_had_two_fingers_ && two_finger_scrolled_) {
                // Was scrolling → start momentum.
                start_momentum_locked();
            }
            tap_candidate_ = false;
            ever_had_two_fingers_ = false;
            two_finger_tap_candidate_ = false;
            two_finger_scrolled_ = false;
        }
        break;
    }

    // ==================================================================
    //  HOVER
    // ==================================================================
    case kTouchHover: {
        if (active_finger_count_ == 0) {
            CGPoint pos = map_to_screen(x, y);
            CGEventRef ev = CGEventCreateMouseEvent(
                event_source_, kCGEventMouseMoved, pos, kCGMouseButtonLeft);
            if (ev) {
                CGEventPost(kCGHIDEventTap, ev);
                CFRelease(ev);
            }
        }
        break;
    }

    case kTouchHoverLeave:
    default:
        break;
    }

    return true;
}

// ---------------------------------------------------------------------------
// inject_pen  —  stylus → Wacom-like tablet events
// ---------------------------------------------------------------------------

bool MacTouchInjector::inject_pen(uint8_t action, uint8_t /*ptr_id*/,
                                  uint8_t tool_type, uint32_t buttons,
                                  uint16_t x, uint16_t y, uint16_t pressure,
                                  uint16_t distance, uint16_t tilt,
                                  uint16_t rotation) {
    (void)distance;

    std::lock_guard<std::mutex> lock(mutex_);
    if (!event_source_) return false;

    // Cancel-all: force-release everything.
    if (action == kTouchCancelAll) {
        release_all_pen_locked();
        return true;
    }

    CGPoint pos = map_to_screen(x, y);

    // ---- Pressure normalization ----
    double pressure_norm = static_cast<double>(pressure) / 65535.0;

    // ---- Tilt decomposition ----
    // Protocol: tilt = 0..90° from vertical, rotation = 0..359°.
    // macOS:    tiltX/Y = 0.0..1.0 (normalized by ~45°).
    //           tiltY is INVERTED (confirmed by Chromium).
    double tilt_x = 0.0;
    double tilt_y = 0.0;
    if (tilt != DS_TOUCH_TILT_UNKNOWN && rotation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        double tilt_rad = static_cast<double>(tilt) * M_PI / 180.0;
        double rot_rad  = static_cast<double>(rotation) * M_PI / 180.0;
        double r = std::sin(tilt_rad);
        double z = std::cos(tilt_rad);

        // Decompose tilt into X/Y components, normalized so 45° ≈ 1.0.
        tilt_x = std::clamp(std::atan2(std::sin(-rot_rad) * r, z) / (M_PI / 4.0),
                            -1.0, 1.0);
        // NOTE: tilt_y inversion is applied inside post_tablet_mouse_event.
        tilt_y = std::clamp(std::atan2(std::cos(-rot_rad) * r, z) / (M_PI / 4.0),
                            -1.0, 1.0);
    }

    // ---- Rotation ----
    double rot_deg = (rotation != DS_TOUCH_ORIENTATION_UNKNOWN)
                         ? static_cast<double>(rotation % 360)
                         : 0.0;

    // ---- Button mapping ----
    // Protocol buttons → tablet point buttons (bit 0 = primary).
    int tablet_buttons = 0;
    if (buttons & 0x02) tablet_buttons |= 2; // right / barrel
    if (buttons & 0x04) tablet_buttons |= 4; // middle

    // ---- Tool type change detection ----
    if (pen_in_proximity_ && tool_type != pen_tool_type_ &&
        tool_type != DS_TOUCH_TOOL_UNKNOWN) {
        // Tool switched (e.g. pen ↔ eraser): exit old, enter new.
        post_proximity_event(false, pen_tool_type_);
        pen_in_proximity_ = false;
    }

    // ---- Action dispatch ----
    switch (action) {
        case kTouchHover: {
            // Ensure proximity.
            if (!pen_in_proximity_) {
                pen_tool_type_ = tool_type;
                post_proximity_event(true, pen_tool_type_);
                pen_in_proximity_ = true;
            } else {
                // Re-send proximity if idle >200ms.
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_proximity_time_).count();
                if (elapsed > kProximityRefreshMs) {
                    post_proximity_event(true, pen_tool_type_);
                }
            }
            last_pen_pos_ = pos;
            post_tablet_mouse_event(kCGEventMouseMoved, pos,
                                    pressure_norm, tilt_x, tilt_y,
                                    rot_deg, tablet_buttons);
            break;
        }

        case kTouchDown: {
            if (!pen_in_proximity_) {
                pen_tool_type_ = tool_type;
                post_proximity_event(true, pen_tool_type_);
                pen_in_proximity_ = true;
            }
            pen_down_ = true;
            last_pen_pos_ = pos;

            // Add tip-down to buttons.
            tablet_buttons |= 1;

            post_tablet_mouse_event(kCGEventLeftMouseDown, pos,
                                    pressure_norm, tilt_x, tilt_y,
                                    rot_deg, tablet_buttons);
            break;
        }

        case kTouchMove: {
            last_pen_pos_ = pos;

            // Re-send proximity if idle >200ms.
            if (pen_in_proximity_) {
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    now - last_proximity_time_).count();
                if (elapsed > kProximityRefreshMs) {
                    post_proximity_event(true, pen_tool_type_);
                }
            }

            if (pen_down_) {
                tablet_buttons |= 1;
                post_tablet_mouse_event(kCGEventLeftMouseDragged, pos,
                                        pressure_norm, tilt_x, tilt_y,
                                        rot_deg, tablet_buttons);
            } else {
                post_tablet_mouse_event(kCGEventMouseMoved, pos,
                                        pressure_norm, tilt_x, tilt_y,
                                        rot_deg, tablet_buttons);
            }
            break;
        }

        case kTouchUp:
        case kTouchCancel: {
            if (pen_down_) {
                last_pen_pos_ = pos;
                post_tablet_mouse_event(kCGEventLeftMouseUp, pos,
                                        0.0, tilt_x, tilt_y,
                                        rot_deg, 0);
                pen_down_ = false;
            }
            break;
        }

        case kTouchHoverLeave: {
            if (pen_down_) {
                // Force mouse-up before leaving proximity.
                post_tablet_mouse_event(kCGEventLeftMouseUp, last_pen_pos_,
                                        0.0, 0.0, 0.0, 0.0, 0);
                pen_down_ = false;
            }
            if (pen_in_proximity_) {
                post_proximity_event(false, pen_tool_type_);
                pen_in_proximity_ = false;
            }
            break;
        }

        case kTouchButtonOnly: {
            // Button change without movement.
            if (pen_down_) {
                tablet_buttons |= 1;
                post_tablet_mouse_event(kCGEventLeftMouseDragged, last_pen_pos_,
                                        pressure_norm, tilt_x, tilt_y,
                                        rot_deg, tablet_buttons);
            }
            break;
        }

        default:
            break;
    }

    return true;
}

// ---------------------------------------------------------------------------
// Cleanup helpers
// ---------------------------------------------------------------------------

void MacTouchInjector::release_all_touch_locked() {
    // Release drag if active.
    if (dragging_ && event_source_) {
        CGPoint pos = map_to_screen(tap_start_x_, tap_start_y_);
        CGEventRef up = CGEventCreateMouseEvent(
            event_source_, kCGEventLeftMouseUp, pos, kCGMouseButtonLeft);
        if (up) {
            CGEventPost(kCGHIDEventTap, up);
            CFRelease(up);
        }
    }
    stop_momentum_locked();
    for (auto& f : fingers_) {
        f = {};
    }
    active_finger_count_ = 0;
    tap_candidate_ = false;
    dragging_ = false;
    ever_had_two_fingers_ = false;
    two_finger_tap_candidate_ = false;
    two_finger_scrolled_ = false;
    scroll_vel_x_ = 0.0;
    scroll_vel_y_ = 0.0;
}

void MacTouchInjector::release_all_pen_locked() {
    if (pen_down_ && event_source_) {
        post_tablet_mouse_event(kCGEventLeftMouseUp, last_pen_pos_,
                                0.0, 0.0, 0.0, 0.0, 0);
        pen_down_ = false;
    }
    if (pen_in_proximity_) {
        post_proximity_event(false, pen_tool_type_);
        pen_in_proximity_ = false;
    }
}

void MacTouchInjector::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_momentum_ = true;
        stop_momentum_locked();
        release_all_touch_locked();
        release_all_pen_locked();
    }

    // Join momentum thread outside the lock.
    if (momentum_thread_.joinable()) {
        momentum_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stop_momentum_ = false;

    if (event_source_) {
        CFRelease(event_source_);
        event_source_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }
}

} // namespace droidscreen
