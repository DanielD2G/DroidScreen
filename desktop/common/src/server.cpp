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
#include <sys/uio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
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

bool TCPClient::connect(uint16_t port) {
#ifdef _WIN32
    if (!init_wsa()) return false;
#endif

    fd_ = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd_ == kInvalidSocket) {
        fprintf(stderr, "[tcp] socket() failed\n");
        return false;
    }

    // Set TCP_NODELAY to disable Nagle's algorithm for low latency.
    int flag = 1;
#ifdef _WIN32
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&flag), sizeof(flag));
#else
    setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
#endif

    // Moderate socket buffers — enough for keyframes, small enough to
    // avoid OS-level queuing latency on USB connections (128 KB each).
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

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr),
                  sizeof(addr)) != 0) {
#ifdef _WIN32
        fprintf(stderr, "[tcp] connect() to 127.0.0.1:%u failed: %d\n",
                port, WSAGetLastError());
        closesocket(fd_);
#else
        fprintf(stderr, "[tcp] connect() to 127.0.0.1:%u failed: %s\n",
                port, strerror(errno));
        ::close(fd_);
#endif
        fd_ = kInvalidSocket;
        return false;
    }

    fprintf(stderr, "[tcp] connected to 127.0.0.1:%u\n", port);
    return true;
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
            fprintf(stderr, "[tcp] writev() failed\n");
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
