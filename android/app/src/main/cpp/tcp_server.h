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
 * Start a TCP server listening on 127.0.0.1:port.
 * Returns the server socket fd, or -1 on error.
 */
int tcp_server_start(int port);

/*
 * Blocking accept on the server socket.
 * Sets TCP_NODELAY on the accepted connection.
 * Returns the client socket fd, or -1 on error.
 */
int tcp_server_accept(int server_fd);

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
