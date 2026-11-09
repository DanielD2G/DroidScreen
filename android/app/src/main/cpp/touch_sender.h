/*
 * DroidScreen - Touch event message builder
 */

#ifndef DROIDSCREEN_TOUCH_SENDER_H
#define DROIDSCREEN_TOUCH_SENDER_H

#include <stddef.h>
#include <stdint.h>

/*
 * Serialize a complete touch message (header + payload) into `out`
 * (DS_OUTBOUND_MAX_MSG bytes). Stamped now, i.e. at enqueue time.
 * Returns the message length.
 */
size_t touch_build(uint8_t *out, int action, int pointer_id,
                   int x_frac, int y_frac, int pressure,
                   int touch_major, int touch_minor, int orientation);

#endif /* DROIDSCREEN_TOUCH_SENDER_H */
