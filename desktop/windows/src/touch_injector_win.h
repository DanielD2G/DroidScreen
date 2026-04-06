/*
 * DroidScreen Windows - Synthetic touch and pen injector
 *
 * Injects touch and stylus events into the Windows input system using the
 * SyntheticPointerDevice API (Windows 10 1809+).
 */

#pragma once

#include "droidscreen/touch_injector.h"

#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>

// Forward-declare Windows types to avoid including windows.h in the header.
struct HSYNTHETICPOINTERDEVICE__;
typedef HSYNTHETICPOINTERDEVICE__* HSYNTHETICPOINTERDEVICE;
struct tagPOINTER_TYPE_INFO;
typedef tagPOINTER_TYPE_INFO POINTER_TYPE_INFO;

namespace droidscreen {

class WinTouchInjector : public TouchInjector {
public:
    WinTouchInjector();
    ~WinTouchInjector() override;

    bool init(uint32_t w, uint32_t h,
              int32_t ox = 0, int32_t oy = 0) override;
    bool inject(uint8_t action, uint8_t ptr_id,
                uint8_t tool_type, uint32_t buttons,
                uint16_t x, uint16_t y, uint16_t pressure,
                uint16_t touch_major, uint16_t touch_minor,
                uint16_t orientation, uint16_t distance,
                uint16_t tilt) override;
    void shutdown() override;

    struct PointerState {
        bool     assigned       = false;
        bool     present        = false;
        bool     in_range       = false;
        bool     in_contact     = false;
        bool     edge_update    = false;
        bool     edge_down      = false;
        bool     edge_up        = false;
        bool     edge_canceled  = false;
        uint8_t  external_id    = 0;
        uint8_t  tool_type      = 0;
        uint32_t buttons        = 0;
        int32_t  pixel_x        = 0;
        int32_t  pixel_y        = 0;
        uint16_t pressure       = 0;
        uint16_t touch_major    = 0;
        uint16_t touch_minor    = 0;
        uint16_t orientation    = 0xFFFFu;
        uint16_t distance       = 0xFFFFu;
        uint16_t tilt           = 0xFFFFu;
    };

private:
    static constexpr uint32_t kMaxContacts = 10;

    uint32_t surface_w_ = 0;
    uint32_t surface_h_ = 0;
    int32_t  offset_x_  = 0;
    int32_t  offset_y_  = 0;

    int32_t vdesk_x_      = 0;
    int32_t vdesk_y_      = 0;
    int32_t vdesk_width_  = 0;
    int32_t vdesk_height_ = 0;

    HSYNTHETICPOINTERDEVICE touch_device_ = nullptr;
    HSYNTHETICPOINTERDEVICE pen_device_ = nullptr;

    std::array<PointerState, kMaxContacts> touch_pointers_ = {};
    PointerState pen_pointer_ = {};

    std::mutex mutex_;
    std::condition_variable repeat_cv_;
    std::thread repeat_thread_;
    bool stop_repeat_ = false;
    bool repeat_reset_pending_ = false;

    bool inject_touch_locked(uint8_t action, uint8_t ptr_id, uint8_t tool_type,
                             uint32_t buttons, int32_t pixel_x, int32_t pixel_y,
                             uint16_t pressure, uint16_t touch_major,
                             uint16_t touch_minor, uint16_t orientation,
                             uint16_t distance, uint16_t tilt);
    bool inject_pen_locked(uint8_t action, uint8_t ptr_id, uint8_t tool_type,
                           uint32_t buttons, int32_t pixel_x, int32_t pixel_y,
                           uint16_t pressure, uint16_t touch_major,
                           uint16_t touch_minor, uint16_t orientation,
                           uint16_t distance, uint16_t tilt);
    bool inject_touch_state_locked();
    bool inject_pen_state_locked();
    bool has_repeatable_inputs_locked() const;
    void clear_touch_edge_flags_locked();
    void clear_pen_edge_flags_locked();
    void clear_pointer_edge_flags_locked(PointerState& ptr);
    void reset_pointer_locked(PointerState& ptr);
    void cancel_all_touches_locked();
    void cancel_pen_locked();
    void update_pointer_state_locked(PointerState& ptr, uint8_t action,
                                     uint8_t tool_type, uint32_t buttons,
                                     int32_t pixel_x, int32_t pixel_y,
                                     uint16_t pressure, uint16_t touch_major,
                                     uint16_t touch_minor, uint16_t orientation,
                                     uint16_t distance, uint16_t tilt);
    void map_to_virtual_desktop_locked(uint16_t x, uint16_t y,
                                       int32_t* pixel_x, int32_t* pixel_y) const;
    int find_touch_slot_by_external_id_locked(uint8_t external_id) const;
    int allocate_touch_slot_locked(uint8_t external_id);
    uint32_t pointer_flags_locked(const PointerState& ptr) const;
    void populate_touch_info_locked(POINTER_TYPE_INFO& info,
                                    const PointerState& ptr) const;
    void populate_pen_info_locked(POINTER_TYPE_INFO& info,
                                  const PointerState& ptr) const;
    void repeat_loop();
};

} // namespace droidscreen
