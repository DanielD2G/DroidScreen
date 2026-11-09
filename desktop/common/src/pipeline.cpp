/*
 * DroidScreen Desktop - Pipeline implementation (near-zero latency)
 *
 * Pipeline:
 *   Stage 1 (SCK thread):    Capture callback -> capture_queue (newest only)
 *   Stage 2 (encode thread): Encode permit + pop capture_queue -> encoder->encode()
 *   Stage 3 (VT callback):   Push encoded packets to send_queue (never blocks)
 *   Stage 4 (send thread):   Pop send_queue -> TCP send
 *
 * The VT output callback never touches the socket and never waits: the
 * encode permit (see pipeline.h) bounds what can be queued, so a slow TCP
 * write cannot block Apple's VideoToolbox thread, and while the link is busy
 * newer captures replace older ones before encoding instead of piling up
 * as encoded packets.
 */

#include "droidscreen/pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef __APPLE__
#include <pthread/qos.h>
#endif

extern "C" {
#include "droidscreen/deck.h"
#include "droidscreen/frame.h"
#include "droidscreen/handshake.h"
#include "droidscreen/keyboard.h"
#include "droidscreen/mouse.h"
#include "droidscreen/pen.h"
#include "droidscreen/protocol.h"
#include "droidscreen/touch.h"
}

namespace droidscreen {

static int64_t now_us() {
  auto tp = std::chrono::steady_clock::now();
  return std::chrono::duration_cast<std::chrono::microseconds>(
             tp.time_since_epoch())
      .count();
}

static int64_t idle_interval_us_for_fps(uint32_t fps) {
  if (fps == 0)
    return 33333;
  return static_cast<int64_t>(1000000.0 / fps + 0.5);
}

static void release_captured_frame(CapturedFrame &frame) {
  if (frame.native_handle && frame.release_fn) {
    frame.release_fn(frame.release_ctx, frame.native_handle);
  }
  frame.native_handle = nullptr;
  frame.release_ctx = nullptr;
  frame.release_fn = nullptr;
}

Pipeline::Pipeline(Capturer *capturer, Encoder *encoder, TCPClient *client,
                   TouchInjector *touch, MouseInjector *mouse,
                   DeckManager *deck)
    : capturer_(capturer), encoder_(encoder), client_(client), touch_(touch),
      mouse_(mouse), deck_(deck) {}

Pipeline::~Pipeline() { stop(); }

void Pipeline::mark_failed(const char *reason) {
  if (!running_.exchange(false))
    return;

  fprintf(stderr, "[pipeline] %s; marking pipeline failed\n",
          reason ? reason : "failure");

  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_cv_.notify_all();
  }
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_cv_.notify_all();
  }
}

void Pipeline::handle_capturer_error(const char *reason) {
  if (!running_.load())
    return;

  if (capturer_restarting_.exchange(true))
    return;

  const std::string message = reason ? reason : "capturer error";
  fprintf(stderr, "[pipeline] %s; scheduling capturer restart\n",
          message.c_str());

  std::lock_guard<std::mutex> thread_lock(capturer_restart_thread_mutex_);
  if (capturer_restart_thread_.joinable())
    capturer_restart_thread_.join();

  capturer_restart_thread_ = std::thread([this, message]() {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    if (!running_.load()) {
      capturer_restarting_.store(false);
      return;
    }

    fprintf(stderr, "[pipeline] %s; restarting capturer\n", message.c_str());

    bool restarted = false;
    {
      std::lock_guard<std::mutex> lock(capturer_mutex_);
      if (running_.load())
        restarted = capturer_->restart(capture_callback_);
    }

    capturer_restarting_.store(false);

    if (!restarted) {
      if (running_.load())
        mark_failed("capturer restart failed");
      return;
    }

    if (running_.load())
      fprintf(stderr, "[pipeline] capturer restarted\n");
  });
}

