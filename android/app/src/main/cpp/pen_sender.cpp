/*
 * DroidScreen - Pen event sender implementation
 */

#include "pen_sender.h"
#include "tcp_server.h"

#include <android/log.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "droidscreen/pen.h"
#include "droidscreen/protocol.h"
}

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uint32_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int pen_sender_send(int fd, int action, int pointer_id,
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
    event.timestamp_ms = get_timestamp_ms();

    uint8_t payload[DS_PEN_EVENT_SIZE];
    ds_pen_serialize(payload, &event);

    ds_header_t header;
    header.type = DS_MSG_PEN_EVENT;
    header.flags = 0;
    header.length = DS_PEN_EVENT_SIZE;

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

    uint8_t wire_buf[DS_HEADER_SIZE + DS_PEN_EVENT_SIZE];
    memcpy(wire_buf, hdr_buf, DS_HEADER_SIZE);
    memcpy(wire_buf + DS_HEADER_SIZE, payload, DS_PEN_EVENT_SIZE);

    if (tcp_send_all(fd, wire_buf, sizeof(wire_buf)) != 0) {
        LOGE("pen_sender_send: failed to send pen event");
        return -1;
    }

    return 0;
}
