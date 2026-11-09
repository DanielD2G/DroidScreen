/*
 * DroidScreen - Mouse event message builder
 */

#ifndef DROIDSCREEN_MOUSE_SENDER_H
#define DROIDSCREEN_MOUSE_SENDER_H

#include <stddef.h>
#include <stdint.h>

/* Complete mouse message into `out` (DS_OUTBOUND_MAX_MSG bytes), stamped now. */
size_t mouse_build(uint8_t *out, int action, int buttons,
                   int x_frac, int y_frac);

#endif /* DROIDSCREEN_MOUSE_SENDER_H */
