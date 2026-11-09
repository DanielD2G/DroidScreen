/*
 * DroidScreen Desktop - TCP client implementation
 */

#include "droidscreen/server.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <unistd.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <errno.h>
#endif

namespace droidscreen {

#ifdef _WIN32
bool TCPClient::wsa_initialized_ = false;

bool TCPClient::init_wsa() {
    if (wsa_initialized_) return true;
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        fprintf(stderr, "[tcp] WSAStartup failed: %d\n", WSAGetLastError());
        return false;
    }
    wsa_initialized_ = true;
    return true;
}
#endif

TCPClient::TCPClient() : fd_(kInvalidSocket) {}

TCPClient::~TCPClient() {
    close();
}

void TCPClient::configure_socket() {
    // Set TCP_NODELAY to disable Nagle's algorithm for low latency.
    int flag = 1;
#ifdef _WIN32
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif

    // Moderate socket buffers — enough for keyframes, small enough to
    // avoid OS-level queuing latency (128 KB each). On Wi-Fi the adaptive
    // rate controller watches the send queue, so a bigger kernel buffer would
    // only hide congestion from it.
    int buf_size = 128 * 1024;  // 131072
#ifdef _WIN32
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&buf_size), sizeof(buf_size));
#else
    setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf_size, sizeof(buf_size));
    setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf_size, sizeof(buf_size));
#endif

#ifdef __linux__
    // TCP_QUICKACK disables delayed ACKs for lower RTT on Linux.
    int quickack = 1;
    setsockopt(fd_, IPPROTO_TCP, TCP_QUICKACK, &quickack, sizeof(quickack));
#endif

#ifdef __APPLE__
    // A write to a peer that vanished must return an error, not kill the app.
    int nosigpipe = 1;
    setsockopt(fd_, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, sizeof(nosigpipe));
#endif

    // Send timeout: a blocking write must NEVER hang forever. If the device
    // stops draining (USB/adb-forward hiccup, Wi-Fi dropout, decoder stall,
    // half-open peer), an unbounded writev() would deadlock the send thread
    // while holding write_mutex_ — which also blocks the ping thread, killing
    // the RTT liveness signal. With SO_SNDTIMEO the write fails with
    // EWOULDBLOCK, the send thread tears the pipeline down, and the owner
    // reconnects.
    //
    // The timeout must be well above the worst-case *legitimate* stall: a heavy
    // motion burst (large keyframe + high-motion deltas) can momentarily
    // overwhelm the decoder, so writev() may make zero progress for a few
    // seconds and then recover on its own. A too-tight timeout kills those
    // healthy bursts and causes reconnect churn. True deadlocks last minutes, so
    // 8 s cleanly separates "slow burst" (ride it out) from "dead link".
    constexpr int kSendTimeoutMs = 8000;
#ifdef _WIN32
    DWORD snd_timeout = kSendTimeoutMs;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&snd_timeout), sizeof(snd_timeout));
#else
    struct timeval snd_timeout;
    snd_timeout.tv_sec  = kSendTimeoutMs / 1000;
    snd_timeout.tv_usec = (kSendTimeoutMs % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout, sizeof(snd_timeout));
#endif
}

bool TCPClient::connect(uint16_t port) {
    return connect("127.0.0.1", port, 3000);
}

static void close_socket(socket_t fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    ::close(fd);
#endif
}

static bool set_blocking(socket_t fd, bool blocking) {
#ifdef _WIN32
    u_long mode = blocking ? 0 : 1;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    flags = blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK);
    return fcntl(fd, F_SETFL, flags) == 0;
#endif
}