bool Pipeline::handshake(uint32_t width, uint32_t height, uint32_t fps,
                         uint32_t bitrate_kbps, bool touch_enabled,
                         ds_codec_t preferred_codec, uint8_t codec_caps) {
  // Desktop sends HANDSHAKE_REQ to Android.
  ds_handshake_req_t req{};
  req.protocol_version = DS_PROTOCOL_VERSION;
  req.width = static_cast<uint16_t>(width);
  req.height = static_cast<uint16_t>(height);
  req.fps = static_cast<uint8_t>(fps);
  req.codec = static_cast<uint8_t>(preferred_codec);
  req.max_bitrate_kbps = bitrate_kbps;
  req.touch_enabled = touch_enabled ? 1 : 0;
  req.frame_interval_us =
      (fps > 0) ? static_cast<uint32_t>(1000000.0 / fps + 0.5) : 16667;
  req.reserved[0] = codec_caps ? codec_caps
                               : static_cast<uint8_t>(DS_CODEC_CAP_H264);

  uint8_t req_buf[DS_HANDSHAKE_REQ_SIZE];
  ds_handshake_req_serialize(req_buf, &req);

  if (!client_->send_message(DS_MSG_HANDSHAKE_REQ, 0, req_buf,
                             DS_HANDSHAKE_REQ_SIZE)) {
    fprintf(stderr, "[pipeline] failed to send handshake request\n");
    return false;
  }

  fprintf(stderr,
          "[pipeline] handshake sent: %ux%u@%u fps, %u kbps, preferred codec=%u caps=0x%02x\n",
          width, height, fps, bitrate_kbps, req.codec, req.reserved[0]);

  // Wait for Android's HANDSHAKE_RESP.
  ds_header_t hdr;
  if (!client_->recv_header(&hdr)) {
    fprintf(stderr, "[pipeline] failed to receive handshake response header\n");
    return false;
  }

  if (hdr.type != DS_MSG_HANDSHAKE_RESP ||
      hdr.length != DS_HANDSHAKE_RESP_SIZE) {
    fprintf(stderr,
            "[pipeline] unexpected message type=0x%02x len=%u "
            "(expected handshake resp)\n",
            hdr.type, hdr.length);
    return false;
  }

  uint8_t resp_buf[DS_HANDSHAKE_RESP_SIZE];
  if (!client_->recv_exact(resp_buf, DS_HANDSHAKE_RESP_SIZE)) {
    fprintf(stderr, "[pipeline] failed to receive handshake response body\n");
    return false;
  }

  ds_handshake_resp_t resp;
  ds_handshake_resp_deserialize(resp_buf, &resp);

  if (resp.protocol_version != DS_PROTOCOL_VERSION) {
    fprintf(stderr,
            "[pipeline] protocol version mismatch: desktop=%u android=%u\n",
            DS_PROTOCOL_VERSION, resp.protocol_version);
    return false;
  }

  fprintf(stderr,
          "[pipeline] handshake complete: peer accepted %ux%u@%u, "
          "codec=%u, max_bitrate=%u kbps, touch=%u\n",
          resp.accepted_width, resp.accepted_height, resp.accepted_fps,
          resp.accepted_codec, resp.decoder_max_bitrate, resp.touch_supported);

  accepted_codec_ = static_cast<ds_codec_t>(resp.accepted_codec);

  return true;
}

bool Pipeline::start(uint32_t width, uint32_t height, uint32_t fps,
                     uint32_t bitrate_kbps, uint32_t min_idle_fps,
                     bool touch_enabled, ds_codec_t preferred_codec,
                     uint8_t codec_caps) {
  if (running_.load()) {
    fprintf(stderr, "[pipeline] already running\n");
    return false;
  }

  // Perform protocol handshake.
  if (!handshake(width, height, fps, bitrate_kbps, touch_enabled,
                 preferred_codec, codec_caps)) {
    return false;
  }

  // Initialize encoder with fixed bitrate (no ramping on USB).
  if (!encoder_->init(width, height, fps, bitrate_kbps, accepted_codec_)) {
    fprintf(stderr, "[pipeline] encoder init failed\n");
    return false;
  }

  target_fps_ = fps;
  target_bitrate_kbps_ = bitrate_kbps;
  quality_window_start_us_ = 0;
  quality_window_bytes_ = 0;
  quality_window_delta_bytes_ = 0;
  quality_window_key_bytes_ = 0;
  quality_window_frames_ = 0;
  quality_window_keyframes_ = 0;
  quality_window_motion_sum_ = 0.0;

  max_idle_interval_us_ = idle_interval_us_for_fps(min_idle_fps);

  // Adaptive bitrate: the configured bitrate is the ceiling ("tope"); back off
  // under sustained send-queue congestion, ramp back up when the link is clear.
  // The floor stays low (<= 8 Mbps) even with a high cap, so a congested
  // Wi-Fi link can actually back off.
  rate_controller_ = std::make_unique<RateController>(
      encoder_, bitrate_kbps,
      /*min_kbps=*/(std::max)(4000u, (std::min)(bitrate_kbps / 4, 8000u)),
      /*max_kbps=*/bitrate_kbps);

  running_.store(true);
  frames_encoded_.store(0);
  frames_captured_.store(0);
  frames_dropped_.store(0);
  frames_idle_.store(0);
  frames_idle_resent_.store(0);
  bytes_sent_.store(0);
  last_encode_us_.store(0);
  last_send_us_.store(0);
  last_capture_to_send_us_.store(0);
  last_capture_callback_us_.store(now_us());
  last_send_activity_us_.store(now_us());
  next_video_sequence_.store(1);
  capturer_restarting_.store(false);
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    encodes_in_flight_ = 0;
  }
  last_pts_us_ = 0;
  send_started_us_.store(0);
  {
    std::lock_guard<std::mutex> lock(rtt_mutex_);
    clock_count_ = 0;
    clock_index_ = 0;
  }
  clock_offset_us_.store(0);
  clock_uncertainty_us_.store(0);
  receiver_backlog_bytes_.store(-1);
  receiver_backlog_at_us_.store(0);

  capturer_->set_error_callback([this](const char *reason) {
    handle_capturer_error(reason ? reason : "capturer error");
  });

  // Start capture -- frames get pushed into capture_queue_.
  // Newest-frame-wins: we keep at most 1 frame, always the latest.
  capture_callback_ = [this](const CapturedFrame &frame) {
    last_capture_callback_us_.store(now_us());

    if (!running_.load()) {
      CapturedFrame releasable = frame;
      release_captured_frame(releasable);
      return;
    }

    if (frame.is_idle) {
      frames_idle_.fetch_add(1);
      CapturedFrame releasable = frame;
      release_captured_frame(releasable);
      return;
    }

    frames_captured_.fetch_add(1);

    std::lock_guard<std::mutex> lock(capture_mutex_);

    // Drop ALL older frames -- always encode the freshest one.
    while (!capture_queue_.empty()) {
      auto &old = capture_queue_.front();
      release_captured_frame(old);
      capture_queue_.pop_front();
      frames_dropped_.fetch_add(1);
    }
    capture_queue_.push_back(frame);
    capture_cv_.notify_one();
  };

  if (!capturer_->start(capture_callback_)) {
    fprintf(stderr, "[pipeline] capturer start failed\n");
    running_.store(false);
    capturer_->stop();
    encoder_->shutdown();
    capturer_->set_error_callback(nullptr);
    return false;
  }

  // Launch worker threads.
  encode_thread_ = std::thread(&Pipeline::encode_loop, this);
  send_thread_ = std::thread(&Pipeline::send_loop, this);
  recv_thread_ = std::thread(&Pipeline::recv_loop, this);
  ping_thread_ = std::thread(&Pipeline::ping_loop, this);
  watchdog_thread_ = std::thread(&Pipeline::watchdog_loop, this);

  // Send initial deck configuration to Android if a DeckManager is set.
  if (deck_) {
    send_deck_config();
  }

  fprintf(stderr, "[pipeline] started (threads: encode, send, recv, ping, watchdog)\n");
  return true;
}

