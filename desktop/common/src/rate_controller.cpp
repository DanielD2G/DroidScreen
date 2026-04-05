/*
 * DroidScreen Desktop - Adaptive bitrate controller implementation
 *
 * Strategy: maintain a sliding window of RTT samples. When the average
 * RTT rises above a high threshold, decrease bitrate. When it falls
 * below a low threshold, increase bitrate. Changes are applied
 * multiplicatively for faster convergence.
 */

#include "droidscreen/rate_controller.h"
#include "droidscreen/encoder.h"

#include <algorithm>
#include <cstdio>
#include <numeric>

namespace droidscreen {

// RTT thresholds in microseconds.
static constexpr int64_t kRttLowUs  = 5000;   //  5 ms - network is happy
static constexpr int64_t kRttHighUs = 20000;   // 20 ms - network is congested

// Multiplicative factors.
static constexpr double kIncreaseFactor = 1.10;  // +10%
static constexpr double kDecreaseFactor = 0.75;  // -25%

RateController::RateController(Encoder* encoder,
                               uint32_t initial_kbps,
                               uint32_t min_kbps,
                               uint32_t max_kbps)
    : encoder_(encoder)
    , current_kbps_(initial_kbps)
    , min_kbps_(min_kbps)
    , max_kbps_(max_kbps)
    , rtt_window_{}
    , rtt_count_(0)
    , rtt_index_(0)
{
}

void RateController::on_pong(int64_t rtt_us) {
    std::lock_guard<std::mutex> lock(mutex_);
    rtt_window_[rtt_index_] = rtt_us;
    rtt_index_ = (rtt_index_ + 1) % kWindowSize;
    if (rtt_count_ < kWindowSize) {
        rtt_count_++;
    }
}

void RateController::update() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Need at least 3 samples before making decisions.
    if (rtt_count_ < 3) return;

    // Compute average RTT over the window.
    int64_t sum = 0;
    for (size_t i = 0; i < rtt_count_; i++) {
        sum += rtt_window_[i];
    }
    int64_t avg_rtt = sum / static_cast<int64_t>(rtt_count_);

    uint32_t new_kbps = current_kbps_;

    if (avg_rtt > kRttHighUs) {
        // Network is congested -- decrease bitrate aggressively.
        new_kbps = static_cast<uint32_t>(
            static_cast<double>(current_kbps_) * kDecreaseFactor);
    } else if (avg_rtt < kRttLowUs) {
        // Network is healthy -- increase bitrate gently.
        new_kbps = static_cast<uint32_t>(
            static_cast<double>(current_kbps_) * kIncreaseFactor);
    }

    // Clamp to bounds.
    new_kbps = std::clamp(new_kbps, min_kbps_, max_kbps_);

    if (new_kbps != current_kbps_) {
        fprintf(stderr, "[rate] bitrate %u -> %u kbps (avg RTT %lld us)\n",
                current_kbps_, new_kbps, static_cast<long long>(avg_rtt));
        current_kbps_ = new_kbps;
        encoder_->set_bitrate(new_kbps);
    }
}

uint32_t RateController::current_bitrate_kbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_kbps_;
}

} // namespace droidscreen
