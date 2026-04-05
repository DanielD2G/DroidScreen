/*
 * DroidScreen Protocol - Touch event messages
 */

#ifndef DROIDSCREEN_TOUCH_H
#define DROIDSCREEN_TOUCH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Touch action types
 */
typedef enum {
    DS_TOUCH_DOWN   = 0,
    DS_TOUCH_MOVE   = 1,
    DS_TOUCH_UP     = 2,
    DS_TOUCH_CANCEL = 3
} ds_touch_action_t;

#define DS_TOUCH_EVENT_SIZE  16

/*
 * Touch event (16 bytes)
 *
 * Coordinates are stored as fractional values (0..65535) representing
 * the position as a fraction of the display dimensions.
 * 0 = left/top edge, 65535 = right/bottom edge.
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  action;
    uint8_t  pointer_id;
    uint16_t x_frac;
    uint16_t y_frac;
    uint16_t pressure;
    uint32_t timestamp_ms;
    uint8_t  reserved[4];
} ds_touch_event_t;
#pragma pack(pop)

/*
 * Serialize touch event to buffer.
 * buf must be at least DS_TOUCH_EVENT_SIZE bytes.
 * Returns DS_TOUCH_EVENT_SIZE on success.
 */
size_t ds_touch_serialize(uint8_t *buf, const ds_touch_event_t *event);

/*
 * Deserialize touch event from buffer.
 * buf must contain at least DS_TOUCH_EVENT_SIZE bytes.
 * Returns DS_TOUCH_EVENT_SIZE on success.
 */
size_t ds_touch_deserialize(const uint8_t *buf, ds_touch_event_t *event);

/*
 * Convert pixel coordinates to fractional coordinates.
 *
 * pixel_x, pixel_y:           pixel position on the surface
 * surface_width, surface_height: dimensions of the surface in pixels
 * x_frac, y_frac:             output fractional values (0..65535)
 */
void ds_touch_to_frac(int pixel_x, int pixel_y,
                      int surface_width, int surface_height,
                      uint16_t *x_frac, uint16_t *y_frac);

/*
 * Convert fractional coordinates back to pixel coordinates.
 *
 * x_frac, y_frac:               fractional values (0..65535)
 * display_width, display_height: dimensions of the display in pixels
 * pixel_x, pixel_y:             output pixel positions
 */
void ds_touch_from_frac(uint16_t x_frac, uint16_t y_frac,
                        int display_width, int display_height,
                        int *pixel_x, int *pixel_y);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_TOUCH_H */