void Pipeline::stop() {
  bool had_restart_thread = false;
  {
    std::lock_guard<std::mutex> lock(capturer_restart_thread_mutex_);
    had_restart_thread = capturer_restart_thread_.joinable();
  }
  const bool had_threads = encode_thread_.joinable() || send_thread_.joinable() ||
                           recv_thread_.joinable() || ping_thread_.joinable() ||
                           watchdog_thread_.joinable() || had_restart_thread;
  bool was_running = running_.exchange(false);

  // Always join threads if they are joinable, even if running_ was
  // already false (e.g., set by recv_loop on connection loss).
  // Without this, the std::thread destructors would call std::terminate.

  if (was_running || had_threads) {
    fprintf(stderr, was_running ? "[pipeline] stopping...\n"
                                : "[pipeline] stopping after failure...\n");

    // Stop capture first (no more frames enqueued).
    std::lock_guard<std::mutex> lock(capturer_mutex_);
    capturer_->stop();
    capturer_->set_error_callback(nullptr);
  }

  // Wake the encode thread so it can exit.
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    capture_cv_.notify_all();
  }

  // Wake the send thread so it can exit.
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    send_cv_.notify_all();
  }

  // Close the socket to unblock recv().
  client_->close();

  // Join threads.
  if (encode_thread_.joinable())
    encode_thread_.join();
  if (send_thread_.joinable())
    send_thread_.join();
  if (recv_thread_.joinable())
    recv_thread_.join();
  if (ping_thread_.joinable())
    ping_thread_.join();
  if (watchdog_thread_.joinable())
    watchdog_thread_.join();

  {
    std::lock_guard<std::mutex> lock(capturer_restart_thread_mutex_);
    if (capturer_restart_thread_.joinable() &&
        capturer_restart_thread_.get_id() != std::this_thread::get_id()) {
      capturer_restart_thread_.join();
    }
  }
  capturer_restarting_.store(false);

  if (!was_running && !had_threads) {
    // Already stopped — threads joined, nothing more to do.
    return;
  }

  // Drain any remaining frames in capture_queue_.
  {
    std::lock_guard<std::mutex> lock(capture_mutex_);
    while (!capture_queue_.empty()) {
      auto &f = capture_queue_.front();
      release_captured_frame(f);
      capture_queue_.pop_front();
    }
  }

  // Shut down the encoder: flushes in-flight VT callbacks (they see
  // running_ == false and drop their packets, releasing their permits).
  encoder_->shutdown();

  // Drop packets that never got sent (releases their permits).
  std::deque<SendPacket> unsent;
  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    unsent.swap(send_queue_);
  }
  unsent.clear();
  touch_->shutdown();
  mouse_->shutdown();
  capture_callback_ = nullptr;
  rate_controller_.reset();

  fprintf(stderr,
          "[pipeline] stopped (encoded %llu frames, idle_resent %llu, "
          "sent %llu bytes)\n",
          static_cast<unsigned long long>(frames_encoded_.load()),
          static_cast<unsigned long long>(frames_idle_resent_.load()),
          static_cast<unsigned long long>(bytes_sent_.load()));
}

