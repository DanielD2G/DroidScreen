/*
 * DroidScreen Desktop - Adaptive bitrate controller (AIMD on queueing delay)
 *
 * See rate_controller.h for the two congestion signals (sender delay and
 * receiver backlog). Sustained congestion cuts bitrate multiplicatively; a
 * sustained clear link ramps it back additively toward the user's "tope".
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

void RateController::update(int64_t sender_delay_us,
                            int64_t receiver_backlog_bytes,
                            int64_t frame_interval_us) {
    std::lock_guard<std::mutex> lock(mutex_);

    const int64_t interval = frame_interval_us > 0 ? frame_interval_us : 16667;
    // bytes -> ms at the current bitrate: bytes * 8 / kbps.
    const int64_t receiver_ms = receiver_backlog_bytes >= 0 && current_kbps_ > 0
        ? receiver_backlog_bytes * 8 / static_cast<int64_t>(current_kbps_)
        : -1;

    const bool congested =
        sender_delay_us > kSenderCongestedFrames * interval ||
        receiver_ms > kReceiverCongestedMs;
    const bool clear =
        sender_delay_us <= kSenderClearFrames * interval &&
        receiver_ms >= 0 && receiver_ms < kReceiverClearMs;

    if (congested) {
        congested_run_++;
        clear_run_ = 0;
        if (congested_run_ >= kSamplesBeforeDecrease) {
            congested_run_ = 0;
            fprintf(stderr, "[rate] congestion: sender_delay=%.1fms receiver_backlog=%lldms\n",
                    sender_delay_us / 1000.0, static_cast<long long>(receiver_ms));
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
        // Borderline load: neither cut nor ramp.
        if (congested_run_ > 0) congested_run_--;
        if (clear_run_ > 0) clear_run_--;
    }
}

uint32_t RateController::current_bitrate_kbps() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return current_kbps_;
}

} // namespace droidscreen
