/*
 * DroidScreen - Pen event message builder
 */

#include "pen_sender.h"
#include "ds_clock.h"

#include <string.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/pen.h"
}

size_t pen_build(uint8_t *out, int action, int pointer_id,
                 int tool_type, int buttons,
                 int x_frac, int y_frac, int pressure,
                 int distance, int tilt, int rotation) {
    ds_pen_event_t event;
    memset(&event, 0, sizeof(event));
    event.action = (uint8_t)action;
    event.pointer_id = (uint8_t)pointer_id;
    event.tool_type = (uint8_t)tool_type;
    event.x_frac = (uint16_t)x_frac;
    event.y_frac = (uint16_t)y_frac;
    event.pressure = (uint16_t)pressure;
    event.distance = (uint16_t)distance;
    event.tilt = (uint16_t)tilt;
    event.rotation = (uint16_t)rotation;
    event.buttons = (uint32_t)buttons;
    event.timestamp_ms = ds_now_ms32();

    ds_header_t header;
    header.type = DS_MSG_PEN_EVENT;
    header.flags = 0;
    header.length = DS_PEN_EVENT_SIZE;
    ds_header_serialize(out, &header);
    ds_pen_serialize(out + DS_HEADER_SIZE, &event);
    return DS_HEADER_SIZE + DS_PEN_EVENT_SIZE;
}
