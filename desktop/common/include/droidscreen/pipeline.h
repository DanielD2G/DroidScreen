/*
 * DroidScreen Desktop - Pipeline orchestrator
 *
 * Pipeline for near-zero latency:
 *   Stage 1 (SCK thread):    Capture callback -> push to capture_queue (newest only)
 *   Stage 2 (encode thread): Take an encode permit, pop the newest frame,
 *                            encoder->encode() (VT fires async)
 *   Stage 3 (VT callback):   Push encoded packets to send_queue (never blocks)
 *   Stage 4 (send thread):   Pop send_queue -> TCP send
 *
 * Admission control: at most kMaxEncodesInFlight submits exist between the
 * encoder and the send thread's pop. A submit's permit is released when its
 * last packet is popped for sending (or the encoder drops it), so the send
 * queue never holds more than one submit's packets (CONFIG + frame) and newer
 * captures replace older ones in capture_queue while the link is busy.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <vector>

#include <memory>

#include "droidscreen/capturer.h"
#include "droidscreen/deck_manager.h"
#include "droidscreen/encoder.h"
#include "droidscreen/mouse_injector.h"
#include "droidscreen/rate_controller.h"
#include "droidscreen/server.h"
#include "droidscreen/touch_injector.h"

namespace droidscreen {

class Pipeline {
public:
    Pipeline(Capturer* capturer, Encoder* encoder,
             TCPClient* client, TouchInjector* touch,
             MouseInjector* mouse, DeckManager* deck = nullptr);
    ~Pipeline();

    /// Start the pipeline: perform handshake, launch threads.
    /// @param width Capture/encode width.
    /// @param height Capture/encode height.
    /// @param fps Target frame rate.
    /// @param bitrate_kbps Fixed bitrate (no ramping on USB).
    /// @param min_idle_fps Minimum frame rate during idle periods.
    /// @param touch_enabled Whether to process touch events.
    /// @return true on success.
    bool start(uint32_t width, uint32_t height,
               uint32_t fps, uint32_t bitrate_kbps,
               uint32_t min_idle_fps,
               bool touch_enabled,
               ds_codec_t preferred_codec = DS_CODEC_H264,
               uint8_t codec_caps = DS_CODEC_CAP_H264);

    /// Stop all threads and clean up.
    void stop();

    /// Send the deck configuration to Android.
    void send_deck_config();

    /// Send the current volume state to Android.
    void send_volume_state(uint16_t level, bool muted);

    /// Send media state JSON to Android.
    void send_media_state(const std::string& json);

    /// True if the pipeline is actively running.
    bool is_running() const { return running_.load(); }

    /// Number of frames encoded so far.
    uint64_t frames_encoded() const { return frames_encoded_.load(); }

    uint64_t frames_captured() const { return frames_captured_.load(); }
    uint64_t frames_dropped() const { return frames_dropped_.load(); }
    uint64_t frames_idle() const { return frames_idle_.load(); }
    uint64_t frames_idle_resent() const { return frames_idle_resent_.load(); }
    uint64_t bytes_sent() const { return bytes_sent_.load(); }
    int64_t last_rtt_us() const { return last_rtt_us_.load(); }
    /// Codec the tablet accepted during the handshake.
    ds_codec_t accepted_codec() const { return accepted_codec_; }
    int64_t last_encode_us() const { return last_encode_us_.load(); }
    /// Sum of VT submit-to-output times and the number of samples, for averaging.
    uint64_t encode_us_sum() const { return encode_us_sum_.load(); }
    uint64_t encode_samples() const { return encode_samples_.load(); }
    int64_t last_send_us() const { return last_send_us_.load(); }
    int64_t last_capture_to_send_us() const { return last_capture_to_send_us_.load(); }
    size_t capture_queue_depth() const;
    size_t send_queue_depth() const;

private:
    // Perform the protocol handshake with the Android device.
    bool handshake(uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate_kbps,
                   bool touch_enabled,
                   ds_codec_t preferred_codec,
                   uint8_t codec_caps);

    // Thread: pop frames from capture_queue and submit to encoder.
    // VT callback pushes results to send_queue (never blocks on TCP).
    void encode_loop();

    // Thread: drain send_queue and write to TCP socket.
    void send_loop();

    // Thread: receive messages from the Android device.
    void recv_loop();

    // Thread: send periodic pings for RTT measurement.
    void ping_loop();

    // Thread: detect a stalled connection (no send progress) and fail fast so
    // the owner can reconnect. Reads atomics only -- never takes write_mutex_,
    // so it stays alive even when the send thread is blocked on a stuck write.
    void watchdog_loop();

    // Handle an incoming control message.
    void handle_control(const uint8_t* data, size_t len);

    // Try to recover a failed capturer stream without dropping the transport.
    void handle_capturer_error(const char* reason);

    // Mark the pipeline failed from a worker/capture callback. The owner will
    // call stop(), close the socket, and reconnect.
    void mark_failed(const char* reason);

    Capturer*      capturer_;
    Encoder*       encoder_;
    TCPClient*     client_;
    TouchInjector* touch_;
    MouseInjector* mouse_;
    DeckManager*   deck_;

    // Adaptive bitrate: backs off encoder bitrate under send-queue congestion,
    // ramps back toward the user-configured ceiling when the link is clear.
    std::unique_ptr<RateController> rate_controller_;

    // --- Capture queue: newest-frame-wins (not FIFO) ---
    // Only holds the most recent frame; stale frames are dropped.
    std::deque<CapturedFrame> capture_queue_;
    std::mutex capture_mutex_;
    std::condition_variable capture_cv_;
    std::function<void(const CapturedFrame&)> capture_callback_;
    std::mutex capturer_mutex_;
    std::thread capturer_restart_thread_;
    std::mutex capturer_restart_thread_mutex_;

    // --- Encode admission ---
    // Released (in its destructor) when every holder is gone: the encoder's
    // per-submit callback and each SendPacket of that submit.
    struct EncodePermit {
        Pipeline* owner;
        ~EncodePermit() { owner->release_encode_permit(); }
    };
    static constexpr int kMaxEncodesInFlight = 1;
    int encodes_in_flight_ = 0;           // guarded by capture_mutex_
    std::shared_ptr<EncodePermit> acquire_encode_permit_locked();
    void release_encode_permit();
    int64_t last_pts_us_ = 0;             // encode thread only: keeps PTS monotonic

    // Called by the encoder for every output packet of a submit.
    void handle_encoded_packet(const EncodedPacket& pkt,
                               const std::shared_ptr<EncodePermit>& permit,
                               int64_t t_enc_start, int64_t submit_pts_us,
                               int64_t display_time_us, bool is_idle,
                               float motion_score);

    // --- Send queue: encoded packets waiting for TCP send ---
    struct SendPacket {
        std::vector<uint8_t> data;
        uint8_t flags;
        int64_t capture_ts_us;
        int64_t capture_to_encode_us;
        int64_t encode_done_us;   // timestamp when VT callback fired
        int64_t display_time_us;
        uint64_t sequence;
        bool is_idle;
        float motion_score;
        std::shared_ptr<EncodePermit> permit;
    };

    std::deque<SendPacket> send_queue_;
    // Steady clock when the send thread started writing the current packet;
    // 0 when no write is in progress. Feeds the sender-delay congestion signal.
    std::atomic<int64_t> send_started_us_{0};
    int64_t sender_delay_us() const;
    std::mutex send_mutex_;
    std::condition_variable send_cv_;

    // Serializes ALL writes to the TCP socket (video, ping, control).
    // send_mutex_ protects send_queue_ only; write_mutex_ prevents
    // interleaved header+payload from different threads.
    std::mutex write_mutex_;

    std::thread encode_thread_;
    std::thread send_thread_;
    std::thread recv_thread_;
    std::thread ping_thread_;
    std::thread watchdog_thread_;

    std::atomic<bool> running_{false};
    std::atomic<bool> capturer_restarting_{false};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_captured_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_idle_{0};
    std::atomic<uint64_t> frames_idle_resent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<int64_t>  last_rtt_us_{0};
    std::atomic<int64_t>  last_encode_us_{0};
    std::atomic<uint64_t> encode_us_sum_{0};
    std::atomic<uint64_t> encode_samples_{0};
    std::atomic<int64_t>  last_send_us_{0};
    std::atomic<int64_t>  last_capture_to_send_us_{0};
    std::atomic<int64_t>  last_capture_callback_us_{0};
    std::atomic<uint64_t> next_video_sequence_{1};
    ds_codec_t accepted_codec_ = DS_CODEC_H264;
    uint32_t target_fps_ = 60;
    uint32_t target_bitrate_kbps_ = 0;

    int64_t quality_window_start_us_ = 0;
    uint64_t quality_window_bytes_ = 0;
    uint64_t quality_window_delta_bytes_ = 0;
    uint64_t quality_window_key_bytes_ = 0;
    uint64_t quality_window_frames_ = 0;
    uint64_t quality_window_keyframes_ = 0;
    double quality_window_motion_sum_ = 0.0;

    // Maximum interval between frames during idle periods (microseconds).
    // When no new capture arrives within this interval, the last frame is
    // re-encoded to keep the decoder pipeline warm. Default: 33 ms ≈ 30 fps.
    int64_t max_idle_interval_us_ = 33333;

    // Desktop<->Android clock offset (android_monotonic - desktop_steady),
    // taken from the PONG with the lowest RTT in a sliding window.
    void record_pong(int64_t desktop_send_us, int64_t android_now_us);
    static constexpr size_t kClockWindowSize = 16;
    int64_t clock_rtt_window_[kClockWindowSize] = {};
    int64_t clock_offset_window_[kClockWindowSize] = {};
    size_t clock_count_ = 0;
    size_t clock_index_ = 0;              // guarded by rtt_mutex_
    std::atomic<int64_t> clock_offset_us_{0};
    std::atomic<uint32_t> clock_uncertainty_us_{0};

    // Receiver backlog reported by Android (DS_CTRL_RECEIVER_BACKLOG).
    std::atomic<int64_t> receiver_backlog_bytes_{-1};
    std::atomic<int64_t> receiver_backlog_at_us_{0};

    // Liveness watchdog: timestamp of the last successful socket activity
    // (video/control byte sent, or message received). If this stops advancing
    // while running, the connection is stalled and the pipeline fails fast.
    // Idle re-encode keeps sending frames even on a static screen, so a lack of
    // progress reliably means a dead link rather than legitimate idleness.
    std::atomic<int64_t> last_send_activity_us_{0};
    // Must exceed the worst-case legitimate burst stall (see kSendTimeoutMs in
    // server.cpp). Heavy-motion bursts can freeze send progress for a few
    // seconds and recover; only a genuinely dead link stays stuck for 12 s+.
    static constexpr int64_t kStallTimeoutUs = 12000000;  // 12 s

    // RTT tracking (replaces RateController for stats-only use).
    static constexpr size_t kRttWindowSize = 10;
    int64_t rtt_window_[kRttWindowSize] = {};
    size_t rtt_count_ = 0;
    size_t rtt_index_ = 0;
    std::mutex rtt_mutex_;
};

} // namespace droidscreen
