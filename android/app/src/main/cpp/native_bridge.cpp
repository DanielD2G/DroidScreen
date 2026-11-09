/*
 * DroidScreen - JNI bridge between Kotlin and native C/C++ code.
 *
 * Manages TCP server, receive/decode threads, and input forwarding.
 * Supports reconnection: recv_thread loops back to accept() after
 * a client disconnects, resetting the decoder and ring buffer.
 *
 * Decoder ownership: g_decoder is created and destroyed only by the recv
 * thread while the decode thread is parked (park_decode_thread), and by
 * nativeStop after both threads are joined. The decode thread uses it without
 * a lock, so no lock is ever held across a blocking MediaCodec call.
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
#include <algorithm>
#include <vector>

#include "tcp_server.h"
#include "decoder.h"
#include "ring_buffer.h"
#include "ds_clock.h"
#include "outbound.h"
#include "mouse_sender.h"
#include "pen_sender.h"
#include "touch_sender.h"
#include "deck_sender.h"
#include "key_sender.h"
#include <sys/socket.h>
#include "sps_patch.h"

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/handshake.h"
#include "droidscreen/hello.h"
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

/* Max single frame payload size: 4 MB */
#define MAX_FRAME_SIZE (4 * 1024 * 1024)

/* Ring buffer physical capacity: 8 MB (power of 2). It only has to admit
 * CONFIG + one maximum-size IDR; how much latency the ring may hold is a
 * separate time budget (kBacklogBudgetMs, see backlog_tick). */
#define RING_BUFFER_CAPACITY (8 * 1024 * 1024)

/* Wire payload can include a telemetry prefix before the NAL payload. */
#define MAX_WIRE_VIDEO_PAYLOAD_SIZE (MAX_FRAME_SIZE + DS_VIDEO_TELEMETRY_SIZE)

typedef struct {
    ds_video_telemetry_t telemetry;
    int64_t recv_us;
    uint64_t rx_seq;   /* receive order, for loss recovery (see enter_recovery) */
} AndroidFrameTelemetry;

/* Internal ring-buffer message size: [flags:u8][telemetry][nal payload] */
#define INTERNAL_VIDEO_HEADER_SIZE (1 + sizeof(AndroidFrameTelemetry))
#define MAX_VIDEO_MSG_SIZE (MAX_FRAME_SIZE + INTERNAL_VIDEO_HEADER_SIZE)

/* CONFIG (SPS/PPS/VPS, a few hundred bytes; 64 KB is generous) plus a max IDR,
 * each with its u32 ring framing. */
static_assert(RING_BUFFER_CAPACITY >=
                  (64 * 1024 + 4) + (MAX_VIDEO_MSG_SIZE + 4),
              "ring must admit CONFIG + a maximum-size IDR");
static_assert((RING_BUFFER_CAPACITY & (RING_BUFFER_CAPACITY - 1)) == 0,
              "ring capacity must be a power of 2");

/* Status constants — must match MainActivity.kt companion object */
#define STATUS_WAITING      0
#define STATUS_CONNECTED    1
#define STATUS_DISCONNECTED 2
#define STATUS_ERROR        3

#define MC_BUFFER_FLAG_KEY_FRAME    1u
#define MC_BUFFER_FLAG_CODEC_CONFIG 2u

/* ---- Decode thread wakeup and parking ----
 * g_decode_cv: signalled by ring buffer writes (recv_thread), async MediaCodec
 * callbacks and session start. The decode thread waits on it (with a short
 * timeout for recovery/backlog timers) instead of polling.
 * g_park_cv: the decode thread reports it is parked (not touching g_decoder),
 * so the recv thread can destroy or recreate the decoder. */
static std::mutex              g_decode_mutex;
static std::condition_variable g_decode_cv;
static std::condition_variable g_park_cv;
static bool                    g_decode_signal = false;  /* guarded */
static bool                    g_decode_parked = true;   /* guarded */

static void wake_decode_thread() {
    {
        std::lock_guard<std::mutex> lock(g_decode_mutex);
        g_decode_signal = true;
    }
    g_decode_cv.notify_one();
}

static void decoder_wakeup_cb(void* /*userdata*/) {
    wake_decode_thread();
}

static void decoder_released_cb(void* userdata, int64_t pts_us, bool rendered);
static void decoder_frame_rendered_cb(void* userdata, int64_t pts_us,
                                      int64_t system_nano);

/* ---- Global state ---- */
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_decoder_configured{false};
static int               g_server_fd = -1;
static int               g_client_fd = -1;
static ANativeWindow*    g_window    = nullptr;
static ring_buffer*      g_ring_buf  = nullptr;
static pthread_t         g_recv_thread;
static pthread_t         g_decode_thread;
static DecoderContext*   g_decoder   = nullptr;   /* see ownership note above */
static std::atomic<uint32_t> g_stream_frame_interval_us{16667};
static std::atomic<int64_t>  g_next_pts_us{0};
static std::atomic<int>      g_active_codec_id{DS_CODEC_H264};
static std::atomic<bool>     g_h264_constraints_only_sps{false};

/* ---- Stats tracking (read from JNI, written from recv/decode threads) ---- */
static std::atomic<uint64_t> g_stats_bytes_received{0};
static std::atomic<uint64_t> g_stats_frames_rendered{0};   /* releases with render=true */
static std::atomic<uint64_t> g_stats_frames_fed{0};
static std::atomic<uint64_t> g_stats_feed_errors{0};
static std::atomic<int64_t>  g_stats_last_frame_arrival_us{0};
static std::atomic<int64_t>  g_stats_frame_jitter_us{0};
static std::atomic<uint64_t> g_stats_frames_skipped{0};
static std::atomic<uint64_t> g_stats_idle_frames{0};
static std::atomic<uint64_t> g_stats_render_callbacks{0};
/* Latest telemetry values (per frame, not smoothed). */
static std::atomic<int64_t>  g_stats_desktop_capture_to_send_us{0};
static std::atomic<int64_t>  g_stats_video_rtt_us{0};
static std::atomic<uint32_t> g_stats_clock_uncertainty_us{0};
/* p50 of the last ~2 s window (0 = no samples), for the stats UI. */
static std::atomic<int64_t>  g_stats_comp_render_p50_us{0};
static std::atomic<int64_t>  g_stats_send_render_p50_us{0};
static std::atomic<int64_t>  g_stats_recv_render_p50_us{0};

/* ---- Pending frame telemetry ----
 * One entry per fed (non-config) frame, keyed by its unique PTS:
 *   FED      -> fed to MediaCodec, output not released yet
 *   RELEASED -> released with render=true, waiting for onFrameRendered
 * Released with render=false, or rendered, frees the entry. Entries whose
 * output never came back (decoder dropped it) or whose render callback never
 * arrived (callbacks may be missing) are expired by age. */
enum : uint8_t { PT_FREE = 0, PT_FED = 1, PT_RELEASED = 2 };

