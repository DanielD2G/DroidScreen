/*
 * DroidScreen Desktop - Adaptive bitrate controller
 *
 * Adjusts the encoder bitrate based on observed RTT from ping/pong
 * measurements. Uses a sliding window of RTT samples to smooth
 * the adaptation.
 */

#pragma once

#include <cstdint>
#include <array>
#include <mutex>

namespace droidscreen {

class Encoder;

class RateController {
public:
    /// @param encoder Encoder to adjust bitrate on.
    /// @param initial_kbps Starting bitrate in kbps.
    /// @param min_kbps Minimum allowed bitrate.
    /// @param max_kbps Maximum allowed bitrate.
    RateController(Encoder* encoder,
                   uint32_t initial_kbps,
                   uint32_t min_kbps = 500,
                   uint32_t max_kbps = 25000);

    /// Record a new RTT observation from a pong reply.
    void on_pong(int64_t rtt_us);

    /// Evaluate the sliding window and adjust bitrate if needed.
    void update();

    /// Current bitrate in kbps.
    uint32_t current_bitrate_kbps() const;

private:
    Encoder* encoder_;
    uint32_t current_kbps_;
    uint32_t min_kbps_;
    uint32_t max_kbps_;

    static constexpr size_t kWindowSize = 10;
    std::array<int64_t, kWindowSize> rtt_window_;
    size_t rtt_count_;
    size_t rtt_index_;

    mutable std::mutex mutex_;
};

} // namespace droidscreen
