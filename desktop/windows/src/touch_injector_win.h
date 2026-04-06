/*
 * DroidScreen Windows - Synthetic touch and pen injector
 *
 * The touch/pen state model in this file is intentionally aligned with the
 * Sunshine host implementation so Windows receives the same pointer semantics:
 * active pointer slots are stored directly as POINTER_TYPE_INFO entries,
 * edge-triggered flags are cleared after a single injection, and active
 * interactions are refreshed periodically to prevent timeout cancellation.
 */

#pragma once

#include "droidscreen/touch_injector.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

namespace droidscreen {

class WinTouchInjector : public TouchInjector {
public:
    WinTouchInjector();
    ~WinTouchInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;
    bool inject_touch(uint8_t action, uint8_t ptr_id,
                      uint16_t x, uint16_t y, uint16_t pressure,
                      uint16_t touch_major, uint16_t touch_minor,
                      uint16_t orientation) override;
    bool inject_pen(uint8_t action, uint8_t ptr_id,
                    uint8_t tool_type, uint32_t buttons,
                    uint16_t x, uint16_t y, uint16_t pressure,
                    uint16_t distance, uint16_t tilt,
                    uint16_t rotation) override;
    void shutdown() override;

private:
    static constexpr uint32_t kMaxContacts = 10;

    uint32_t surface_w_ = 0;
    uint32_t surface_h_ = 0;
    int32_t offset_x_ = 0;
    int32_t offset_y_ = 0;
    int32_t vdesk_x_ = 0;
    int32_t vdesk_y_ = 0;
    int32_t vdesk_width_ = 0;
    int32_t vdesk_height_ = 0;

    HSYNTHETICPOINTERDEVICE touch_device_ = nullptr;
    HSYNTHETICPOINTERDEVICE pen_device_ = nullptr;

    std::array<POINTER_TYPE_INFO, kMaxContacts> touch_infos_ = {};
    uint32_t active_touch_slots_ = 0;
    POINTER_TYPE_INFO pen_info_ = {};

    std::mutex mutex_;
    std::condition_variable repeat_cv_;
    std::thread repeat_thread_;
    bool stop_repeat_ = false;
    bool repeat_reset_pending_ = false;

    bool inject_touch_locked(uint8_t action, uint8_t ptr_id,
                             int32_t pixel_x, int32_t pixel_y,
                             uint16_t pressure, uint16_t touch_major,
                             uint16_t touch_minor, uint16_t orientation);
    bool inject_pen_locked(uint8_t action, uint8_t ptr_id, uint8_t tool_type,
                           uint32_t buttons, int32_t pixel_x, int32_t pixel_y,
                           uint16_t pressure, uint16_t distance,
                           uint16_t tilt, uint16_t rotation);
    void perform_touch_compaction_locked();
    POINTER_TYPE_INFO* touch_pointer_by_id_locked(uint8_t ptr_id, uint8_t action);
    void populate_common_pointer_info_locked(POINTER_INFO& pointer_info,
                                             uint8_t action,
                                             int32_t pixel_x,
                                             int32_t pixel_y) const;
    void populate_touch_fields_locked(POINTER_TYPE_INFO& info,
                                      uint16_t pressure,
                                      uint16_t touch_major,
                                      uint16_t touch_minor,
                                      uint16_t orientation) const;
    void populate_pen_fields_locked(POINTER_TYPE_INFO& info,
                                    uint8_t tool_type,
                                    uint32_t buttons,
                                    uint16_t pressure,
                                    uint16_t tilt,
                                    uint16_t rotation) const;
    bool inject_active_touches_locked();
    bool inject_active_pen_locked();
    bool has_repeatable_inputs_locked() const;
    void clear_edge_triggered_flags_locked(POINTER_INFO& pointer_info) const;
    void cancel_all_touches_locked();
    void cancel_active_pen_locked();
    void map_to_virtual_desktop_locked(uint16_t x, uint16_t y,
                                       int32_t* pixel_x,
                                       int32_t* pixel_y) const;
    void repeat_loop();
};

} // namespace droidscreen
