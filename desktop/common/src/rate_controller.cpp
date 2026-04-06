/*
 * DroidScreen Desktop - Rate controller (stats-only for USB)
 *
 * On wired USB, bitrate is fixed -- no adaptive ramping needed.
 * This file is kept for API compatibility. on_pong() still records
 * RTT samples for diagnostic purposes, but update() is a no-op.
 */

#include "droidscreen/rate_controller.h"
#include "droidscreen/encoder.h"

#include <algorithm>
#include <cstdio>

namespace droidscreen {

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
    // No-op on USB: fixed bitrate, no adaptive changes.
    // RTT samples are still collected by on_pong() for diagnostics.
}

uint32_t RateController::current_bitrate_kbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_kbps_;
}

} // namespace droidscreen
