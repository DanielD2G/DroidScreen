/*
 * DroidScreen Desktop - Pipeline orchestrator
 *
 * Ties together capture, encoding, networking, and touch injection
 * into a coherent pipeline with proper threading and flow control.
 */

#pragma once

#include <cstdint>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <deque>

#include "droidscreen/capturer.h"
#include "droidscreen/encoder.h"
#include "droidscreen/server.h"
#include "droidscreen/touch_injector.h"
#include "droidscreen/rate_controller.h"

namespace droidscreen {

class Pipeline {
public:
    Pipeline(Capturer* capturer, Encoder* encoder,
             TCPClient* client, TouchInjector* touch);
    ~Pipeline();

    /// Start the pipeline: perform handshake, launch threads.
    /// @param width Capture/encode width.
    /// @param height Capture/encode height.
    /// @param fps Target frame rate.
    /// @param bitrate_kbps Initial bitrate.
    /// @param touch_enabled Whether to process touch events.
    /// @return true on success.
    bool start(uint32_t width, uint32_t height,
               uint32_t fps, uint32_t bitrate_kbps,
               bool touch_enabled);

    /// Stop all threads and clean up.
    void stop();

    /// True if the pipeline is actively running.
    bool is_running() const { return running_.load(); }

    /// Number of frames encoded so far.
    uint64_t frames_encoded() const { return frames_encoded_.load(); }

    /// Number of bytes sent so far.
    uint64_t bytes_sent() const { return bytes_sent_.load(); }

    /// Last measured RTT in microseconds.
    int64_t last_rtt_us() const { return last_rtt_us_.load(); }

private:
    // Perform the protocol handshake with the Android device.
    bool handshake(uint32_t width, uint32_t height,
                   uint32_t fps, uint32_t bitrate_kbps,
                   bool touch_enabled);

    // Thread: encode frames from queue and send over TCP.
    void encode_send_loop();

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

    std::unique_ptr<RateController> rate_ctrl_;

    // Frame queue with bounded capacity.
    static constexpr size_t kMaxQueueSize = 3;
    std::deque<CapturedFrame> frame_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    std::thread encode_thread_;
    std::thread recv_thread_;
    std::thread ping_thread_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<int64_t>  last_rtt_us_{0};

    // Timestamp of last ping sent, for RTT calculation.
    std::atomic<int64_t> ping_sent_us_{0};
};

} // namespace droidscreen
