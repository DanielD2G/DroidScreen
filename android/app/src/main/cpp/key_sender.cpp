/*
 * DroidScreen - Keyboard event messages
 */

#include "key_sender.h"
#include "outbound.h"

#include <string.h>

static int enqueue_key_event(const ds_key_event_t *ev) {
    uint8_t wire[DS_OUTBOUND_MAX_MSG];
    size_t payload_len = ds_key_event_serialize(wire + DS_HEADER_SIZE, ev);

    ds_header_t header;
    header.type   = DS_MSG_KEY_EVENT;
    header.flags  = 0;
    header.length = (uint32_t)payload_len;
    ds_header_serialize(wire, &header);

    return outbound_enqueue(wire, DS_HEADER_SIZE + payload_len) ? 0 : -1;
}

int key_text_enqueue(const char *utf8, size_t len) {
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
        if (enqueue_key_event(&ev) != 0) return -1;
        utf8 += chunk;
        len  -= chunk;
    }
    return 0;
}

int key_special_enqueue(int key, int modifiers) {
    ds_key_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.kind      = DS_KEY_KIND_SPECIAL;
    ev.modifiers = (uint8_t)modifiers;
    ev.key       = (uint16_t)key;
    return enqueue_key_event(&ev);
}
