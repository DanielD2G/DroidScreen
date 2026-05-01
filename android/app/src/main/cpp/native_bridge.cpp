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
#include <time.h>

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

/* Wire payload can include a telemetry prefix before the NAL payload. */
#define MAX_WIRE_VIDEO_PAYLOAD_SIZE (MAX_FRAME_SIZE + DS_VIDEO_TELEMETRY_SIZE)

typedef struct {
    ds_video_telemetry_t telemetry;
    int64_t recv_us;
} AndroidFrameTelemetry;

/* Internal ring-buffer message size: [flags:u8][telemetry][nal payload] */
#define INTERNAL_VIDEO_HEADER_SIZE (1 + sizeof(AndroidFrameTelemetry))
#define MAX_VIDEO_MSG_SIZE (MAX_FRAME_SIZE + INTERNAL_VIDEO_HEADER_SIZE)

/* Status constants — must match MainActivity.kt companion object */
#define STATUS_WAITING      0
#define STATUS_CONNECTED    1
#define STATUS_DISCONNECTED 2
#define STATUS_ERROR        3

#define MC_BUFFER_FLAG_KEY_FRAME    1u
#define MC_BUFFER_FLAG_CODEC_CONFIG 2u

/* ---- Decode thread condition variable ----
 * Signalled by: async MediaCodec callbacks, ring buffer writes (recv_thread).
 * Waited on by: decode_thread instead of polling with usleep. */
static std::mutex              g_decode_mutex;
static std::condition_variable g_decode_cv;

static void decoder_wakeup_cb(void* /*userdata*/) {
    g_decode_cv.notify_one();
}

static void decoder_rendered_cb(void* /*userdata*/, int64_t pts_us);

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
static std::mutex        g_decoder_api_mutex;
static std::atomic<uint32_t> g_stream_frame_interval_us{16667};
static std::atomic<int64_t>  g_next_pts_us{0};
static std::atomic<bool>     g_decoder_direct_submit{false};
static std::atomic<int>      g_active_codec_id{DS_CODEC_H264};
static std::atomic<bool>     g_h264_constraints_only_sps{false};

/* ---- Stats tracking (read from JNI, written from recv/decode threads) ---- */
static std::atomic<uint64_t> g_stats_bytes_received{0};
static std::atomic<uint64_t> g_stats_frames_decoded{0};
static std::atomic<uint64_t> g_stats_frames_fed{0};
static std::atomic<uint64_t> g_stats_feed_errors{0};
static std::atomic<int64_t>  g_stats_last_frame_arrival_us{0};
static std::atomic<int64_t>  g_stats_frame_jitter_us{0};
static std::atomic<uint64_t> g_stats_frames_skipped{0};
static std::atomic<int64_t>  g_stats_latency_to_feed_us{0};
static std::atomic<int64_t>  g_stats_latency_to_release_us{0};
static std::atomic<int64_t>  g_stats_desktop_capture_to_send_us{0};
static std::atomic<int64_t>  g_stats_desktop_capture_to_encode_us{0};
static std::atomic<int64_t>  g_stats_desktop_encode_to_send_us{0};
static std::atomic<int64_t>  g_stats_android_recv_to_feed_us{0};
static std::atomic<int64_t>  g_stats_android_recv_to_release_us{0};
static std::atomic<int64_t>  g_stats_video_rtt_us{0};
static std::atomic<uint64_t> g_stats_idle_frames{0};

typedef struct {
    bool valid;
    int64_t pts_us;
    AndroidFrameTelemetry meta;
} PendingFrameTelemetry;

#define PENDING_TELEMETRY_CAP 256
static PendingFrameTelemetry g_pending_telemetry[PENDING_TELEMETRY_CAP];
static std::mutex g_pending_telemetry_mutex;

typedef struct {
    int codec_id;
    char mime[32];
    char codec_name[128];
    DecoderVendorHints hints;
    bool valid;
} NativeDecoderSelection;


/* ---- JNI callback state ---- */
static JavaVM*           g_jvm      = nullptr;
static jobject           g_activity = nullptr;   /* global ref */
static jmethodID         g_onStatusChanged = nullptr;
static jmethodID         g_onDeckConfigReceived = nullptr;
static jmethodID         g_onMediaStateReceived = nullptr;
static jmethodID         g_onVolumeStateReceived = nullptr;
static jclass            g_codecSelectorClass = nullptr;
static jclass            g_decoderChoiceClass = nullptr;
static jmethodID         g_chooseDecoderForNative = nullptr;
static jfieldID          g_choiceCodecId = nullptr;
static jfieldID          g_choiceMime = nullptr;
static jfieldID          g_choiceDecoderName = nullptr;
static jfieldID          g_choiceDirectSubmit = nullptr;
static jfieldID          g_choiceHasAndroidLowLatency = nullptr;
static jfieldID          g_choiceIsQcomC2 = nullptr;
static jfieldID          g_choiceIsQcomOmx = nullptr;

