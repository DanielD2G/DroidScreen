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

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <synchapi.h>
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

} // extern "C"

namespace droidscreen {

WinTouchInjector::WinTouchInjector() = default;

WinTouchInjector::~WinTouchInjector() {
    shutdown();
}

bool WinTouchInjector::init(uint32_t w, uint32_t h,
                             int32_t ox, int32_t oy) {
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

    fprintf(stderr, "[touch] injector initialized: surface=%ux%u offset=(%d,%d) "
            "vdesk=%dx%d+%d+%d\n",
            w, h, ox, oy,
            vdesk_width_, vdesk_height_, vdesk_x_, vdesk_y_);
    return true;
}

bool WinTouchInjector::inject(uint8_t action, uint8_t ptr_id,
                               uint16_t x, uint16_t y, uint16_t pressure) {
    if (!device_) return false;
    if (ptr_id >= kMaxContacts) {
        fprintf(stderr, "[touch] pointer_id %u exceeds max contacts\n", ptr_id);
        return false;
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
    auto& ptr = pointers_[ptr_id];

    switch (action) {
        case kTouchDown:
            ptr.active   = true;
            ptr.pixel_x  = pixel_x;
            ptr.pixel_y  = pixel_y;
            ptr.pressure = pressure;
            break;

        case kTouchMove:
            if (!ptr.active) {
                // Stale move after an up -- ignore.
                return true;
            }
            ptr.pixel_x  = pixel_x;
            ptr.pixel_y  = pixel_y;
            ptr.pressure = pressure;
            break;

        case kTouchUp:
        case kTouchCancel:
            ptr.pixel_x  = pixel_x;
            ptr.pixel_y  = pixel_y;
            ptr.pressure = 0;
            // Will be marked inactive after injection.
            break;

        default:
            fprintf(stderr, "[touch] unknown action %u\n", action);
            return false;
    }

    // Build the POINTER_TYPE_INFO array with all active pointers.
    // InjectSyntheticPointerInput requires the full set.
    POINTER_TYPE_INFO infos[kMaxContacts] = {};
    uint32_t count = 0;

    for (uint32_t i = 0; i < kMaxContacts; i++) {
        auto& p = pointers_[i];
        if (!p.active && !(i == ptr_id &&
            (action == kTouchUp || action == kTouchCancel))) {
            continue;
        }

        auto& info = infos[count];
        memset(&info, 0, sizeof(info));

        info.type = PT_TOUCH;

        // Fill POINTER_INFO fields.
        POINTER_INFO& pi = info.touchInfo.pointerInfo;
        pi.pointerType = PT_TOUCH;
        pi.pointerId   = i;
        pi.frameId     = 0;  // system assigns

        // Determine pointer flags based on action for this pointer.
        if (i == ptr_id) {
            switch (action) {
                case kTouchDown:
                    pi.pointerFlags = POINTER_FLAG_INRANGE |
                                      POINTER_FLAG_INCONTACT |
                                      POINTER_FLAG_DOWN;
                    break;
                case kTouchMove:
                    pi.pointerFlags = POINTER_FLAG_INRANGE |
                                      POINTER_FLAG_INCONTACT |
                                      POINTER_FLAG_UPDATE;
                    break;
                case kTouchUp:
                    pi.pointerFlags = POINTER_FLAG_UP;
                    break;
                case kTouchCancel:
                    pi.pointerFlags = POINTER_FLAG_UP |
                                      POINTER_FLAG_CANCELED;
                    break;
            }
        } else {
            // Other active pointers are reported as unchanged (UPDATE).
            pi.pointerFlags = POINTER_FLAG_INRANGE |
                              POINTER_FLAG_INCONTACT |
                              POINTER_FLAG_UPDATE;
        }

        // Set the screen coordinates.
        pi.ptPixelLocation.x = p.pixel_x;
        pi.ptPixelLocation.y = p.pixel_y;
        pi.ptPixelLocationRaw = pi.ptPixelLocation;

        // POINTER_TOUCH_INFO fields.
        info.touchInfo.touchFlags   = TOUCH_FLAG_NONE;
        info.touchInfo.touchMask    = TOUCH_MASK_PRESSURE |
                                      TOUCH_MASK_CONTACTAREA;
        info.touchInfo.pressure     =
            static_cast<UINT32>((static_cast<uint64_t>(p.pressure) * 1024) / 65535);

        // Contact area: small rectangle around the touch point.
        int32_t contact_radius = 4;
        info.touchInfo.rcContact.left   = p.pixel_x - contact_radius;
        info.touchInfo.rcContact.top    = p.pixel_y - contact_radius;
        info.touchInfo.rcContact.right  = p.pixel_x + contact_radius;
        info.touchInfo.rcContact.bottom = p.pixel_y + contact_radius;
        info.touchInfo.rcContactRaw     = info.touchInfo.rcContact;

        count++;
    }

    if (count == 0) return true;

    // Inject the touch events.
    BOOL ok = InjectSyntheticPointerInput(device_, infos, count);
    if (!ok) {
        DWORD err = GetLastError();
        fprintf(stderr, "[touch] InjectSyntheticPointerInput failed: %lu\n",
                err);
        return false;
    }

    // Mark pointers inactive after UP/CANCEL.
    if (action == kTouchUp || action == kTouchCancel) {
        pointers_[ptr_id].active = false;
    }

    return true;
}

void WinTouchInjector::shutdown() {
    if (device_) {
        DestroySyntheticPointerDevice(device_);
        device_ = nullptr;
        fprintf(stderr, "[touch] injector shut down\n");
    }
}

} // namespace droidscreen