// --------------------------------------------------------------------------
// Encode admission
// --------------------------------------------------------------------------
std::shared_ptr<Pipeline::EncodePermit> Pipeline::acquire_encode_permit_locked() {
  encodes_in_flight_++;
  return std::shared_ptr<EncodePermit>(new EncodePermit{this});
}

void Pipeline::release_encode_permit() {
  std::lock_guard<std::mutex> lock(capture_mutex_);
  if (encodes_in_flight_ > 0)
    encodes_in_flight_--;
  capture_cv_.notify_all();
}

// Runs on the encoder's output thread (VT callback) or, for synchronous
// encoders, inside encode(). Never blocks: the permit already reserved room.
void Pipeline::handle_encoded_packet(const EncodedPacket &pkt,
                                     const std::shared_ptr<EncodePermit> &permit,
                                     int64_t t_enc_start, int64_t submit_pts_us,
                                     int64_t display_time_us, bool is_idle,
                                     float motion_score) {
  int64_t t_enc_end = now_us();
  // The packet's own timestamp (the PTS it was encoded with) is its capture
  // time. A synchronous encoder may hand back an earlier frame's packet from
  // this submit's callback; then this submit's DisplayTime doesn't apply.
  const int64_t capture_ts_us = pkt.timestamp_us;
  if (pkt.timestamp_us != submit_pts_us)
    display_time_us = 0;
  last_encode_us_.store(t_enc_end - t_enc_start);
  if (!pkt.is_config) {
    encode_us_sum_.fetch_add(static_cast<uint64_t>(t_enc_end - t_enc_start));
    encode_samples_.fetch_add(1);
  }

  uint8_t flags = 0;
  if (pkt.is_keyframe)
    flags |= DS_FLAG_KEYFRAME;
  if (pkt.is_config)
    flags |= DS_FLAG_CONFIG;

  SendPacket sp;
  sp.data.assign(pkt.data, pkt.data + pkt.size);
  sp.flags = flags;
  sp.capture_ts_us = capture_ts_us;
  sp.capture_to_encode_us = t_enc_end - capture_ts_us;
  sp.encode_done_us = t_enc_end;
  sp.display_time_us = display_time_us;
  sp.sequence = next_video_sequence_.fetch_add(1);
  sp.is_idle = is_idle;
  sp.motion_score = pkt.is_config ? 0.0f : motion_score;
  sp.permit = permit;

  {
    std::lock_guard<std::mutex> lock(send_mutex_);
    if (!running_.load())
      return;
    send_queue_.push_back(std::move(sp));
    send_cv_.notify_one();
  }

  frames_encoded_.fetch_add(1);
}

// --------------------------------------------------------------------------
// Stage 2: encode thread
// Takes an encode permit, pops the newest frame from capture_queue_ and
// submits it to the encoder. While the permit is busy, new captures keep
// replacing the waiting one (newest-frame-wins), so the frame that finally
// gets encoded is the freshest -- and the last change is never dropped.
// We NEVER do TCP I/O here.
//
// Idle frame re-sending: when no new frame arrives within max_idle_interval_us_
// and a permit is free, the last captured frame is re-encoded to keep the
// decoder pipeline warm ("cold decoder" lag after idle periods). Under
// congestion (no permit) the idle re-send is skipped.
// --------------------------------------------------------------------------
void Pipeline::encode_loop() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  fprintf(stderr, "[encode] thread started\n");

  // Retained copy of the last non-idle frame for idle re-sending.
  CapturedFrame last_frame{};
  last_frame.native_handle = nullptr;
  last_frame.release_fn = nullptr;

  while (running_.load()) {
    CapturedFrame frame;
    bool got_new_frame = false;
    bool do_idle = false;
    std::shared_ptr<EncodePermit> permit;

    {
      std::unique_lock<std::mutex> lock(capture_mutex_);
      const auto idle_interval = std::chrono::microseconds(max_idle_interval_us_);
      const bool woke = capture_cv_.wait_for(lock, idle_interval, [this] {
        return !running_.load() ||
               (!capture_queue_.empty() &&
                encodes_in_flight_ < kMaxEncodesInFlight);
      });

      if (!running_.load())
        break;

      const bool permit_free = encodes_in_flight_ < kMaxEncodesInFlight;
      if (!capture_queue_.empty() && permit_free) {
        got_new_frame = true;
        frame = capture_queue_.back();
        capture_queue_.pop_back();
        while (!capture_queue_.empty()) {
          auto &old = capture_queue_.front();
          release_captured_frame(old);
          capture_queue_.pop_front();
          frames_dropped_.fetch_add(1);
        }
        permit = acquire_encode_permit_locked();
      } else if (!woke && capture_queue_.empty() && permit_free &&
                 last_frame.native_handle) {
        do_idle = true;
        permit = acquire_encode_permit_locked();
      }
      // Otherwise a frame is waiting for the permit (we'll be notified when
      // it frees up) or the link is busy: skip the idle re-send.
    }

    if (!got_new_frame && capturer_->emits_idle_frames()) {
      const int64_t now = now_us();
      const int64_t last_callback = last_capture_callback_us_.load();
      if (last_callback > 0 && now - last_callback > 5000000) {
        mark_failed("capturer stopped delivering frames for 5s");
        break;
      }
    }

    if (got_new_frame) {
      const int64_t t_enc_start = now_us();
      const int64_t pts = (std::max)(frame.timestamp_us, last_pts_us_ + 1);
      last_pts_us_ = pts;
      bool ok = encoder_->encode(
          frame.native_handle, pts,
          [this, permit, t_enc_start, pts,
           display_ts = frame.display_time_us,
           motion = frame.motion_score](const EncodedPacket &pkt) {
            handle_encoded_packet(pkt, permit, t_enc_start, pts,
                                  display_ts, false, motion);
          });
      permit.reset();  // only the encoder's callback (and its packets) hold it now
      if (!ok) {
        fprintf(stderr, "[encode] encode submit failed\n");
      }

      // Keep this frame alive for future idle re-sends.
      release_captured_frame(last_frame);
      last_frame = frame;
    } else if (do_idle) {
      const int64_t t_enc_start = now_us();
      const int64_t pts = (std::max)(t_enc_start, last_pts_us_ + 1);
      last_pts_us_ = pts;
      bool ok = encoder_->encode(
          last_frame.native_handle, pts,
          [this, permit, t_enc_start, pts](const EncodedPacket &pkt) {
            handle_encoded_packet(pkt, permit, t_enc_start, pts, 0,
                                  true, 0.0f);
          });
      permit.reset();
      if (!ok) {
        fprintf(stderr, "[encode] idle re-encode failed\n");
      }
      frames_idle_resent_.fetch_add(1);
    }
  }

  // Cleanup: release the retained last frame.
  release_captured_frame(last_frame);

  fprintf(stderr, "[encode] thread exiting\n");
}

