/*
 * DroidScreen Windows - Touch injector implementation
 *
 * Uses CreateSyntheticPointerDevice / InjectSyntheticPointerInput
 * to inject touch events into the Windows input pipeline.
 *
 * The API requires all active pointers to be submitted together in
 * a single InjectSyntheticPointerInput call. We maintain per-pointer
 * state and rebuild the full POINTER_TYPE_INFO array on each injection.
 *
 * Pointer lifecycle:
 *   DOWN  -> new pointer becomes active
 *   MOVE  -> update position of active pointer
 *   UP    -> pointer goes inactive
 *   CANCEL-> pointer goes inactive (cancellation)
 */

#include "touch_injector_win.h"

extern "C" {
#include "droidscreen/touch.h"
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <synchapi.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

// The SyntheticPointerDevice API is available in Windows 10 1809+.
// We link against user32.dll which exports these functions.

extern "C" {

// Touch action constants matching the protocol.
// (These shadow ds_touch_action_t for convenience.)
static constexpr uint8_t kTouchDown   = 0;
static constexpr uint8_t kTouchMove   = 1;
static constexpr uint8_t kTouchUp     = 2;
static constexpr uint8_t kTouchCancel = 3;
static constexpr uint8_t kTouchHover = 4;
static constexpr uint8_t kTouchHoverLeave = 5;
static constexpr uint8_t kTouchButtonOnly = 6;

} // extern "C"

namespace droidscreen {

constexpr auto kRepeatInterval = std::chrono::milliseconds(50);
constexpr uint32_t kDefaultPressure = 512;
constexpr int32_t kContactRadius = 4;
constexpr double kPi = 3.14159265358979323846;

WinTouchInjector::WinTouchInjector() = default;

WinTouchInjector::~WinTouchInjector() {
    shutdown();
}

bool WinTouchInjector::init(uint32_t w, uint32_t h,
                             int32_t ox, int32_t oy) {
    shutdown();

    surface_w_ = w;
    surface_h_ = h;
    offset_x_  = ox;
    offset_y_  = oy;

    // Cache virtual desktop metrics for coordinate mapping.
    vdesk_x_      = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vdesk_y_      = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vdesk_width_  = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vdesk_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vdesk_width_ == 0 || vdesk_height_ == 0) {
        fprintf(stderr, "[touch] virtual desktop metrics are zero\n");
        return false;
    }

    // Create a synthetic pointer device for touch input.
    device_ = CreateSyntheticPointerDevice(
        PT_TOUCH,
        kMaxContacts,
        POINTER_FEEDBACK_DEFAULT);

    if (!device_) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] CreateSyntheticPointerDevice failed: %lu\n",
                err);
        return false;
    }

    // Reset pointer states.
    for (auto& p : pointers_) {
        p = {};
    }

    stop_repeat_ = false;
    repeat_reset_pending_ = false;
    repeat_thread_ = std::thread(&WinTouchInjector::repeat_loop, this);

    fprintf(stderr, "[touch] injector initialized: surface=%ux%u offset=(%d,%d) "
            "vdesk=%dx%d+%d+%d\n",
            w, h, ox, oy,
            vdesk_width_, vdesk_height_, vdesk_x_, vdesk_y_);
    return true;
}

