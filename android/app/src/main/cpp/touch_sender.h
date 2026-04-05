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
 * Sends DS_HEADER_SIZE (6) + DS_TOUCH_EVENT_SIZE (16) = 22 bytes.
 *
 * Returns 0 on success, -1 on error.
 */
int touch_sender_send(int fd, int action, int pointer_id,
                      int x_frac, int y_frac, int pressure);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_TOUCH_SENDER_H */
