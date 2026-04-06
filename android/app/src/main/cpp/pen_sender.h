/*
 * DroidScreen - Pen event sender
 */

#ifndef DROIDSCREEN_PEN_SENDER_H
#define DROIDSCREEN_PEN_SENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int pen_sender_send(int fd, int action, int pointer_id,
                    int tool_type, int buttons,
                    int x_frac, int y_frac, int pressure,
                    int distance, int tilt, int rotation);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_PEN_SENDER_H */
