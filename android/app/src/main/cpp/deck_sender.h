/*
 * DroidScreen - Deck event sender
 */

#ifndef DROIDSCREEN_DECK_SENDER_H
#define DROIDSCREEN_DECK_SENDER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Serialize and send a deck action over the TCP connection.
 * Sends DS_HEADER_SIZE + DS_DECK_ACTION_SIZE bytes.
 *
 * tile_id is converted to a button_id (max 11 chars + NUL).
 *
 * Returns 0 on success, -1 on error.
 */
int deck_action_send(int fd, int action_type, const char* tile_id);

/*
 * Serialize and send a volume change over the TCP connection.
 * Sends DS_HEADER_SIZE + DS_VOLUME_STATE_SIZE bytes.
 *
 * Returns 0 on success, -1 on error.
 */
int volume_change_send(int fd, int volume, int muted);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_DECK_SENDER_H */