typedef struct {
    uint8_t state;
    int64_t pts_us;
    int64_t stamp_us;   /* feed time (FED) or release time (RELEASED) */
    AndroidFrameTelemetry meta;
} PendingFrameTelemetry;

#define PENDING_TELEMETRY_CAP 64
static constexpr int64_t kPendingFedMaxAgeUs   = 2000000;  /* output never released */
static constexpr int64_t kPendingRenderWaitUs  = 1000000;  /* render callback missing */
static PendingFrameTelemetry g_pending_telemetry[PENDING_TELEMETRY_CAP];
static std::mutex g_pending_telemetry_mutex;
static uint64_t g_pending_expired = 0;     /* guarded: lost outputs / callbacks */
static std::atomic<uint32_t> g_decoder_inflight{0};  /* FED entries */

/* ---- Per-frame latency samples for the current log window (all frames,
 * no smoothing). Filled by the render callback, swapped out by the logger. */
typedef struct {
    std::vector<int32_t> comp_render_us;   /* desktop compositor -> Android render */
    std::vector<int32_t> send_render_us;   /* desktop send start -> Android render */
    std::vector<int32_t> recv_render_us;   /* Android receive   -> Android render */
} LatencyWindow;
static LatencyWindow g_lat_window;
static std::mutex    g_lat_mutex;

/* ---- Receiver backlog (ring time budget) ---- */
static std::atomic<uint32_t> g_ring_frames{0};        /* video messages in the ring */
static std::atomic<uint64_t> g_recv_rate_Bps{0};      /* measured receive byte-rate */
static std::atomic<uint64_t> g_bitrate_floor_Bps{0};  /* negotiated bitrate */
static constexpr int64_t kBacklogReportUs = 100000;   /* 10 Hz report to the desktop */
static constexpr int64_t kBacklogBudgetMs = 500;

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
static jmethodID         g_authorizeDesktop = nullptr;
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
    return ds_now_us();
}

/* ---- Loss recovery ----
 * The desktop sends keyframes only on request: periodic ones were 20-40x a
 * normal frame at 2560x1600 and caused a hitch every 2 s on Wi-Fi. So every
 * way the decoder can lose references enters recovery here: session start,
 * and any dropped or failed video *or config* packet. While recovering,
 * dependent frames are skipped (a frozen picture beats a corrupted one) and a
 * keyframe is requested. Requests are retried by a timer even if no video
 * arrives, and if recovery doesn't complete in time the connection is dropped
 * so a fresh handshake rebuilds encoder and decoder.
 *
 * Every video message gets a receive sequence (rx_seq). Recovery ends only
 * when config and then a keyframe received *after* the newest loss were fed
 * successfully, so a keyframe already queued before the loss can't end it. */
static std::mutex           g_recovery_mutex;
static bool                 g_recovering = false;        /* guarded */
static uint64_t             g_loss_seq = 0;              /* guarded */
static bool                 g_config_since_loss = false; /* guarded: decoder has usable config */
static uint64_t             g_config_loss_seq = 0;       /* guarded: newest config loss / session start */
static int64_t              g_recovery_start_us = 0;     /* guarded */
static std::atomic<bool>    g_recovering_hint{false};    /* lock-free hot-path read */
static std::atomic<int64_t> g_last_keyframe_request_us{0};
static std::atomic<uint64_t> g_rx_seq{0};
static constexpr int64_t kKeyframeRequestMinGapUs = 100000;   /* 100 ms */
static constexpr int64_t kKeyframeRetryUs         = 250000;   /* 250 ms */
static constexpr int64_t kRecoveryDeadlineUs      = 3000000;  /* 3 s */

static void request_keyframe(const char* why) {
    int64_t now = now_us();
    int64_t last = g_last_keyframe_request_us.load(std::memory_order_relaxed);
    if (now - last < kKeyframeRequestMinGapUs) return;
    /* One caller wins the slot; concurrent callers don't double-send. */
    if (!g_last_keyframe_request_us.compare_exchange_strong(last, now)) return;

    int fd = g_client_fd;
    if (fd < 0) return;
    uint8_t buf[DS_HEADER_SIZE + 1];
    ds_header_t hdr;
    hdr.type = DS_MSG_CONTROL;
    hdr.flags = 0;
    hdr.length = 1;
    ds_header_serialize(buf, &hdr);
    buf[DS_HEADER_SIZE] = DS_CTRL_REQUEST_KEYFRAME;
    if (tcp_send_all(fd, buf, sizeof(buf)) == 0) {
        LOGW("requesting keyframe (%s)", why);
    } else {
        LOGE("keyframe request failed to send (%s)", why);
    }
}

/* A packet at or before receive sequence `seq` was lost or failed to decode. */
static void enter_recovery(uint64_t seq, bool config_lost, const char* why) {
    {
        std::lock_guard<std::mutex> lock(g_recovery_mutex);
        if (!g_recovering) {
            g_recovering = true;
            g_recovery_start_us = now_us();
        }
        if (seq > g_loss_seq) g_loss_seq = seq;
        /* Only a lost config packet requires fresh parameter sets: the decoder
         * keeps the ones it has otherwise (and Windows sends them only once). */
        if (config_lost) {
            g_config_since_loss = false;
            if (seq > g_config_loss_seq) g_config_loss_seq = seq;
        }
        g_recovering_hint.store(true, std::memory_order_release);
    }
    request_keyframe(why);
}

/* New session: the decoder has nothing yet; wait for config + keyframe. */
static void reset_recovery_for_session() {
    std::lock_guard<std::mutex> lock(g_recovery_mutex);
    g_recovering = true;
    g_recovery_start_us = now_us();
    /* Anything still in flight from an older session can't end recovery. */
    g_loss_seq = g_rx_seq.load(std::memory_order_acquire);
    g_config_since_loss = false;   /* a new decoder has no parameter sets */
    g_config_loss_seq = g_loss_seq;
    g_last_keyframe_request_us.store(0, std::memory_order_relaxed);
    g_recovering_hint.store(true, std::memory_order_release);
}

