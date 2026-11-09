/*
 * DroidScreen Desktop - TCP client
 *
 * Connects to the Android device's TCP server, either through the adb
 * forward tunnel on localhost (USB) or directly over the local network (Wi-Fi).
 * Handles the binary wire protocol framing (6-byte headers).
 */

#pragma once

#include <cstdint>
#include <cstddef>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/hello.h"
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

    /// Connect to localhost:port (the adb-forward endpoint).
    bool connect(uint16_t port);

    /// Connect to host:port (IPv4 literal or hostname such as "pad.local").
    /// Gives up after timeout_ms so an unreachable tablet cannot hang the caller.
    bool connect(const char* host, uint16_t port, int timeout_ms);

    /// Set (timeout_ms > 0) or clear (0) the receive timeout.
    void set_recv_timeout(int timeout_ms);

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
    void configure_socket();

    socket_t fd_;

#ifdef _WIN32
    static bool wsa_initialized_;
    static bool init_wsa();
#endif
};

/// Outcome of the wireless hello exchange.
enum class HelloResult {
    Accepted,   ///< Tablet accepted; token holds the (possibly new) pairing token.
    Rejected,   ///< User denied it on the tablet, or the request timed out there.
    Failed      ///< Transport/protocol error.
};

/// Identify this desktop to a tablet reached over Wi-Fi. Must be the first
/// exchange on the connection, before the handshake. May block up to
/// DS_HELLO_APPROVAL_TIMEOUT_MS while the user decides on the tablet.
/// @param token  In: token stored from a previous pairing (zeros if none).
///               Out: token to store when the result is Accepted.
HelloResult perform_hello(TCPClient* client,
                          const uint8_t desktop_id[DS_HELLO_ID_SIZE],
                          uint8_t token[DS_HELLO_TOKEN_SIZE],
                          const char* desktop_name);

/// Run a speed test over an established TCP connection.
/// Sends burst data and measures throughput.
/// The connection must have already completed the protocol handshake.
/// @param client  Connected TCPClient (post-handshake).
/// @param duration_ms  How long to run the burst (default 2 seconds).
/// @return Estimated throughput in kbps, or 0 on failure.
uint32_t run_speed_test(TCPClient* client, uint32_t duration_ms = 2000);

} // namespace droidscreen
