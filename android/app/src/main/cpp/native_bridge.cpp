/*
 * DroidScreen - JNI bridge between Kotlin and native C/C++ code.
 *
 * Manages TCP server, receive/decode threads, and touch forwarding.
 * Supports reconnection: recv_thread loops back to accept() after
 * a client disconnects, resetting the decoder and ring buffer.
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <mutex>
#include <condition_variable>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>

#include "tcp_server.h"
#include "decoder.h"
#include "ring_buffer.h"
#include "mouse_sender.h"
#include "pen_sender.h"
#include "touch_sender.h"
#include "deck_sender.h"
#include "sps_patch.h"

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/handshake.h"
#include "droidscreen/frame.h"
#include "droidscreen/mouse.h"
#include "droidscreen/pen.h"
#include "droidscreen/touch.h"
#include "droidscreen/deck.h"
}

#define TAG "DroidScreen"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Ring buffer capacity: 16 MB.
 * 4K keyframes and brief decode stalls can easily overflow 4 MB. */
#define RING_BUFFER_CAPACITY (16 * 1024 * 1024)

/* Max single frame payload size: 4 MB */
#define MAX_FRAME_SIZE (4 * 1024 * 1024)

/* Internal ring-buffer message size: [flags:u8][nal payload] */
#define MAX_VIDEO_MSG_SIZE (MAX_FRAME_SIZE + 1)

/* Status constants — must match MainActivity.kt companion object */
#define STATUS_WAITING      0
#define STATUS_CONNECTED    1
#define STATUS_DISCONNECTED 2
#define STATUS_ERROR        3

/* ---- Decode thread condition variable ----
 * Signalled by: async MediaCodec callbacks, ring buffer writes (recv_thread).
 * Waited on by: decode_thread instead of polling with usleep. */
static std::mutex              g_decode_mutex;
static std::condition_variable g_decode_cv;

static void decoder_wakeup_cb(void* /*userdata*/) {
    g_decode_cv.notify_one();
}

/* ---- Global state ---- */
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_decoder_configured{false};
static int               g_server_fd = -1;
static int               g_client_fd = -1;
static ANativeWindow*    g_window    = nullptr;
static ring_buffer*      g_ring_buf  = nullptr;
static pthread_t         g_recv_thread;
static pthread_t         g_decode_thread;
static DecoderContext*   g_decoder   = nullptr;
static std::atomic<uint32_t> g_stream_frame_interval_us{16667};

/* ---- Stats tracking (read from JNI, written from recv/decode threads) ---- */
static std::atomic<uint64_t> g_stats_bytes_received{0};
static std::atomic<uint64_t> g_stats_frames_decoded{0};
static std::atomic<uint64_t> g_stats_frames_fed{0};
static std::atomic<uint64_t> g_stats_feed_errors{0};
static std::atomic<int64_t>  g_stats_last_frame_arrival_us{0};
static std::atomic<int64_t>  g_stats_frame_jitter_us{0};
static std::atomic<uint64_t> g_stats_frames_skipped{0};

/* ---- JNI callback state ---- */
static JavaVM*           g_jvm      = nullptr;
static jobject           g_activity = nullptr;   /* global ref */
static jmethodID         g_onStatusChanged = nullptr;
static jmethodID         g_onDeckConfigReceived = nullptr;
static jmethodID         g_onMediaStateReceived = nullptr;
static jmethodID         g_onVolumeStateReceived = nullptr;

/**
 * Notify Java about native connection status change.
 * Safe to call from any thread — attaches/detaches JNI as needed.
 */
static void notify_status(int status) {
    if (!g_jvm || !g_activity || !g_onStatusChanged) return;

    JNIEnv* env = nullptr;
    bool did_attach = false;

    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("notify_status: AttachCurrentThread failed");
            return;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("notify_status: GetEnv failed: %d", result);
        return;
    }

    env->CallVoidMethod(g_activity, g_onStatusChanged, (jint)status);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (did_attach) {
        g_jvm->DetachCurrentThread();
    }
}

/**
 * Notify Java with a string callback.
 * Safe to call from any thread — attaches/detaches JNI as needed.
 */
