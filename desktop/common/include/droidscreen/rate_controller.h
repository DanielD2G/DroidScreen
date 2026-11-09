/*
 * DroidScreen Desktop - Adaptive bitrate controller (AIMD on queueing delay)
 *
 * Congestion is measured as time, not queue depth. Two signals:
 *   - sender delay: age of the oldest video packet that is queued or being
 *     written to the socket (shows a slow link / blocked writev);
 *   - receiver backlog: compressed video waiting on the tablet (ring buffer),
 *     converted to milliseconds at the current bitrate. Over USB the adb
 *     forward buffers hide the link, so this is the signal that sees it.
 * When either stays high we multiplicatively cut the encoder bitrate; when both
 * stay low we additively ramp back toward max_kbps (the user's "tope").
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

    /// Feed one periodic sample (expected every ~100 ms).
    /// @param sender_delay_us Age of the oldest queued/in-flight video packet (0 = none).
    /// @param receiver_backlog_bytes Bytes waiting on the tablet, or -1 if unknown/stale.
    /// @param frame_interval_us Nominal frame interval at the target fps.
    void update(int64_t sender_delay_us, int64_t receiver_backlog_bytes,
                int64_t frame_interval_us);

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
    static constexpr int   kSamplesBeforeDecrease = 3;   // ~300 ms of congestion
    static constexpr int   kSamplesBeforeIncrease = 40;  // ~4 s clear before ramp
    // Thresholds. Sender: frames worth of delay. Receiver: milliseconds.
    static constexpr int64_t kSenderCongestedFrames = 3;
    static constexpr int64_t kSenderClearFrames     = 1;
    static constexpr int64_t kReceiverCongestedMs   = 100;
    static constexpr int64_t kReceiverClearMs       = 20;
    static constexpr double kDecreaseFactor       = 0.75;
    static constexpr uint32_t kIncreaseStepKbps   = 2000;

    mutable std::mutex mutex_;
};

} // namespace droidscreen