// --------------------------------------------------------------------------
// Stage 4: send thread
// Drains send_queue_ and writes to TCP. This is the ONLY thread that
// touches the TCP socket for video data. If the socket blocks, it only
// affects this thread -- encode and VT callback continue at full speed.
// --------------------------------------------------------------------------
void Pipeline::send_loop() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  fprintf(stderr, "[send] thread started\n");

  while (running_.load()) {
    SendPacket pkt;

    // Wait for a packet in the send queue.
    {
      std::unique_lock<std::mutex> lock(send_mutex_);
      send_cv_.wait(
          lock, [this] { return !send_queue_.empty() || !running_.load(); });
      if (!running_.load() && send_queue_.empty())
        break;
      if (send_queue_.empty())
        continue;

      pkt = std::move(send_queue_.front());
      send_queue_.pop_front();
      send_started_us_.store(now_us());
    }
    // Popped for sending: this submit no longer occupies the queue, so the
    // encoder may start the next one while we write this packet.
    pkt.permit.reset();

    int64_t t_send_start = now_us();
    const int64_t encode_to_send_us = t_send_start - pkt.encode_done_us;
    const int64_t capture_to_send_us = t_send_start - pkt.capture_ts_us;
    last_capture_to_send_us_.store(capture_to_send_us);

    ds_video_telemetry_t telemetry{};
    telemetry.sequence = pkt.sequence;
    telemetry.capture_to_encode_us = pkt.capture_to_encode_us;
    telemetry.encode_to_send_us = encode_to_send_us;
    telemetry.capture_to_send_us = capture_to_send_us;
    telemetry.rtt_us = last_rtt_us_.load();
    telemetry.flags = pkt.is_idle ? DS_VIDEO_TELEMETRY_FLAG_IDLE : 0;
    telemetry.display_time_us = pkt.display_time_us;
    telemetry.desktop_send_us = t_send_start;
    telemetry.clock_uncertainty_us = clock_uncertainty_us_.load();
    telemetry.clock_offset_us = clock_offset_us_.load();

    std::vector<uint8_t> wire_payload;
    wire_payload.resize(DS_VIDEO_TELEMETRY_SIZE + pkt.data.size());
    ds_frame_write_telemetry(wire_payload.data(), wire_payload.size(), &telemetry);
    memcpy(wire_payload.data() + DS_VIDEO_TELEMETRY_SIZE,
           pkt.data.data(), pkt.data.size());

    {
      std::lock_guard<std::mutex> wlock(write_mutex_);
      if (client_->send_message(DS_MSG_VIDEO_FRAME, pkt.flags,
                                wire_payload.data(), wire_payload.size())) {
        int64_t t_send_end = now_us();
        send_started_us_.store(0);
        last_send_us_.store(t_send_end - t_send_start);
        bytes_sent_.fetch_add(DS_HEADER_SIZE + wire_payload.size());
        last_send_activity_us_.store(t_send_end);

        if (!pkt.is_idle && (pkt.flags & DS_FLAG_CONFIG) == 0) {
          if (quality_window_start_us_ == 0) {
            quality_window_start_us_ = t_send_end;
          }
          quality_window_bytes_ += pkt.data.size();
          quality_window_frames_++;
          quality_window_motion_sum_ += pkt.motion_score;
          if ((pkt.flags & DS_FLAG_KEYFRAME) != 0) {
            quality_window_keyframes_++;
            quality_window_key_bytes_ += pkt.data.size();
          } else {
            quality_window_delta_bytes_ += pkt.data.size();
          }

          int64_t elapsed_us = t_send_end - quality_window_start_us_;
          if (elapsed_us >= 1000000 && quality_window_frames_ > 0) {
            double elapsed_s = (double)elapsed_us / 1000000.0;
            double fps = (double)quality_window_frames_ / elapsed_s;
            double kbps = ((double)quality_window_bytes_ * 8.0 / 1000.0) / elapsed_s;
            double frame_kbits =
                ((double)quality_window_bytes_ * 8.0 / 1000.0) /
                (double)quality_window_frames_;
            uint64_t delta_frames =
                quality_window_frames_ > quality_window_keyframes_
                    ? quality_window_frames_ - quality_window_keyframes_
                    : 0;
            double delta_kbits = delta_frames
                ? ((double)quality_window_delta_bytes_ * 8.0 / 1000.0) /
                      (double)delta_frames
                : 0.0;
            double key_kbits = quality_window_keyframes_
                ? ((double)quality_window_key_bytes_ * 8.0 / 1000.0) /
                      (double)quality_window_keyframes_
                : 0.0;
            double motion_luma =
                quality_window_motion_sum_ / (double)quality_window_frames_;
            double bits_per_motion =
                motion_luma > 0.25
                    ? (frame_kbits * 1000.0) / motion_luma
                    : 0.0;
            double budget_kbits = target_fps_ > 0
                ? (double)target_bitrate_kbps_ / (double)target_fps_
                : 0.0;
            fprintf(stderr,
                    "[quality] fps=%.1f kbps=%.0f frame=%.1fkb "
                    "delta=%.1fkb key=%.1fkb motion=%.2f bits_per_motion=%.0f "
                    "budget=%.1fkb\n",
                    fps, kbps, frame_kbits, delta_kbits, key_kbits,
                    motion_luma, bits_per_motion, budget_kbits);

            quality_window_start_us_ = t_send_end;
            quality_window_bytes_ = 0;
            quality_window_delta_bytes_ = 0;
            quality_window_key_bytes_ = 0;
            quality_window_frames_ = 0;
            quality_window_keyframes_ = 0;
            quality_window_motion_sum_ = 0.0;
          }
        }
      } else {
        fprintf(stderr, "[send] TCP send failed\n");
        running_.store(false);
        break;
      }
    }
  }

  fprintf(stderr, "[send] thread exiting\n");
}