/* A packet was fed to the decoder successfully. */
static void on_packet_fed(uint64_t seq, bool is_config, bool is_keyframe) {
    if (!g_recovering_hint.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(g_recovery_mutex);
    if (!g_recovering) return;
    if (is_config) {
        /* Config validity has its own watermark: an ordinary frame loss after
         * this config was queued doesn't make it unusable. */
        if (seq > g_config_loss_seq) g_config_since_loss = true;
        return;
    }
    if (seq <= g_loss_seq) return;
    if (is_keyframe && g_config_since_loss) {
        g_recovering = false;
        g_recovering_hint.store(false, std::memory_order_release);
        LOGI("recovered at keyframe (rx_seq=%llu) after %.0f ms",
             (unsigned long long)seq, (now_us() - g_recovery_start_us) / 1000.0);
    }
}

/* Called from the decode loop on every iteration: retries the request even if
 * no video arrives, and gives up on the connection after the deadline. */
static void recovery_tick() {
    if (!g_recovering_hint.load(std::memory_order_acquire)) return;
    bool deadline = false;
    {
        std::lock_guard<std::mutex> lock(g_recovery_mutex);
        if (!g_recovering) return;
        int64_t now = now_us();
        if (now - g_recovery_start_us >= kRecoveryDeadlineUs) {
            deadline = true;
            g_recovery_start_us = now;  /* one shutdown per deadline */
        }
    }
    if (deadline) {
        int fd = g_client_fd;
        LOGE("no keyframe within %lld ms; dropping the connection to resync",
             (long long)(kRecoveryDeadlineUs / 1000));
        if (fd >= 0) shutdown(fd, SHUT_RDWR);
        return;
    }
    if (now_us() - g_last_keyframe_request_us.load(std::memory_order_relaxed) >= kKeyframeRetryUs) {
        request_keyframe("waiting for keyframe");
    }
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
static void clear_pending_telemetry();
static void record_fed_frame(const AndroidFrameTelemetry* meta);
static void park_decode_thread();

static void bytes_to_hex(const uint8_t* in, size_t len, char* out) {
    static const char kHex[] = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        out[i * 2]     = kHex[in[i] >> 4];
        out[i * 2 + 1] = kHex[in[i] & 0x0F];
    }
    out[len * 2] = '\0';
}

static bool hex_to_bytes(const char* in, uint8_t* out, size_t len) {
    if (!in || strlen(in) != len * 2) return false;
    auto nibble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < len; ++i) {
        int hi = nibble(in[i * 2]);
        int lo = nibble(in[i * 2 + 1]);
        if (hi < 0 || lo < 0) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

/**
 * Ask Java whether a desktop that connected over Wi-Fi may stream to us.
 * Blocks until the user decides (bounded by DS_HELLO_APPROVAL_TIMEOUT_MS on
 * the Java side). On success writes the token the desktop must keep.
 */
static bool authorize_desktop(const ds_hello_t& hello, uint8_t out_token[DS_HELLO_TOKEN_SIZE]) {
    if (!g_jvm || !g_activity || !g_authorizeDesktop) return false;

    JNIEnv* env = nullptr;
    bool did_attach = false;
    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("authorize_desktop: AttachCurrentThread failed");
            return false;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("authorize_desktop: GetEnv failed: %d", result);
        return false;
    }

    char id_hex[DS_HELLO_ID_SIZE * 2 + 1];
    char token_hex[DS_HELLO_TOKEN_SIZE * 2 + 1];
    bytes_to_hex(hello.desktop_id, DS_HELLO_ID_SIZE, id_hex);
    bytes_to_hex(hello.token, DS_HELLO_TOKEN_SIZE, token_hex);

    jstring j_id = env->NewStringUTF(id_hex);
    jstring j_name = env->NewStringUTF(hello.name);
    jstring j_token = env->NewStringUTF(token_hex);
    auto j_result = (jstring)env->CallObjectMethod(g_activity, g_authorizeDesktop,
                                                   j_id, j_name, j_token);
    bool ok = false;
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    } else if (j_result) {
        const char* chars = env->GetStringUTFChars(j_result, nullptr);
        ok = hex_to_bytes(chars, out_token, DS_HELLO_TOKEN_SIZE);
        env->ReleaseStringUTFChars(j_result, chars);
        env->DeleteLocalRef(j_result);
    }
    env->DeleteLocalRef(j_id);
    env->DeleteLocalRef(j_name);
    env->DeleteLocalRef(j_token);

    if (did_attach) g_jvm->DetachCurrentThread();
    return ok;
}

/**
 * Wireless gate: read DS_MSG_HELLO, ask for authorization, reply.
 * Returns true when the desktop may continue with the handshake.
 */