bool TCPClient::connect(const char* host, uint16_t port, int timeout_ms) {
#ifdef _WIN32
    if (!init_wsa()) return false;
#endif
    close();

    // The Android server listens on IPv4 only.
    struct addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    struct addrinfo* res = nullptr;
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);
    int gai = getaddrinfo(host, port_str, &hints, &res);
    if (gai != 0 || !res) {
        fprintf(stderr, "[tcp] cannot resolve %s: %s\n", host, gai_strerror(gai));
        return false;
    }

    fd_ = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd_ == kInvalidSocket) {
        fprintf(stderr, "[tcp] socket() failed\n");
        freeaddrinfo(res);
        return false;
    }
    configure_socket();

    // Non-blocking connect so an unreachable host fails after timeout_ms
    // instead of the OS default (75 s on macOS).
    bool ok = set_blocking(fd_, false);
    if (ok) {
        int rc = ::connect(fd_, res->ai_addr, static_cast<int>(res->ai_addrlen));
#ifdef _WIN32
        bool in_progress = rc != 0 && WSAGetLastError() == WSAEWOULDBLOCK;
#else
        bool in_progress = rc != 0 && errno == EINPROGRESS;
#endif
        if (rc == 0) {
            ok = true;
        } else if (in_progress) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd_, &wfds);
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            int sel = ::select(static_cast<int>(fd_) + 1, nullptr, &wfds, nullptr, &tv);
            if (sel <= 0) {
                fprintf(stderr, "[tcp] connect() to %s:%u timed out\n", host, port);
                ok = false;
            } else {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(fd_, SOL_SOCKET, SO_ERROR,
                           reinterpret_cast<char*>(&err), &len);
                if (err != 0) {
                    fprintf(stderr, "[tcp] connect() to %s:%u failed: %s\n",
                            host, port, strerror(err));
                }
                ok = err == 0;
            }
        } else {
            fprintf(stderr, "[tcp] connect() to %s:%u failed\n", host, port);
            ok = false;
        }
    }
    freeaddrinfo(res);

    if (!ok || !set_blocking(fd_, true)) {
        close_socket(fd_);
        fd_ = kInvalidSocket;
        return false;
    }

    fprintf(stderr, "[tcp] connected to %s:%u\n", host, port);
    return true;
}

void TCPClient::set_recv_timeout(int timeout_ms) {
    if (fd_ == kInvalidSocket) return;
#ifdef _WIN32
    DWORD tv = static_cast<DWORD>(timeout_ms);
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&tv), sizeof(tv));
#else
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

HelloResult perform_hello(TCPClient* client,
                          const uint8_t desktop_id[DS_HELLO_ID_SIZE],
                          uint8_t token[DS_HELLO_TOKEN_SIZE],
                          const char* desktop_name) {
    ds_hello_t hello{};
    memcpy(hello.desktop_id, desktop_id, DS_HELLO_ID_SIZE);
    memcpy(hello.token, token, DS_HELLO_TOKEN_SIZE);
    snprintf(hello.name, sizeof(hello.name), "%s", desktop_name ? desktop_name : "");

    uint8_t buf[DS_HELLO_SIZE];
    ds_hello_serialize(buf, &hello);
    if (!client->send_message(DS_MSG_HELLO, 0, buf, DS_HELLO_SIZE)) {
        return HelloResult::Failed;
    }

    // The tablet may be showing an approval prompt; wait a bit longer than it does.
    client->set_recv_timeout(DS_HELLO_APPROVAL_TIMEOUT_MS + 5000);
    ds_header_t hdr;
    uint8_t resp_buf[DS_HELLO_RESP_SIZE];
    bool got = client->recv_header(&hdr) &&
               hdr.type == DS_MSG_HELLO_RESP &&
               hdr.length == DS_HELLO_RESP_SIZE &&
               client->recv_exact(resp_buf, DS_HELLO_RESP_SIZE);
    client->set_recv_timeout(0);
    if (!got) {
        fprintf(stderr, "[tcp] no valid hello response\n");
        return HelloResult::Failed;
    }

    ds_hello_resp_t resp;
    ds_hello_resp_deserialize(resp_buf, &resp);
    if (resp.status != DS_HELLO_ACCEPTED) {
        return HelloResult::Rejected;
    }
    memcpy(token, resp.token, DS_HELLO_TOKEN_SIZE);
    return HelloResult::Accepted;
}

bool TCPClient::send_all(const void* data, size_t len) {
    if (fd_ == kInvalidSocket) return false;

    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = len;

    while (remaining > 0) {
#ifdef _WIN32
        int sent = ::send(fd_, reinterpret_cast<const char*>(ptr),
                          static_cast<int>(remaining), 0);
#else
        ssize_t sent = ::send(fd_, ptr, remaining, MSG_NOSIGNAL);
#endif
        if (sent <= 0) {
            fprintf(stderr, "[tcp] send() failed\n");
            return false;
        }
        ptr += sent;
        remaining -= static_cast<size_t>(sent);
    }
    return true;
}