void Pipeline::recv_loop() {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  fprintf(stderr, "[recv] thread started\n");

  while (running_.load()) {
    ds_header_t hdr;
    if (!client_->recv_header(&hdr)) {
      if (running_.load()) {
        fprintf(stderr, "[recv] connection lost\n");
      }
      running_.store(false);
      break;
    }

    // Inbound traffic proves the link is alive -- feed the watchdog.
    last_send_activity_us_.store(now_us());

    // Read the payload if any.
    std::vector<uint8_t> payload;
    if (hdr.length > 0) {
      payload.resize(hdr.length);
      if (!client_->recv_exact(payload.data(), hdr.length)) {
        fprintf(stderr, "[recv] failed to read payload\n");
        running_.store(false);
        break;
      }
    }

    switch (hdr.type) {
    case DS_MSG_PONG: {
      if (hdr.length < DS_PONG_PAYLOAD_SIZE)
        break;
      int64_t now = now_us();
      int64_t sent = 0;
      int64_t android_now = 0;
      memcpy(&sent, payload.data(), sizeof(sent));          // little-endian hosts
      memcpy(&android_now, payload.data() + 8, sizeof(android_now));
      int64_t rtt = now - sent;
      last_rtt_us_.store(rtt);
      record_pong(sent, android_now);

      // Track RTT for stats (no bitrate adjustment on USB).
      {
        std::lock_guard<std::mutex> lock(rtt_mutex_);
        rtt_window_[rtt_index_] = rtt;
        rtt_index_ = (rtt_index_ + 1) % kRttWindowSize;
        if (rtt_count_ < kRttWindowSize)
          rtt_count_++;
      }
      break;
    }

    case DS_MSG_TOUCH_EVENT: {
      if (hdr.length == DS_TOUCH_EVENT_SIZE) {
        ds_touch_event_t ev;
        ds_touch_deserialize(payload.data(), &ev);
        touch_->inject_touch(ev.action, ev.pointer_id, ev.x_frac, ev.y_frac,
                             ev.pressure, ev.touch_major, ev.touch_minor,
                             ev.orientation, ev.timestamp_ms);
      }
      break;
    }

    case DS_MSG_PEN_EVENT: {
      if (hdr.length == DS_PEN_EVENT_SIZE) {
        ds_pen_event_t ev;
        ds_pen_deserialize(payload.data(), &ev);
        touch_->inject_pen(ev.action, ev.pointer_id, ev.tool_type, ev.buttons,
                           ev.x_frac, ev.y_frac, ev.pressure, ev.distance,
                           ev.tilt, ev.rotation);
      }
      break;
    }

    case DS_MSG_MOUSE_EVENT: {
      if (hdr.length == DS_MOUSE_EVENT_SIZE) {
        ds_mouse_event_t ev;
        ds_mouse_deserialize(payload.data(), &ev);
        mouse_->inject_mouse(ev.action, ev.buttons, ev.x_frac, ev.y_frac);
      }
      break;
    }

    case DS_MSG_KEY_EVENT: {
      ds_key_event_t ev;
      if (ds_key_event_deserialize(payload.data(), payload.size(), &ev) > 0) {
        mouse_->inject_key(ev.kind, ev.modifiers, ev.key, ev.text, ev.text_len);
      }
      break;
    }

    case DS_MSG_DECK_ACTION: {
      if (deck_ && hdr.length >= DS_DECK_ACTION_SIZE) {
        deck_->handle_deck_action(payload.data(), payload.size());
      }
      break;
    }

    case DS_MSG_VOLUME_CHANGE: {
      if (deck_ && hdr.length >= DS_VOLUME_STATE_SIZE) {
        deck_->handle_volume_change(payload.data(), payload.size());
      }
      break;
    }

    case DS_MSG_CONTROL: {
      if (hdr.length > 0) {
        handle_control(payload.data(), payload.size());
      }
      break;
    }

    default:
      fprintf(stderr, "[recv] unknown message type 0x%02x\n", hdr.type);
      break;
    }
  }

  fprintf(stderr, "[recv] thread exiting\n");
}

