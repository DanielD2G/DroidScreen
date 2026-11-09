/*
 * DroidScreen - Pen event message builder
 */

#ifndef DROIDSCREEN_PEN_SENDER_H
#define DROIDSCREEN_PEN_SENDER_H

#include <stddef.h>
#include <stdint.h>

/* Complete pen message into `out` (DS_OUTBOUND_MAX_MSG bytes), stamped now. */
size_t pen_build(uint8_t *out, int action, int pointer_id,
                 int tool_type, int buttons,
                 int x_frac, int y_frac, int pressure,
                 int distance, int tilt, int rotation);

#endif /* DROIDSCREEN_PEN_SENDER_H */