static bool run_wireless_hello(int fd) {
    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_t hdr;
    if (tcp_recv_exact(fd, hdr_buf, DS_HEADER_SIZE) != 0) {
        LOGE("hello: failed to read header");
        return false;
    }
    ds_header_deserialize(hdr_buf, &hdr);
    if (hdr.type != DS_MSG_HELLO || hdr.length != DS_HELLO_SIZE) {
        LOGE("hello: network peer did not start with HELLO (type=0x%02x len=%u)",
             hdr.type, hdr.length);
        return false;
    }
    uint8_t buf[DS_HELLO_SIZE];
    if (tcp_recv_exact(fd, buf, DS_HELLO_SIZE) != 0) {
        LOGE("hello: failed to read payload");
        return false;
    }
    ds_hello_t hello;
    ds_hello_deserialize(buf, &hello);
    LOGI("hello: desktop '%s' asking to connect", hello.name);

    ds_hello_resp_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.status = authorize_desktop(hello, resp.token) ? DS_HELLO_ACCEPTED : DS_HELLO_REJECTED;
    LOGI("hello: desktop '%s' %s", hello.name,
         resp.status == DS_HELLO_ACCEPTED ? "accepted" : "rejected");

    /* One message = one tcp_send_all() (see g_send_mutex in tcp_server.cpp). */
    uint8_t wire[DS_HEADER_SIZE + DS_HELLO_RESP_SIZE];
    ds_header_t resp_header;
    resp_header.type = DS_MSG_HELLO_RESP;
    resp_header.flags = 0;
    resp_header.length = DS_HELLO_RESP_SIZE;
    ds_header_serialize(wire, &resp_header);
    ds_hello_resp_serialize(wire + DS_HEADER_SIZE, &resp);
    if (tcp_send_all(fd, wire, sizeof(wire)) != 0) {
        LOGE("hello: failed to send response");
        return false;
    }
    return resp.status == DS_HELLO_ACCEPTED;
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
        int is_loopback = 0;
        g_client_fd = tcp_server_accept(g_server_fd, &is_loopback);
        if (g_client_fd < 0) {
            if (g_running.load(std::memory_order_acquire)) {
                LOGW("recv_thread: accept failed, retrying in 500ms...");
                usleep(500000);
            }
            continue;
        }
        LOGI("recv_thread: client connected, fd=%d", g_client_fd);

        /* Peers that did not come through the adb tunnel must identify
         * themselves and be approved on the tablet before anything else.
         * A short receive timeout keeps a silent LAN peer from holding the
         * single connection slot forever (the approval wait itself happens
         * on our side, not in recv). */
        if (!is_loopback) {
            tcp_set_recv_timeout(g_client_fd, 5000);
            if (!run_wireless_hello(g_client_fd)) {
                tcp_close(g_client_fd);
                g_client_fd = -1;
                continue;
            }
            /* Approved: behave exactly like a USB peer from here on (the
             * desktop sets up its virtual display before the handshake). */
            tcp_set_recv_timeout(g_client_fd, 0);
        }

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

        /* The decode thread is parked here (configured == false), so the
         * decoder can be (re)created without any lock. */
        if (g_decoder) {
            decoder_destroy(g_decoder);
            g_decoder = nullptr;
        }
        g_decoder = decoder_create(g_window, selection.mime,
                                   selection.codec_name, &selection.hints);
        if (g_decoder) {
            decoder_set_wakeup(g_decoder, decoder_wakeup_cb, nullptr);
            decoder_set_output_callbacks(g_decoder, decoder_released_cb,
                                         decoder_frame_rendered_cb, nullptr);
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
                        decoder_set_output_callbacks(g_decoder, decoder_released_cb,
                                                     decoder_frame_rendered_cb,
                                                     nullptr);
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

        if (!g_decoder) {
            LOGE("recv_thread: failed to create/configure decoder");
            notify_status(STATUS_ERROR);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        g_active_codec_id.store(selection.codec_id, std::memory_order_release);
        g_h264_constraints_only_sps.store(
            selection.hints.has_android_low_latency ||
                selection.hints.is_qcom_c2 ||
                selection.hints.is_qcom_omx,
            std::memory_order_release);
        LOGI("recv_thread: accepted codec=%d", selection.codec_id);
        /* Floor for the backlog time budget: the stream's negotiated bitrate
         * (the measured rate collapses to almost nothing on a still screen). */
        g_bitrate_floor_Bps.store(
            req.max_bitrate_kbps > 0 ? (uint64_t)req.max_bitrate_kbps * 125u
                                     : 250000u,
            std::memory_order_relaxed);
        g_recv_rate_Bps.store(0, std::memory_order_relaxed);

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

        /* One message = one tcp_send_all() (see g_send_mutex in tcp_server.cpp). */
        uint8_t resp_wire[DS_HEADER_SIZE + DS_HANDSHAKE_RESP_SIZE];
        ds_header_t resp_header;
        resp_header.type   = DS_MSG_HANDSHAKE_RESP;
        resp_header.flags  = 0;
        resp_header.length = DS_HANDSHAKE_RESP_SIZE;
        ds_header_serialize(resp_wire, &resp_header);
        ds_handshake_resp_serialize(resp_wire + DS_HEADER_SIZE, &resp);

        if (tcp_send_all(g_client_fd, resp_wire, sizeof(resp_wire)) != 0) {
            LOGE("recv_thread: failed to send handshake response");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }
        LOGI("recv_thread: handshake complete");

        /* Only now let the decode thread run: its first act is a keyframe
         * request, and the desktop expects HANDSHAKE_RESP as the first message. */
        reset_recovery_for_session();
        outbound_set_session(g_client_fd);
        {
            std::lock_guard<std::mutex> lock(g_decode_mutex);
            g_decoder_configured.store(true, std::memory_order_release);
            g_decode_signal = true;
        }
        g_decode_cv.notify_one();

        /* Notify Java: connected and streaming */
        notify_status(STATUS_CONNECTED);

        /* Receive byte-rate over ~500 ms windows (backlog time budget). */
        int64_t rate_window_start_us = now_us();
        uint64_t rate_window_bytes = 0;

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
                    rate_window_bytes += DS_HEADER_SIZE + hdr.length;
                    if (recv_us - rate_window_start_us >= 500000) {
                        g_recv_rate_Bps.store(
                            rate_window_bytes * 1000000ull /
                                (uint64_t)(recv_us - rate_window_start_us),
                            std::memory_order_relaxed);
                        rate_window_start_us = recv_us;
                        rate_window_bytes = 0;
                    }

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
                    android_telemetry.rx_seq =
                        g_rx_seq.fetch_add(1, std::memory_order_acq_rel) + 1;
                    memcpy(video_msg_buf + 1, &android_telemetry,
                           sizeof(android_telemetry));
                    if (nal_len > 0) {
                        memcpy(video_msg_buf + INTERNAL_VIDEO_HEADER_SIZE,
                               nal_payload, nal_len);
                    }

                    size_t msg_len = nal_len + INTERNAL_VIDEO_HEADER_SIZE;
                    if (ring_buffer_write_message(g_ring_buf, video_msg_buf, msg_len) != 0) {
                        LOGW("recv_thread: ring buffer full, dropping frame");
                        enter_recovery(android_telemetry.rx_seq,
                                       (hdr.flags & DS_FLAG_CONFIG) != 0, "ring buffer full");
                    } else {
                        g_ring_frames.fetch_add(1, std::memory_order_acq_rel);
                        wake_decode_thread();
                    }
                    break;
                }

                case DS_MSG_PING: {
                    /* PONG: echoed desktop_send_us + our CLOCK_MONOTONIC now,
                     * for RTT and the desktop<->Android clock offset. Sent
                     * right here (not via the outbound queue) so queued input
                     * can't inflate the RTT. */
                    if (hdr.length < DS_PING_PAYLOAD_SIZE) {
                        LOGW("recv_thread: short PING (%u bytes)", hdr.length);
                        break;
                    }
                    uint8_t pong_buf[DS_HEADER_SIZE + DS_PONG_PAYLOAD_SIZE];
                    ds_header_t pong;
                    pong.type   = DS_MSG_PONG;
                    pong.flags  = 0;
                    pong.length = DS_PONG_PAYLOAD_SIZE;
                    ds_header_serialize(pong_buf, &pong);
                    memcpy(pong_buf + DS_HEADER_SIZE, payload_buf, 8);  /* already LE */
                    uint64_t android_now = (uint64_t)now_us();
                    for (int i = 0; i < 8; i++) {
                        pong_buf[DS_HEADER_SIZE + 8 + i] = (uint8_t)(android_now >> (8 * i));
                    }
                    tcp_send_all(g_client_fd, pong_buf, sizeof(pong_buf));
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

        /* No more input/backlog messages for this connection. */
        outbound_set_session(-1);

        /* Close client socket */
        if (g_client_fd >= 0) {
            tcp_close(g_client_fd);
            g_client_fd = -1;
        }

        /* Park the decode thread (it stops touching g_decoder), then destroy
         * the decoder; decoder_destroy joins the sync output thread before it
         * deletes the codec. No lock is held here that either thread needs. */
        park_decode_thread();
        if (g_decoder) {
            decoder_destroy(g_decoder);
            g_decoder = nullptr;
        }

        /* Nothing touches the ring or the telemetry now: reset for next session. */
        if (g_ring_buf) {
            ring_buffer_reset(g_ring_buf);
        }
        g_ring_frames.store(0, std::memory_order_relaxed);
        clear_pending_telemetry();
        {
            std::lock_guard<std::mutex> lock(g_lat_mutex);
            g_lat_window.comp_render_us.clear();
            g_lat_window.send_render_us.clear();
            g_lat_window.recv_render_us.clear();
        }

        /* Reset stats for next session */
        g_stats_bytes_received.store(0, std::memory_order_relaxed);
        g_stats_frames_rendered.store(0, std::memory_order_relaxed);
        g_stats_frames_fed.store(0, std::memory_order_relaxed);
        g_stats_feed_errors.store(0, std::memory_order_relaxed);
        g_stats_last_frame_arrival_us.store(0, std::memory_order_relaxed);
        g_stats_frame_jitter_us.store(0, std::memory_order_relaxed);
        g_stats_frames_skipped.store(0, std::memory_order_relaxed);
        g_stats_idle_frames.store(0, std::memory_order_relaxed);
        g_stats_render_callbacks.store(0, std::memory_order_relaxed);
        g_stats_desktop_capture_to_send_us.store(0, std::memory_order_relaxed);
        g_stats_video_rtt_us.store(0, std::memory_order_relaxed);
        g_stats_clock_uncertainty_us.store(0, std::memory_order_relaxed);
        g_stats_comp_render_p50_us.store(0, std::memory_order_relaxed);
        g_stats_send_render_p50_us.store(0, std::memory_order_relaxed);
        g_stats_recv_render_p50_us.store(0, std::memory_order_relaxed);
        g_recv_rate_Bps.store(0, std::memory_order_relaxed);
        g_next_pts_us.store(0, std::memory_order_relaxed);
        g_active_codec_id.store(DS_CODEC_H264, std::memory_order_relaxed);
        g_h264_constraints_only_sps.store(false, std::memory_order_relaxed);

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

/* ---- Pending frame telemetry (see PendingFrameTelemetry) ---- */

static void pending_free_locked(PendingFrameTelemetry* e) {
    if (e->state == PT_FED) {
        g_decoder_inflight.fetch_sub(1, std::memory_order_relaxed);
    }
    e->state = PT_FREE;
}

static void store_pending_telemetry(int64_t pts_us,
                                    const AndroidFrameTelemetry* meta) {
    if (!meta || meta->telemetry.sequence == 0) return;
    int64_t now = now_us();
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    PendingFrameTelemetry* slot = nullptr;
    PendingFrameTelemetry* oldest = nullptr;
    for (size_t i = 0; i < PENDING_TELEMETRY_CAP; i++) {
        PendingFrameTelemetry* e = &g_pending_telemetry[i];
        if (e->state == PT_FED && now - e->stamp_us > kPendingFedMaxAgeUs) {
            pending_free_locked(e);
            g_pending_expired++;
        } else if (e->state == PT_RELEASED && now - e->stamp_us > kPendingRenderWaitUs) {
            pending_free_locked(e);
            g_pending_expired++;
        }
        if (e->state == PT_FREE) {
            if (!slot) slot = e;
        } else if (!oldest || e->stamp_us < oldest->stamp_us) {
            oldest = e;
        }
    }
    if (!slot) {
        slot = oldest;
        pending_free_locked(slot);
        g_pending_expired++;
    }
    slot->state = PT_FED;
    slot->pts_us = pts_us;
    slot->stamp_us = now;
    slot->meta = *meta;
    g_decoder_inflight.fetch_add(1, std::memory_order_relaxed);
}

static PendingFrameTelemetry* find_pending_locked(int64_t pts_us) {
    for (size_t i = 0; i < PENDING_TELEMETRY_CAP; i++) {
        PendingFrameTelemetry* e = &g_pending_telemetry[i];
        if (e->state != PT_FREE && e->pts_us == pts_us) return e;
    }
    return nullptr;
}

/* Feed failed: the frame never reached the decoder. */
static void remove_pending_telemetry(int64_t pts_us) {
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    PendingFrameTelemetry* e = find_pending_locked(pts_us);
    if (e) pending_free_locked(e);
}

static void clear_pending_telemetry() {
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    for (size_t i = 0; i < PENDING_TELEMETRY_CAP; i++) {
        g_pending_telemetry[i].state = PT_FREE;
    }
    g_pending_expired = 0;
    g_decoder_inflight.store(0, std::memory_order_relaxed);
}

/* Successful feed of a non-config frame: latest desktop-side telemetry. */
static void record_fed_frame(const AndroidFrameTelemetry* meta) {
    if (!meta || meta->telemetry.sequence == 0) return;
    if (meta->telemetry.flags & DS_VIDEO_TELEMETRY_FLAG_IDLE) {
        g_stats_idle_frames.fetch_add(1, std::memory_order_relaxed);
    }
    g_stats_desktop_capture_to_send_us.store(meta->telemetry.capture_to_send_us,
                                             std::memory_order_relaxed);
    g_stats_video_rtt_us.store(meta->telemetry.rtt_us, std::memory_order_relaxed);
    g_stats_clock_uncertainty_us.store(meta->telemetry.clock_uncertainty_us,
                                       std::memory_order_relaxed);
}

/* Every output release (sync: output thread, async: decode thread). The
 * rendered-FPS counter lives here: render callbacks can be batched or missing. */
static void decoder_released_cb(void* /*userdata*/, int64_t pts_us, bool rendered) {
    if (rendered) {
        g_stats_frames_rendered.fetch_add(1, std::memory_order_relaxed);
    }
    std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
    PendingFrameTelemetry* e = find_pending_locked(pts_us);
    if (!e) return;
    if (!rendered) {
        pending_free_locked(e);          /* dropped by render-queue-depth=1 */
    } else if (e->state == PT_FED) {
        g_decoder_inflight.fetch_sub(1, std::memory_order_relaxed);
        e->state = PT_RELEASED;
        e->stamp_us = now_us();
    }
}

static int64_t clamp_sample(int64_t us) {
    if (us < INT32_MIN) return INT32_MIN;
    if (us > INT32_MAX) return INT32_MAX;
    return us;
}

/* onFrameRendered (NDK internal thread): the frame reached the display at
 * system_nano (CLOCK_MONOTONIC, same clock as now_us()). */
static void decoder_frame_rendered_cb(void* /*userdata*/, int64_t pts_us,
                                      int64_t system_nano) {
    g_stats_render_callbacks.fetch_add(1, std::memory_order_relaxed);

    AndroidFrameTelemetry meta;
    {
        std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
        PendingFrameTelemetry* e = find_pending_locked(pts_us);
        if (!e) return;
        meta = e->meta;
        pending_free_locked(e);
    }
    if (meta.telemetry.flags & DS_VIDEO_TELEMETRY_FLAG_IDLE) return;

    const int64_t render_us = system_nano / 1000;
    const ds_video_telemetry_t& t = meta.telemetry;
    const bool clock_ok = t.clock_uncertainty_us > 0;
    const int64_t recv_render = render_us - meta.recv_us;
    int64_t comp_render = -1;
    int64_t send_render = -1;
    if (clock_ok && t.display_time_us != 0) {
        comp_render = render_us - (t.display_time_us + t.clock_offset_us);
    }
    if (clock_ok && t.desktop_send_us != 0) {
        send_render = render_us - (t.desktop_send_us + t.clock_offset_us);
    }

    {
        std::lock_guard<std::mutex> lock(g_lat_mutex);
        g_lat_window.recv_render_us.push_back((int32_t)clamp_sample(recv_render));
        if (comp_render != -1) {
            g_lat_window.comp_render_us.push_back((int32_t)clamp_sample(comp_render));
        }
        if (send_render != -1) {
            g_lat_window.send_render_us.push_back((int32_t)clamp_sample(send_render));
        }
    }

    /* "Wake" latency: first real frame after the screen sat still. This is
     * what typing into an idle editor feels like. */
    static std::atomic<int64_t> last_real_render_us{0};
    int64_t last = last_real_render_us.exchange(render_us, std::memory_order_relaxed);
    if (last > 0 && render_us - last > 500000) {
        LOGI("wake: gap=%lldms comp_render=%.1fms send_render=%.1fms recv_render=%.1fms",
             (long long)((render_us - last) / 1000),
             comp_render != -1 ? comp_render / 1000.0 : -1.0,
             send_render != -1 ? send_render / 1000.0 : -1.0,
             recv_render / 1000.0);
    }
}

/* ---- Window percentiles (all samples of the window, nearest-rank) ---- */
typedef struct {
    size_t n;
    double p50_ms, p95_ms, p99_ms, max_ms;
} LatencySummary;

static LatencySummary summarize(std::vector<int32_t>& v) {
    LatencySummary s = {v.size(), 0.0, 0.0, 0.0, 0.0};
    if (v.empty()) return s;
    std::sort(v.begin(), v.end());
    auto rank = [&](double p) {
        size_t idx = (size_t)(p * (double)v.size());
        if (idx >= v.size()) idx = v.size() - 1;
        return v[idx] / 1000.0;
    };
    s.p50_ms = rank(0.50);
    s.p95_ms = rank(0.95);
    s.p99_ms = rank(0.99);
    s.max_ms = v.back() / 1000.0;
    return s;
}

/* ---- Receiver backlog: report to the desktop and enforce the time budget ---- */
static void backlog_tick() {
    static int64_t last_report_us = 0;
    static int over_budget_ticks = 0;
    int64_t now = now_us();
    if (now - last_report_us < kBacklogReportUs) return;
    last_report_us = now;

    uint32_t ring_bytes = (uint32_t)ring_buffer_available_read(g_ring_buf);
    uint32_t ring_frames = g_ring_frames.load(std::memory_order_acquire);
    uint32_t decoder_frames = g_decoder_inflight.load(std::memory_order_relaxed);

    uint8_t msg[DS_HEADER_SIZE + DS_BACKLOG_PAYLOAD_SIZE];
    ds_header_t hdr;
    hdr.type = DS_MSG_CONTROL;
    hdr.flags = 0;
    hdr.length = DS_BACKLOG_PAYLOAD_SIZE;
    ds_header_serialize(msg, &hdr);
    uint8_t* p = msg + DS_HEADER_SIZE;
    p[0] = DS_CTRL_RECEIVER_BACKLOG;
    const uint32_t vals[3] = {ring_bytes, ring_frames, decoder_frames};
    for (int v = 0; v < 3; v++) {
        for (int i = 0; i < 4; i++) {
            p[1 + v * 4 + i] = (uint8_t)(vals[v] >> (8 * i));
        }
    }
    outbound_enqueue(msg, sizeof(msg));

    /* Time budget: bytes waiting / receive rate (never below the negotiated
     * bitrate). Must hold for two consecutive reports (>= 100 ms) so a single
     * large IDR that is fed a moment later doesn't count. Over budget, skip to
     * the next keyframe through the normal loss-recovery path. */
    uint64_t rate = std::max(g_recv_rate_Bps.load(std::memory_order_relaxed),
                             g_bitrate_floor_Bps.load(std::memory_order_relaxed));
    int64_t backlog_ms = rate > 0 ? (int64_t)((uint64_t)ring_bytes * 1000u / rate) : 0;
    if (backlog_ms > kBacklogBudgetMs) {
        if (++over_budget_ticks >= 2 &&
                !g_recovering_hint.load(std::memory_order_acquire)) {
            LOGW("receiver backlog %lld ms (%u bytes, %u frames) over %lld ms budget",
                 (long long)backlog_ms, ring_bytes, ring_frames,
                 (long long)kBacklogBudgetMs);
            enter_recovery(g_rx_seq.load(std::memory_order_acquire), false,
                           "receiver backlog");
            over_budget_ticks = 0;
        }
    } else {
        over_budget_ticks = 0;
    }
}

/* Recv thread: stop the decode thread from touching g_decoder and wait until
 * it has. The decode thread never blocks for long (bounded MediaCodec waits),
 * and holds no lock while doing so. */
static void park_decode_thread() {
    std::unique_lock<std::mutex> lock(g_decode_mutex);
    g_decoder_configured.store(false, std::memory_order_release);
    g_decode_signal = true;
    g_decode_cv.notify_all();
    g_park_cv.wait(lock, [] { return g_decode_parked; });
}

static bool decode_active() {
    return g_running.load(std::memory_order_acquire) &&
           g_decoder_configured.load(std::memory_order_acquire);
}

/* Sync mode input wait: the output thread frees buffers concurrently, so wait
 * for one in slices (checking for session end) instead of giving up. */
static constexpr int64_t kInputWaitSliceUs = 5000;
static constexpr int64_t kInputWaitTotalUs = 40000;

/* ---- Decode thread ----
 * Reads NAL units from the ring buffer and feeds them to MediaCodec.
 * Survives reconnections — parks while the decoder is unconfigured.
 *
 *   ASYNC (non-Qualcomm): input indices come from MediaCodec callbacks
 *     (decoder_pop_input); outputs are drained here (decoder_drain).
 *   SYNC (Qualcomm): dequeueInputBuffer waits here; the decoder's own output
 *     thread is the single consumer of dequeue/releaseOutputBuffer.
 *
 * Both: when there is nothing to do, wait on g_decode_cv (ring writes, async
 * callbacks). Render-queue-depth=1 at the output.
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
        std::lock_guard<std::mutex> lock(g_decode_mutex);
        g_decode_parked = true;
        g_park_cv.notify_all();
        return nullptr;
    }

    int32_t held_async_input_idx = -1;
    int64_t last_log_us = now_us();
    uint64_t last_log_fed = 0;
    uint64_t last_log_rendered = 0;
    uint64_t last_log_cb = 0;

    while (g_running.load(std::memory_order_acquire)) {
        /* Park between sessions; the recv thread waits for g_decode_parked
         * before it destroys or recreates the decoder. */
        {
            std::unique_lock<std::mutex> lock(g_decode_mutex);
            if (!g_decoder_configured.load(std::memory_order_acquire)) {
                if (!g_decode_parked) {
                    g_decode_parked = true;
                    held_async_input_idx = -1;
                    g_park_cv.notify_all();
                }
                g_decode_cv.wait_for(lock, std::chrono::milliseconds(100), [] {
                    return g_decoder_configured.load(std::memory_order_acquire) ||
                           !g_running.load(std::memory_order_acquire);
                });
                if (g_decoder_configured.load(std::memory_order_acquire)) {
                    /* New session: fresh counters for the log window. */
                    last_log_us = now_us();
                    last_log_fed = 0;
                    last_log_rendered = 0;
                    last_log_cb = 0;
                }
                continue;
            }
            g_decode_parked = false;
        }

        DecoderContext* decoder = g_decoder;
        bool is_async = decoder_is_async(decoder);
        bool any_work = false;
        recovery_tick();
        backlog_tick();

        /* ---- 1. Drain output (async only; sync has its output thread) ---- */
        if (is_async && decoder_drain(decoder) > 0) {
            any_work = true;
        }

        /* ---- 2. Feed NALs from ring buffer ---- */
        while (decode_active()) {
            backlog_tick();  /* a long burst must not starve the 10 Hz report */
            int32_t input_idx = -1;
            if (is_async) {
                input_idx = held_async_input_idx;
                if (input_idx >= 0) {
                    held_async_input_idx = -1;
                } else {
                    input_idx = decoder_pop_input(decoder);
                }
                if (input_idx < 0) break;  /* no input buffer available */
            }

            size_t msg_len = ring_buffer_read_message(
                g_ring_buf, nal_buf, MAX_VIDEO_MSG_SIZE);
            if (msg_len < INTERNAL_VIDEO_HEADER_SIZE + 1) {
                /* Keep a dequeued async input buffer for the next real NAL.
                 * Queueing empty buffers adds avoidable MediaCodec work. */
                if (is_async) held_async_input_idx = input_idx;
                break;
            }
            g_ring_frames.fetch_sub(1, std::memory_order_acq_rel);
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
            const bool is_key = (mc_flags & MC_BUFFER_FLAG_KEY_FRAME) != 0;
            if (!is_config && !is_key &&
                    g_recovering_hint.load(std::memory_order_acquire)) {
                if (is_async) held_async_input_idx = input_idx;  /* reuse it */
                g_stats_frames_skipped.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            if (!is_async) {
                for (int64_t waited = 0;
                        input_idx < 0 && waited < kInputWaitTotalUs && decode_active();
                        waited += kInputWaitSliceUs) {
                    input_idx = decoder_dequeue_input(decoder, kInputWaitSliceUs);
                }
            }

            int64_t pts_us = reserve_frame_pts_us(is_config != 0);
            if (!is_config) {
                store_pending_telemetry(pts_us, &frame_meta);
            }
            int ret = input_idx >= 0
                ? decoder_feed_index(decoder, input_idx, video_data, nal_len,
                                     pts_us, mc_flags)
                : -1;
            if (ret == 0) {
                g_stats_frames_fed.fetch_add(1, std::memory_order_relaxed);
                on_packet_fed(frame_meta.rx_seq, is_config != 0, is_key);
                if (!is_config) {
                    record_fed_frame(&frame_meta);
                }
            } else {
                if (!is_config) {
                    remove_pending_telemetry(pts_us);
                }
                if (!decode_active()) break;  /* session ending, not a loss */
                /* Config loss too: later frames can't decode without it. */
                const char* why = is_config
                    ? "config feed failed"
                    : (input_idx < 0 ? "decoder input busy" : "feed failed");
                enter_recovery(frame_meta.rx_seq, is_config != 0, why);
                g_stats_feed_errors.fetch_add(1, std::memory_order_relaxed);
            }
        }

        /* ---- 3. Drain again after feeding (async) ---- */
        if (any_work && is_async) {
            decoder_drain(decoder);
        }

        /* ---- 4. Wait for the next event ---- */
        if (!any_work) {
            std::unique_lock<std::mutex> lock(g_decode_mutex);
            g_decode_cv.wait_for(
                lock, std::chrono::milliseconds(is_async ? 1 : 10), [] {
                    return g_decode_signal ||
                           !g_decoder_configured.load(std::memory_order_acquire) ||
                           !g_running.load(std::memory_order_acquire);
                });
            g_decode_signal = false;
        }

        /* ---- 5. Stats log (~2 s window, every frame of the window) ---- */
        int64_t log_us = now_us();
        uint64_t total_fed = g_stats_frames_fed.load(std::memory_order_relaxed);
        if (log_us - last_log_us >= 2000000 && total_fed != last_log_fed) {
            double elapsed = (log_us - last_log_us) / 1e6;
            uint64_t total_rendered =
                g_stats_frames_rendered.load(std::memory_order_relaxed);
            uint64_t total_cb = g_stats_render_callbacks.load(std::memory_order_relaxed);

            LatencyWindow w;
            {
                std::lock_guard<std::mutex> lock(g_lat_mutex);
                std::swap(w, g_lat_window);
                /* Keep the capacity: no reallocations in the render callback. */
                g_lat_window.comp_render_us.reserve(w.comp_render_us.capacity());
                g_lat_window.send_render_us.reserve(w.send_render_us.capacity());
                g_lat_window.recv_render_us.reserve(w.recv_render_us.capacity());
            }
            LatencySummary comp = summarize(w.comp_render_us);
            LatencySummary send = summarize(w.send_render_us);
            LatencySummary recv = summarize(w.recv_render_us);
            g_stats_comp_render_p50_us.store((int64_t)(comp.p50_ms * 1000), std::memory_order_relaxed);
            g_stats_send_render_p50_us.store((int64_t)(send.p50_ms * 1000), std::memory_order_relaxed);
            g_stats_recv_render_p50_us.store((int64_t)(recv.p50_ms * 1000), std::memory_order_relaxed);

            uint64_t expired;
            {
                std::lock_guard<std::mutex> lock(g_pending_telemetry_mutex);
                expired = g_pending_expired;
            }
            size_t ring_used = ring_buffer_available_read(g_ring_buf);
            uint64_t rate = std::max(g_recv_rate_Bps.load(std::memory_order_relaxed),
                                     g_bitrate_floor_Bps.load(std::memory_order_relaxed));

            LOGI("decode[%s]: fed=%llu rendered=%llu err=%llu | "
                 "%.1f fed/s %.1f render/s idle=%llu skip=%llu | "
                 "ring=%zu bytes %u frames %.0fms dec=%u | "
                 "comp_render n=%zu p50=%.1f p95=%.1f p99=%.1f max=%.1f | "
                 "send_render n=%zu p50=%.1f p95=%.1f p99=%.1f max=%.1f | "
                 "recv_render n=%zu p50=%.1f p95=%.1f p99=%.1f max=%.1f | "
                 "clock_unc=%.2fms rtt=%.1fms capsend=%.1fms | cb=%llu expired=%llu",
                 is_async ? "async" : "sync",
                 (unsigned long long)total_fed, (unsigned long long)total_rendered,
                 (unsigned long long)g_stats_feed_errors.load(std::memory_order_relaxed),
                 (total_fed - last_log_fed) / elapsed,
                 (total_rendered - last_log_rendered) / elapsed,
                 (unsigned long long)g_stats_idle_frames.load(std::memory_order_relaxed),
                 (unsigned long long)g_stats_frames_skipped.load(std::memory_order_relaxed),
                 ring_used, g_ring_frames.load(std::memory_order_relaxed),
                 rate > 0 ? ring_used * 1000.0 / rate : 0.0,
                 g_decoder_inflight.load(std::memory_order_relaxed),
                 comp.n, comp.p50_ms, comp.p95_ms, comp.p99_ms, comp.max_ms,
                 send.n, send.p50_ms, send.p95_ms, send.p99_ms, send.max_ms,
                 recv.n, recv.p50_ms, recv.p95_ms, recv.p99_ms, recv.max_ms,
                 g_stats_clock_uncertainty_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_video_rtt_us.load(std::memory_order_relaxed) / 1000.0,
                 g_stats_desktop_capture_to_send_us.load(std::memory_order_relaxed) / 1000.0,
                 (unsigned long long)(total_cb - last_log_cb),
                 (unsigned long long)expired);

            last_log_us = log_us;
            last_log_fed = total_fed;
            last_log_rendered = total_rendered;
            last_log_cb = total_cb;
        }
    }

    free(nal_buf);
    {
        /* Never leave the recv thread waiting in park_decode_thread. */
        std::lock_guard<std::mutex> lock(g_decode_mutex);
        g_decode_parked = true;
    }
    g_park_cv.notify_all();
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
    g_authorizeDesktop = env->GetMethodID(clazz, "authorizeDesktop",
        "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
    if (!g_authorizeDesktop) {
        LOGE("nativeInit: authorizeDesktop not found; Wi-Fi connections will be rejected");
        env->ExceptionClear();
    }
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
    outbound_start();
    pthread_create(&g_recv_thread, nullptr, recv_thread_func, nullptr);
    pthread_create(&g_decode_thread, nullptr, decode_thread_func, nullptr);

    LOGI("nativeInit: started successfully");
}

JNIEXPORT jint JNICALL
Java_com_droidscreen_app_MainActivity_nativeProtocolVersion(
        JNIEnv* /*env*/, jobject /*thiz*/) {
    return DS_PROTOCOL_VERSION;
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

    /* Join threads (wake the decode thread if it is waiting) */
    g_decode_cv.notify_all();
    pthread_join(g_recv_thread, nullptr);
    pthread_join(g_decode_thread, nullptr);
    outbound_stop();

    /* Cleanup decoder: both threads are gone, nobody else touches it. */
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

/* Input and Deck/volume: built and stamped here (UI thread), sent in order
 * by the outbound sender thread. These calls never block on the socket. */
JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendTouch(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint pointerId, jint xFrac, jint yFrac,
        jint pressure, jint touchMajor, jint touchMinor,
        jint orientation) {
    if (!outbound_connected()) return;
    uint8_t msg[DS_OUTBOUND_MAX_MSG];
    size_t len = touch_build(msg, action, pointerId, xFrac, yFrac, pressure,
                             touchMajor, touchMinor, orientation);
    outbound_enqueue(msg, len);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendPen(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint pointerId, jint toolType, jint buttons,
        jint xFrac, jint yFrac, jint pressure,
        jint distance, jint tilt, jint rotation) {
    if (!outbound_connected()) return;
    uint8_t msg[DS_OUTBOUND_MAX_MSG];
    size_t len = pen_build(msg, action, pointerId, toolType, buttons,
                           xFrac, yFrac, pressure, distance, tilt, rotation);
    outbound_enqueue(msg, len);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendMouse(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint buttons, jint xFrac, jint yFrac) {
    if (!outbound_connected()) return;
    uint8_t msg[DS_OUTBOUND_MAX_MSG];
    size_t len = mouse_build(msg, action, buttons, xFrac, yFrac);
    outbound_enqueue(msg, len);
}

/*
 * Stats for the UI (index -> meaning, must match MainActivity):
 *   0 bytes received        1 frames rendered (releases)   2 frames fed
 *   3 feed errors           4 arrival jitter us (smoothed)  5 frame interval us
 *   6 frames skipped        7 compositor->render p50 us     8 desktop send->render p50 us
 *   9 desktop capture->send us (last frame)                10 Android recv->render p50 us
 *  11 clock offset uncertainty us                          12 RTT us (last frame)
 *  13 idle frames
 * The p50 values cover the last ~2 s log window; 0 = no samples.
 */
JNIEXPORT jlongArray JNICALL
Java_com_droidscreen_app_MainActivity_nativeGetStats(
        JNIEnv* env, jobject /*thiz*/) {
    jlong stats[14];
    stats[0] = static_cast<jlong>(g_stats_bytes_received.load(std::memory_order_relaxed));
    stats[1] = static_cast<jlong>(g_stats_frames_rendered.load(std::memory_order_relaxed));
    stats[2] = static_cast<jlong>(g_stats_frames_fed.load(std::memory_order_relaxed));
    stats[3] = static_cast<jlong>(g_stats_feed_errors.load(std::memory_order_relaxed));
    stats[4] = static_cast<jlong>(g_stats_frame_jitter_us.load(std::memory_order_relaxed));
    stats[5] = static_cast<jlong>(g_stream_frame_interval_us.load(std::memory_order_relaxed));
    stats[6] = static_cast<jlong>(g_stats_frames_skipped.load(std::memory_order_relaxed));
    stats[7] = static_cast<jlong>(g_stats_comp_render_p50_us.load(std::memory_order_relaxed));
    stats[8] = static_cast<jlong>(g_stats_send_render_p50_us.load(std::memory_order_relaxed));
    stats[9] = static_cast<jlong>(g_stats_desktop_capture_to_send_us.load(std::memory_order_relaxed));
    stats[10] = static_cast<jlong>(g_stats_recv_render_p50_us.load(std::memory_order_relaxed));
    stats[11] = static_cast<jlong>(g_stats_clock_uncertainty_us.load(std::memory_order_relaxed));
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
    if (!outbound_connected()) return;
    uint8_t msg[DS_OUTBOUND_MAX_MSG];
    size_t len = deck_action_build(msg, actionType, slotIndex);
    outbound_enqueue(msg, len);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendVolumeChange(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint volume, jint muted) {
    if (!outbound_connected()) return;
    uint8_t msg[DS_OUTBOUND_MAX_MSG];
    size_t len = volume_change_build(msg, volume, muted);
    outbound_enqueue(msg, len);
}

/* Soft keyboard text. Kotlin passes UTF-8 bytes (not jstring: JNI's modified
 * UTF-8 would mangle emoji/supplementary characters). */
JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendKeyText(
        JNIEnv* env, jobject /*thiz*/, jbyteArray utf8) {
    if (!outbound_connected() || utf8 == nullptr) {
        return;
    }
    jsize len = env->GetArrayLength(utf8);
    if (len <= 0) {
        return;
    }
    jbyte* bytes = env->GetByteArrayElements(utf8, nullptr);
    if (bytes == nullptr) {
        return;
    }
    key_text_enqueue(reinterpret_cast<const char*>(bytes), (size_t)len);
    env->ReleaseByteArrayElements(utf8, bytes, JNI_ABORT);
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendKey(
        JNIEnv* /*env*/, jobject /*thiz*/, jint key, jint modifiers) {
    if (!outbound_connected()) return;
    key_special_enqueue(key, modifiers);
}

} /* extern "C" */