static int64_t now_us() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static uint8_t codec_cap_for_codec(uint8_t codec) {
    switch (codec) {
        case DS_CODEC_HEVC: return DS_CODEC_CAP_HEVC;
        case DS_CODEC_H264:
        default: return DS_CODEC_CAP_H264;
    }
}

static int64_t reserve_frame_pts_us(bool is_config) {
    int64_t current = g_next_pts_us.load(std::memory_order_relaxed);
    if (is_config) {
        return current;
    }
    int64_t interval = g_stream_frame_interval_us.load(std::memory_order_acquire);
    return g_next_pts_us.fetch_add(interval, std::memory_order_acq_rel);
}


static bool choose_decoder_for_stream(uint8_t codec_mask, uint16_t width,
                                      uint16_t height, uint8_t fps,
                                      NativeDecoderSelection* out) {
    if (!out) return false;
    memset(out, 0, sizeof(*out));

    if (!g_jvm || !g_codecSelectorClass || !g_chooseDecoderForNative) {
        LOGW("choose_decoder: Kotlin selector unavailable, using H.264 fallback");
        out->codec_id = DS_CODEC_H264;
        snprintf(out->mime, sizeof(out->mime), "%s", "video/avc");
        out->valid = true;
        return true;
    }

    JNIEnv* env = nullptr;
    bool did_attach = false;
    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("choose_decoder: AttachCurrentThread failed");
            return false;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("choose_decoder: GetEnv failed: %d", result);
        return false;
    }

    jobject choice = env->CallStaticObjectMethod(
        g_codecSelectorClass, g_chooseDecoderForNative,
        (jint)codec_mask, (jint)width, (jint)height, (jint)fps);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        choice = nullptr;
    }

    if (choice) {
        out->codec_id = env->GetIntField(choice, g_choiceCodecId);
        out->hints.direct_submit =
            env->GetBooleanField(choice, g_choiceDirectSubmit) == JNI_TRUE;
        out->hints.has_android_low_latency =
            env->GetBooleanField(choice, g_choiceHasAndroidLowLatency) == JNI_TRUE;
        out->hints.is_qcom_c2 =
            env->GetBooleanField(choice, g_choiceIsQcomC2) == JNI_TRUE;
        out->hints.is_qcom_omx =
            env->GetBooleanField(choice, g_choiceIsQcomOmx) == JNI_TRUE;
        out->hints.codec_id = out->codec_id;

        jstring mime = static_cast<jstring>(env->GetObjectField(choice, g_choiceMime));
        jstring decoder_name = static_cast<jstring>(
            env->GetObjectField(choice, g_choiceDecoderName));

        const char* mime_chars = mime ? env->GetStringUTFChars(mime, nullptr) : nullptr;
        const char* name_chars = decoder_name
            ? env->GetStringUTFChars(decoder_name, nullptr) : nullptr;

        snprintf(out->mime, sizeof(out->mime), "%s",
                 mime_chars ? mime_chars : "video/avc");
        snprintf(out->codec_name, sizeof(out->codec_name), "%s",
                 name_chars ? name_chars : "");

        if (name_chars) env->ReleaseStringUTFChars(decoder_name, name_chars);
        if (mime_chars) env->ReleaseStringUTFChars(mime, mime_chars);
        if (decoder_name) env->DeleteLocalRef(decoder_name);
        if (mime) env->DeleteLocalRef(mime);
        env->DeleteLocalRef(choice);

        out->valid = true;
    }

    if (did_attach) {
        g_jvm->DetachCurrentThread();
    }

    if (!out->valid) {
        LOGW("choose_decoder: no Kotlin decoder choice for mask=0x%02x", codec_mask);
    } else {
        LOGI("choose_decoder: codec=%d mime=%s name=%s direct=%d ll=%d qcomC2=%d qcomOmx=%d",
             out->codec_id, out->mime, out->codec_name,
             out->hints.direct_submit ? 1 : 0,
             out->hints.has_android_low_latency ? 1 : 0,
             out->hints.is_qcom_c2 ? 1 : 0,
             out->hints.is_qcom_omx ? 1 : 0);
    }

    return out->valid;
}

static int64_t ewma_us(int64_t prev, int64_t sample) {
    if (sample < 0) return prev;
    if (prev <= 0) return sample;
    return prev + (sample - prev) / 10;
}

