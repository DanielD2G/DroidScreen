/*
 * DroidScreen Desktop - Mouse injection interface
 */

#pragma once

#include <cstdint>

namespace droidscreen {

class MouseInjector {
public:
    virtual ~MouseInjector() = default;

    virtual bool init(uint32_t w, uint32_t h,
                      int32_t ox = 0, int32_t oy = 0) = 0;
    virtual bool inject_mouse(uint8_t action, uint8_t buttons,
                              uint16_t x, uint16_t y) = 0;
    virtual void shutdown() = 0;
};

class NullMouseInjector : public MouseInjector {
public:
    bool init(uint32_t, uint32_t, int32_t, int32_t) override { return true; }
    bool inject_mouse(uint8_t, uint8_t, uint16_t, uint16_t) override { return true; }
    void shutdown() override {}
};

} // namespace droidscreen
