/*
 * DroidScreen Desktop - Adaptive bitrate controller (AIMD on send-queue depth)
 *
 * See rate_controller.h. The controller reacts to the local send queue, which
 * is the earliest place congestion shows up: it only backs up when the encoder
 * outpaces what the transport + Android decoder can drain. On sustained
 * congestion we cut bitrate multiplicatively (fast relief); on sustained clear
 * we ramp back up additively toward the configured ceiling (the user's "tope").
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
{
    // Keep the floor below the ceiling even for low user caps.
    if (min_kbps_ >= max_kbps_) {
        min_kbps_ = std::max<uint32_t>(500, max_kbps_ / 2);
    }
    current_kbps_ = std::clamp(current_kbps_, min_kbps_, max_kbps_);
}

void RateController::apply_locked(uint32_t new_kbps, const char* reason) {
    new_kbps = std::clamp(new_kbps, min_kbps_, max_kbps_);
    if (new_kbps == current_kbps_) {
        return;
    }
    fprintf(stderr, "[rate] %s: %u -> %u kbps (cap=%u, floor=%u)\n",
            reason, current_kbps_, new_kbps, max_kbps_, min_kbps_);
    current_kbps_ = new_kbps;
    if (encoder_) {
        encoder_->set_bitrate(current_kbps_);
    }
}

void RateController::update(size_t send_queue_depth, size_t send_queue_capacity) {
    std::lock_guard<std::mutex> lock(mutex_);

    // Congested when the queue is at/above ~75% of capacity (the encoder is
    // outrunning the drain). Clear only when it is fully empty.
    const size_t congested_threshold =
        send_queue_capacity > 1 ? (send_queue_capacity * 3) / 4 : 1;
    const bool congested = send_queue_depth >= congested_threshold;
    const bool clear = send_queue_depth == 0;

    if (congested) {
        congested_run_++;
        clear_run_ = 0;
        if (congested_run_ >= kSamplesBeforeDecrease) {
            congested_run_ = 0;
            apply_locked(static_cast<uint32_t>(current_kbps_ * kDecreaseFactor),
                         "congestion, cutting bitrate");
        }
    } else if (clear) {
        clear_run_++;
        congested_run_ = 0;
        if (clear_run_ >= kSamplesBeforeIncrease && current_kbps_ < max_kbps_) {
            clear_run_ = 0;
            apply_locked(current_kbps_ + kIncreaseStepKbps,
                         "link clear, ramping bitrate");
        }
    } else {
        // Partial occupancy: neither pressure nor fully idle. Decay both runs so
        // we neither cut nor ramp on borderline load.
        if (congested_run_ > 0) congested_run_--;
        if (clear_run_ > 0) clear_run_--;
    }
}

uint32_t RateController::current_bitrate_kbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_kbps_;
}

} // namespace droidscreen
