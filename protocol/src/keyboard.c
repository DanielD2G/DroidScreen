/*
 * DroidScreen Protocol - Keyboard input serialization
 */

#include "droidscreen/keyboard.h"

#include <string.h>

size_t ds_key_event_serialize(uint8_t *buf, const ds_key_event_t *event) {
    uint16_t len = event->text_len;
    if (len > DS_KEY_TEXT_MAX) len = DS_KEY_TEXT_MAX;
    buf[0] = event->kind;
    buf[1] = event->modifiers;
    buf[2] = (uint8_t)(event->key & 0xFF);
    buf[3] = (uint8_t)(event->key >> 8);
    buf[4] = (uint8_t)(len & 0xFF);
    buf[5] = (uint8_t)(len >> 8);
    if (len > 0 && event->text) memcpy(buf + DS_KEY_EVENT_HEADER_SIZE, event->text, len);
    return DS_KEY_EVENT_HEADER_SIZE + (size_t)len;
}

size_t ds_key_event_deserialize(const uint8_t *buf, size_t len, ds_key_event_t *event) {
    if (len < DS_KEY_EVENT_HEADER_SIZE) return 0;
    event->kind      = buf[0];
    event->modifiers = buf[1];
    event->key       = (uint16_t)(buf[2] | (buf[3] << 8));
    event->text_len  = (uint16_t)(buf[4] | (buf[5] << 8));
    if (event->text_len > DS_KEY_TEXT_MAX ||
        (size_t)DS_KEY_EVENT_HEADER_SIZE + event->text_len > len) {
        return 0;
    }
    event->text = event->text_len ? (const char *)(buf + DS_KEY_EVENT_HEADER_SIZE) : NULL;
    return DS_KEY_EVENT_HEADER_SIZE + (size_t)event->text_len;
}
