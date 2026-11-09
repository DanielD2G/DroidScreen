/*
 * DroidScreen Desktop - Adaptive bitrate controller (AIMD on send-queue depth)
 *
 * The send queue depth is a direct congestion signal: it fills only when the
 * encoder is producing faster than the transport + Android decoder can drain.
 * When it stays full we multiplicatively cut the encoder bitrate; when it stays
 * empty we additively ramp back up toward max_kbps (the user-configured cap /
 * "tope"). This lets a heavy scene degrade in quality instead of stalling the
 * pipeline, while idle/easy content runs at full configured quality.
 */

#pragma once

#include <cstdint>
#include <cstddef>
#include <mutex>

namespace droidscreen {

class Encoder;

class RateController {
public:
    /// @param encoder Encoder to adjust bitrate on.
    /// @param initial_kbps Starting bitrate in kbps (also the ceiling by default).
    /// @param min_kbps Minimum (floor) bitrate under sustained congestion.
    /// @param max_kbps Maximum (ceiling / "tope") — never ramp above this.
    RateController(Encoder* encoder,
                   uint32_t initial_kbps,
                   uint32_t min_kbps = 4000,
                   uint32_t max_kbps = 80000);

    /// Feed one periodic sample of the current send-queue depth (and its max
    /// capacity). Applies bitrate changes via the encoder when warranted.
    /// Expected to be called at a steady cadence (~2 Hz).
    void update(size_t send_queue_depth, size_t send_queue_capacity);

    /// Current bitrate in kbps.
    uint32_t current_bitrate_kbps() const;

private:
    void apply_locked(uint32_t new_kbps, const char* reason);

    Encoder* encoder_;
    uint32_t current_kbps_;
    uint32_t min_kbps_;
    uint32_t max_kbps_;

    // AIMD state: runs of congested vs clear samples.
    int congested_run_ = 0;
    int clear_run_ = 0;

    // Tuning.
    static constexpr int   kSamplesBeforeDecrease = 2;   // ~1 s of congestion
    static constexpr int   kSamplesBeforeIncrease = 8;   // ~4 s clear before ramp
    static constexpr double kDecreaseFactor       = 0.75;
    static constexpr uint32_t kIncreaseStepKbps   = 2000;

    mutable std::mutex mutex_;
};

} // namespace droidscreen