static void notify_string_callback(jmethodID method, const char* data, size_t len) {
    if (!g_jvm || !g_activity || !method) return;

    JNIEnv* env = nullptr;
    bool did_attach = false;

    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("notify_string_callback: AttachCurrentThread failed");
            return;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("notify_string_callback: GetEnv failed: %d", result);
        return;
    }

    /* Create a jstring from the raw bytes (treated as modified UTF-8) */
    char* tmp = static_cast<char*>(malloc(len + 1));
    if (tmp) {
        memcpy(tmp, data, len);
        tmp[len] = '\0';
        jstring jstr = env->NewStringUTF(tmp);
        if (jstr) {
            env->CallVoidMethod(g_activity, method, jstr);
            env->DeleteLocalRef(jstr);
        }
        free(tmp);
    }

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (did_attach) {
        g_jvm->DetachCurrentThread();
    }
}

/**
 * Notify Java about volume state changes.
 * Safe to call from any thread — attaches/detaches JNI as needed.
 */
static void notify_volume_state(int volume, bool muted) {
    if (!g_jvm || !g_activity || !g_onVolumeStateReceived) return;

    JNIEnv* env = nullptr;
    bool did_attach = false;

    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("notify_volume_state: AttachCurrentThread failed");
            return;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("notify_volume_state: GetEnv failed: %d", result);
        return;
    }

    env->CallVoidMethod(g_activity, g_onVolumeStateReceived,
                        (jint)volume, (jboolean)(muted ? JNI_TRUE : JNI_FALSE));

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (did_attach) {
        g_jvm->DetachCurrentThread();
    }
}

/* ---- Receive thread ----
 * Accepts connections in a loop. For each connection:
 *   1. Performs handshake
 *   2. Configures decoder
 *   3. Runs message loop (video frames → ring buffer)
 *   4. On disconnect: cleans up and loops back to accept
 */