void Pipeline::ping_loop() {
  fprintf(stderr, "[ping] thread started\n");

  int pings_sent = 0;
  while (running_.load()) {
    // Ping fast at first so the clock offset converges quickly, then 1 Hz.
    const int steps = pings_sent < 8 ? 2 : 10;
    for (int i = 0; i < steps && running_.load(); i++) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (!running_.load())
      break;

    {
      std::lock_guard<std::mutex> wlock(write_mutex_);
      // Stamp after taking the write lock so RTT excludes the wait for it.
      int64_t send_us = now_us();
      uint8_t ping_payload[DS_PING_PAYLOAD_SIZE];
      memcpy(ping_payload, &send_us, sizeof(send_us));       // little-endian hosts
      pings_sent++;
      if (!client_->send_message(DS_MSG_PING, 0, ping_payload,
                                 sizeof(ping_payload))) {
        if (running_.load()) {
          fprintf(stderr, "[ping] send failed\n");
        }
        running_.store(false);
        break;
      }
    }
  }

  fprintf(stderr, "[ping] thread exiting\n");
}

// --------------------------------------------------------------------------
// Watchdog thread
// Detects a stalled connection and fails the pipeline so the owner (stats
// timer -> disconnect -> auto-reconnect) recovers within seconds instead of
// freezing indefinitely. Reads atomics only; never takes write_mutex_, so it
// keeps ticking even while the send thread is blocked on a stuck socket write.
// --------------------------------------------------------------------------
void Pipeline::watchdog_loop() {
  fprintf(stderr, "[watchdog] thread started\n");

  while (running_.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!running_.load())
      break;

    const int64_t idle_us = now_us() - last_send_activity_us_.load();
    if (idle_us > kStallTimeoutUs) {
      char reason[128];
      snprintf(reason, sizeof(reason),
               "no socket progress for %.1fs (connection stalled)",
               idle_us / 1000000.0);
      mark_failed(reason);
      break;
    }

    // Adaptive bitrate: sample queueing delay on both ends at ~10 Hz.
    if (rate_controller_) {
      int64_t backlog = receiver_backlog_bytes_.load();
      if (now_us() - receiver_backlog_at_us_.load() > 1000000)
        backlog = -1;  // stale report: unknown
      rate_controller_->update(sender_delay_us(), backlog,
                               idle_interval_us_for_fps(target_fps_));
    }
  }

  fprintf(stderr, "[watchdog] thread exiting\n");
}

int64_t Pipeline::sender_delay_us() const {
  const int64_t now = now_us();
  int64_t delay = 0;
  const int64_t started = send_started_us_.load();
  if (started > 0)
    delay = now - started;
  {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(send_mutex_));
    if (!send_queue_.empty())
      delay = (std::max)(delay, now - send_queue_.front().encode_done_us);
  }
  return delay;
}