static uint32_t media_codec_flags_from_video_flags(uint8_t video_flags,
                                                   int* is_config) {
    int keyframe = 0;
    int config = 0;
    ds_frame_parse_flags(video_flags, &keyframe, &config);

    uint32_t mc_flags = 0;
    if (keyframe) {
        mc_flags |= MC_BUFFER_FLAG_KEY_FRAME;
    }
    if (config) {
        mc_flags |= MC_BUFFER_FLAG_CODEC_CONFIG;
    }
    if (is_config) {
        *is_config = config;
    }
    return mc_flags;
}

static void patch_h264_config_for_decoder(uint8_t* video_data,
                                          size_t* nal_len) {
    if (!video_data || !nal_len ||
            g_active_codec_id.load(std::memory_order_relaxed) != DS_CODEC_H264) {
        return;
    }

    if (g_h264_constraints_only_sps.load(std::memory_order_relaxed)) {
        sps_patch_h264_constraints_only(video_data, *nal_len);
    } else {
        sps_patch_h264_low_latency(video_data, nal_len, MAX_FRAME_SIZE);
    }
}

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

static void store_pending_telemetry(int64_t pts_us,
                                    const AndroidFrameTelemetry* meta);
static void remove_pending_telemetry(int64_t pts_us);
static void record_feed_latency(const AndroidFrameTelemetry* meta,
                                int64_t feed_us);
