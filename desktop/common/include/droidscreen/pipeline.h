/*
 * DroidScreen Desktop - Pipeline orchestrator
 *
 * True 3-stage pipeline for near-zero latency:
 *   Stage 1 (SCK thread):    Capture callback -> push to capture_queue (newest only)
 *   Stage 2 (encode thread): Pop capture_queue -> encoder->encode() (VT fires async)
 *   Stage 3 (VT callback):   Push encoded packet to send_queue
 *   Stage 4 (send thread):   Pop send_queue -> TCP send (can block without stalling encode)
 *
 * The VT output callback never touches the socket -- it only enqueues packets.
 * This decouples encode latency from network latency entirely.
 */

#pragma once

#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>
#include <vector>

#include "droidscreen/capturer.h"
#include "droidscreen/deck_manager.h"
#include "droidscreen/encoder.h"
#include "droidscreen/mouse_injector.h"
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
    int64_t last_encode_us() const { return last_encode_us_.load(); }
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

    // Handle an incoming control message.
    void handle_control(const uint8_t* data, size_t len);

    Capturer*      capturer_;
    Encoder*       encoder_;
    TCPClient*     client_;
    TouchInjector* touch_;
    MouseInjector* mouse_;
    DeckManager*   deck_;

    // --- Capture queue: newest-frame-wins (not FIFO) ---
    // Only holds the most recent frame; stale frames are dropped.
    std::deque<CapturedFrame> capture_queue_;
    std::mutex capture_mutex_;
    std::condition_variable capture_cv_;

    // --- Send queue: encoded packets waiting for TCP send ---
    struct SendPacket {
        std::vector<uint8_t> data;
        uint8_t flags;
        int64_t capture_ts_us;
        int64_t capture_to_encode_us;
        int64_t encode_done_us;   // timestamp when VT callback fired
        uint64_t sequence;
        bool is_idle;
        float motion_score;
    };

    static constexpr size_t kMaxSendQueueSize = 4;
    std::deque<SendPacket> send_queue_;
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

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_captured_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> frames_idle_{0};
    std::atomic<uint64_t> frames_idle_resent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<int64_t>  last_rtt_us_{0};
    std::atomic<int64_t>  last_encode_us_{0};
    std::atomic<int64_t>  last_send_us_{0};
    std::atomic<int64_t>  last_capture_to_send_us_{0};
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

    // Timestamp of last ping sent, for RTT calculation.
    std::atomic<int64_t> ping_sent_us_{0};

    // RTT tracking (replaces RateController for stats-only use).
    static constexpr size_t kRttWindowSize = 10;
    int64_t rtt_window_[kRttWindowSize] = {};
    size_t rtt_count_ = 0;
    size_t rtt_index_ = 0;
    std::mutex rtt_mutex_;
};

} // namespace droidscreen