void Pipeline::record_pong(int64_t desktop_send_us, int64_t android_now_us) {
  const int64_t now = now_us();
  const int64_t rtt = now - desktop_send_us;
  if (rtt <= 0 || android_now_us <= 0)
    return;
  // Assume a symmetric path: Android answered at the RTT midpoint.
  const int64_t offset = android_now_us - (desktop_send_us + rtt / 2);

  std::lock_guard<std::mutex> lock(rtt_mutex_);
  clock_rtt_window_[clock_index_] = rtt;
  clock_offset_window_[clock_index_] = offset;
  clock_index_ = (clock_index_ + 1) % kClockWindowSize;
  if (clock_count_ < kClockWindowSize)
    clock_count_++;

  size_t best = 0;
  for (size_t i = 1; i < clock_count_; i++) {
    if (clock_rtt_window_[i] < clock_rtt_window_[best])
      best = i;
  }
  clock_offset_us_.store(clock_offset_window_[best]);
  clock_uncertainty_us_.store(static_cast<uint32_t>(
      std::max<int64_t>(1, clock_rtt_window_[best] / 2)));
}

void Pipeline::handle_control(const uint8_t *data, size_t len) {
  if (len < 1)
    return;

  uint8_t ctrl_type = data[0];

  switch (ctrl_type) {
  case DS_CTRL_REQUEST_KEYFRAME:
    fprintf(stderr, "[ctrl] keyframe requested\n");
    encoder_->force_keyframe();
    break;

  case DS_CTRL_BITRATE_CHANGE:
    if (len >= 5) {
      uint32_t new_bitrate = static_cast<uint32_t>(data[1]) |
                             (static_cast<uint32_t>(data[2]) << 8) |
                             (static_cast<uint32_t>(data[3]) << 16) |
                             (static_cast<uint32_t>(data[4]) << 24);
      fprintf(stderr, "[ctrl] bitrate change -> %u kbps\n", new_bitrate);
      encoder_->set_bitrate(new_bitrate);
    }
    break;

  case DS_CTRL_DISCONNECT:
    fprintf(stderr, "[ctrl] disconnect requested\n");
    running_.store(false);
    break;

  case DS_CTRL_DISPLAY_OFF:
    fprintf(stderr, "[ctrl] display off (ignoring on desktop)\n");
    break;

  case DS_CTRL_DISPLAY_ON:
    fprintf(stderr, "[ctrl] display on (ignoring on desktop)\n");
    break;

  case DS_CTRL_RECEIVER_BACKLOG:
    if (len >= DS_BACKLOG_PAYLOAD_SIZE) {
      uint32_t ring_bytes = static_cast<uint32_t>(data[1]) |
                            (static_cast<uint32_t>(data[2]) << 8) |
                            (static_cast<uint32_t>(data[3]) << 16) |
                            (static_cast<uint32_t>(data[4]) << 24);
      receiver_backlog_bytes_.store(static_cast<int64_t>(ring_bytes));
      receiver_backlog_at_us_.store(now_us());
    }
    break;

  default:
    fprintf(stderr, "[ctrl] unknown control type 0x%02x\n", ctrl_type);
    break;
  }
}

size_t Pipeline::capture_queue_depth() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(capture_mutex_));
  return capture_queue_.size();
}

size_t Pipeline::send_queue_depth() const {
  std::lock_guard<std::mutex> lock(const_cast<std::mutex &>(send_mutex_));
  return send_queue_.size();
}

// --------------------------------------------------------------------------
// Deck: send configuration, volume state, media state
// --------------------------------------------------------------------------

void Pipeline::send_deck_config() {
  if (!deck_ || !client_)
    return;

  std::lock_guard<std::mutex> wlock(write_mutex_);
  std::string json = deck_->serialize_config();
  if (!client_->send_message(DS_MSG_DECK_CONFIG, 0,
                             reinterpret_cast<const uint8_t *>(json.data()),
                             json.size())) {
    fprintf(stderr, "[pipeline] failed to send deck config\n");
  } else {
    fprintf(stderr, "[pipeline] sent deck config (%zu bytes)\n", json.size());
  }
}

void Pipeline::send_volume_state(uint16_t level, bool muted) {
  if (!client_)
    return;

  std::lock_guard<std::mutex> wlock(write_mutex_);
  ds_volume_state_t state{};
  state.level = level;
  state.muted = muted ? 1 : 0;

  uint8_t buf[DS_VOLUME_STATE_SIZE];
  ds_volume_state_serialize(buf, &state);

  if (!client_->send_message(DS_MSG_VOLUME_STATE, 0, buf,
                             DS_VOLUME_STATE_SIZE)) {
    fprintf(stderr, "[pipeline] failed to send volume state\n");
  }
}

void Pipeline::send_media_state(const std::string &json) {
  if (!client_)
    return;

  std::lock_guard<std::mutex> wlock(write_mutex_);
  if (!client_->send_message(DS_MSG_MEDIA_STATE, 0,
                             reinterpret_cast<const uint8_t *>(json.data()),
                             json.size())) {
    fprintf(stderr, "[pipeline] failed to send media state\n");
  }
}

} // namespace droidscreen
