/*
 * DroidScreen macOS - Touch and pen injection implementation
 *
 * Touch (finger) → touchscreen-style input via CGEvent.
 *   A small per-gesture state machine classifies each contact:
 *   - 1 finger, lifted quickly without moving   → left click.
 *     Repeated taps within the system double-click interval carry
 *     kCGMouseEventClickState 2, 3, … (double / triple click).
 *   - 1 finger, moves past the slop before the long-press time
 *                                               → scroll, 1:1 with finger.
 *   - 1 finger, held still past the long-press time, then moved
 *                                               → left-button drag from
 *                                                 the touch-down point.
 *   - 1 finger, held still past the long-press time, then lifted
 *                                               → right click.
 *   - 2 fingers, centroid moves past the slop   → scroll.
 *   - 2 fingers, quick tap                      → right click.
 *   - 3+ fingers                                → gesture cancelled.
 *
 *   Scrolls are continuous pixel scroll events with Began / Changed /
 *   Ended phases; on lift, momentum events (MomentumPhase Begin /
 *   Continue / End) are generated from the release velocity measured
 *   with the Android-side timestamps.  Phases are always closed.
 *
 * Pen (stylus) → mouse events with tablet subtype and fields,
 *                emulating a Wacom tablet for full pressure/tilt
 *                support in drawing applications.
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

#import <AppKit/AppKit.h>

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

// ---- Gesture thresholds (distances in display points, times in ms) ----

// Movement below this is finger jitter: still a tap / long-press.
static constexpr double kTouchSlopPts = 10.0;
// Holding still this long turns the touch into a long-press.
static constexpr int32_t kLongPressMs = 450;
// A two-finger tap must be fully released within this time.
static constexpr int32_t kTwoTapMaxMs = 350;
// Consecutive taps further apart than this are not a multi-click.
static constexpr double kMultiClickMaxDistPts = 20.0;
// Movements smaller than this (finger tremor) are scrolled but do not
// count as a velocity sample, so a resting finger reads as stopped.
static constexpr double kVelocitySampleMinPts = 0.5;
// Scroll axis lock: lock to one axis when it dominates by this ratio
// at the moment the scroll starts.
static constexpr double kAxisLockRatio = 2.0;

// ---- Momentum ----

// Release velocity is measured over the last part of the swipe.
static constexpr int32_t kVelocityWindowMs = 80;
// If the finger rested this long before lifting, there is no fling.
static constexpr int32_t kVelocityStaleMs = 60;
// Velocity is measured over at least this span, so two packets of
// the same frame cannot produce an absurd release speed.
static constexpr int32_t kVelocityMinSpanMs = 16;
// Release speed is clamped to this (pts/s).
static constexpr double kMomentumMaxVelocity = 8000.0;
// Below this release speed no momentum is started (pts/s).
static constexpr double kMomentumMinStartVelocity = 200.0;
// Momentum ends when the speed decays below this (pts/s).
static constexpr double kMomentumStopVelocity = 20.0;
// Exponential decay time constant (s), close to the trackpad feel.
static constexpr double kMomentumTau = 0.325;
// Momentum tick (~120 Hz).
static constexpr auto kMomentumInterval = std::chrono::milliseconds(8);

// Signed difference between two wrapping Android millisecond stamps.
static inline int32_t elapsed_ms(uint32_t now, uint32_t then) {
    return static_cast<int32_t>(now - then);
}

static inline double distance(CGPoint a, CGPoint b) {
    return std::hypot(a.x - b.x, a.y - b.y);
}

namespace droidscreen {

MacTouchInjector::MacTouchInjector() = default;

MacTouchInjector::~MacTouchInjector() {
    shutdown();
}

bool MacTouchInjector::init(uint32_t /*w*/, uint32_t /*h*/,
                            int32_t /*ox*/, int32_t /*oy*/) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Close any gesture still open (button held, scroll/momentum phase).
    release_all_touch_locked();
    click_count_ = 0;
    double_click_ms_ = [NSEvent doubleClickInterval] * 1000.0;

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

    if (!momentum_thread_.joinable()) {
        worker_stop_ = false;
        momentum_thread_ = std::thread(&MacTouchInjector::momentum_loop, this);
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
// Mouse helpers
// ---------------------------------------------------------------------------

void MacTouchInjector::post_mouse(CGEventType type, CGPoint pos,
                                  CGMouseButton button, int click_state) {
    CGEventRef ev = CGEventCreateMouseEvent(event_source_, type, pos, button);
    if (!ev) return;
    if (click_state > 0) {
        CGEventSetIntegerValueField(ev, kCGMouseEventClickState, click_state);
    }
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

void MacTouchInjector::click_locked(CGPoint pos, uint32_t down_ts) {
    const int32_t since_last = elapsed_ms(down_ts, last_click_ts_);
    if (click_count_ > 0 && since_last >= 0 &&
        since_last <= double_click_ms_ &&
        distance(pos, last_click_pos_) <= kMultiClickMaxDistPts) {
        click_count_++;
    } else {
        click_count_ = 1;
    }
    last_click_ts_ = down_ts;
    last_click_pos_ = pos;

    post_mouse(kCGEventLeftMouseDown, pos, kCGMouseButtonLeft, click_count_);
    post_mouse(kCGEventLeftMouseUp, pos, kCGMouseButtonLeft, click_count_);
}

void MacTouchInjector::right_click_locked(CGPoint pos) {
    click_count_ = 0;
    post_mouse(kCGEventRightMouseDown, pos, kCGMouseButtonRight, 1);
    post_mouse(kCGEventRightMouseUp, pos, kCGMouseButtonRight, 1);
}

void MacTouchInjector::start_drag_locked(CGPoint to) {
    click_count_ = 0;
    post_mouse(kCGEventLeftMouseDown, start_pos_, kCGMouseButtonLeft, 1);
    post_mouse(kCGEventLeftMouseDragged, to, kCGMouseButtonLeft, 1);
}

// ---------------------------------------------------------------------------
// Scroll (continuous, phased)
// ---------------------------------------------------------------------------

void MacTouchInjector::post_scroll_locked(double dx, double dy,
                                          uint32_t phase,
                                          uint32_t momentum_phase) {
    if (!event_source_) return;

    // Carry the sub-pixel remainder so slow swipes still scroll.
    scroll_rem_x_ += dx;
    scroll_rem_y_ += dy;
    const int32_t ix = static_cast<int32_t>(std::trunc(scroll_rem_x_));
    const int32_t iy = static_cast<int32_t>(std::trunc(scroll_rem_y_));
    scroll_rem_x_ -= ix;
    scroll_rem_y_ -= iy;

    // Phase transitions are always posted; plain updates only when
    // they carry a whole-pixel delta.
    const bool is_update = phase == kCGScrollPhaseChanged ||
                           momentum_phase == kCGMomentumScrollPhaseContinue;
    if (is_update && ix == 0 && iy == 0) return;

    // Positive delta = finger moved down/right = content follows it.
    CGEventRef ev = CGEventCreateScrollWheelEvent2(
        event_source_, kCGScrollEventUnitPixel, 2, iy, ix, 0);
    if (!ev) return;
    CGEventSetIntegerValueField(ev, kCGScrollWheelEventIsContinuous, 1);
    CGEventSetIntegerValueField(ev, kCGScrollWheelEventScrollPhase, phase);
    CGEventSetIntegerValueField(ev, kCGScrollWheelEventMomentumPhase,
                                momentum_phase);
    // Keep targeting the view under the touch-down point.
    CGEventSetLocation(ev, scroll_anchor_);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

void MacTouchInjector::begin_scroll_locked(CGPoint anchor, CGPoint offset,
                                           uint32_t ts) {
    stop_momentum_locked();
    click_count_ = 0;

    // Scroll events go to the view under the cursor: park it on the anchor.
    scroll_anchor_ = anchor;
    post_mouse(kCGEventMouseMoved, anchor, kCGMouseButtonLeft, 0);

    const double ax = std::abs(offset.x);
    const double ay = std::abs(offset.y);
    if (ay > ax * kAxisLockRatio) {
        axis_lock_ = AxisLock::Y;
    } else if (ax > ay * kAxisLockRatio) {
        axis_lock_ = AxisLock::X;
    } else {
        axis_lock_ = AxisLock::None;
    }

    scroll_rem_x_ = 0.0;
    scroll_rem_y_ = 0.0;
    scroll_pos_x_ = 0.0;
    scroll_pos_y_ = 0.0;
    scroll_sample_count_ = 0;
    scroll_sample_head_ = 0;
    // The slop offset accumulated since the gesture started.
    scroll_samples_[0] = {start_ts_, 0.0, 0.0};
    scroll_sample_count_ = 1;
    scroll_sample_head_ = 1;

    post_scroll_locked(0.0, 0.0, kCGScrollPhaseBegan,
                       kCGMomentumScrollPhaseNone);
    // Apply the movement that crossed the slop, so content tracks the
    // finger 1:1 from the touch-down point (and quick flicks count).
    scroll_by_locked(offset.x, offset.y, ts);
}

void MacTouchInjector::scroll_by_locked(double dx, double dy, uint32_t ts) {
    if (axis_lock_ == AxisLock::Y) dx = 0.0;
    if (axis_lock_ == AxisLock::X) dy = 0.0;
    if (dx == 0.0 && dy == 0.0) return;

    scroll_pos_x_ += dx;
    scroll_pos_y_ += dy;

    // A resting (or trembling) finger must not refresh the newest
    // sample, or the stale-velocity check could never fire.
    const int newest_idx =
        (scroll_sample_head_ + kMaxScrollSamples - 1) % kMaxScrollSamples;
    const ScrollSample& newest = scroll_samples_[newest_idx];
    if (std::hypot(scroll_pos_x_ - newest.x, scroll_pos_y_ - newest.y) >=
        kVelocitySampleMinPts) {
        scroll_samples_[scroll_sample_head_] = {ts, scroll_pos_x_, scroll_pos_y_};
        scroll_sample_head_ = (scroll_sample_head_ + 1) % kMaxScrollSamples;
        scroll_sample_count_ = std::min(scroll_sample_count_ + 1, kMaxScrollSamples);
    }

    post_scroll_locked(dx, dy, kCGScrollPhaseChanged,
                       kCGMomentumScrollPhaseNone);
}

void MacTouchInjector::end_scroll_locked(uint32_t ts, ScrollEnd how) {
    post_scroll_locked(0.0, 0.0,
                       how == ScrollEnd::Cancelled ? kCGScrollPhaseCancelled
                                                   : kCGScrollPhaseEnded,
                       kCGMomentumScrollPhaseNone);

    if (how == ScrollEnd::Momentum && scroll_sample_count_ > 1) {
        // Release velocity from the samples inside the window that ends
        // at the newest sample.  Android timestamps, so Wi-Fi jitter in
        // packet arrival does not distort it.
        const int newest_idx =
            (scroll_sample_head_ + kMaxScrollSamples - 1) % kMaxScrollSamples;
        const ScrollSample& newest = scroll_samples_[newest_idx];
        const ScrollSample* oldest = &newest;
        for (int i = 1; i < scroll_sample_count_; ++i) {
            const int idx = (newest_idx + kMaxScrollSamples - i) % kMaxScrollSamples;
            if (elapsed_ms(newest.ts, scroll_samples_[idx].ts) > kVelocityWindowMs) break;
            oldest = &scroll_samples_[idx];
        }
        const int32_t dt_ms =
            std::max(elapsed_ms(newest.ts, oldest->ts), kVelocityMinSpanMs);
        if (elapsed_ms(ts, newest.ts) <= kVelocityStaleMs) {
            double vx = (newest.x - oldest->x) * 1000.0 / dt_ms;
            double vy = (newest.y - oldest->y) * 1000.0 / dt_ms;
            const double speed = std::hypot(vx, vy);
            if (speed > kMomentumMaxVelocity) {
                vx *= kMomentumMaxVelocity / speed;
                vy *= kMomentumMaxVelocity / speed;
            }
            start_momentum_locked(vx, vy);
        }
    }

    scroll_sample_count_ = 0;
    scroll_sample_head_ = 0;
}

// ---------------------------------------------------------------------------
// Scroll momentum (trackpad inertia)
// ---------------------------------------------------------------------------

void MacTouchInjector::stop_momentum_locked() {
    if (!momentum_active_) return;
    if (momentum_began_) {
        post_scroll_locked(0.0, 0.0, 0, kCGMomentumScrollPhaseEnd);
    }
    momentum_active_ = false;
    momentum_began_ = false;
    momentum_cv_.notify_all();
}

void MacTouchInjector::start_momentum_locked(double vx, double vy) {
    stop_momentum_locked();
    if (std::hypot(vx, vy) < kMomentumMinStartVelocity) return;

    momentum_vx_ = vx;
    momentum_vy_ = vy;
    momentum_began_ = false;
    momentum_active_ = true;
    momentum_cv_.notify_all();
}

void MacTouchInjector::momentum_loop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (true) {
        momentum_cv_.wait(lock, [this] { return worker_stop_ || momentum_active_; });
        if (worker_stop_) break;

        auto last = std::chrono::steady_clock::now();
        while (momentum_active_ && !worker_stop_) {
            momentum_cv_.wait_for(lock, kMomentumInterval, [this] {
                return !momentum_active_ || worker_stop_;
            });
            if (!momentum_active_ || worker_stop_) break;

            const auto now = std::chrono::steady_clock::now();
            const double dt = std::chrono::duration<double>(now - last).count();
            last = now;

            // Exact integral of v·e^(−t/τ) over this tick.
            const double decay = std::exp(-dt / kMomentumTau);
            const double dx = momentum_vx_ * kMomentumTau * (1.0 - decay);
            const double dy = momentum_vy_ * kMomentumTau * (1.0 - decay);
            momentum_vx_ *= decay;
            momentum_vy_ *= decay;

            // Posted under the lock so a concurrent stop can never slip
            // its End event in between Begin/Continue events.
            post_scroll_locked(dx, dy, 0,
                               momentum_began_ ? kCGMomentumScrollPhaseContinue
                                               : kCGMomentumScrollPhaseBegin);
            momentum_began_ = true;

            if (std::hypot(momentum_vx_, momentum_vy_) < kMomentumStopVelocity) {
                stop_momentum_locked();
            }
        }
    }
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
// inject_touch  —  touchscreen-style gestures (see file header)
// ---------------------------------------------------------------------------

void MacTouchInjector::cancel_gesture_locked(uint32_t ts) {
    switch (gesture_) {
    case Gesture::OneScroll:
    case Gesture::TwoScroll:
        end_scroll_locked(ts, ScrollEnd::Cancelled);
        break;
    case Gesture::OneDrag: {
        // Never leave the left button stuck down.
        FingerSlot* primary = find_finger(primary_id_);
        post_mouse(kCGEventLeftMouseUp, primary ? primary->pos : start_pos_,
                   kCGMouseButtonLeft, 1);
        break;
    }
    default:
        break;
    }
    gesture_ = Gesture::Dead;
}

bool MacTouchInjector::inject_touch(uint8_t action, uint8_t ptr_id,
                                    uint16_t x, uint16_t y,
                                    uint16_t /*pressure*/,
                                    uint16_t /*touch_major*/,
                                    uint16_t /*touch_minor*/,
                                    uint16_t /*orientation*/,
                                    uint32_t ts) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!event_source_) return false;

    if (action == kTouchCancelAll) {
        release_all_touch_locked();
        return true;
    }

    const CGPoint pos = map_to_screen(x, y);

    switch (action) {

    // ==================================================================
    //  FINGER DOWN
    // ==================================================================
    case kTouchDown: {
        if (find_finger(ptr_id)) break;  // already tracked

        FingerSlot* slot = alloc_finger(ptr_id);
        if (!slot) {
            cancel_gesture_locked(ts);
            break;
        }
        slot->pos = pos;

        switch (gesture_) {
        case Gesture::Idle:
            // A touch that stops a fling only stops it; it is not a tap.
            suppress_tap_ = momentum_active_;
            stop_momentum_locked();
            gesture_ = Gesture::OnePending;
            primary_id_ = ptr_id;
            start_pos_ = pos;
            start_ts_ = ts;
            post_mouse(kCGEventMouseMoved, pos, kCGMouseButtonLeft, 0);
            break;

        case Gesture::OneScroll:
        case Gesture::OnePending: {
            // Only a fresh two-finger touch can be a two-finger tap.
            two_tap_ok_ = gesture_ == Gesture::OnePending && !suppress_tap_;
            if (gesture_ == Gesture::OneScroll) {
                end_scroll_locked(ts, ScrollEnd::Ended);
            }
            gesture_ = Gesture::TwoPending;
            start_ts_ = ts;
            two_offset_ = CGPointZero;
            two_anchor_ = CGPointZero;
            for (const auto& f : fingers_) {
                if (!f.active) continue;
                two_anchor_.x += f.pos.x / active_finger_count_;
                two_anchor_.y += f.pos.y / active_finger_count_;
            }
            break;
        }

        case Gesture::OneDrag:
            // One extra finger (e.g. a resting palm) is ignored while
            // dragging; a third contact cancels like everywhere else.
            if (active_finger_count_ >= 3) {
                cancel_gesture_locked(ts);
            }
            break;

        case Gesture::TwoPending:
        case Gesture::TwoScroll:
        case Gesture::TwoReleasing:
            // Third finger: not a supported gesture.
            cancel_gesture_locked(ts);
            break;

        case Gesture::Dead:
            // A new contact still stops a fling left by the last gesture.
            stop_momentum_locked();
            break;
        }
        break;
    }

    // ==================================================================
    //  FINGER MOVE
    // ==================================================================
    case kTouchMove: {
        FingerSlot* slot = find_finger(ptr_id);
        if (!slot) break;

        const CGPoint prev = slot->pos;
        slot->pos = pos;
        const double dx = pos.x - prev.x;
        const double dy = pos.y - prev.y;

        switch (gesture_) {
        case Gesture::OnePending: {
            const CGPoint offset = CGPointMake(pos.x - start_pos_.x,
                                               pos.y - start_pos_.y);
            if (std::hypot(offset.x, offset.y) <= kTouchSlopPts) break;

            if (elapsed_ms(ts, start_ts_) >= kLongPressMs) {
                // Held still first → drag (select, move window, slider…).
                gesture_ = Gesture::OneDrag;
                start_drag_locked(pos);
            } else {
                // Swiped right away → scroll.
                gesture_ = Gesture::OneScroll;
                begin_scroll_locked(start_pos_, offset, ts);
            }
            break;
        }

        case Gesture::OneScroll:
            scroll_by_locked(dx, dy, ts);
            break;

        case Gesture::OneDrag:
            if (ptr_id == primary_id_) {
                post_mouse(kCGEventLeftMouseDragged, pos, kCGMouseButtonLeft, 1);
            }
            break;

        case Gesture::TwoPending: {
            // Each finger reports its own move, so the centroid moves by
            // delta / N per packet.
            two_offset_.x += dx / active_finger_count_;
            two_offset_.y += dy / active_finger_count_;
            if (std::hypot(two_offset_.x, two_offset_.y) <= kTouchSlopPts) break;

            gesture_ = Gesture::TwoScroll;
            begin_scroll_locked(two_anchor_, two_offset_, ts);
            break;
        }

        case Gesture::TwoReleasing:
            // The remaining finger sliding away means it was not a tap.
            two_offset_.x += dx;
            two_offset_.y += dy;
            if (std::hypot(two_offset_.x, two_offset_.y) > kTouchSlopPts) {
                gesture_ = Gesture::Dead;
            }
            break;

        case Gesture::TwoScroll:
            scroll_by_locked(dx / active_finger_count_,
                             dy / active_finger_count_, ts);
            break;

        default:
            break;
        }
        break;
    }

    // ==================================================================
    //  FINGER UP
    // ==================================================================
    case kTouchUp: {
        FingerSlot* slot = find_finger(ptr_id);
        if (!slot) break;
        const CGPoint prev = slot->pos;
        const int fingers_before = active_finger_count_;
        // Movement reported only by the UP packet.
        const double up_dx = pos.x - prev.x;
        const double up_dy = pos.y - prev.y;
        free_finger(ptr_id);

        switch (gesture_) {
        case Gesture::OnePending:
            // A flick that lifted beyond the slop is not a tap.
            if (!suppress_tap_ && distance(pos, start_pos_) <= kTouchSlopPts) {
                if (elapsed_ms(ts, start_ts_) < kLongPressMs) {
                    click_locked(start_pos_, start_ts_);
                } else {
                    right_click_locked(start_pos_);
                }
            }
            break;

        case Gesture::OneScroll:
            scroll_by_locked(up_dx, up_dy, ts);
            end_scroll_locked(ts, ScrollEnd::Momentum);
            break;

        case Gesture::OneDrag:
            if (ptr_id == primary_id_) {
                post_mouse(kCGEventLeftMouseUp, pos, kCGMouseButtonLeft, 1);
                gesture_ = Gesture::Dead;
            }
            break;

        case Gesture::TwoPending: {
            two_offset_.x += up_dx / fingers_before;
            two_offset_.y += up_dy / fingers_before;
            const bool still_tap =
                two_tap_ok_ && elapsed_ms(ts, start_ts_) <= kTwoTapMaxMs &&
                std::hypot(two_offset_.x, two_offset_.y) <= kTouchSlopPts;
            // two_offset_ keeps accumulating in TwoReleasing, so the
            // whole tap is held to the same slop.
            gesture_ = still_tap ? Gesture::TwoReleasing : Gesture::Dead;
            break;
        }

        case Gesture::TwoScroll:
            // Lifting either finger ends the scroll, like a trackpad.
            scroll_by_locked(up_dx / fingers_before, up_dy / fingers_before, ts);
            end_scroll_locked(ts, ScrollEnd::Momentum);
            gesture_ = Gesture::Dead;
            break;

        case Gesture::TwoReleasing:
            two_offset_.x += up_dx;
            two_offset_.y += up_dy;
            if (elapsed_ms(ts, start_ts_) <= kTwoTapMaxMs &&
                std::hypot(two_offset_.x, two_offset_.y) <= kTouchSlopPts) {
                right_click_locked(start_pos_);
            }
            gesture_ = Gesture::Dead;
            break;

        default:
            break;
        }

        if (active_finger_count_ == 0) {
            gesture_ = Gesture::Idle;
        }
        break;
    }

    // ==================================================================
    //  FINGER CANCEL  —  abort without clicking or flinging
    // ==================================================================
    case kTouchCancel: {
        if (!find_finger(ptr_id)) break;
        // A cancelled extra finger (palm rejection) does not end a drag
        // it never took part in.
        if (!(gesture_ == Gesture::OneDrag && ptr_id != primary_id_)) {
            cancel_gesture_locked(ts);
            stop_momentum_locked();
        }
        free_finger(ptr_id);
        if (active_finger_count_ == 0) {
            gesture_ = Gesture::Idle;
        }
        break;
    }

    // ==================================================================
    //  HOVER
    // ==================================================================
    case kTouchHover: {
        if (active_finger_count_ == 0) {
            post_mouse(kCGEventMouseMoved, pos, kCGMouseButtonLeft, 0);
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
    if (event_source_) {
        cancel_gesture_locked(0);
        stop_momentum_locked();
    }
    for (auto& f : fingers_) {
        f = {};
    }
    active_finger_count_ = 0;
    gesture_ = Gesture::Idle;
    suppress_tap_ = false;
    scroll_sample_count_ = 0;
    momentum_active_ = false;
    momentum_began_ = false;
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
        release_all_touch_locked();
        release_all_pen_locked();
        worker_stop_ = true;
        momentum_cv_.notify_all();
    }

    // Join the momentum worker outside the lock.
    if (momentum_thread_.joinable()) {
        momentum_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    worker_stop_ = false;

    if (event_source_) {
        CFRelease(event_source_);
        event_source_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }
}

} // namespace droidscreen
