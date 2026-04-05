/*
 * DroidScreen Desktop - TCP client
 *
 * Connects to the Android device's TCP server via adb reverse.
 * Handles the binary wire protocol framing (6-byte headers).
 */

#pragma once

#include <cstdint>
#include <cstddef>

extern "C" {
#include "droidscreen/protocol.h"
}

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
constexpr socket_t kInvalidSocket = INVALID_SOCKET;
#else
using socket_t = int;
constexpr socket_t kInvalidSocket = -1;
#endif

namespace droidscreen {

class TCPClient {
public:
    TCPClient();
    ~TCPClient();

    /// Connect to localhost:port (the adb-reverse endpoint).
    bool connect(uint16_t port);

    /// Send exactly len bytes. Returns true on success.
    bool send_all(const void* data, size_t len);

    /// Receive exactly len bytes into buf. Returns true on success.
    bool recv_exact(void* buf, size_t len);

    /// Receive and deserialize a protocol header (6 bytes).
    bool recv_header(ds_header_t* header);

    /// Serialize a header and send it followed by the payload.
    bool send_message(uint8_t type, uint8_t flags,
                      const void* data, size_t len);

    /// Close the connection.
    void close();

    /// Return the raw socket descriptor.
    socket_t fd() const { return fd_; }

    /// Check if the socket is connected.
    bool is_connected() const { return fd_ != kInvalidSocket; }

private:
    socket_t fd_;

#ifdef _WIN32
    static bool wsa_initialized_;
    static bool init_wsa();
#endif
};

} // namespace droidscreen
