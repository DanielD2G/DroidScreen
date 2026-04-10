/*
 * DroidScreen - Touch event sender
 */

#ifndef DROIDSCREEN_TOUCH_SENDER_H
#define DROIDSCREEN_TOUCH_SENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize and send a touch event over the TCP connection.
 * Sends DS_HEADER_SIZE + DS_TOUCH_EVENT_SIZE bytes.
 *
 * Returns 0 on success, -1 on error.
 */
int touch_sender_send(int fd, int action, int pointer_id,
                      int x_frac, int y_frac, int pressure,
                      int touch_major, int touch_minor, int orientation);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_TOUCH_SENDER_H */
