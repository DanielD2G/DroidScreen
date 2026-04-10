/*
 * DroidScreen Windows - Absolute mouse injector
 */

#pragma once

#include "droidscreen/mouse_injector.h"

#include <cstdint>
#include <mutex>

namespace droidscreen {

class WinMouseInjector : public MouseInjector {
public:
    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;
    bool inject_mouse(uint8_t action, uint8_t buttons,
                      uint16_t x, uint16_t y) override;
    void shutdown() override;

private:
    uint32_t surface_w_ = 0;
    uint32_t surface_h_ = 0;
    int32_t offset_x_ = 0;
    int32_t offset_y_ = 0;
    int32_t vdesk_x_ = 0;
    int32_t vdesk_y_ = 0;
    int32_t vdesk_width_ = 0;
    int32_t vdesk_height_ = 0;
    uint8_t current_buttons_ = 0;
    std::mutex mutex_;
};

} // namespace droidscreen