bool WinTouchInjector::inject(uint8_t action, uint8_t ptr_id,
                               uint16_t x, uint16_t y, uint16_t pressure,
                               uint16_t touch_major, uint16_t touch_minor,
                               uint16_t orientation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!device_) return false;
    int slot = find_slot_by_external_id_locked(ptr_id);
    if (slot < 0 && (action == kTouchDown || action == kTouchHover)) {
        slot = allocate_slot_locked(ptr_id);
        if (slot < 0) {
            fprintf(stderr, "[touch] no free touch slots; cancelling all active touches\n");
            cancel_all_locked();
            slot = allocate_slot_locked(ptr_id);
            if (slot < 0) {
                return false;
            }
        }
    } else if (slot < 0) {
        return true;
    }

    // Convert fractional coordinates (0..65535) to pixel coordinates
    // on the surface, then offset to virtual desktop coordinates.
    uint32_t local_x = 0;
    uint32_t local_y = 0;
    if (surface_w_ > 1) {
        local_x = static_cast<uint32_t>(
            (static_cast<uint64_t>(x) * (surface_w_ - 1)) / 65535);
    }
    if (surface_h_ > 1) {
        local_y = static_cast<uint32_t>(
            (static_cast<uint64_t>(y) * (surface_h_ - 1)) / 65535);
    }

    // InjectSyntheticPointerInput expects coordinates relative to the
    // top-left of the virtual screen, not the primary monitor origin.
    int32_t pixel_x = (offset_x_ - vdesk_x_) + static_cast<int32_t>(local_x);
    int32_t pixel_y = (offset_y_ - vdesk_y_) + static_cast<int32_t>(local_y);

    if (pixel_x < 0) pixel_x = 0;
    if (pixel_y < 0) pixel_y = 0;
    if (pixel_x >= vdesk_width_)  pixel_x = vdesk_width_ - 1;
    if (pixel_y >= vdesk_height_) pixel_y = vdesk_height_ - 1;

    // Update per-pointer state.
    auto& ptr = pointers_[slot];

    switch (action) {
        case kTouchDown:
            ptr.assigned      = true;
            ptr.present       = true;
            ptr.in_range      = true;
            ptr.in_contact    = true;
            ptr.edge_update   = false;
            ptr.edge_down     = true;
            ptr.edge_up       = false;
            ptr.edge_canceled = false;
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.pressure      = pressure;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            break;

        case kTouchMove:
            if (!ptr.present) {
                // Stale move after an up -- ignore.
                return true;
            }
            ptr.present       = true;
            ptr.in_range      = true;
            ptr.in_contact    = true;
            ptr.edge_update   = true;
            ptr.edge_down     = false;
            ptr.edge_up       = false;
            ptr.edge_canceled = false;
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.pressure      = pressure;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            break;

        case kTouchHover:
            ptr.assigned      = true;
            ptr.present       = true;
            ptr.in_range      = true;
            ptr.in_contact    = false;
            ptr.edge_update   = true;
            ptr.edge_down     = false;
            ptr.edge_up       = false;
            ptr.edge_canceled = false;
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.pressure      = 0;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            break;

        case kTouchUp:
            if (!ptr.present) {
                return true;
            }
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.pressure      = 0;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            ptr.in_contact    = false;
            ptr.in_range      = false;
            ptr.edge_update   = false;
            ptr.edge_down     = false;
            ptr.edge_up       = true;
            ptr.edge_canceled = false;
            break;

        case kTouchCancel:
            if (!ptr.present) {
                return true;
            }
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.pressure      = 0;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            ptr.edge_update   = !ptr.in_contact;
            ptr.edge_down     = false;
            ptr.edge_up       = ptr.in_contact;
            ptr.edge_canceled = true;
            ptr.in_contact    = false;
            ptr.in_range      = false;
            break;

        case kTouchHoverLeave:
            if (!ptr.present) {
                return true;
            }
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            ptr.pressure      = 0;
            ptr.in_contact    = false;
            ptr.in_range      = false;
            ptr.edge_update   = true;
            ptr.edge_down     = false;
            ptr.edge_up       = false;
            ptr.edge_canceled = false;
            break;

        case kTouchButtonOnly:
            if (!ptr.present) {
                return true;
            }
            ptr.pixel_x       = pixel_x;
            ptr.pixel_y       = pixel_y;
            ptr.touch_major   = touch_major;
            ptr.touch_minor   = touch_minor;
            ptr.orientation   = orientation;
            ptr.edge_update   = true;
            ptr.edge_down     = false;
            ptr.edge_up       = false;
            ptr.edge_canceled = false;
            break;

        default:
            fprintf(stderr, "[touch] unknown action %u\n", action);
            return false;
    }

    if (!inject_current_state_locked()) {
        return false;
    }

    repeat_reset_pending_ = true;
    repeat_cv_.notify_one();
    return true;
}