static void* recv_thread_func(void* /*arg*/) {
    /* Set high priority for low latency */
    setpriority(PRIO_PROCESS, 0, -10);

    /* Use a static thread-local buffer to avoid per-connection malloc. */
    static uint8_t payload_buf[MAX_FRAME_SIZE];
    static uint8_t video_msg_buf[MAX_VIDEO_MSG_SIZE];
    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_t hdr;

    /* ---- Outer reconnection loop ---- */
    while (g_running.load(std::memory_order_acquire)) {

        /* Notify Java: waiting for connection */
        notify_status(STATUS_WAITING);
        LOGI("recv_thread: waiting for connection on fd=%d", g_server_fd);

        /* Accept a client connection (blocking). */
        g_client_fd = tcp_server_accept(g_server_fd);
        if (g_client_fd < 0) {
            if (g_running.load(std::memory_order_acquire)) {
                LOGW("recv_thread: accept failed, retrying in 500ms...");
                usleep(500000);
            }
            continue;
        }
        LOGI("recv_thread: client connected, fd=%d", g_client_fd);

        /* ---- Handshake ---- */
        if (tcp_recv_exact(g_client_fd, hdr_buf, DS_HEADER_SIZE) != 0) {
            LOGE("recv_thread: failed to read handshake header");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        ds_header_deserialize(hdr_buf, &hdr);

        if (hdr.type != DS_MSG_HANDSHAKE_REQ || hdr.length != DS_HANDSHAKE_REQ_SIZE) {
            LOGE("recv_thread: unexpected first message type=0x%02x len=%u",
                 hdr.type, hdr.length);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        uint8_t hs_buf[DS_HANDSHAKE_REQ_SIZE];
        if (tcp_recv_exact(g_client_fd, hs_buf, DS_HANDSHAKE_REQ_SIZE) != 0) {
            LOGE("recv_thread: failed to read handshake payload");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        ds_handshake_req_t req;
        ds_handshake_req_deserialize(hs_buf, &req);
        if (req.protocol_version != DS_PROTOCOL_VERSION) {
            LOGE("recv_thread: protocol version mismatch android=%u desktop=%u",
                 DS_PROTOCOL_VERSION, req.protocol_version);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }
        LOGI("recv_thread: handshake req: %ux%u @ %u fps, codec=%u, interval=%u us",
             req.width, req.height, req.fps, req.codec, req.frame_interval_us);

        /* Prefer the precise frame_interval_us when available (non-zero).
         * Old desktops zero-fill reserved bytes → falls back to 1000000/fps. */
        uint32_t interval_us = req.frame_interval_us;
        if (interval_us == 0) {
            uint32_t fps = req.fps > 0 ? req.fps : 60;
            interval_us = 1000000u / fps;
        }
        g_stream_frame_interval_us.store(interval_us, std::memory_order_release);

        /* Configure decoder with the negotiated resolution.
         * Set wakeup callback first so async mode is enabled if API >= 28. */
        if (g_decoder) {
            decoder_set_wakeup(g_decoder, decoder_wakeup_cb, nullptr);
            decoder_configure(g_decoder, req.width, req.height, req.fps);
            g_decoder_configured.store(true, std::memory_order_release);
        }

        /* Send handshake response */
        ds_handshake_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.protocol_version   = DS_PROTOCOL_VERSION;
        resp.accepted_width     = req.width;
        resp.accepted_height    = req.height;
        resp.accepted_fps       = req.fps;
        resp.accepted_codec     = req.codec;
        resp.decoder_max_bitrate = req.max_bitrate_kbps;
        resp.touch_supported    = 1;

        uint8_t resp_hdr[DS_HEADER_SIZE];
        ds_header_t resp_header;
        resp_header.type   = DS_MSG_HANDSHAKE_RESP;
        resp_header.flags  = 0;
        resp_header.length = DS_HANDSHAKE_RESP_SIZE;
        ds_header_serialize(resp_hdr, &resp_header);

        uint8_t resp_payload[DS_HANDSHAKE_RESP_SIZE];
        ds_handshake_resp_serialize(resp_payload, &resp);

        if (tcp_send_all(g_client_fd, resp_hdr, DS_HEADER_SIZE) != 0 ||
            tcp_send_all(g_client_fd, resp_payload, DS_HANDSHAKE_RESP_SIZE) != 0) {
            LOGE("recv_thread: failed to send handshake response");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }
        LOGI("recv_thread: handshake complete");

        /* Notify Java: connected and streaming */
        notify_status(STATUS_CONNECTED);

        /* ---- Message loop ---- */
        while (g_running.load(std::memory_order_acquire)) {
            /* Read message header */
            if (tcp_recv_exact(g_client_fd, hdr_buf, DS_HEADER_SIZE) != 0) {
                LOGW("recv_thread: connection closed or error reading header");
                break;
            }
            ds_header_deserialize(hdr_buf, &hdr);

            if (hdr.length > MAX_FRAME_SIZE) {
                LOGE("recv_thread: payload too large: %u", hdr.length);
                break;
            }

            /* Read payload */
            if (hdr.length > 0) {
                if (tcp_recv_exact(g_client_fd, payload_buf, hdr.length) != 0) {
                    LOGW("recv_thread: connection closed or error reading payload");
                    break;
                }
            }

            switch (hdr.type) {
                case DS_MSG_VIDEO_FRAME: {
                    /* Stats: track bytes received */
                    g_stats_bytes_received.fetch_add(
                        DS_HEADER_SIZE + hdr.length, std::memory_order_relaxed);

                    /* Stats: track frame pacing (jitter via EWMA) */
                    {
                        struct timespec ts;
                        clock_gettime(CLOCK_MONOTONIC, &ts);
                        int64_t now_us = ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
                        int64_t last = g_stats_last_frame_arrival_us.exchange(
                            now_us, std::memory_order_relaxed);
                        if (last > 0) {
                            int64_t interval = now_us - last;
                            int64_t expected = (int64_t)g_stream_frame_interval_us.load(
                                std::memory_order_relaxed);
                            int64_t deviation = (interval > expected)
                                ? (interval - expected) : (expected - interval);
                            int64_t prev = g_stats_frame_jitter_us.load(
                                std::memory_order_relaxed);
                            int64_t smoothed = prev + (deviation - prev) / 10;
                            g_stats_frame_jitter_us.store(
                                smoothed, std::memory_order_relaxed);
                        }
                    }

                    /* Preserve protocol flags; Android must not infer config/keyframe
                     * from the first NAL because Windows keyframes may start with SPS. */
                    video_msg_buf[0] = hdr.flags;
                    if (hdr.length > 0) {
                        memcpy(video_msg_buf + 1, payload_buf, hdr.length);
                    }

                    if (ring_buffer_write_message(
                            g_ring_buf, video_msg_buf, hdr.length + 1) != 0) {
                        LOGW("recv_thread: ring buffer full, dropping frame");
                    } else {
                        /* Wake decode thread — new data available */
                        g_decode_cv.notify_one();
                    }
                    break;
                }

                case DS_MSG_PING: {
                    /* Respond with PONG */
                    ds_header_t pong;
                    pong.type   = DS_MSG_PONG;
                    pong.flags  = 0;
                    pong.length = 0;
                    uint8_t pong_buf[DS_HEADER_SIZE];
                    ds_header_serialize(pong_buf, &pong);
                    tcp_send_all(g_client_fd, pong_buf, DS_HEADER_SIZE);
                    break;
                }

                case DS_MSG_CONTROL: {
                    if (hdr.length > 0 && payload_buf[0] == DS_CTRL_DISCONNECT) {
                        LOGI("recv_thread: received disconnect control");
                        goto end_message_loop;
                    }
                    break;
                }

                case DS_MSG_DECK_CONFIG: {
                    LOGD("recv_thread: received deck config (%u bytes)", hdr.length);
                    notify_string_callback(g_onDeckConfigReceived,
                                           reinterpret_cast<const char*>(payload_buf),
                                           hdr.length);
                    break;
                }

                case DS_MSG_MEDIA_STATE: {
                    LOGD("recv_thread: received media state (%u bytes)", hdr.length);
                    notify_string_callback(g_onMediaStateReceived,
                                           reinterpret_cast<const char*>(payload_buf),
                                           hdr.length);
                    break;
                }

                case DS_MSG_VOLUME_STATE: {
                    if (hdr.length >= DS_VOLUME_STATE_SIZE) {
                        ds_volume_state_t vol;
                        ds_volume_state_deserialize(payload_buf, &vol);
                        LOGD("recv_thread: received volume state level=%u muted=%u",
                             vol.level, vol.muted);
                        notify_volume_state(vol.level, vol.muted != 0);
                    }
                    break;
                }

                default:
                    LOGD("recv_thread: ignoring message type=0x%02x", hdr.type);
                    break;
            }
        }

    end_message_loop:

        /* ---- Cleanup between sessions ---- */
        LOGI("recv_thread: client disconnected, cleaning up for reconnection");

        /* Close client socket */
        if (g_client_fd >= 0) {
            tcp_close(g_client_fd);
            g_client_fd = -1;
        }

        /* Signal decode thread to pause */
        g_decoder_configured.store(false, std::memory_order_release);

        /* Reset stats for next session */
        g_stats_bytes_received.store(0, std::memory_order_relaxed);
        g_stats_frames_decoded.store(0, std::memory_order_relaxed);
        g_stats_frames_fed.store(0, std::memory_order_relaxed);
        g_stats_feed_errors.store(0, std::memory_order_relaxed);
        g_stats_last_frame_arrival_us.store(0, std::memory_order_relaxed);
        g_stats_frame_jitter_us.store(0, std::memory_order_relaxed);
        g_stats_frames_skipped.store(0, std::memory_order_relaxed);

        /* Give decode thread time to notice and stop touching the decoder */
        usleep(5000);  /* 5ms — decode thread polls at 100us */

        /* Reset ring buffer to purge stale video data */
        if (g_ring_buf) {
            ring_buffer_reset(g_ring_buf);
        }

        /* Destroy and recreate decoder for clean state */
        if (g_decoder) {
            decoder_destroy(g_decoder);
            g_decoder = nullptr;
        }
        if (g_window) {
            g_decoder = decoder_create(g_window);
            if (!g_decoder) {
                LOGE("recv_thread: failed to recreate decoder");
                notify_status(STATUS_ERROR);
                break;  /* Fatal — exit thread */
            }
        }

        /* Notify Java: disconnected (will show "reconnecting..." UI) */
        if (g_running.load(std::memory_order_acquire)) {
            notify_status(STATUS_DISCONNECTED);
            /* Brief debounce before re-accepting */
            usleep(200000);  /* 200ms */
        }
    }

    LOGI("recv_thread: exiting");
    return nullptr;
}

/* ---- Decode thread ----
 * Reads NAL units from ring buffer and feeds them to MediaCodec decoder.
 * Survives reconnections — pauses when decoder is unconfigured.
 *
 * Two operation modes (selected automatically per-session):
 *
 *   ASYNC (API 28+):  Event-driven via condition variable.
 *     MediaCodec callbacks + ring-buffer writes signal g_decode_cv.
 *     Uses decoder_pop_input() / decoder_feed_index() — zero polling.
 *
 *   SYNC (API 26-27): Burst-read ring buffer, feed with dequeueInputBuffer.
 *     Falls back to 50µs polling when idle.
 *
 * Both modes feed ALL NALs (never skip at input) and use decoder_drain()
 * for render-queue-depth=1 at the output level.
 */
static void* decode_thread_func(void* /*arg*/) {
    /* Set high priority */
    setpriority(PRIO_PROCESS, 0, -8);

    /* Try to pin decode thread to big cores (cores 4-7 on typical ARM big.LITTLE). */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        int first_big = ncpus > 4 ? ncpus / 2 : 0;
        for (int i = first_big; i < ncpus; i++) {
            CPU_SET(i, &cpuset);
        }
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
            LOGI("decode_thread: pinned to cores %d-%d", first_big, ncpus - 1);
        }
    }

    LOGI("decode_thread: started, waiting for decoder configuration...");

    auto* nal_buf = static_cast<uint8_t*>(malloc(MAX_VIDEO_MSG_SIZE));
    if (!nal_buf) {
        LOGE("decode_thread: failed to allocate NAL buffer");
        return nullptr;
    }

    int64_t pts_us = 0;
    uint32_t frames_fed = 0;
    uint32_t frames_rendered = 0;
    uint32_t feed_errors = 0;
    uint32_t frames_skipped = 0;
    uint32_t last_logged_fed = 0;
    int32_t pending_input_idx = -1;
    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (g_running.load(std::memory_order_acquire)) {
        /* Wait for decoder to be configured (pauses between reconnections) */
        if (!g_decoder_configured.load(std::memory_order_acquire)) {
            usleep(1000);
            frames_fed = 0;
            frames_rendered = 0;
            feed_errors = 0;
            frames_skipped = 0;
            last_logged_fed = 0;
            pending_input_idx = -1;
            pts_us = 0;
            clock_gettime(CLOCK_MONOTONIC, &ts_start);
            continue;
        }

        bool is_async = decoder_is_async(g_decoder);
        bool any_work = false;

        /* ---- 1. Drain output (both modes) ---- */
        int r = decoder_drain(g_decoder);
        if (r > 0) {
            frames_rendered += r;
            g_stats_frames_decoded.fetch_add(r, std::memory_order_relaxed);
            any_work = true;
        }

        /* ---- 2. Feed NALs from ring buffer ---- */
        if (is_async) {
            /* ASYNC: use pre-dequeued input indices from callbacks */
            while (g_running.load(std::memory_order_acquire)) {
                int32_t input_idx = pending_input_idx;
                if (input_idx < 0) {
                    input_idx = decoder_pop_input(g_decoder);
                    if (input_idx < 0) break;  /* no input buffer available */
                }

                size_t msg_len = ring_buffer_read_message(
                    g_ring_buf, nal_buf, MAX_VIDEO_MSG_SIZE);
                if (msg_len < 2) {
                    pending_input_idx = input_idx;
                    break;
                }
                any_work = true;
                pending_input_idx = -1;

                uint8_t video_flags = nal_buf[0];
                uint8_t* video_data = nal_buf + 1;
                size_t nal_len = msg_len - 1;

                uint32_t mc_flags = 0;
                int is_config = 0;
                ds_frame_parse_flags(video_flags, nullptr, &is_config);

                if (is_config) {
                    mc_flags = 2; /* BUFFER_FLAG_CODEC_CONFIG */
                    sps_patch_constraints(video_data, nal_len);
                }

                int ret = decoder_feed_index(g_decoder, input_idx,
                                             video_data, nal_len,
                                             pts_us, mc_flags);
                if (ret == 0) {
                    frames_fed++;
                    g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    feed_errors++;
                    g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
                }

                if (!is_config) {
                    pts_us += g_stream_frame_interval_us.load(
                        std::memory_order_acquire);
                }
            }
        } else {
            /* SYNC: burst-read ring buffer, dequeue input buffers inline */
            while (g_running.load(std::memory_order_acquire)) {
                size_t msg_len = ring_buffer_read_message(
                    g_ring_buf, nal_buf, MAX_VIDEO_MSG_SIZE);
                if (msg_len < 2) break;
                any_work = true;

                uint8_t video_flags = nal_buf[0];
                uint8_t* video_data = nal_buf + 1;
                size_t nal_len = msg_len - 1;

                uint32_t mc_flags = 0;
                int is_config = 0;
                ds_frame_parse_flags(video_flags, nullptr, &is_config);

                if (is_config) {
                    mc_flags = 2;
                    sps_patch_constraints(video_data, nal_len);
                }

                int ret = decoder_feed(g_decoder, video_data, nal_len,
                                       pts_us, mc_flags);
                if (ret == 0) {
                    frames_fed++;
                    g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    feed_errors++;
                    g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
                }

                if (!is_config) {
                    pts_us += g_stream_frame_interval_us.load(
                        std::memory_order_acquire);
                }
            }
        }

        /* ---- 3. Drain again after feeding ---- */
        if (any_work) {
            r = decoder_drain(g_decoder);
            if (r > 0) {
                frames_rendered += r;
                g_stats_frames_decoded.fetch_add(r, std::memory_order_relaxed);
            }
        }

        /* ---- 4. Wait for next event ---- */
        if (!any_work) {
            if (is_async) {
                /* Event-driven: wait on condition variable.
                 * Woken by: MediaCodec callbacks OR ring buffer writes. */
                std::unique_lock<std::mutex> lock(g_decode_mutex);
                g_decode_cv.wait_for(lock, std::chrono::milliseconds(5));
            } else {
                /* Sync fallback: brief polling sleep */
                usleep(50);
            }
        }

        /* ---- 5. Stats logging ---- */
        if ((frames_fed % 120) == 0 && frames_fed > 0
                && frames_fed != last_logged_fed) {
            last_logged_fed = frames_fed;
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double elapsed = (ts_now.tv_sec - ts_start.tv_sec)
                           + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            size_t ring_used = ring_buffer_available_read(g_ring_buf);
            LOGI("decode[%s]: fed=%u rendered=%u err=%u | "
                 "%.1f fed/s %.1f render/s | ring=%zu bytes",
                 is_async ? "async" : "sync",
                 frames_fed, frames_rendered, feed_errors,
                 frames_fed / elapsed, frames_rendered / elapsed,
                 ring_used);
        }
    }

    free(nal_buf);
    LOGI("decode_thread: exiting");
    return nullptr;
}

/* ---- JNI functions ---- */

extern "C" {

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeInit(
        JNIEnv* env, jobject thiz, jobject surface, jint port) {

    if (g_running.load()) {
        LOGW("nativeInit: already running, ignoring");
        return;
    }

    LOGI("nativeInit: starting with port=%d", port);

    /* Cache JVM and Activity reference for callbacks */
    env->GetJavaVM(&g_jvm);
    g_activity = env->NewGlobalRef(thiz);

    jclass clazz = env->GetObjectClass(thiz);
    g_onStatusChanged = env->GetMethodID(clazz, "onNativeStatusChanged", "(I)V");
    if (!g_onStatusChanged) {
        LOGE("nativeInit: could not find onNativeStatusChanged(I)V method");
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Cache deck callback method IDs (optional — deck feature may not be present) */
    g_onDeckConfigReceived = env->GetMethodID(clazz, "onDeckConfigReceived", "(Ljava/lang/String;)V");
    g_onMediaStateReceived = env->GetMethodID(clazz, "onMediaStateReceived", "(Ljava/lang/String;)V");
    g_onVolumeStateReceived = env->GetMethodID(clazz, "onVolumeStateReceived", "(IZ)V");
    if (!g_onDeckConfigReceived) {
        LOGW("nativeInit: deck callbacks not found (deck feature unavailable)");
        env->ExceptionClear();
    }

    /* Get native window from Surface */
    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) {
        LOGE("nativeInit: failed to get ANativeWindow");
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Create ring buffer */
    g_ring_buf = ring_buffer_create(RING_BUFFER_CAPACITY);
    if (!g_ring_buf) {
        LOGE("nativeInit: failed to create ring buffer");
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Create decoder */
    g_decoder = decoder_create(g_window);
    if (!g_decoder) {
        LOGE("nativeInit: failed to create decoder");
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Start TCP server */
    g_server_fd = tcp_server_start(port);
    if (g_server_fd < 0) {
        LOGE("nativeInit: failed to start TCP server on port %d", port);
        decoder_destroy(g_decoder);
        g_decoder = nullptr;
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    g_running.store(true, std::memory_order_release);
    g_decoder_configured.store(false, std::memory_order_release);

    /* Spawn threads */
    pthread_create(&g_recv_thread, nullptr, recv_thread_func, nullptr);
    pthread_create(&g_decode_thread, nullptr, decode_thread_func, nullptr);

    LOGI("nativeInit: started successfully");
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeStop(
        JNIEnv* env, jobject /*thiz*/) {

    if (!g_running.load()) {
        LOGW("nativeStop: not running");
        return;
    }

    LOGI("nativeStop: stopping...");
    g_running.store(false, std::memory_order_release);

    /* Close sockets to unblock recv/accept */
    if (g_client_fd >= 0) {
        tcp_close(g_client_fd);
        g_client_fd = -1;
    }
    if (g_server_fd >= 0) {
        tcp_close(g_server_fd);
        g_server_fd = -1;
    }

    /* Join threads */
    pthread_join(g_recv_thread, nullptr);
    pthread_join(g_decode_thread, nullptr);

    /* Cleanup decoder */
    if (g_decoder) {
        decoder_destroy(g_decoder);
        g_decoder = nullptr;
    }

    /* Cleanup ring buffer */
    if (g_ring_buf) {
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
    }

    /* Release native window */
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }

    g_decoder_configured.store(false, std::memory_order_release);

    /* Cleanup JNI refs */
    if (g_activity) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
    g_onStatusChanged = nullptr;
    g_onDeckConfigReceived = nullptr;
    g_onMediaStateReceived = nullptr;
    g_onVolumeStateReceived = nullptr;
    g_jvm = nullptr;

    LOGI("nativeStop: stopped");
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendTouch(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint pointerId, jint xFrac, jint yFrac,
        jint pressure, jint touchMajor, jint touchMinor,
        jint orientation) {

    if (g_client_fd < 0) {
        return;
    }

    touch_sender_send(g_client_fd, action, pointerId, xFrac, yFrac, pressure,
                      touchMajor, touchMinor, orientation);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendPen(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint pointerId, jint toolType, jint buttons,
        jint xFrac, jint yFrac, jint pressure,
        jint distance, jint tilt, jint rotation) {

    if (g_client_fd < 0) {
        return;
    }

    pen_sender_send(g_client_fd, action, pointerId, toolType, buttons,
                    xFrac, yFrac, pressure, distance, tilt, rotation);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendMouse(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint buttons, jint xFrac, jint yFrac) {

    if (g_client_fd < 0) {
        return;
    }

    mouse_sender_send(g_client_fd, action, buttons, xFrac, yFrac);
}

JNIEXPORT jlongArray JNICALL
Java_com_droidscreen_app_MainActivity_nativeGetStats(
        JNIEnv* env, jobject /*thiz*/) {
    jlong stats[7];
    stats[0] = static_cast<jlong>(g_stats_bytes_received.load(std::memory_order_relaxed));
    stats[1] = static_cast<jlong>(g_stats_frames_decoded.load(std::memory_order_relaxed));
    stats[2] = static_cast<jlong>(g_stats_frames_fed.load(std::memory_order_relaxed));
    stats[3] = static_cast<jlong>(g_stats_feed_errors.load(std::memory_order_relaxed));
    stats[4] = static_cast<jlong>(g_stats_frame_jitter_us.load(std::memory_order_relaxed));
    stats[5] = static_cast<jlong>(g_stream_frame_interval_us.load(std::memory_order_relaxed));
    stats[6] = static_cast<jlong>(g_stats_frames_skipped.load(std::memory_order_relaxed));

    jlongArray result = env->NewLongArray(7);
    env->SetLongArrayRegion(result, 0, 7, stats);
    return result;
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendDeckAction(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint actionType, jint slotIndex) {

    if (g_client_fd < 0) {
        return;
    }

    deck_action_send(g_client_fd, actionType, slotIndex);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendVolumeChange(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint volume, jint muted) {

    if (g_client_fd < 0) {
        return;
    }

    volume_change_send(g_client_fd, volume, muted);
}

} /* extern "C" */
