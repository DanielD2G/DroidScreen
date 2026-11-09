/*
 * DroidScreen Protocol - Keyboard input (tablet soft keyboard -> desktop)
 *
 * Payload: [kind:u8][modifiers:u8][key:u16 LE][text_len:u16 LE][text: UTF-8]
 *   kind = DS_KEY_KIND_TEXT:    type `text` (key ignored)
 *   kind = DS_KEY_KIND_SPECIAL: press and release `key` (text_len = 0)
 */

#ifndef DROIDSCREEN_KEYBOARD_H
#define DROIDSCREEN_KEYBOARD_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DS_KEY_EVENT_HEADER_SIZE 6
/** Longest text in one message; longer input is split by the sender. */
#define DS_KEY_TEXT_MAX          512

typedef enum {
    DS_KEY_KIND_TEXT    = 1,
    DS_KEY_KIND_SPECIAL = 2
} ds_key_kind_t;

typedef enum {
    DS_KEY_BACKSPACE   = 1,
    DS_KEY_ENTER       = 2,
    DS_KEY_TAB         = 3,
    DS_KEY_ESCAPE      = 4,
    DS_KEY_LEFT        = 5,
    DS_KEY_RIGHT       = 6,
    DS_KEY_UP          = 7,
    DS_KEY_DOWN        = 8,
    DS_KEY_FORWARD_DEL = 9,
    DS_KEY_HOME        = 10,
    DS_KEY_END         = 11
} ds_key_code_t;

typedef enum {
    DS_KEY_MOD_SHIFT   = 1 << 0,
    DS_KEY_MOD_CONTROL = 1 << 1,
    DS_KEY_MOD_OPTION  = 1 << 2,
    DS_KEY_MOD_COMMAND = 1 << 3
} ds_key_modifier_t;

typedef struct {
    uint8_t     kind;
    uint8_t     modifiers;
    uint16_t    key;
    uint16_t    text_len;
    const char *text;      /* not NUL-terminated; points into the buffer on deserialize */
} ds_key_event_t;

/** Serialize into buf (needs DS_KEY_EVENT_HEADER_SIZE + text_len bytes). Returns bytes written. */
size_t ds_key_event_serialize(uint8_t *buf, const ds_key_event_t *event);

/** Parse a payload of `len` bytes. Returns 0 if malformed (short, or text_len past the end). */
size_t ds_key_event_deserialize(const uint8_t *buf, size_t len, ds_key_event_t *event);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_KEYBOARD_H */
