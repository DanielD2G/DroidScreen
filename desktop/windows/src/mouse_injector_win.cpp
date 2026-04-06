/*
 * DroidScreen Windows - Absolute mouse injector
 */

#include "mouse_injector_win.h"

extern "C" {
#include "droidscreen/mouse.h"
}

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <algorithm>
#include <array>
#include <cstdio>

namespace droidscreen {

bool WinMouseInjector::init(uint32_t w, uint32_t h, int32_t ox, int32_t oy) {
    std::lock_guard<std::mutex> lock(mutex_);
    surface_w_ = w;
    surface_h_ = h;
    offset_x_ = ox;
    offset_y_ = oy;
    vdesk_x_ = GetSystemMetrics(SM_XVIRTUALSCREEN);
    vdesk_y_ = GetSystemMetrics(SM_YVIRTUALSCREEN);
    vdesk_width_ = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    vdesk_height_ = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    current_buttons_ = 0;
    return vdesk_width_ > 0 && vdesk_height_ > 0;
}

bool WinMouseInjector::inject_mouse(uint8_t action, uint8_t buttons,
                                    uint16_t x, uint16_t y) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (vdesk_width_ <= 0 || vdesk_height_ <= 0) {
        return false;
    }

    uint32_t local_x = surface_w_ > 1
        ? static_cast<uint32_t>((static_cast<uint64_t>(x) * (surface_w_ - 1)) / 65535)
        : 0;
    uint32_t local_y = surface_h_ > 1
        ? static_cast<uint32_t>((static_cast<uint64_t>(y) * (surface_h_ - 1)) / 65535)
        : 0;

    int32_t pixel_x = (offset_x_ - vdesk_x_) + static_cast<int32_t>(local_x);
    int32_t pixel_y = (offset_y_ - vdesk_y_) + static_cast<int32_t>(local_y);
    pixel_x = std::clamp(pixel_x, 0, std::max(0, vdesk_width_ - 1));
    pixel_y = std::clamp(pixel_y, 0, std::max(0, vdesk_height_ - 1));

    LONG absolute_x = vdesk_width_ > 1
        ? static_cast<LONG>((static_cast<uint64_t>(pixel_x) * 65535) / (vdesk_width_ - 1))
        : 0;
    LONG absolute_y = vdesk_height_ > 1
        ? static_cast<LONG>((static_cast<uint64_t>(pixel_y) * 65535) / (vdesk_height_ - 1))
        : 0;

    std::array<INPUT, 4> inputs = {};
    UINT count = 0;

    if (action != DS_TOUCH_HOVER_LEAVE && action != DS_TOUCH_BUTTON_ONLY) {
        INPUT move = {};
        move.type = INPUT_MOUSE;
        move.mi.dx = absolute_x;
        move.mi.dy = absolute_y;
        move.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
        inputs[count++] = move;
    }

    const uint8_t desired_buttons =
        (action == DS_TOUCH_UP || action == DS_TOUCH_CANCEL || action == DS_TOUCH_HOVER_LEAVE)
            ? 0
            : buttons;

    const uint8_t changed = current_buttons_ ^ desired_buttons;
    auto append_button = [&](DWORD flags) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        inputs[count++] = input;
    };

    if ((changed & DS_MOUSE_BUTTON_LEFT) != 0) {
        append_button((desired_buttons & DS_MOUSE_BUTTON_LEFT) != 0
                         ? MOUSEEVENTF_LEFTDOWN
                         : MOUSEEVENTF_LEFTUP);
    }
    if ((changed & DS_MOUSE_BUTTON_RIGHT) != 0) {
        append_button((desired_buttons & DS_MOUSE_BUTTON_RIGHT) != 0
                         ? MOUSEEVENTF_RIGHTDOWN
                         : MOUSEEVENTF_RIGHTUP);
    }
    if ((changed & DS_MOUSE_BUTTON_MIDDLE) != 0) {
        append_button((desired_buttons & DS_MOUSE_BUTTON_MIDDLE) != 0
                         ? MOUSEEVENTF_MIDDLEDOWN
                         : MOUSEEVENTF_MIDDLEUP);
    }

    current_buttons_ = desired_buttons;

    if (count == 0) {
        return true;
    }

    return SendInput(count, inputs.data(), sizeof(INPUT)) == count;
}

void WinMouseInjector::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (current_buttons_ == 0) {
        return;
    }

    std::array<INPUT, 3> inputs = {};
    UINT count = 0;
    auto append_button = [&](DWORD flags) {
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dwFlags = flags;
        inputs[count++] = input;
    };

    if ((current_buttons_ & DS_MOUSE_BUTTON_LEFT) != 0) {
        append_button(MOUSEEVENTF_LEFTUP);
    }
    if ((current_buttons_ & DS_MOUSE_BUTTON_RIGHT) != 0) {
        append_button(MOUSEEVENTF_RIGHTUP);
    }
    if ((current_buttons_ & DS_MOUSE_BUTTON_MIDDLE) != 0) {
        append_button(MOUSEEVENTF_MIDDLEUP);
    }

    if (count > 0) {
        SendInput(count, inputs.data(), sizeof(INPUT));
    }
    current_buttons_ = 0;
}

} // namespace droidscreen
