/*
 * DroidScreen - Keyboard event sender implementation
 */

#include "key_sender.h"
#include "tcp_server.h"

#include <android/log.h>
#include <string.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/keyboard.h"
}

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static int send_key_event(int fd, const ds_key_event_t *ev) {
    /* Header + payload in one buffer, one send, like the other senders. */
    uint8_t wire[DS_HEADER_SIZE + DS_KEY_EVENT_HEADER_SIZE + DS_KEY_TEXT_MAX];
    size_t payload_len = ds_key_event_serialize(wire + DS_HEADER_SIZE, ev);

    ds_header_t header;
    header.type   = DS_MSG_KEY_EVENT;
    header.flags  = 0;
    header.length = (uint32_t)payload_len;
    ds_header_serialize(wire, &header);

    if (tcp_send_all(fd, wire, DS_HEADER_SIZE + payload_len) != 0) {
        LOGE("key event send failed");
        return -1;
    }
    return 0;
}

int key_text_send(int fd, const char *utf8, size_t len) {
    while (len > 0) {
        size_t chunk = len < DS_KEY_TEXT_MAX ? len : DS_KEY_TEXT_MAX;
        /* Never cut a multi-byte UTF-8 sequence: back off to a lead byte. */
        if (chunk < len) {
            while (chunk > 0 && ((unsigned char)utf8[chunk] & 0xC0) == 0x80) chunk--;
            if (chunk == 0) return -1;
        }
        ds_key_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.kind     = DS_KEY_KIND_TEXT;
        ev.text_len = (uint16_t)chunk;
        ev.text     = utf8;
        if (send_key_event(fd, &ev) != 0) return -1;
        utf8 += chunk;
        len  -= chunk;
    }
    return 0;
}

int key_special_send(int fd, int key, int modifiers) {
    ds_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind      = DS_KEY_KIND_SPECIAL;
    ev.modifiers = (uint8_t)modifiers;
    ev.key       = (uint16_t)key;
    return send_key_event(fd, &ev);
}