bool WinTouchInjector::inject_current_state_locked() {
    POINTER_TYPE_INFO infos[kMaxContacts] = {};
    uint32_t count = 0;

    const int32_t surface_left = offset_x_ - vdesk_x_;
    const int32_t surface_top = offset_y_ - vdesk_y_;
    const int32_t surface_right = surface_left +
        static_cast<int32_t>(surface_w_ > 0 ? surface_w_ - 1 : 0);
    const int32_t surface_bottom = surface_top +
        static_cast<int32_t>(surface_h_ > 0 ? surface_h_ - 1 : 0);

    for (uint32_t i = 0; i < kMaxContacts; i++) {
        const auto& p = pointers_[i];
        if (!p.present) {
            continue;
        }

        const uint32_t flags = pointer_flags_locked(p);
        if (flags == POINTER_FLAG_NONE) {
            continue;
        }

        auto& info = infos[count++];
        memset(&info, 0, sizeof(info));
        info.type = PT_TOUCH;

        POINTER_INFO& pi = info.touchInfo.pointerInfo;
        pi.pointerType = PT_TOUCH;
        pi.pointerId = i;
        pi.frameId = 0;
        pi.pointerFlags = flags;
        pi.ptPixelLocation.x = p.pixel_x;
        pi.ptPixelLocation.y = p.pixel_y;
        pi.ptPixelLocationRaw = pi.ptPixelLocation;

        info.touchInfo.touchFlags = TOUCH_FLAG_NONE;
        info.touchInfo.touchMask = TOUCH_MASK_NONE;

        if (p.in_contact) {
            info.touchInfo.touchMask |= TOUCH_MASK_PRESSURE | TOUCH_MASK_CONTACTAREA;
            info.touchInfo.pressure = p.pressure != 0
                ? static_cast<UINT32>((static_cast<uint64_t>(p.pressure) * 1024) / 65535)
                : kDefaultPressure;

            int32_t half_width = kContactRadius;
            int32_t half_height = kContactRadius;
            if (p.touch_major != 0 && p.touch_minor != 0) {
                const double major_pixels =
                    (static_cast<double>(p.touch_major) * static_cast<double>(surface_w_)) / 65535.0;
                const double minor_pixels =
                    (static_cast<double>(p.touch_minor) * static_cast<double>(surface_h_)) / 65535.0;
                const double rotation_degrees =
                    p.orientation == DS_TOUCH_ORIENTATION_UNKNOWN ? 45.0
                                                                  : static_cast<double>(p.orientation % 360);
                const double major_axis = rotation_degrees * (kPi / 180.0);
                const double minor_axis = major_axis + (kPi / 2.0);
                const double contact_width =
                    std::abs(std::cos(major_axis) * major_pixels) +
                    std::abs(std::cos(minor_axis) * minor_pixels);
                const double contact_height =
                    std::abs(std::sin(major_axis) * major_pixels) +
                    std::abs(std::sin(minor_axis) * minor_pixels);
                half_width = std::max<int32_t>(kContactRadius,
                    static_cast<int32_t>(std::ceil(contact_width / 2.0)));
                half_height = std::max<int32_t>(kContactRadius,
                    static_cast<int32_t>(std::ceil(contact_height / 2.0)));
            }

            info.touchInfo.rcContact.left =
                std::max(surface_left, p.pixel_x - half_width);
            info.touchInfo.rcContact.top =
                std::max(surface_top, p.pixel_y - half_height);
            info.touchInfo.rcContact.right =
                std::min(surface_right, p.pixel_x + half_width);
            info.touchInfo.rcContact.bottom =
                std::min(surface_bottom, p.pixel_y + half_height);
            info.touchInfo.rcContactRaw = info.touchInfo.rcContact;

            if (p.orientation != DS_TOUCH_ORIENTATION_UNKNOWN) {
                info.touchInfo.touchMask |= TOUCH_MASK_ORIENTATION;
                info.touchInfo.orientation = p.orientation % 360;
            }
        }
    }

    if (count == 0) {
        clear_edge_flags_locked();
        return true;
    }

    if (!InjectSyntheticPointerInput(device_, infos, count)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput failed: %lu\n", err);
        return false;
    }

    clear_edge_flags_locked();
    return true;
}

