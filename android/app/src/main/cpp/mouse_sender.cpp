/*
 * DroidScreen - Mouse event sender implementation
 */

#include "mouse_sender.h"
#include "tcp_server.h"

#include <android/log.h>
#include <string.h>
#include <time.h>

extern "C" {
#include "droidscreen/mouse.h"
#include "droidscreen/protocol.h"
}

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uint32_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int mouse_sender_send(int fd, int action, int buttons,
                      int x_frac, int y_frac) {
    ds_mouse_event_t event;
    memset(&event, 0, sizeof(event));
    event.action = (uint8_t)action;
    event.buttons = (uint8_t)buttons;
    event.x_frac = (uint16_t)x_frac;
    event.y_frac = (uint16_t)y_frac;
    event.timestamp_ms = get_timestamp_ms();

    uint8_t payload[DS_MOUSE_EVENT_SIZE];
    ds_mouse_serialize(payload, &event);

    ds_header_t header;
    header.type = DS_MSG_MOUSE_EVENT;
    header.flags = 0;
    header.length = DS_MOUSE_EVENT_SIZE;

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

    uint8_t wire_buf[DS_HEADER_SIZE + DS_MOUSE_EVENT_SIZE];
    memcpy(wire_buf, hdr_buf, DS_HEADER_SIZE);
    memcpy(wire_buf + DS_HEADER_SIZE, payload, DS_MOUSE_EVENT_SIZE);

    if (tcp_send_all(fd, wire_buf, sizeof(wire_buf)) != 0) {
        LOGE("mouse_sender_send: failed to send mouse event");
        return -1;
    }

    return 0;
}
