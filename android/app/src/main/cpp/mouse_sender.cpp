/*
 * DroidScreen - Mouse event message builder
 */

#include "mouse_sender.h"
#include "ds_clock.h"

#include <string.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/mouse.h"
}

size_t mouse_build(uint8_t *out, int action, int buttons,
                   int x_frac, int y_frac) {
    ds_mouse_event_t event;
    memset(&event, 0, sizeof(event));
    event.action = (uint8_t)action;
    event.buttons = (uint8_t)buttons;
    event.x_frac = (uint16_t)x_frac;
    event.y_frac = (uint16_t)y_frac;
    event.timestamp_ms = ds_now_ms32();

    ds_header_t header;
    header.type = DS_MSG_MOUSE_EVENT;
    header.flags = 0;
    header.length = DS_MOUSE_EVENT_SIZE;
    ds_header_serialize(out, &header);
    ds_mouse_serialize(out + DS_HEADER_SIZE, &event);
    return DS_HEADER_SIZE + DS_MOUSE_EVENT_SIZE;
}