bool WinTouchInjector::has_repeatable_touches_locked() const {
    for (const auto& ptr : pointers_) {
        if (ptr.present) {
            return true;
        }
    }
    return false;
}

void WinTouchInjector::clear_edge_flags_locked() {
    for (auto& ptr : pointers_) {
        if (!ptr.assigned) {
            continue;
        }

        ptr.edge_update = false;
        ptr.edge_down = false;
        ptr.edge_up = false;
        ptr.edge_canceled = false;

        if (!ptr.in_range && !ptr.in_contact) {
            ptr.assigned = false;
            ptr.present = false;
            ptr.external_id = 0;
            ptr.pressure = 0;
            ptr.touch_major = 0;
            ptr.touch_minor = 0;
            ptr.orientation = DS_TOUCH_ORIENTATION_UNKNOWN;
        }
    }
}

void WinTouchInjector::cancel_all_locked() {
    bool has_active = false;
    for (auto& ptr : pointers_) {
        if (!ptr.assigned) {
            continue;
        }
        const bool was_in_contact = ptr.in_contact;
        ptr.in_contact = false;
        ptr.in_range = false;
        ptr.edge_down = false;
        ptr.edge_update = !was_in_contact;
        ptr.edge_up = was_in_contact;
        ptr.edge_canceled = true;
        ptr.pressure = 0;
        has_active = true;
    }

    if (has_active) {
        inject_current_state_locked();
    }
}

int WinTouchInjector::find_slot_by_external_id_locked(uint8_t external_id) const {
    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        const auto& ptr = pointers_[i];
        if (ptr.assigned && ptr.external_id == external_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int WinTouchInjector::allocate_slot_locked(uint8_t external_id) {
    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        auto& ptr = pointers_[i];
        if (!ptr.assigned) {
            ptr = {};
            ptr.assigned = true;
            ptr.external_id = external_id;
            ptr.orientation = DS_TOUCH_ORIENTATION_UNKNOWN;
            return static_cast<int>(i);
        }
    }
    return -1;
}

uint32_t WinTouchInjector::pointer_flags_locked(const PointerState& ptr) const {
    uint32_t flags = POINTER_FLAG_NONE;
    if (ptr.in_range) {
        flags |= POINTER_FLAG_INRANGE;
    }
    if (ptr.in_contact) {
        flags |= POINTER_FLAG_INCONTACT;
    }
    if (ptr.edge_update) {
        flags |= POINTER_FLAG_UPDATE;
    }
    if (ptr.edge_down) {
        flags |= POINTER_FLAG_DOWN;
    }
    if (ptr.edge_up) {
        flags |= POINTER_FLAG_UP;
    }
    if (ptr.edge_canceled) {
        flags |= POINTER_FLAG_CANCELED;
    }
    return flags;
}

void WinTouchInjector::repeat_loop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stop_repeat_) {
        repeat_cv_.wait(lock, [this] {
            return stop_repeat_ || has_repeatable_touches_locked();
        });
        if (stop_repeat_) {
            break;
        }

        repeat_reset_pending_ = false;
        if (repeat_cv_.wait_for(lock, kRepeatInterval, [this] {
                return stop_repeat_ || repeat_reset_pending_ || !has_repeatable_touches_locked();
            })) {
            if (stop_repeat_) {
                break;
            }
            continue;
        }

        inject_current_state_locked();
    }
}

void WinTouchInjector::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_repeat_ = true;
        repeat_reset_pending_ = true;
        repeat_cv_.notify_all();
    }

    if (repeat_thread_.joinable()) {
        repeat_thread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    stop_repeat_ = false;
    repeat_reset_pending_ = false;
    for (auto& p : pointers_) {
        p = {};
    }

    if (device_) {
        DestroySyntheticPointerDevice(device_);
        device_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }
}

} // namespace droidscreen