bool TCPClient::recv_exact(void* buf, size_t len) {
    if (fd_ == kInvalidSocket) return false;

    uint8_t* ptr = static_cast<uint8_t*>(buf);
    size_t remaining = len;

    while (remaining > 0) {
#ifdef _WIN32
        int recvd = ::recv(fd_, reinterpret_cast<char*>(ptr),
                           static_cast<int>(remaining), 0);
#else
        ssize_t recvd = ::recv(fd_, ptr, remaining, 0);
#endif
        if (recvd <= 0) {
            if (recvd == 0) {
                fprintf(stderr, "[tcp] connection closed by peer\n");
            } else {
                fprintf(stderr, "[tcp] recv() failed\n");
            }
            return false;
        }
        ptr += recvd;
        remaining -= static_cast<size_t>(recvd);
    }
    return true;
}

bool TCPClient::recv_header(ds_header_t* header) {
    uint8_t buf[DS_HEADER_SIZE];
    if (!recv_exact(buf, DS_HEADER_SIZE)) return false;
    ds_header_deserialize(buf, header);
    return true;
}

bool TCPClient::send_message(uint8_t type, uint8_t flags,
                             const void* data, size_t len) {
    ds_header_t header;
    header.type = type;
    header.flags = flags;
    header.length = static_cast<uint32_t>(len);

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

#ifdef _WIN32
    // Windows: two separate sends (no writev).
    if (!send_all(hdr_buf, DS_HEADER_SIZE)) return false;
    if (len > 0 && data != nullptr) {
        if (!send_all(data, len)) return false;
    }
#else
    // POSIX: use writev() to send header+payload atomically in one syscall,
    // eliminating any Nagle-related delay between header and payload.
    struct iovec iov[2];
    int iovcnt = 1;

    iov[0].iov_base = hdr_buf;
    iov[0].iov_len  = DS_HEADER_SIZE;

    if (len > 0 && data != nullptr) {
        iov[1].iov_base = const_cast<void*>(data);
        iov[1].iov_len  = len;
        iovcnt = 2;
    }

    size_t total = DS_HEADER_SIZE + (iovcnt == 2 ? len : 0);
    size_t written = 0;

    while (written < total) {
        ssize_t n = ::writev(fd_, iov, iovcnt);
        if (n <= 0) {
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK ||
                          errno == ETIMEDOUT)) {
                fprintf(stderr,
                        "[tcp] writev() timed out after %zu/%zu bytes "
                        "(peer not draining); dropping connection\n",
                        written, total);
            } else {
                fprintf(stderr, "[tcp] writev() failed\n");
            }
            return false;
        }
        written += static_cast<size_t>(n);

        // Advance iovec past bytes already written.
        size_t advance = static_cast<size_t>(n);
        for (int i = 0; i < iovcnt; ) {
            if (advance >= iov[i].iov_len) {
                advance -= iov[i].iov_len;
                iov[i].iov_len = 0;
                i++;
            } else {
                iov[i].iov_base = static_cast<uint8_t*>(iov[i].iov_base) + advance;
                iov[i].iov_len -= advance;
                break;
            }
        }
    }
#endif
    return true;
}

void TCPClient::close() {
    if (fd_ != kInvalidSocket) {
#ifdef _WIN32
        ::shutdown(fd_, SD_BOTH);
        closesocket(fd_);
#else
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
#endif
        fd_ = kInvalidSocket;
    }
}

uint32_t run_speed_test(TCPClient* client, uint32_t duration_ms) {
    if (!client || !client->is_connected()) return 0;

    // 64 KB payload per control message.
    constexpr size_t kChunkSize = 64 * 1024;
    std::vector<uint8_t> payload(kChunkSize, 0);
    payload[0] = DS_CTRL_SPEED_TEST;  // control sub-type

    auto start = std::chrono::steady_clock::now();
    uint64_t total_bytes = 0;

    while (true) {
        auto now = std::chrono::steady_clock::now();
        auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - start).count();
        if (elapsed_ms >= (int64_t)duration_ms) break;

        if (!client->send_message(DS_MSG_CONTROL, 0,
                                  payload.data(), payload.size())) {
            break;  // Connection error — return what we measured so far.
        }
        total_bytes += DS_HEADER_SIZE + payload.size();
    }

    auto end = std::chrono::steady_clock::now();
    double elapsed_sec = std::chrono::duration<double>(end - start).count();
    if (elapsed_sec <= 0.0) return 0;

    // Convert bytes to kilobits per second.
    uint64_t bits = total_bytes * 8;
    uint32_t kbps = static_cast<uint32_t>(bits / elapsed_sec / 1000.0);

    fprintf(stderr, "[speed_test] sent %llu bytes in %.2f s -> %u kbps (%.1f Mbps)\n",
            (unsigned long long)total_bytes, elapsed_sec,
            kbps, kbps / 1000.0);

    return kbps;
}

} // namespace droidscreen
