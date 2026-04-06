/*
 * DroidScreen - Touch event sender implementation
 */

#include "touch_sender.h"
#include "tcp_server.h"

#include <android/log.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/touch.h"
}

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uint32_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int touch_sender_send(int fd, int action, int pointer_id,
                      int tool_type, int buttons,
                      int x_frac, int y_frac, int pressure,
                      int touch_major, int touch_minor, int orientation,
                      int distance, int tilt) {
    /* Build touch event */
    ds_touch_event_t event;
    memset(&event, 0, sizeof(event));
    event.action       = (uint8_t)action;
    event.pointer_id   = (uint8_t)pointer_id;
    event.tool_type    = (uint8_t)tool_type;
    event.x_frac       = (uint16_t)x_frac;
    event.y_frac       = (uint16_t)y_frac;
    event.pressure     = (uint16_t)pressure;
    event.touch_major  = (uint16_t)touch_major;
    event.touch_minor  = (uint16_t)touch_minor;
    event.orientation  = (uint16_t)orientation;
    event.distance     = (uint16_t)distance;
    event.tilt         = (uint16_t)tilt;
    event.buttons      = (uint32_t)buttons;
    event.timestamp_ms = get_timestamp_ms();

    /* Serialize payload */
    uint8_t payload[DS_TOUCH_EVENT_SIZE];
    ds_touch_serialize(payload, &event);

    /* Serialize header */
    ds_header_t header;
    header.type   = DS_MSG_TOUCH_EVENT;
    header.flags  = 0;
    header.length = DS_TOUCH_EVENT_SIZE;

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

    /* Send header + payload */
    uint8_t wire_buf[DS_HEADER_SIZE + DS_TOUCH_EVENT_SIZE];
    memcpy(wire_buf, hdr_buf, DS_HEADER_SIZE);
    memcpy(wire_buf + DS_HEADER_SIZE, payload, DS_TOUCH_EVENT_SIZE);

    if (tcp_send_all(fd, wire_buf, sizeof(wire_buf)) != 0) {
        LOGE("touch_sender_send: failed to send touch event");
        return -1;
    }

    return 0;
}
