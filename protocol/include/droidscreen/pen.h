/*
 * DroidScreen Protocol - Pen event messages
 */

#ifndef DROIDSCREEN_PEN_H
#define DROIDSCREEN_PEN_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "droidscreen/touch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DS_PEN_EVENT_SIZE  32

/*
 * Pen event (32 bytes)
 *
 * Coordinates are stored as fractional values (0..65535) representing
 * the position as a fraction of the display dimensions.
 * pressure / distance are normalized (0..65535), or the UNKNOWN sentinel.
 * tilt is degrees (0..90) or DS_TOUCH_TILT_UNKNOWN.
 * rotation is degrees (0..359) or DS_TOUCH_ORIENTATION_UNKNOWN.
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  action;
    uint8_t  pointer_id;
    uint8_t  tool_type;
    uint8_t  reserved0;
    uint16_t x_frac;
    uint16_t y_frac;
    uint16_t pressure;
    uint16_t distance;
    uint16_t tilt;
    uint16_t rotation;
    uint32_t buttons;
    uint32_t timestamp_ms;
    uint8_t  reserved[8];
} ds_pen_event_t;
#pragma pack(pop)

size_t ds_pen_serialize(uint8_t *buf, const ds_pen_event_t *event);
size_t ds_pen_deserialize(const uint8_t *buf, ds_pen_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_PEN_H */
