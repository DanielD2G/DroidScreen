/*
 * DroidScreen Windows - Touch injector implementation
 *
 * Uses CreateSyntheticPointerDevice / InjectSyntheticPointerInput
 * to inject touch and pen events into the Windows input pipeline.
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
#include <cstring>

extern "C" {

static constexpr uint8_t kTouchDown = 0;
static constexpr uint8_t kTouchMove = 1;
static constexpr uint8_t kTouchUp = 2;
static constexpr uint8_t kTouchCancel = 3;
static constexpr uint8_t kTouchHover = 4;
static constexpr uint8_t kTouchHoverLeave = 5;
static constexpr uint8_t kTouchButtonOnly = 6;

} // extern "C"

namespace droidscreen {

constexpr auto kRepeatInterval = std::chrono::milliseconds(50);
constexpr uint32_t kDefaultTouchPressure = 512;
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
    offset_x_ = ox;
    offset_y_ = oy;

    vdesk_x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vdesk_y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vdesk_width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vdesk_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vdesk_width_ <= 0 || vdesk_height_ <= 0) {
        fprintf(stderr, "[touch] virtual desktop metrics are zero\n");
        return false;
    }

    touch_device_ = CreateSyntheticPointerDevice(
        PT_TOUCH,
        kMaxContacts,
        POINTER_FEEDBACK_DEFAULT);
    if (!touch_device_) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] CreateSyntheticPointerDevice(PT_TOUCH) failed: %lu\n",
                err);
        return false;
    }

    pen_device_ = CreateSyntheticPointerDevice(
        PT_PEN,
        1,
        POINTER_FEEDBACK_DEFAULT);
    if (!pen_device_) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] CreateSyntheticPointerDevice(PT_PEN) failed: %lu\n",
                err);
    }

    for (auto& p : touch_pointers_) {
        reset_pointer_locked(p);
    }
    reset_pointer_locked(pen_pointer_);

    stop_repeat_ = false;
    repeat_reset_pending_ = false;
    repeat_thread_ = std::thread(&WinTouchInjector::repeat_loop, this);

    fprintf(stderr,
            "[touch] injector initialized: surface=%ux%u offset=(%d,%d) "
            "vdesk=%dx%d+%d+%d pen=%s\n",
            w, h, ox, oy,
            vdesk_width_, vdesk_height_, vdesk_x_, vdesk_y_,
            pen_device_ ? "on" : "off");
    return true;
}

bool WinTouchInjector::inject_touch(uint8_t action, uint8_t ptr_id,
                                    uint16_t x, uint16_t y, uint16_t pressure,
                                    uint16_t touch_major, uint16_t touch_minor,
                                    uint16_t orientation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!touch_device_) {
        return false;
    }

    int32_t pixel_x = 0;
    int32_t pixel_y = 0;
    map_to_virtual_desktop_locked(x, y, &pixel_x, &pixel_y);

    const bool ok = inject_touch_locked(action, ptr_id, pixel_x, pixel_y,
                                        pressure, touch_major, touch_minor,
                                        orientation);

    if (ok) {
        repeat_reset_pending_ = true;
        repeat_cv_.notify_one();
    }
    return ok;
}

bool WinTouchInjector::inject_pen(uint8_t action, uint8_t ptr_id,
                                  uint8_t tool_type, uint32_t buttons,
                                  uint16_t x, uint16_t y, uint16_t pressure,
                                  uint16_t distance, uint16_t tilt,
                                  uint16_t rotation) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!pen_device_) {
        return false;
    }

    int32_t pixel_x = 0;
    int32_t pixel_y = 0;
    map_to_virtual_desktop_locked(x, y, &pixel_x, &pixel_y);

    const bool ok = inject_pen_locked(action, ptr_id, tool_type, buttons,
                                      pixel_x, pixel_y, pressure, distance,
                                      tilt, rotation);
    if (ok) {
        repeat_reset_pending_ = true;
        repeat_cv_.notify_one();
    }
    return ok;
}

bool WinTouchInjector::inject_touch_locked(uint8_t action, uint8_t ptr_id,
                                           int32_t pixel_x, int32_t pixel_y,
                                           uint16_t pressure,
                                           uint16_t touch_major,
                                           uint16_t touch_minor,
                                           uint16_t orientation) {
    int slot = find_touch_slot_by_external_id_locked(ptr_id);
    if (slot < 0 && (action == kTouchDown || action == kTouchHover)) {
        slot = allocate_touch_slot_locked(ptr_id);
        if (slot < 0) {
            fprintf(stderr,
                    "[touch] no free touch slots; cancelling all active touches\n");
            cancel_all_touches_locked();
            slot = allocate_touch_slot_locked(ptr_id);
            if (slot < 0) {
                return false;
            }
        }
    } else if (slot < 0) {
        return true;
    }

    update_touch_state_locked(touch_pointers_[slot], action, pixel_x, pixel_y,
                              pressure, touch_major, touch_minor, orientation);
    return inject_touch_state_locked();
}

bool WinTouchInjector::inject_pen_locked(uint8_t action, uint8_t ptr_id,
                                         uint8_t tool_type, uint32_t buttons,
                                         int32_t pixel_x, int32_t pixel_y,
                                         uint16_t pressure,
                                         uint16_t distance,
                                         uint16_t tilt,
                                         uint16_t rotation) {
    if (!pen_pointer_.assigned &&
        action != kTouchDown &&
        action != kTouchHover) {
        return true;
    }

    if (pen_pointer_.assigned &&
        pen_pointer_.external_id != ptr_id &&
        pen_pointer_.present) {
        cancel_pen_locked();
    }

    if (!pen_pointer_.assigned) {
        pen_pointer_.assigned = true;
        pen_pointer_.external_id = ptr_id;
    }

    update_pen_state_locked(pen_pointer_, action, tool_type, buttons,
                            pixel_x, pixel_y, pressure, distance, tilt,
                            rotation);
    return inject_pen_state_locked();
}

bool WinTouchInjector::inject_touch_state_locked() {
    POINTER_TYPE_INFO infos[kMaxContacts] = {};
    uint32_t count = 0;

    for (const auto& ptr : touch_pointers_) {
        if (!ptr.present) {
            continue;
        }

        const uint32_t flags = pointer_flags_locked(ptr);
        if (flags == POINTER_FLAG_NONE) {
            continue;
        }

        populate_touch_info_locked(infos[count++], ptr);
    }

    if (count == 0) {
        clear_touch_edge_flags_locked();
        return true;
    }

    if (!InjectSyntheticPointerInput(touch_device_, infos, count)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput(PT_TOUCH) failed: %lu\n",
                err);
        return false;
    }

    clear_touch_edge_flags_locked();
    return true;
}

bool WinTouchInjector::inject_pen_state_locked() {
    if (!pen_device_) {
        return true;
    }

    const uint32_t flags = pointer_flags_locked(pen_pointer_);
    if (!pen_pointer_.present || flags == POINTER_FLAG_NONE) {
        clear_pen_edge_flags_locked();
        return true;
    }

    POINTER_TYPE_INFO info = {};
    populate_pen_info_locked(info, pen_pointer_);

    if (!InjectSyntheticPointerInput(pen_device_, &info, 1)) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput(PT_PEN) failed: %lu\n",
                err);
        return false;
    }

    clear_pen_edge_flags_locked();
    return true;
}

bool WinTouchInjector::has_repeatable_inputs_locked() const {
    for (const auto& ptr : touch_pointers_) {
        if (ptr.present) {
            return true;
        }
    }
    return pen_pointer_.present;
}

void WinTouchInjector::clear_touch_edge_flags_locked() {
    for (auto& ptr : touch_pointers_) {
        clear_pointer_edge_flags_locked(ptr);
    }
}

void WinTouchInjector::clear_pen_edge_flags_locked() {
    clear_pointer_edge_flags_locked(pen_pointer_);
}

void WinTouchInjector::clear_pointer_edge_flags_locked(PointerState& ptr) {
    if (!ptr.assigned) {
        return;
    }

    ptr.edge_update = false;
    ptr.edge_down = false;
    ptr.edge_up = false;
    ptr.edge_canceled = false;

    if (!ptr.in_range && !ptr.in_contact) {
        reset_pointer_locked(ptr);
    }
}

void WinTouchInjector::reset_pointer_locked(PointerState& ptr) {
    ptr = {};
    ptr.orientation = DS_TOUCH_ORIENTATION_UNKNOWN;
    ptr.distance = DS_TOUCH_DISTANCE_UNKNOWN;
    ptr.tilt = DS_TOUCH_TILT_UNKNOWN;
}

void WinTouchInjector::cancel_all_touches_locked() {
    bool has_active = false;
    for (auto& ptr : touch_pointers_) {
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
        ptr.buttons = 0;
        has_active = true;
    }

    if (has_active) {
        inject_touch_state_locked();
    }
}

void WinTouchInjector::cancel_pen_locked() {
    if (!pen_pointer_.assigned) {
        return;
    }

    const bool was_in_contact = pen_pointer_.in_contact;
    pen_pointer_.in_contact = false;
    pen_pointer_.in_range = false;
    pen_pointer_.edge_down = false;
    pen_pointer_.edge_update = !was_in_contact;
    pen_pointer_.edge_up = was_in_contact;
    pen_pointer_.edge_canceled = true;
    pen_pointer_.pressure = 0;
    pen_pointer_.buttons = 0;
    inject_pen_state_locked();
}

void WinTouchInjector::update_touch_state_locked(PointerState& ptr,
                                                 uint8_t action,
                                                 int32_t pixel_x,
                                                 int32_t pixel_y,
                                                 uint16_t pressure,
                                                 uint16_t touch_major,
                                                 uint16_t touch_minor,
                                                 uint16_t orientation) {
    const bool should_update_position =
        action != kTouchUp && action != kTouchCancel && action != kTouchButtonOnly;

    ptr.assigned = true;
    ptr.tool_type = DS_TOUCH_TOOL_FINGER;
    ptr.buttons = 0;
    ptr.touch_major = touch_major;
    ptr.touch_minor = touch_minor;
    ptr.orientation = orientation;
    ptr.distance = DS_TOUCH_DISTANCE_UNKNOWN;
    ptr.tilt = DS_TOUCH_TILT_UNKNOWN;

    if (should_update_position || !ptr.present) {
        ptr.pixel_x = pixel_x;
        ptr.pixel_y = pixel_y;
    }

    switch (action) {
        case kTouchDown:
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = true;
            ptr.edge_update = false;
            ptr.edge_down = true;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = pressure;
            break;

        case kTouchMove:
            if (!ptr.present) {
                return;
            }
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = true;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = pressure;
            break;

        case kTouchHover:
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = false;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchUp:
            if (!ptr.present) {
                return;
            }
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.edge_update = false;
            ptr.edge_down = false;
            ptr.edge_up = true;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchCancel:
            if (!ptr.present) {
                return;
            }
            ptr.edge_update = !ptr.in_contact;
            ptr.edge_down = false;
            ptr.edge_up = ptr.in_contact;
            ptr.edge_canceled = true;
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.pressure = 0;
            break;

        case kTouchHoverLeave:
            if (!ptr.present) {
                return;
            }
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchButtonOnly:
            if (!ptr.present) {
                return;
            }
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            break;

        default:
            fprintf(stderr, "[touch] unknown action %u\n", action);
            return;
    }
}

void WinTouchInjector::update_pen_state_locked(PointerState& ptr,
                                               uint8_t action,
                                               uint8_t tool_type,
                                               uint32_t buttons,
                                               int32_t pixel_x,
                                               int32_t pixel_y,
                                               uint16_t pressure,
                                               uint16_t distance,
                                               uint16_t tilt,
                                               uint16_t rotation) {
    const bool should_update_position =
        action != kTouchUp && action != kTouchCancel && action != kTouchButtonOnly;

    ptr.assigned = true;
    ptr.tool_type = tool_type;
    ptr.buttons = buttons;
    ptr.touch_major = 0;
    ptr.touch_minor = 0;
    ptr.orientation = rotation;
    ptr.distance = distance;
    ptr.tilt = tilt;

    if (should_update_position || !ptr.present) {
        ptr.pixel_x = pixel_x;
        ptr.pixel_y = pixel_y;
    }

    switch (action) {
        case kTouchDown:
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = true;
            ptr.edge_update = false;
            ptr.edge_down = true;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = pressure;
            break;

        case kTouchMove:
            if (!ptr.present) {
                return;
            }
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = true;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = pressure;
            break;

        case kTouchHover:
            ptr.present = true;
            ptr.in_range = true;
            ptr.in_contact = false;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchUp:
            if (!ptr.present) {
                return;
            }
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.edge_update = false;
            ptr.edge_down = false;
            ptr.edge_up = true;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchCancel:
            if (!ptr.present) {
                return;
            }
            ptr.edge_update = !ptr.in_contact;
            ptr.edge_down = false;
            ptr.edge_up = ptr.in_contact;
            ptr.edge_canceled = true;
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.pressure = 0;
            break;

        case kTouchHoverLeave:
            if (!ptr.present) {
                return;
            }
            ptr.in_contact = false;
            ptr.in_range = false;
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            ptr.pressure = 0;
            break;

        case kTouchButtonOnly:
            if (!ptr.present) {
                return;
            }
            ptr.edge_update = true;
            ptr.edge_down = false;
            ptr.edge_up = false;
            ptr.edge_canceled = false;
            break;

        default:
            fprintf(stderr, "[touch] unknown pen action %u\n", action);
            return;
    }
}

void WinTouchInjector::map_to_virtual_desktop_locked(uint16_t x, uint16_t y,
                                                     int32_t* pixel_x,
                                                     int32_t* pixel_y) const {
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

    *pixel_x = (offset_x_ - vdesk_x_) + static_cast<int32_t>(local_x);
    *pixel_y = (offset_y_ - vdesk_y_) + static_cast<int32_t>(local_y);

    *pixel_x = std::clamp(*pixel_x, 0, std::max(0, vdesk_width_ - 1));
    *pixel_y = std::clamp(*pixel_y, 0, std::max(0, vdesk_height_ - 1));
}

int WinTouchInjector::find_touch_slot_by_external_id_locked(uint8_t external_id) const {
    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        const auto& ptr = touch_pointers_[i];
        if (ptr.assigned && ptr.external_id == external_id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

int WinTouchInjector::allocate_touch_slot_locked(uint8_t external_id) {
    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        auto& ptr = touch_pointers_[i];
        if (!ptr.assigned) {
            reset_pointer_locked(ptr);
            ptr.assigned = true;
            ptr.external_id = external_id;
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

void WinTouchInjector::populate_touch_info_locked(POINTER_TYPE_INFO& info,
                                                  const PointerState& ptr) const {
    std::memset(&info, 0, sizeof(info));
    info.type = PT_TOUCH;

    POINTER_INFO& pi = info.touchInfo.pointerInfo;
    pi.pointerType = PT_TOUCH;
    pi.pointerId = ptr.external_id;
    pi.frameId = 0;
    pi.pointerFlags = pointer_flags_locked(ptr);
    pi.ptPixelLocation.x = ptr.pixel_x;
    pi.ptPixelLocation.y = ptr.pixel_y;
    pi.ptPixelLocationRaw = pi.ptPixelLocation;

    info.touchInfo.touchFlags = TOUCH_FLAG_NONE;
    info.touchInfo.touchMask = TOUCH_MASK_NONE;

    const int32_t surface_left = offset_x_ - vdesk_x_;
    const int32_t surface_top = offset_y_ - vdesk_y_;
    const int32_t surface_right = surface_left +
        static_cast<int32_t>(surface_w_ > 0 ? surface_w_ - 1 : 0);
    const int32_t surface_bottom = surface_top +
        static_cast<int32_t>(surface_h_ > 0 ? surface_h_ - 1 : 0);

    if (ptr.in_contact) {
        info.touchInfo.touchMask |= TOUCH_MASK_PRESSURE;
        info.touchInfo.pressure = ptr.pressure != 0
            ? static_cast<UINT32>((static_cast<uint64_t>(ptr.pressure) * 1024) / 65535)
            : kDefaultTouchPressure;

        int32_t half_width = kContactRadius;
        int32_t half_height = kContactRadius;
        if (ptr.touch_major != 0 && ptr.touch_minor != 0) {
            const double major_pixels =
                (static_cast<double>(ptr.touch_major) * static_cast<double>(surface_w_)) / 65535.0;
            const double minor_pixels =
                (static_cast<double>(ptr.touch_minor) * static_cast<double>(surface_h_)) / 65535.0;
            const double rotation_degrees =
                ptr.orientation == DS_TOUCH_ORIENTATION_UNKNOWN
                    ? 45.0
                    : static_cast<double>(ptr.orientation % 360);
            const double major_axis = rotation_degrees * (kPi / 180.0);
            const double minor_axis = major_axis + (kPi / 2.0);
            const double contact_width =
                std::abs(std::cos(major_axis) * major_pixels) +
                std::abs(std::cos(minor_axis) * minor_pixels);
            const double contact_height =
                std::abs(std::sin(major_axis) * major_pixels) +
                std::abs(std::sin(minor_axis) * minor_pixels);
            half_width = std::max<int32_t>(
                kContactRadius,
                static_cast<int32_t>(std::ceil(contact_width / 2.0)));
            half_height = std::max<int32_t>(
                kContactRadius,
                static_cast<int32_t>(std::ceil(contact_height / 2.0)));
        }

        info.touchInfo.rcContact.left =
            std::max(surface_left, ptr.pixel_x - half_width);
        info.touchInfo.rcContact.top =
            std::max(surface_top, ptr.pixel_y - half_height);
        info.touchInfo.rcContact.right =
            std::min(surface_right, ptr.pixel_x + half_width);
        info.touchInfo.rcContact.bottom =
            std::min(surface_bottom, ptr.pixel_y + half_height);
        info.touchInfo.rcContactRaw = info.touchInfo.rcContact;
        info.touchInfo.touchMask |= TOUCH_MASK_CONTACTAREA;
    }

    if (ptr.orientation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        info.touchInfo.touchMask |= TOUCH_MASK_ORIENTATION;
        info.touchInfo.orientation = ptr.orientation % 360;
    }
}

void WinTouchInjector::populate_pen_info_locked(POINTER_TYPE_INFO& info,
                                                const PointerState& ptr) const {
    std::memset(&info, 0, sizeof(info));
    info.type = PT_PEN;

    POINTER_INFO& pi = info.penInfo.pointerInfo;
    pi.pointerType = PT_PEN;
    pi.pointerId = 0;
    pi.frameId = 0;
    pi.pointerFlags = pointer_flags_locked(ptr);
    pi.ptPixelLocation.x = ptr.pixel_x;
    pi.ptPixelLocation.y = ptr.pixel_y;
    pi.ptPixelLocationRaw = pi.ptPixelLocation;

    info.penInfo.penFlags = PEN_FLAG_NONE;
    if (ptr.buttons != 0) {
        info.penInfo.penFlags |= PEN_FLAG_BARREL;
    }
    if (ptr.tool_type == DS_TOUCH_TOOL_ERASER) {
        info.penInfo.penFlags |= PEN_FLAG_ERASER;
    }

    info.penInfo.penMask = PEN_MASK_NONE;
    if (ptr.in_contact && ptr.pressure != 0) {
        info.penInfo.penMask |= PEN_MASK_PRESSURE;
        info.penInfo.pressure =
            static_cast<UINT32>((static_cast<uint64_t>(ptr.pressure) * 1024) / 65535);
    }

    if (ptr.orientation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        info.penInfo.penMask |= PEN_MASK_ROTATION;
        info.penInfo.rotation = ptr.orientation % 360;
    }

    if (ptr.tilt != DS_TOUCH_TILT_UNKNOWN &&
        ptr.orientation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        const auto rotation_rads =
            static_cast<double>(ptr.orientation % 360) * (kPi / 180.0);
        const auto tilt_rads =
            static_cast<double>(ptr.tilt) * (kPi / 180.0);
        const auto r = std::sin(tilt_rads);
        const auto z = std::cos(tilt_rads);

        info.penInfo.penMask |= PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
        info.penInfo.tiltX = static_cast<INT32>(
            std::atan2(std::sin(-rotation_rads) * r, z) * 180.0 / kPi);
        info.penInfo.tiltY = static_cast<INT32>(
            std::atan2(std::cos(-rotation_rads) * r, z) * 180.0 / kPi);
    }
}

void WinTouchInjector::repeat_loop() {
    std::unique_lock<std::mutex> lock(mutex_);

    while (!stop_repeat_) {
        repeat_cv_.wait(lock, [this] {
            return stop_repeat_ || has_repeatable_inputs_locked();
        });
        if (stop_repeat_) {
            break;
        }

        repeat_reset_pending_ = false;
        if (repeat_cv_.wait_for(lock, kRepeatInterval, [this] {
                return stop_repeat_ || repeat_reset_pending_ || !has_repeatable_inputs_locked();
            })) {
            if (stop_repeat_) {
                break;
            }
            continue;
        }

        inject_touch_state_locked();
        inject_pen_state_locked();
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

    for (auto& ptr : touch_pointers_) {
        reset_pointer_locked(ptr);
    }
    reset_pointer_locked(pen_pointer_);

    if (pen_device_) {
        DestroySyntheticPointerDevice(pen_device_);
        pen_device_ = nullptr;
    }
    if (touch_device_) {
        DestroySyntheticPointerDevice(touch_device_);
        touch_device_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }
}

} // namespace droidscreen
