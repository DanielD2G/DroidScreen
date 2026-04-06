/*
 * DroidScreen Windows - Touch injector implementation
 *
 * Uses CreateSyntheticPointerDevice / InjectSyntheticPointerInput with touch
 * and pen state handling aligned to Sunshine's Windows host implementation.
 */

#include "touch_injector_win.h"

extern "C" {
#include "droidscreen/touch.h"
}

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
constexpr uint32_t kEdgeTriggeredPointerFlags =
    POINTER_FLAG_DOWN | POINTER_FLAG_UP | POINTER_FLAG_CANCELED | POINTER_FLAG_UPDATE;
constexpr double kPi = 3.14159265358979323846;

WinTouchInjector::WinTouchInjector() = default;

WinTouchInjector::~WinTouchInjector() {
    shutdown();
}

bool WinTouchInjector::init(uint32_t w, uint32_t h, int32_t ox, int32_t oy) {
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
        const DWORD err = GetLastError();
        fprintf(stderr, "[touch] CreateSyntheticPointerDevice(PT_TOUCH) failed: %lu\n", err);
        return false;
    }

    pen_device_ = CreateSyntheticPointerDevice(
        PT_PEN,
        1,
        POINTER_FEEDBACK_DEFAULT);
    if (!pen_device_) {
        const DWORD err = GetLastError();
        fprintf(stderr, "[touch] CreateSyntheticPointerDevice(PT_PEN) failed: %lu\n", err);
    }

    std::memset(touch_infos_.data(), 0, sizeof(POINTER_TYPE_INFO) * touch_infos_.size());
    std::memset(&pen_info_, 0, sizeof(pen_info_));
    active_touch_slots_ = 0;

    stop_repeat_ = false;
    repeat_reset_pending_ = false;
    repeat_thread_ = std::thread(&WinTouchInjector::repeat_loop, this);

    fprintf(stderr,
            "[touch] injector initialized: surface=%ux%u offset=(%d,%d) "
            "vdesk=%dx%d+%d+%d pen=%s\n",
            w, h, ox, oy, vdesk_width_, vdesk_height_, vdesk_x_, vdesk_y_,
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
    (void)distance;

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
    POINTER_TYPE_INFO* pointer = touch_pointer_by_id_locked(ptr_id, action);
    if (!pointer) {
        fprintf(stderr, "[touch] no unused pointer entries; cancelling active touches\n");
        cancel_all_touches_locked();
        pointer = touch_pointer_by_id_locked(ptr_id, action);
        if (!pointer) {
            return false;
        }
    }

    pointer->type = PT_TOUCH;
    auto& touch_info = pointer->touchInfo;
    touch_info.pointerInfo.pointerType = PT_TOUCH;

    populate_common_pointer_info_locked(touch_info.pointerInfo, action, pixel_x, pixel_y);
    populate_touch_fields_locked(*pointer, pressure, touch_major, touch_minor, orientation);

    if (!inject_active_touches_locked()) {
        return false;
    }

    clear_edge_triggered_flags_locked(touch_info.pointerInfo);
    return true;
}

bool WinTouchInjector::inject_pen_locked(uint8_t action, uint8_t ptr_id,
                                         uint8_t tool_type, uint32_t buttons,
                                         int32_t pixel_x, int32_t pixel_y,
                                         uint16_t pressure,
                                         uint16_t distance,
                                         uint16_t tilt,
                                         uint16_t rotation) {
    (void)ptr_id;
    (void)distance;

    pen_info_.type = PT_PEN;
    auto& pen_info = pen_info_.penInfo;
    pen_info.pointerInfo.pointerType = PT_PEN;
    pen_info.pointerInfo.pointerId = 0;

    populate_common_pointer_info_locked(pen_info.pointerInfo, action, pixel_x, pixel_y);
    populate_pen_fields_locked(pen_info_, tool_type, buttons, pressure, tilt, rotation);

    if (!inject_active_pen_locked()) {
        return false;
    }

    clear_edge_triggered_flags_locked(pen_info.pointerInfo);
    return true;
}