static bool feed_video_message_direct(const uint8_t* msg, size_t msg_len,
                                      int32_t input_idx);

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
    static uint8_t payload_buf[MAX_WIRE_VIDEO_PAYLOAD_SIZE];
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

        uint8_t codec_mask = req.reserved[0];
        if (codec_mask == 0) {
            codec_mask = codec_cap_for_codec(req.codec);
        }

        /* Prefer the precise frame_interval_us when available (non-zero).
         * Old desktops zero-fill reserved bytes → falls back to 1000000/fps. */
        uint32_t interval_us = req.frame_interval_us;
        if (interval_us == 0) {
            uint32_t fps = req.fps > 0 ? req.fps : 60;
            interval_us = 1000000u / fps;
        }
        g_stream_frame_interval_us.store(interval_us, std::memory_order_release);
        g_next_pts_us.store(0, std::memory_order_release);

        NativeDecoderSelection selection;
        if (!choose_decoder_for_stream(codec_mask, req.width, req.height,
                                       req.fps, &selection)) {
            LOGW("recv_thread: decoder choice failed for mask=0x%02x; trying H.264 fallback",
                 codec_mask);
            choose_decoder_for_stream(DS_CODEC_CAP_H264, req.width, req.height,
                                      req.fps, &selection);
        }
        if (!selection.valid) {
            LOGE("recv_thread: no usable decoder for stream");
            notify_status(STATUS_ERROR);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
            if (g_decoder) {
                decoder_destroy(g_decoder);
                g_decoder = nullptr;
            }
            g_decoder = decoder_create(g_window, selection.mime,
                                       selection.codec_name, &selection.hints);
            if (g_decoder) {
                decoder_set_wakeup(g_decoder, decoder_wakeup_cb, nullptr);
                decoder_set_rendered_callback(g_decoder, decoder_rendered_cb,
                                              nullptr, false);
                if (decoder_configure(g_decoder, req.width, req.height,
                                      req.fps) != 0) {
                    LOGW("recv_thread: selected decoder failed; trying H.264 fallback");
                    decoder_destroy(g_decoder);
                    g_decoder = nullptr;

                    NativeDecoderSelection fallback;
                    if (selection.codec_id != DS_CODEC_H264 &&
                            choose_decoder_for_stream(DS_CODEC_CAP_H264,
                                                      req.width, req.height,
                                                      req.fps, &fallback)) {
                        selection = fallback;
                        g_decoder = decoder_create(g_window, selection.mime,
                                                   selection.codec_name,
                                                   &selection.hints);
                        if (g_decoder) {
                            decoder_set_wakeup(g_decoder, decoder_wakeup_cb, nullptr);
                            decoder_set_rendered_callback(g_decoder,
                                                          decoder_rendered_cb,
                                                          nullptr, false);
                        }
                    }

                    if (!g_decoder ||
                            decoder_configure(g_decoder, req.width, req.height,
                                              req.fps) != 0) {
                        if (g_decoder) {
                            decoder_destroy(g_decoder);
                            g_decoder = nullptr;
                        }
                    }
                }
            }
        }

        if (!g_decoder) {
            LOGE("recv_thread: failed to create/configure decoder");
            notify_status(STATUS_ERROR);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        g_decoder_direct_submit.store(
            selection.hints.direct_submit && decoder_is_async(g_decoder),
            std::memory_order_release);
        g_active_codec_id.store(selection.codec_id, std::memory_order_release);
        g_h264_constraints_only_sps.store(
            selection.hints.has_android_low_latency ||
                selection.hints.is_qcom_c2 ||
                selection.hints.is_qcom_omx,
            std::memory_order_release);
        g_decoder_configured.store(true, std::memory_order_release);
        LOGI("recv_thread: accepted codec=%d direct_submit=%d",
             selection.codec_id,
             g_decoder_direct_submit.load(std::memory_order_relaxed) ? 1 : 0);

        /* Send handshake response */
        ds_handshake_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.protocol_version   = DS_PROTOCOL_VERSION;
        resp.accepted_width     = req.width;
        resp.accepted_height    = req.height;
        resp.accepted_fps       = req.fps;
        resp.accepted_codec     = (uint8_t)selection.codec_id;
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

            if (hdr.length > MAX_WIRE_VIDEO_PAYLOAD_SIZE) {
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
                    int64_t recv_us = now_us();

                    /* Stats: track bytes received */
                    g_stats_bytes_received.fetch_add(
                        DS_HEADER_SIZE + hdr.length, std::memory_order_relaxed);

                    /* Stats: track frame pacing (jitter via EWMA) */
                    {
                        int64_t last = g_stats_last_frame_arrival_us.exchange(
                            recv_us, std::memory_order_relaxed);
                        if (last > 0) {
                            int64_t interval = recv_us - last;
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

                    ds_video_telemetry_t wire_telemetry;
                    memset(&wire_telemetry, 0, sizeof(wire_telemetry));
                    size_t telemetry_header_size = 0;
                    const uint8_t* nal_payload = payload_buf;
                    size_t nal_len = hdr.length;
                    if (ds_frame_read_telemetry(payload_buf, hdr.length,
                                                &wire_telemetry,
                                                &telemetry_header_size)) {
                        nal_payload += telemetry_header_size;
                        nal_len -= telemetry_header_size;
                    }
                    if (nal_len > MAX_FRAME_SIZE) {
                        LOGE("recv_thread: NAL payload too large after telemetry: %zu", nal_len);
                        break;
                    }

                    /* Preserve protocol flags; Android must not infer config/keyframe
                     * from the first NAL because Windows keyframes may start with SPS. */
                    video_msg_buf[0] = hdr.flags;
                    AndroidFrameTelemetry android_telemetry;
                    android_telemetry.telemetry = wire_telemetry;
                    android_telemetry.recv_us = recv_us;
                    memcpy(video_msg_buf + 1, &android_telemetry,
                           sizeof(android_telemetry));
                    if (nal_len > 0) {
                        memcpy(video_msg_buf + INTERNAL_VIDEO_HEADER_SIZE,
                               nal_payload, nal_len);
                    }

                    size_t msg_len = nal_len + INTERNAL_VIDEO_HEADER_SIZE;
                    bool consumed = false;
                    if (g_decoder_direct_submit.load(std::memory_order_acquire) &&
                            g_decoder_configured.load(std::memory_order_acquire) &&
                            ring_buffer_available_read(g_ring_buf) == 0) {
                        int32_t input_idx = -1;
                        {
                            std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
                            if (g_decoder) {
                                input_idx = decoder_pop_input(g_decoder);
                            }
                        }
                        if (input_idx >= 0) {
                            consumed = feed_video_message_direct(video_msg_buf,
                                                                 msg_len,
                                                                 input_idx);
                            if (consumed) {
                                g_decode_cv.notify_one();
                            }
                        }
                    }

                    if (!consumed && ring_buffer_write_message(
                            g_ring_buf, video_msg_buf, msg_len) != 0) {
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
        g_stats_latency_to_feed_us.store(0, std::memory_order_relaxed);
        g_stats_latency_to_release_us.store(0, std::memory_order_relaxed);
        g_stats_desktop_capture_to_send_us.store(0, std::memory_order_relaxed);
        g_stats_desktop_capture_to_encode_us.store(0, std::memory_order_relaxed);
        g_stats_desktop_encode_to_send_us.store(0, std::memory_order_relaxed);
        g_stats_android_recv_to_feed_us.store(0, std::memory_order_relaxed);
        g_stats_android_recv_to_release_us.store(0, std::memory_order_relaxed);
        g_stats_video_rtt_us.store(0, std::memory_order_relaxed);
        g_stats_idle_frames.store(0, std::memory_order_relaxed);
        g_next_pts_us.store(0, std::memory_order_relaxed);
        g_decoder_direct_submit.store(false, std::memory_order_relaxed);
        g_active_codec_id.store(DS_CODEC_H264, std::memory_order_relaxed);
        g_h264_constraints_only_sps.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
            memset(g_pending_telemetry, 0, sizeof(g_pending_telemetry));
        }

        /* Give decode thread time to notice and stop touching the decoder */
        usleep(5000);  /* 5ms — decode thread polls at 100us */

        /* Reset ring buffer to purge stale video data */
        if (g_ring_buf) {
            ring_buffer_reset(g_ring_buf);
        }

        /* Destroy decoder for clean state. The next handshake chooses codec again. */
        {
            std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
            if (g_decoder) {
                decoder_destroy(g_decoder);
                g_decoder = nullptr;
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

static void store_pending_telemetry(int64_t pts_us,
                                    const AndroidFrameTelemetry* meta) {
    if (!meta || meta->telemetry.sequence == 0) return;
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    size_t slot = (size_t)(meta->telemetry.sequence & (PENDING_TELEMETRY_CAP - 1));
    g_pending_telemetry[slot].pts_us = pts_us;
    g_pending_telemetry[slot].meta = *meta;
    g_pending_telemetry[slot].valid = true;
}

static void remove_pending_telemetry(int64_t pts_us) {
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    for (size_t i = 0; i < PENDING_TELEMETRY_CAP; i++) {
        if (g_pending_telemetry[i].valid &&
                g_pending_telemetry[i].pts_us == pts_us) {
            g_pending_telemetry[i].valid = false;
            return;
        }
    }
}

static bool take_pending_telemetry(int64_t pts_us,
                                   AndroidFrameTelemetry* out) {
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    for (size_t i = 0; i < PENDING_TELEMETRY_CAP; i++) {
        if (g_pending_telemetry[i].valid &&
                g_pending_telemetry[i].pts_us == pts_us) {
            if (out) *out = g_pending_telemetry[i].meta;
            g_pending_telemetry[i].valid = false;
            return true;
        }
    }
    return false;
}

static void record_feed_latency(const AndroidFrameTelemetry* meta,
                                int64_t feed_us) {
    if (!meta || meta->telemetry.sequence == 0) return;
    int64_t recv_to_feed = feed_us - meta->recv_us;
    bool is_idle = (meta->telemetry.flags & DS_VIDEO_TELEMETRY_FLAG_IDLE) != 0;
    if (is_idle) {
        g_stats_idle_frames.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    int64_t transit_est = meta->telemetry.rtt_us > 0
        ? meta->telemetry.rtt_us / 2 : 0;
    int64_t e2e_feed = meta->telemetry.capture_to_send_us
        + transit_est + recv_to_feed;

    g_stats_desktop_capture_to_send_us.store(
        meta->telemetry.capture_to_send_us, std::memory_order_relaxed);
    g_stats_desktop_capture_to_encode_us.store(
        meta->telemetry.capture_to_encode_us, std::memory_order_relaxed);
    g_stats_desktop_encode_to_send_us.store(
        meta->telemetry.encode_to_send_us, std::memory_order_relaxed);
    g_stats_video_rtt_us.store(meta->telemetry.rtt_us, std::memory_order_relaxed);
    g_stats_android_recv_to_feed_us.store(
        ewma_us(g_stats_android_recv_to_feed_us.load(std::memory_order_relaxed),
                recv_to_feed),
        std::memory_order_relaxed);
    g_stats_latency_to_feed_us.store(
        ewma_us(g_stats_latency_to_feed_us.load(std::memory_order_relaxed),
                e2e_feed),
        std::memory_order_relaxed);
}

static void record_release_latency(int64_t pts_us, int64_t release_us) {
    AndroidFrameTelemetry meta;
    if (!take_pending_telemetry(pts_us, &meta)) return;

    int64_t recv_to_release = release_us - meta.recv_us;
    bool is_idle = (meta.telemetry.flags & DS_VIDEO_TELEMETRY_FLAG_IDLE) != 0;
    if (is_idle) {
        return;
    }
    int64_t transit_est = meta.telemetry.rtt_us > 0
        ? meta.telemetry.rtt_us / 2 : 0;
    int64_t e2e_release = meta.telemetry.capture_to_send_us
        + transit_est + recv_to_release;

    g_stats_android_recv_to_release_us.store(
        ewma_us(g_stats_android_recv_to_release_us.load(std::memory_order_relaxed),
                recv_to_release),
        std::memory_order_relaxed);
    g_stats_latency_to_release_us.store(
        ewma_us(g_stats_latency_to_release_us.load(std::memory_order_relaxed),
                e2e_release),
        std::memory_order_relaxed);
}

static void decoder_rendered_cb(void* /*userdata*/, int64_t pts_us) {
    g_stats_frames_decoded.fetch_add(1, std::memory_order_relaxed);
    record_release_latency(pts_us, now_us());
    g_decode_cv.notify_one();
}

static bool feed_video_message_direct(const uint8_t* msg, size_t msg_len,
                                      int32_t input_idx) {
    if (!msg || msg_len < INTERNAL_VIDEO_HEADER_SIZE + 1 || input_idx < 0) {
        return false;
    }

    uint8_t video_flags = msg[0];
    AndroidFrameTelemetry frame_meta;
    memcpy(&frame_meta, msg + 1, sizeof(frame_meta));
    uint8_t* video_data = const_cast<uint8_t*>(msg + INTERNAL_VIDEO_HEADER_SIZE);
    size_t nal_len = msg_len - INTERNAL_VIDEO_HEADER_SIZE;

    int is_config = 0;
    uint32_t mc_flags =
        media_codec_flags_from_video_flags(video_flags, &is_config);

    if (is_config) {
        patch_h264_config_for_decoder(video_data, &nal_len);
    }

    int64_t pts_us = reserve_frame_pts_us(is_config != 0);
    if (!is_config) {
        store_pending_telemetry(pts_us, &frame_meta);
    }
    int ret = -1;
    {
        std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
        if (g_decoder && g_decoder_configured.load(std::memory_order_acquire)) {
            ret = decoder_feed_index(g_decoder, input_idx, video_data, nal_len,
                                     pts_us, mc_flags);
        }
    }

    if (ret == 0) {
        g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
        if (!is_config) {
            int64_t feed_us = now_us();
            record_feed_latency(&frame_meta, feed_us);
        }
        return true;
    }

    if (!is_config) {
        remove_pending_telemetry(pts_us);
    }
    g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
    return false;
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

    uint32_t frames_fed = 0;
    uint32_t frames_rendered = 0;
    uint32_t feed_errors = 0;
    uint32_t frames_skipped = 0;
    uint64_t last_logged_fed = 0;
    int64_t last_log_us = now_us();
    int32_t held_async_input_idx = -1;
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
            last_log_us = now_us();
            held_async_input_idx = -1;
            g_next_pts_us.store(0, std::memory_order_relaxed);
            clock_gettime(CLOCK_MONOTONIC, &ts_start);
            continue;
        }

        bool is_async = decoder_is_async(g_decoder);
        bool any_work = false;

        /* ---- 1. Drain output (both modes) ---- */
        int64_t rendered_pts_us = -1;
        int r = 0;
        {
            std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
            r = decoder_drain(g_decoder, &rendered_pts_us);
        }
        if (r > 0) {
            frames_rendered += r;
            g_stats_frames_decoded.fetch_add(r, std::memory_order_relaxed);
            record_release_latency(rendered_pts_us, now_us());
            any_work = true;
        }

        /* ---- 2. Feed NALs from ring buffer ---- */
        if (is_async) {
            /* ASYNC: use pre-dequeued input indices from callbacks */
            while (g_running.load(std::memory_order_acquire)) {
                int32_t input_idx = held_async_input_idx;
                if (input_idx >= 0) {
                    held_async_input_idx = -1;
                } else {
                    std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
                    input_idx = g_decoder ? decoder_pop_input(g_decoder) : -1;
                }
                if (input_idx < 0) break;  /* no input buffer available */

                size_t msg_len = ring_buffer_read_message(
                    g_ring_buf, nal_buf, MAX_VIDEO_MSG_SIZE);
                if (msg_len < INTERNAL_VIDEO_HEADER_SIZE + 1) {
                    /* Keep the dequeued input buffer for the next real NAL.
                     * Queueing empty buffers adds avoidable MediaCodec work. */
                    held_async_input_idx = input_idx;
                    break;
                }
                any_work = true;

                uint8_t video_flags = nal_buf[0];
                AndroidFrameTelemetry frame_meta;
                memcpy(&frame_meta, nal_buf + 1, sizeof(frame_meta));
                uint8_t* video_data = nal_buf + INTERNAL_VIDEO_HEADER_SIZE;
                size_t nal_len = msg_len - INTERNAL_VIDEO_HEADER_SIZE;

                int is_config = 0;
                uint32_t mc_flags =
                    media_codec_flags_from_video_flags(video_flags, &is_config);

                if (is_config) {
                    patch_h264_config_for_decoder(video_data, &nal_len);
                }

                int64_t pts_us = reserve_frame_pts_us(is_config != 0);
                if (!is_config) {
                    store_pending_telemetry(pts_us, &frame_meta);
                }
                int ret = -1;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
                    ret = decoder_feed_index(g_decoder, input_idx,
                                             video_data, nal_len,
                                             pts_us, mc_flags);
                }
                if (ret == 0) {
                    frames_fed++;
                    g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
                    if (!is_config) {
                        int64_t feed_us = now_us();
                        record_feed_latency(&frame_meta, feed_us);
                    }
                } else {
                    if (!is_config) {
                        remove_pending_telemetry(pts_us);
                    }
                    feed_errors++;
                    g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
                }

            }
        } else {
            /* SYNC: burst-read ring buffer, dequeue input buffers inline */
            while (g_running.load(std::memory_order_acquire)) {
                size_t msg_len = ring_buffer_read_message(
                    g_ring_buf, nal_buf, MAX_VIDEO_MSG_SIZE);
                if (msg_len < INTERNAL_VIDEO_HEADER_SIZE + 1) break;
                any_work = true;

                uint8_t video_flags = nal_buf[0];
                AndroidFrameTelemetry frame_meta;
                memcpy(&frame_meta, nal_buf + 1, sizeof(frame_meta));
                uint8_t* video_data = nal_buf + INTERNAL_VIDEO_HEADER_SIZE;
                size_t nal_len = msg_len - INTERNAL_VIDEO_HEADER_SIZE;

                int is_config = 0;
                uint32_t mc_flags =
                    media_codec_flags_from_video_flags(video_flags, &is_config);

                if (is_config) {
                    patch_h264_config_for_decoder(video_data, &nal_len);
                }

                int64_t pts_us = reserve_frame_pts_us(is_config != 0);
                if (!is_config) {
                    store_pending_telemetry(pts_us, &frame_meta);
                }
                int ret = -1;
                {
                    std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
                    ret = decoder_feed(g_decoder, video_data, nal_len,
                                       pts_us, mc_flags);
                }
                if (ret == 0) {
                    frames_fed++;
                    g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
                    if (!is_config) {
                        int64_t feed_us = now_us();
                        record_feed_latency(&frame_meta, feed_us);
                    }
                } else {
                    if (!is_config) {
                        remove_pending_telemetry(pts_us);
                    }
                    feed_errors++;
                    g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
                }

            }
        }

        /* ---- 3. Drain again after feeding ---- */
        if (any_work) {
            rendered_pts_us = -1;
            {
                std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
                r = decoder_drain(g_decoder, &rendered_pts_us);
            }
            if (r > 0) {
                frames_rendered += r;
                g_stats_frames_decoded.fetch_add(r, std::memory_order_relaxed);
                record_release_latency(rendered_pts_us, now_us());
            }
        }

        /* ---- 4. Wait for next event ---- */
        if (!any_work) {
            if (is_async) {
                /* Event-driven: wait on condition variable.
                 * Woken by: MediaCodec callbacks OR ring buffer writes. */
                std::unique_lock<std::mutex> lock(g_decode_mutex);
                g_decode_cv.wait_for(lock, std::chrono::milliseconds(1));
            } else {
                /* Sync fallback: brief polling sleep */
                usleep(50);
            }
        }

        /* ---- 5. Stats logging ---- */
        uint64_t total_fed = g_stats_frames_fed.load(std::memory_order_relaxed);
        int64_t log_us = now_us();
        if (total_fed > 0 && total_fed != last_logged_fed &&
                log_us - last_log_us >= 2000000) {
            last_logged_fed = total_fed;
            last_log_us = log_us;
            uint64_t total_rendered =
                g_stats_frames_decoded.load(std::memory_order_relaxed);
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double elapsed = (ts_now.tv_sec - ts_start.tv_sec)
                           + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            size_t ring_used = ring_buffer_available_read(g_ring_buf);
            LOGI("decode[%s]: fed=%u rendered=%u err=%u | "
                 "%.1f fed/s %.1f render/s idle=%llu | ring=%zu bytes | "
                 "lat feed=%.1fms release=%.1fms desk=%.1fms android=%.1fms rtt=%.1fms "
                 "capenc=%.1fms encsend=%.1fms",
                 is_async ? "async" : "sync",
                 (unsigned)total_fed, (unsigned)total_rendered, feed_errors,
                 total_fed / elapsed, total_rendered / elapsed,
                 (unsigned long long)g_stats_idle_frames.load(std::memory_order_relaxed),
                 ring_used,
                 g_stats_latency_to_feed_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_latency_to_release_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_desktop_capture_to_send_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_android_recv_to_release_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_video_rtt_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_desktop_capture_to_encode_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_desktop_encode_to_send_us.load(std::memory_order_relaxed) / 1000.0);
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

    jclass selectorLocal = env->FindClass("com/droidscreen/app/CodecSelector");
    jclass choiceLocal = env->FindClass("com/droidscreen/app/NativeDecoderChoice");
    if (selectorLocal && choiceLocal) {
        g_codecSelectorClass = static_cast<jclass>(env->NewGlobalRef(selectorLocal));
        g_decoderChoiceClass = static_cast<jclass>(env->NewGlobalRef(choiceLocal));
        g_chooseDecoderForNative = env->GetStaticMethodID(
            g_codecSelectorClass, "chooseDecoderForNative",
            "(IIII)Lcom/droidscreen/app/NativeDecoderChoice;");
        g_choiceCodecId = env->GetFieldID(g_decoderChoiceClass, "codecId", "I");
        g_choiceMime = env->GetFieldID(g_decoderChoiceClass, "mime", "Ljava/lang/String;");
        g_choiceDecoderName = env->GetFieldID(g_decoderChoiceClass,
                                              "decoderName", "Ljava/lang/String;");
        g_choiceDirectSubmit = env->GetFieldID(g_decoderChoiceClass,
                                               "directSubmit", "Z");
        g_choiceHasAndroidLowLatency = env->GetFieldID(
            g_decoderChoiceClass, "hasAndroidLowLatency", "Z");
        g_choiceIsQcomC2 = env->GetFieldID(g_decoderChoiceClass, "isQcomC2", "Z");
        g_choiceIsQcomOmx = env->GetFieldID(g_decoderChoiceClass, "isQcomOmx", "Z");
        if (!g_chooseDecoderForNative || !g_choiceCodecId || !g_choiceMime ||
                !g_choiceDecoderName || !g_choiceDirectSubmit ||
                !g_choiceHasAndroidLowLatency || !g_choiceIsQcomC2 ||
                !g_choiceIsQcomOmx) {
            LOGW("nativeInit: CodecSelector JNI lookup incomplete; fallback will be used");
            env->ExceptionClear();
            if (g_codecSelectorClass) env->DeleteGlobalRef(g_codecSelectorClass);
            if (g_decoderChoiceClass) env->DeleteGlobalRef(g_decoderChoiceClass);
            g_codecSelectorClass = nullptr;
            g_decoderChoiceClass = nullptr;
            g_chooseDecoderForNative = nullptr;
        }
    } else {
        LOGW("nativeInit: CodecSelector classes not found; fallback will be used");
        env->ExceptionClear();
    }
    if (selectorLocal) env->DeleteLocalRef(selectorLocal);
    if (choiceLocal) env->DeleteLocalRef(choiceLocal);

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

    /* Start TCP server */
    g_server_fd = tcp_server_start(port);
    if (g_server_fd < 0) {
        LOGE("nativeInit: failed to start TCP server on port %d", port);
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
    {
        std::lock_guard<std::mutex> lock(g_decoder_api_mutex);
        if (g_decoder) {
            decoder_destroy(g_decoder);
            g_decoder = nullptr;
        }
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
    if (g_codecSelectorClass) {
        env->DeleteGlobalRef(g_codecSelectorClass);
        g_codecSelectorClass = nullptr;
    }
    if (g_decoderChoiceClass) {
        env->DeleteGlobalRef(g_decoderChoiceClass);
        g_decoderChoiceClass = nullptr;
    }
    g_onStatusChanged = nullptr;
    g_onDeckConfigReceived = nullptr;
    g_onMediaStateReceived = nullptr;
    g_onVolumeStateReceived = nullptr;
    g_chooseDecoderForNative = nullptr;
    g_choiceCodecId = nullptr;
    g_choiceMime = nullptr;
    g_choiceDecoderName = nullptr;
    g_choiceDirectSubmit = nullptr;
    g_choiceHasAndroidLowLatency = nullptr;
    g_choiceIsQcomC2 = nullptr;
    g_choiceIsQcomOmx = nullptr;
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
    jlong stats[14];
    stats[0] = static_cast<jlong>(g_stats_bytes_received.load(std::memory_order_relaxed));
    stats[1] = static_cast<jlong>(g_stats_frames_decoded.load(std::memory_order_relaxed));
    stats[2] = static_cast<jlong>(g_stats_frames_fed.load(std::memory_order_relaxed));
    stats[3] = static_cast<jlong>(g_stats_feed_errors.load(std::memory_order_relaxed));
    stats[4] = static_cast<jlong>(g_stats_frame_jitter_us.load(std::memory_order_relaxed));
    stats[5] = static_cast<jlong>(g_stream_frame_interval_us.load(std::memory_order_relaxed));
    stats[6] = static_cast<jlong>(g_stats_frames_skipped.load(std::memory_order_relaxed));
    stats[7] = static_cast<jlong>(g_stats_latency_to_feed_us.load(std::memory_order_relaxed));
    stats[8] = static_cast<jlong>(g_stats_latency_to_release_us.load(std::memory_order_relaxed));
    stats[9] = static_cast<jlong>(g_stats_desktop_capture_to_send_us.load(std::memory_order_relaxed));
    stats[10] = static_cast<jlong>(g_stats_android_recv_to_feed_us.load(std::memory_order_relaxed));
    stats[11] = static_cast<jlong>(g_stats_android_recv_to_release_us.load(std::memory_order_relaxed));
    stats[12] = static_cast<jlong>(g_stats_video_rtt_us.load(std::memory_order_relaxed));
    stats[13] = static_cast<jlong>(g_stats_idle_frames.load(std::memory_order_relaxed));

    jlongArray result = env->NewLongArray(14);
    env->SetLongArrayRegion(result, 0, 14, stats);
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
