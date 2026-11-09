/*
 * DroidScreen macOS - Wi-Fi tablet discovery (Bonjour / mDNS)
 *
 * Uses the dns_sd C API driven by a private serial dispatch queue:
 * DNSServiceBrowse finds instances, DNSServiceResolve gets host/port/TXT for
 * each one. Results are published to the owner on the main queue.
 */

#include "wifi_discovery.h"

#include <arpa/inet.h>
#include <dispatch/dispatch.h>
#include <dns_sd.h>

#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

namespace droidscreen {

namespace {
constexpr const char* kServiceType = "_droidscreen._tcp";

std::string txt_value(uint16_t txt_len, const unsigned char* txt, const char* key) {
    uint8_t value_len = 0;
    const void* value = TXTRecordGetValuePtr(txt_len, txt, key, &value_len);
    if (!value) return {};
    return std::string(static_cast<const char*>(value), value_len);
}

uint32_t to_u32(const std::string& s) {
    return s.empty() ? 0 : static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
}
} // namespace

struct WifiDiscovery::Impl {
    struct Pending {
        Impl* impl;
        std::string key;
        DNSServiceRef resolve_ref = nullptr;
    };

    dispatch_queue_t queue = nullptr;
    DNSServiceRef browse_ref = nullptr;
    ChangeCallback on_change;

    mutable std::mutex mutex;
    std::map<std::string, WifiTablet> tablets;            // key: service name + domain
    std::map<std::string, std::unique_ptr<Pending>> resolving;

    void notify() {
        ChangeCallback cb = on_change;
        if (!cb) return;
        dispatch_async(dispatch_get_main_queue(), ^{ cb(); });
    }

    // ---- dns_sd callbacks (run on `queue`) ----

    static void DNSSD_API on_browse(DNSServiceRef, DNSServiceFlags flags, uint32_t iface,
                                    DNSServiceErrorType err, const char* name,
                                    const char* type, const char* domain, void* ctx) {
        auto* self = static_cast<Impl*>(ctx);
        if (err != kDNSServiceErr_NoError) {
            fprintf(stderr, "[wifi] browse error %d\n", err);
            return;
        }
        std::string key = std::string(name) + "|" + domain;

        if (!(flags & kDNSServiceFlagsAdd)) {
            bool changed = false;
            {
                std::lock_guard<std::mutex> lock(self->mutex);
                changed = self->tablets.erase(key) > 0;
                auto it = self->resolving.find(key);
                if (it != self->resolving.end()) {
                    DNSServiceRefDeallocate(it->second->resolve_ref);
                    self->resolving.erase(it);
                }
            }
            fprintf(stderr, "[wifi] tablet gone: %s\n", name);
            if (changed) self->notify();
            return;
        }

        std::lock_guard<std::mutex> lock(self->mutex);
        if (self->resolving.count(key)) return;
        auto pending = std::make_unique<Pending>();
        pending->impl = self;
        pending->key = key;
        DNSServiceErrorType rerr = DNSServiceResolve(&pending->resolve_ref, 0, iface,
                                                     name, type, domain,
                                                     &Impl::on_resolve, pending.get());
        if (rerr != kDNSServiceErr_NoError) {
            fprintf(stderr, "[wifi] resolve(%s) failed: %d\n", name, rerr);
            return;
        }
        DNSServiceSetDispatchQueue(pending->resolve_ref, self->queue);
        self->resolving[key] = std::move(pending);
    }

    static void DNSSD_API on_resolve(DNSServiceRef, DNSServiceFlags, uint32_t,
                                     DNSServiceErrorType err, const char*,
                                     const char* host, uint16_t port_be,
                                     uint16_t txt_len, const unsigned char* txt, void* ctx) {
        auto* pending = static_cast<Pending*>(ctx);
        Impl* self = pending->impl;
        std::string key = pending->key;

        WifiTablet tablet;
        bool ok = err == kDNSServiceErr_NoError;
        if (ok) {
            tablet.service_name = key.substr(0, key.find('|'));
            tablet.host = host;
            tablet.port = ntohs(port_be);
            tablet.id = txt_value(txt_len, txt, "id");
            tablet.width = to_u32(txt_value(txt_len, txt, "w"));
            tablet.height = to_u32(txt_value(txt_len, txt, "h"));
            tablet.protocol_version = to_u32(txt_value(txt_len, txt, "v"));
            ok = !tablet.id.empty();
        }

        {
            std::lock_guard<std::mutex> lock(self->mutex);
            if (ok) self->tablets[key] = tablet;
            auto it = self->resolving.find(key);
            if (it != self->resolving.end()) {
                // One answer is enough; stop resolving (this frees `pending`).
                DNSServiceRefDeallocate(it->second->resolve_ref);
                self->resolving.erase(it);
            }
        }

        if (ok) {
            fprintf(stderr, "[wifi] tablet found: %s at %s:%u (%ux%u, protocol v%u)\n",
                    tablet.service_name.c_str(), tablet.host.c_str(), tablet.port,
                    tablet.width, tablet.height, tablet.protocol_version);
            self->notify();
        } else {
            fprintf(stderr, "[wifi] could not resolve %s (err %d)\n", key.c_str(), err);
        }
    }
};

WifiDiscovery::WifiDiscovery() : impl_(std::make_unique<Impl>()) {}

WifiDiscovery::~WifiDiscovery() { stop(); }

bool WifiDiscovery::start(ChangeCallback on_change) {
    if (impl_->browse_ref) return true;
    impl_->on_change = std::move(on_change);
    impl_->queue = dispatch_queue_create("com.droidscreen.wifi-discovery", DISPATCH_QUEUE_SERIAL);

    __block DNSServiceErrorType err = kDNSServiceErr_NoError;
    Impl* impl = impl_.get();
    dispatch_sync(impl->queue, ^{
        err = DNSServiceBrowse(&impl->browse_ref, 0, kDNSServiceInterfaceIndexAny,
                               kServiceType, nullptr, &Impl::on_browse, impl);
        if (err == kDNSServiceErr_NoError) {
            DNSServiceSetDispatchQueue(impl->browse_ref, impl->queue);
        }
    });
    if (err != kDNSServiceErr_NoError) {
        fprintf(stderr, "[wifi] DNSServiceBrowse failed: %d\n", err);
        impl_->browse_ref = nullptr;
        return false;
    }
    fprintf(stderr, "[wifi] browsing for %s\n", kServiceType);
    return true;
}

void WifiDiscovery::stop() {
    if (!impl_ || !impl_->queue) return;
    Impl* impl = impl_.get();
    // Deallocate on the queue so no callback runs concurrently.
    dispatch_sync(impl->queue, ^{
        std::lock_guard<std::mutex> lock(impl->mutex);
        for (auto& entry : impl->resolving) {
            DNSServiceRefDeallocate(entry.second->resolve_ref);
        }
        impl->resolving.clear();
        impl->tablets.clear();
        if (impl->browse_ref) {
            DNSServiceRefDeallocate(impl->browse_ref);
            impl->browse_ref = nullptr;
        }
    });
    impl->on_change = nullptr;
    impl->queue = nullptr;
}

std::vector<WifiTablet> WifiDiscovery::tablets() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    std::vector<WifiTablet> out;
    out.reserve(impl_->tablets.size());
    for (const auto& entry : impl_->tablets) out.push_back(entry.second);
    return out;
}

bool WifiDiscovery::find(const std::string& id, WifiTablet* out) const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (const auto& entry : impl_->tablets) {
        if (entry.second.id == id) {
            if (out) *out = entry.second;
            return true;
        }
    }
    return false;
}

} // namespace droidscreen
