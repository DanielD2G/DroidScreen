/*
 * DroidScreen - Mouse event sender
 */

#ifndef DROIDSCREEN_MOUSE_SENDER_H
#define DROIDSCREEN_MOUSE_SENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int mouse_sender_send(int fd, int action, int buttons,
                      int x_frac, int y_frac);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_MOUSE_SENDER_H */
