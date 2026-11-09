/*
 * DroidScreen - TCP server utilities
 */

#ifndef DROIDSCREEN_TCP_SERVER_H
#define DROIDSCREEN_TCP_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Start a TCP server listening on all interfaces (0.0.0.0:port), so both the
 * adb tunnel (loopback) and the local Wi-Fi network can reach it.
 * Returns the server socket fd, or -1 on error.
 */
int tcp_server_start(int port);

/*
 * Blocking accept on the server socket.
 * Sets TCP_NODELAY on the accepted connection.
 * If is_loopback is non-NULL it is set to 1 when the peer is 127.0.0.0/8
 * (the adb tunnel), 0 when it came over the network.
 * Returns the client socket fd, or -1 on error.
 */
int tcp_server_accept(int server_fd, int *is_loopback);

/*
 * Set (timeout_ms > 0) or clear (timeout_ms == 0) the receive timeout.
 */
void tcp_set_recv_timeout(int fd, int timeout_ms);

/*
 * Blocking receive of exactly `len` bytes into `buf`.
 * Handles partial reads internally.
 * Returns 0 on success, -1 on error or connection close.
 */
int tcp_recv_exact(int fd, uint8_t *buf, size_t len);

/*
 * Blocking send of exactly `len` bytes from `buf`.
 * Handles partial writes internally.
 * Returns 0 on success, -1 on error.
 */
int tcp_send_all(int fd, const uint8_t *buf, size_t len);

/*
 * Set TCP_NODELAY on the socket.
 */
void tcp_set_nodelay(int fd);

/*
 * Close a socket fd.
 */
void tcp_close(int fd);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_TCP_SERVER_H */
