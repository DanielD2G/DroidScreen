/*
 * DroidScreen - Keyboard event messages (tablet soft keyboard -> desktop)
 */

#ifndef DROIDSCREEN_KEY_SENDER_H
#define DROIDSCREEN_KEY_SENDER_H

#include <stddef.h>

/** Queue UTF-8 text, split into protocol-sized chunks on character boundaries. 0 on success. */
int key_text_enqueue(const char *utf8, size_t len);

/** Queue one special key press+release (DS_KEY_*), with DS_KEY_MOD_* modifiers. 0 on success. */
int key_special_enqueue(int key, int modifiers);

#endif /* DROIDSCREEN_KEY_SENDER_H */
