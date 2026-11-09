/*
 * DroidScreen - Touch event message builder
 */

#include "touch_sender.h"
#include "ds_clock.h"

#include <string.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/touch.h"
}

size_t touch_build(uint8_t *out, int action, int pointer_id,
                   int x_frac, int y_frac, int pressure,
                   int touch_major, int touch_minor, int orientation) {
    ds_touch_event_t event;
    memset(&event, 0, sizeof(event));
    event.action       = (uint8_t)action;
    event.pointer_id   = (uint8_t)pointer_id;
    event.x_frac       = (uint16_t)x_frac;
    event.y_frac       = (uint16_t)y_frac;
    event.pressure     = (uint16_t)pressure;
    event.touch_major  = (uint16_t)touch_major;
    event.touch_minor  = (uint16_t)touch_minor;
    event.orientation  = (uint16_t)orientation;
    event.timestamp_ms = ds_now_ms32();

    ds_header_t header;
    header.type   = DS_MSG_TOUCH_EVENT;
    header.flags  = 0;
    header.length = DS_TOUCH_EVENT_SIZE;
    ds_header_serialize(out, &header);
    ds_touch_serialize(out + DS_HEADER_SIZE, &event);
    return DS_HEADER_SIZE + DS_TOUCH_EVENT_SIZE;
}
