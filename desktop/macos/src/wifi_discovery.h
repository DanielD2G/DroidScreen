/*
 * DroidScreen macOS - Wi-Fi tablet discovery (Bonjour / mDNS)
 *
 * Browses for `_droidscreen._tcp` services advertised by the Android app and
 * resolves each one to a host, port and the TXT metadata the tablet publishes
 * (stable id, native panel size, protocol version).
 */

#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace droidscreen {

struct WifiTablet {
    std::string service_name;   ///< Bonjour instance name (device name on the tablet).
    std::string host;           ///< Resolvable hostname, e.g. "Android-3.local."
    uint16_t    port = 0;
    std::string id;             ///< Stable tablet id (hex), used to remember pairing.
    uint32_t    width = 0;      ///< Native panel size, landscape.
    uint32_t    height = 0;
    uint32_t    protocol_version = 0;
};

class WifiDiscovery {
public:
    /// Called on the main queue whenever the set of resolved tablets changes.
    using ChangeCallback = std::function<void()>;

    WifiDiscovery();
    ~WifiDiscovery();

    bool start(ChangeCallback on_change);
    void stop();

    /// Snapshot of the currently resolved tablets (thread-safe).
    std::vector<WifiTablet> tablets() const;

    /// Find a resolved tablet by id.
    bool find(const std::string& id, WifiTablet* out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace droidscreen
