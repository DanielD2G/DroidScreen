/*
 * DroidScreen - Keyboard event sender (tablet soft keyboard -> desktop)
 */

#ifndef DROIDSCREEN_KEY_SENDER_H
#define DROIDSCREEN_KEY_SENDER_H

#include <stddef.h>

/** Send UTF-8 text, split into protocol-sized chunks on character boundaries. 0 on success. */
int key_text_send(int fd, const char *utf8, size_t len);

/** Press and release one special key (DS_KEY_*), with DS_KEY_MOD_* modifiers. 0 on success. */
int key_special_send(int fd, int key, int modifiers);

#endif /* DROIDSCREEN_KEY_SENDER_H */
