/*
 * DroidScreen - outbound queue to the desktop.
 *
 * Input events (touch, pen, mouse, keyboard), Deck/volume actions and the
 * receiver-backlog report are built on the caller's thread (stamped there)
 * and pushed into a bounded FIFO. One sender thread drains it in order and
 * does the blocking tcp_send_all(), so the UI thread never waits on the
 * socket or on g_send_mutex.
 */

#ifndef DROIDSCREEN_OUTBOUND_H
#define DROIDSCREEN_OUTBOUND_H

#include <stddef.h>
#include <stdint.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/keyboard.h"
}

/* Largest single message any builder produces (a keyboard text chunk). */
#define DS_OUTBOUND_MAX_MSG (DS_HEADER_SIZE + DS_KEY_EVENT_HEADER_SIZE + DS_KEY_TEXT_MAX)

/* Start / stop the sender thread (nativeInit / nativeStop). */
void outbound_start(void);
void outbound_stop(void);

/*
 * A new connection is live on `fd` (fd >= 0), or the current one ended
 * (fd = -1). Ending a session drops everything still queued for it.
 */
void outbound_set_session(int fd);

/* True while a session is live (callers can skip building messages). */
bool outbound_connected(void);

/*
 * Queue one complete message (header + payload, len <= DS_OUTBOUND_MAX_MSG).
 * Never blocks. Returns false if no session is live or the queue is full
 * (the event is dropped and counted; the queue only fills when the socket
 * has stalled, which ends the connection anyway).
 */
bool outbound_enqueue(const uint8_t *msg, size_t len);

#endif /* DROIDSCREEN_OUTBOUND_H */
