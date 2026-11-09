/*
 * DroidScreen - TCP server implementation
 */

#include "tcp_server.h"

#include <android/log.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <arpa/inet.h>

#include <mutex>

/* Several threads write to the desktop socket (UI thread: touch, pen, mouse,
 * Deck, volume, keyboard; native thread: pong, keyframe requests). send() can
 * write a message partially when the socket buffer is full, so without this
 * lock another thread's message could land in the middle and corrupt the
 * framing. Every caller passes one complete message per call. */
static std::mutex g_send_mutex;

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)

int tcp_server_start(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        LOGE("tcp_server_start: socket() failed: %s", strerror(errno));
        return -1;
    }

    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        LOGE("tcp_server_start: bind() failed on port %d: %s", port, strerror(errno));
        close(fd);
        return -1;
    }

    if (listen(fd, 1) < 0) {
        LOGE("tcp_server_start: listen() failed: %s", strerror(errno));
        close(fd);
        return -1;
    }

    LOGI("tcp_server_start: listening on 0.0.0.0:%d", port);
    return fd;
}

int tcp_server_accept(int server_fd, int *is_loopback) {
    struct sockaddr_in client_addr;
    socklen_t addr_len = sizeof(client_addr);

    int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &addr_len);
    if (client_fd < 0) {
        LOGE("tcp_server_accept: accept() failed: %s", strerror(errno));
        return -1;
    }

    tcp_set_nodelay(client_fd);
    int loopback = (ntohl(client_addr.sin_addr.s_addr) >> 24) == 127;
    if (is_loopback) *is_loopback = loopback;
    char ip[INET_ADDRSTRLEN] = {0};
    inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
    LOGI("tcp_server_accept: client %s connected (%s), fd=%d",
         ip, loopback ? "usb" : "wifi", client_fd);
    return client_fd;
}

int tcp_recv_exact(int fd, uint8_t *buf, size_t len) {
    size_t received = 0;
    while (received < len) {
        ssize_t n = recv(fd, buf + received, len - received, 0);
        if (n <= 0) {
            if (n == 0) {
                /* Connection closed */
                return -1;
            }
            if (errno == EINTR) {
                continue;
            }
            LOGE("tcp_recv_exact: recv() error: %s", strerror(errno));
            return -1;
        }
        received += (size_t)n;
    }
    return 0;
}

int tcp_send_all(int fd, const uint8_t *buf, size_t len) {
    std::lock_guard<std::mutex> lock(g_send_mutex);
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            LOGE("tcp_send_all: send() error: %s", strerror(errno));
            /* A failed or timed-out send may have left a partial message on
             * the wire, so the framing is broken: drop the connection (the
             * recv thread sees it and the desktop reconnects). */
            shutdown(fd, SHUT_RDWR);
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

void tcp_set_nodelay(int fd) {
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    /* Moderate socket buffers — large enough for keyframes, small enough
     * to avoid OS-level queuing latency on USB connections (128 KB each). */
    int bufsize = 131072;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsize, sizeof(bufsize));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsize, sizeof(bufsize));

    /* Bound every send: with g_send_mutex a stalled send() would otherwise
     * block all senders (UI thread, recv thread's pong, keyframe requests). */
    struct timeval snd_timeout;
    snd_timeout.tv_sec = 2;
    snd_timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));

    /* Disable delayed ACKs for lower round-trip latency (Linux/Android only) */
#ifdef TCP_QUICKACK
    int quickack = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif
}

void tcp_set_recv_timeout(int fd, int timeout_ms) {
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

void tcp_close(int fd) {
    if (fd >= 0) {
        /* Same lock as tcp_send_all(): a close can't land between a failed
         * send and its shutdown(), so that shutdown never hits a reused fd.
         * (Waits at most one bounded send, see SO_SNDTIMEO.) */
        std::lock_guard<std::mutex> lock(g_send_mutex);
        shutdown(fd, SHUT_RDWR);
        close(fd);
    }
}