void WinTouchInjector::perform_touch_compaction_locked() {
    uint32_t i = 0;
    for (; i < kMaxContacts; ++i) {
        if (touch_infos_[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
            for (uint32_t j = i + 1; j < kMaxContacts; ++j) {
                if (touch_infos_[j].touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
                    std::swap(touch_infos_[i], touch_infos_[j]);
                    break;
                }
            }

            if (touch_infos_[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
                break;
            }
        }
    }

    active_touch_slots_ = i;
}

POINTER_TYPE_INFO* WinTouchInjector::touch_pointer_by_id_locked(uint8_t ptr_id, uint8_t action) {
    perform_touch_compaction_locked();

    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        auto& info = touch_infos_[i];
        if (info.touchInfo.pointerInfo.pointerId == ptr_id &&
            info.touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
            if (action == kTouchDown &&
                (info.touchInfo.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT)) {
                fprintf(stderr, "[touch] pointer %u already down; possible dropped up/cancel\n",
                        static_cast<unsigned>(ptr_id));
            }
            return &info;
        }
    }

    if (action != kTouchHover && action != kTouchDown) {
        fprintf(stderr, "[touch] unexpected new pointer %u for action %u\n",
                static_cast<unsigned>(ptr_id), static_cast<unsigned>(action));
    }

    for (uint32_t i = 0; i < kMaxContacts; ++i) {
        if (touch_infos_[i].touchInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
            std::memset(&touch_infos_[i], 0, sizeof(touch_infos_[i]));
            touch_infos_[i].touchInfo.pointerInfo.pointerId = ptr_id;
            active_touch_slots_ = i + 1;
            return &touch_infos_[i];
        }
    }

    return nullptr;
}

void WinTouchInjector::populate_common_pointer_info_locked(POINTER_INFO& pointer_info,
                                                           uint8_t action,
                                                           int32_t pixel_x,
                                                           int32_t pixel_y) const {
    switch (action) {
        case kTouchHover:
            pointer_info.pointerFlags &= ~POINTER_FLAG_INCONTACT;
            pointer_info.pointerFlags |= POINTER_FLAG_INRANGE | POINTER_FLAG_UPDATE;
            pointer_info.ptPixelLocation.x = pixel_x;
            pointer_info.ptPixelLocation.y = pixel_y;
            break;

        case kTouchDown:
            pointer_info.pointerFlags |=
                POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_DOWN;
            pointer_info.ptPixelLocation.x = pixel_x;
            pointer_info.ptPixelLocation.y = pixel_y;
            break;

        case kTouchUp:
            pointer_info.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
            pointer_info.pointerFlags |= POINTER_FLAG_UP;
            break;

        case kTouchMove:
            pointer_info.pointerFlags |=
                POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT | POINTER_FLAG_UPDATE;
            pointer_info.ptPixelLocation.x = pixel_x;
            pointer_info.ptPixelLocation.y = pixel_y;
            break;

        case kTouchCancel:
            if (pointer_info.pointerFlags & POINTER_FLAG_INCONTACT) {
                pointer_info.pointerFlags |= POINTER_FLAG_UP;
            } else {
                pointer_info.pointerFlags |= POINTER_FLAG_UPDATE;
            }
            pointer_info.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
            pointer_info.pointerFlags |= POINTER_FLAG_CANCELED;
            break;

        case kTouchHoverLeave:
            pointer_info.pointerFlags &= ~(POINTER_FLAG_INCONTACT | POINTER_FLAG_INRANGE);
            pointer_info.pointerFlags |= POINTER_FLAG_UPDATE;
            break;

        case kTouchButtonOnly:
            if (pointer_info.pointerFlags != POINTER_FLAG_NONE) {
                pointer_info.pointerFlags |= POINTER_FLAG_UPDATE;
            }
            break;

        default:
            fprintf(stderr, "[touch] unknown action %u\n", static_cast<unsigned>(action));
            break;
    }

    pointer_info.ptPixelLocationRaw = pointer_info.ptPixelLocation;
}

void WinTouchInjector::populate_touch_fields_locked(POINTER_TYPE_INFO& info,
                                                    uint16_t pressure,
                                                    uint16_t touch_major,
                                                    uint16_t touch_minor,
                                                    uint16_t orientation) const {
    auto& touch_info = info.touchInfo;
    touch_info.touchFlags = TOUCH_FLAG_NONE;
    touch_info.touchMask = TOUCH_MASK_NONE;

    if (touch_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) {
        if (pressure != 0) {
            touch_info.touchMask |= TOUCH_MASK_PRESSURE;
            touch_info.pressure =
                static_cast<UINT32>((static_cast<uint64_t>(pressure) * 1024) / 65535);
        } else {
            touch_info.pressure = kDefaultTouchPressure;
        }

        if (touch_major != 0 && touch_minor != 0) {
            const float rotation_degrees =
                orientation == DS_TOUCH_ORIENTATION_UNKNOWN ? 45.0f : static_cast<float>(orientation);
            const float major_axis_angle = rotation_degrees * static_cast<float>(kPi / 180.0);
            const float minor_axis_angle = major_axis_angle + static_cast<float>(kPi / 2.0);
            const float major_pixels =
                (static_cast<float>(touch_major) * static_cast<float>(surface_w_)) / 65535.0f;
            const float minor_pixels =
                (static_cast<float>(touch_minor) * static_cast<float>(surface_h_)) / 65535.0f;
            const float contact_width =
                (std::cos(major_axis_angle) * major_pixels) +
                (std::cos(minor_axis_angle) * minor_pixels);
            const float contact_height =
                (std::sin(major_axis_angle) * major_pixels) +
                (std::sin(minor_axis_angle) * minor_pixels);

            const LONG surface_left = offset_x_ - vdesk_x_;
            const LONG surface_top = offset_y_ - vdesk_y_;
            const LONG surface_right = surface_left + static_cast<LONG>(surface_w_);
            const LONG surface_bottom = surface_top + static_cast<LONG>(surface_h_);

            touch_info.rcContact.left = std::max<LONG>(
                surface_left,
                touch_info.pointerInfo.ptPixelLocation.x -
                    static_cast<LONG>(std::floor(contact_width / 2.0f)));
            touch_info.rcContact.right = std::min<LONG>(
                surface_right,
                touch_info.pointerInfo.ptPixelLocation.x +
                    static_cast<LONG>(std::ceil(contact_width / 2.0f)));
            touch_info.rcContact.top = std::max<LONG>(
                surface_top,
                touch_info.pointerInfo.ptPixelLocation.y -
                    static_cast<LONG>(std::floor(contact_height / 2.0f)));
            touch_info.rcContact.bottom = std::min<LONG>(
                surface_bottom,
                touch_info.pointerInfo.ptPixelLocation.y +
                    static_cast<LONG>(std::ceil(contact_height / 2.0f)));

            touch_info.touchMask |= TOUCH_MASK_CONTACTAREA;
        }
    } else {
        touch_info.pressure = 0;
        touch_info.rcContact = {};
    }

    if (orientation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        touch_info.touchMask |= TOUCH_MASK_ORIENTATION;
        touch_info.orientation = orientation % 360;
    } else {
        touch_info.orientation = 0;
    }

    touch_info.rcContactRaw = touch_info.rcContact;
}

void WinTouchInjector::populate_pen_fields_locked(POINTER_TYPE_INFO& info,
                                                  uint8_t tool_type,
                                                  uint32_t buttons,
                                                  uint16_t pressure,
                                                  uint16_t tilt,
                                                  uint16_t rotation) const {
    auto& pen_info = info.penInfo;

    if (buttons != 0) {
        pen_info.penFlags |= PEN_FLAG_BARREL;
    } else {
        pen_info.penFlags &= ~PEN_FLAG_BARREL;
    }

    switch (tool_type) {
        case DS_TOUCH_TOOL_ERASER:
            pen_info.penFlags |= PEN_FLAG_ERASER;
            break;
        case DS_TOUCH_TOOL_UNKNOWN:
            break;
        default:
            pen_info.penFlags &= ~PEN_FLAG_ERASER;
            break;
    }

    pen_info.penMask = PEN_MASK_NONE;
    if ((pen_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) && pressure != 0) {
        pen_info.penMask |= PEN_MASK_PRESSURE;
        pen_info.pressure =
            static_cast<UINT32>((static_cast<uint64_t>(pressure) * 1024) / 65535);
    } else {
        pen_info.pressure = 0;
    }

    if (rotation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        pen_info.penMask |= PEN_MASK_ROTATION;
        pen_info.rotation = rotation % 360;
    } else {
        pen_info.rotation = 0;
    }

    if (tilt != DS_TOUCH_TILT_UNKNOWN && rotation != DS_TOUCH_ORIENTATION_UNKNOWN) {
        const auto rotation_rads = static_cast<double>(rotation % 360) * (kPi / 180.0);
        const auto tilt_rads = static_cast<double>(tilt) * (kPi / 180.0);
        const auto r = std::sin(tilt_rads);
        const auto z = std::cos(tilt_rads);

        pen_info.penMask |= PEN_MASK_TILT_X | PEN_MASK_TILT_Y;
        pen_info.tiltX = static_cast<INT32>(
            std::atan2(std::sin(-rotation_rads) * r, z) * 180.0 / kPi);
        pen_info.tiltY = static_cast<INT32>(
            std::atan2(std::cos(-rotation_rads) * r, z) * 180.0 / kPi);
    } else {
        pen_info.tiltX = 0;
        pen_info.tiltY = 0;
    }
}

bool WinTouchInjector::inject_active_touches_locked() {
    perform_touch_compaction_locked();

    if (active_touch_slots_ == 0) {
        return true;
    }

    if (!InjectSyntheticPointerInput(touch_device_, touch_infos_.data(), active_touch_slots_)) {
        const DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput(PT_TOUCH) failed: %lu\n", err);
        return false;
    }
    return true;
}

bool WinTouchInjector::inject_active_pen_locked() {
    if (pen_info_.penInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        return true;
    }

    if (!InjectSyntheticPointerInput(pen_device_, &pen_info_, 1)) {
        const DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput(PT_PEN) failed: %lu\n", err);
        return false;
    }
    return true;
}

bool WinTouchInjector::has_repeatable_inputs_locked() const {
    for (const auto& info : touch_infos_) {
        if (info.touchInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE) {
            return true;
        }
    }
    return pen_info_.penInfo.pointerInfo.pointerFlags != POINTER_FLAG_NONE;
}

void WinTouchInjector::clear_edge_triggered_flags_locked(POINTER_INFO& pointer_info) const {
    pointer_info.pointerFlags &= ~kEdgeTriggeredPointerFlags;
}

void WinTouchInjector::cancel_all_touches_locked() {
    perform_touch_compaction_locked();
    if (active_touch_slots_ > 0) {
        for (uint32_t i = 0; i < active_touch_slots_; ++i) {
            populate_common_pointer_info_locked(
                touch_infos_[i].touchInfo.pointerInfo, kTouchCancel, 0, 0);
            touch_infos_[i].touchInfo.touchMask = TOUCH_MASK_NONE;
        }
        inject_active_touches_locked();
    }

    std::memset(touch_infos_.data(), 0, sizeof(POINTER_TYPE_INFO) * touch_infos_.size());
    active_touch_slots_ = 0;
}

void WinTouchInjector::cancel_active_pen_locked() {
    if (pen_info_.penInfo.pointerInfo.pointerFlags == POINTER_FLAG_NONE) {
        return;
    }

    populate_common_pointer_info_locked(pen_info_.penInfo.pointerInfo, kTouchCancel, 0, 0);
    pen_info_.penInfo.penMask = PEN_MASK_NONE;
    inject_active_pen_locked();
    std::memset(&pen_info_, 0, sizeof(pen_info_));
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

        inject_active_touches_locked();
        inject_active_pen_locked();
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

    cancel_all_touches_locked();
    cancel_active_pen_locked();

    if (pen_device_) {
        DestroySyntheticPointerDevice(pen_device_);
        pen_device_ = nullptr;
    }
    if (touch_device_) {
        DestroySyntheticPointerDevice(touch_device_);
        touch_device_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }

    std::memset(touch_infos_.data(), 0, sizeof(POINTER_TYPE_INFO) * touch_infos_.size());
    std::memset(&pen_info_, 0, sizeof(pen_info_));
    active_touch_slots_ = 0;
}

} // namespace droidscreen
