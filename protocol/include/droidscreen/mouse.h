/*
 * DroidScreen Protocol - Mouse event messages
 */

#ifndef DROIDSCREEN_MOUSE_H
#define DROIDSCREEN_MOUSE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "droidscreen/touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DS_MOUSE_BUTTON_LEFT   = 1 << 0,
    DS_MOUSE_BUTTON_RIGHT  = 1 << 1,
    DS_MOUSE_BUTTON_MIDDLE = 1 << 2
} ds_mouse_button_t;

#define DS_MOUSE_EVENT_SIZE  16

#pragma pack(push, 1)
typedef struct {
    uint8_t  action;
    uint8_t  buttons;
    uint8_t  reserved0[2];
    uint16_t x_frac;
    uint16_t y_frac;
    uint32_t timestamp_ms;
    uint8_t  reserved[4];
} ds_mouse_event_t;
#pragma pack(pop)

size_t ds_mouse_serialize(uint8_t *buf, const ds_mouse_event_t *event);
size_t ds_mouse_deserialize(const uint8_t *buf, ds_mouse_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_MOUSE_H */
