/*
 * DroidScreen - H.264 hardware decoder implementation
 *
 * Uses the Android NDK AMediaCodec API to decode H.264 NAL units
 * and render directly to an ANativeWindow (SurfaceView).
 *
 * Two operation modes (never mixed on one codec):
 *   - Sync (Qualcomm): the feed thread dequeues input buffers; a dedicated
 *     output thread (owned here) is the single consumer of
 *     dequeueOutputBuffer/releaseOutputBuffer.
 *   - Async (others): AMediaCodec async callbacks, event-driven
 *
 * Both modes report real render times via AMediaCodec_setOnFrameRenderedCallback
 * (API 33), which works with sync dequeue too.
 *
 * Vendor-specific low-latency flags sourced from Moonlight's
 * MediaCodecHelper.java — unrecognized keys are silently ignored.
 */

#include "decoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <android/api-level.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>
#include <atomic>
#include <mutex>

#define TAG "DroidScreen"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Sync output thread: how long one dequeueOutputBuffer may block. It is also
 * the bound on how long decoder_destroy waits for the thread to notice the
 * stop flag (MediaCodec has no way to interrupt a blocked dequeue). */
#define OUTPUT_WAIT_US 10000

/* Max entries in the lock-free input/output ring queues (must be power of 2) */
#define ASYNC_QUEUE_CAP  32
#define ASYNC_QUEUE_MASK (ASYNC_QUEUE_CAP - 1)

/* ---- Async output entry ---- */
typedef struct {
    int32_t              index;
    AMediaCodecBufferInfo info;
} OutputEntry;

/* ---- Decoder context ---- */
struct DecoderContext {
    AMediaCodec  *codec;
    ANativeWindow *window;
    char          mime[32];
    char          codec_name[128];
    DecoderVendorHints hints;
    bool          configured;
    bool          async_mode;
    DecoderRenderMode render_mode;

    /* Wakeup callback — invoked from MediaCodec's internal thread */
    decoder_wakeup_fn wakeup_fn;
    void             *wakeup_userdata;

    /* Output observers */
    decoder_released_fn       released_fn;
    decoder_frame_rendered_fn frame_rendered_fn;
    void                     *output_userdata;

    /* Sync mode output thread */
    pthread_t         output_thread;
    bool              output_thread_started;
    std::atomic<bool> output_running;

    /* Async mode: lock-free SPSC queues (producer = codec thread,
     * consumer = decode thread).  Atomic indices, fixed-size arrays. */
    volatile int32_t input_queue[ASYNC_QUEUE_CAP];
    volatile int     input_wr;
    volatile int     input_rd;
    std::mutex        input_mutex;

    OutputEntry      output_queue[ASYNC_QUEUE_CAP];
    volatile int     output_wr;
    volatile int     output_rd;
    std::mutex        output_mutex;
};

static void release_output(DecoderContext *ctx, size_t index,
                           int64_t pts_us, bool render);

/* ---- Async queue helpers (single-producer, single-consumer) ---- */

static inline void input_queue_push(DecoderContext *ctx, int32_t index) {
    std::lock_guard<std::mutex> lock(ctx->input_mutex);
    int wr = ctx->input_wr;
    ctx->input_queue[wr & ASYNC_QUEUE_MASK] = index;
    __atomic_store_n(&ctx->input_wr, wr + 1, __ATOMIC_RELEASE);
}

static inline int32_t input_queue_pop(DecoderContext *ctx) {
    std::lock_guard<std::mutex> lock(ctx->input_mutex);
    int rd = ctx->input_rd;
    int wr = __atomic_load_n(&ctx->input_wr, __ATOMIC_ACQUIRE);
    if (rd == wr) return -1;
    int32_t val = ctx->input_queue[rd & ASYNC_QUEUE_MASK];
    __atomic_store_n(&ctx->input_rd, rd + 1, __ATOMIC_RELEASE);
    return val;
}

static inline void output_queue_push(DecoderContext *ctx, int32_t index,
                                     const AMediaCodecBufferInfo *info) {
    std::lock_guard<std::mutex> lock(ctx->output_mutex);
    int wr = ctx->output_wr;
    ctx->output_queue[wr & ASYNC_QUEUE_MASK].index = index;
    ctx->output_queue[wr & ASYNC_QUEUE_MASK].info  = *info;
    __atomic_store_n(&ctx->output_wr, wr + 1, __ATOMIC_RELEASE);
}

static inline bool output_queue_pop(DecoderContext *ctx, OutputEntry *out) {
    std::lock_guard<std::mutex> lock(ctx->output_mutex);
    int rd = ctx->output_rd;
    int wr = __atomic_load_n(&ctx->output_wr, __ATOMIC_ACQUIRE);
    if (rd == wr) return false;
    *out = ctx->output_queue[rd & ASYNC_QUEUE_MASK];
    __atomic_store_n(&ctx->output_rd, rd + 1, __ATOMIC_RELEASE);
    return true;
}

/* ---- Async callbacks (fired on MediaCodec's internal thread) ---- */

static void on_async_input(AMediaCodec *codec, void *userdata, int32_t index) {
    (void)codec;
    DecoderContext *ctx = static_cast<DecoderContext*>(userdata);
    input_queue_push(ctx, index);
    if (ctx->wakeup_fn) ctx->wakeup_fn(ctx->wakeup_userdata);
}

static void on_async_output(AMediaCodec *codec, void *userdata,
                            int32_t index, AMediaCodecBufferInfo *bufferInfo) {
    (void)codec;
    DecoderContext *ctx = static_cast<DecoderContext*>(userdata);
    output_queue_push(ctx, index, bufferInfo);
    if (ctx->wakeup_fn) ctx->wakeup_fn(ctx->wakeup_userdata);
}

static void on_async_format(AMediaCodec *codec, void *userdata,
                            AMediaFormat *format) {
    (void)codec; (void)userdata;
    LOGI("decoder_async: output format changed: %s",
         AMediaFormat_toString(format));
}

static void on_async_error(AMediaCodec *codec, void *userdata,
                           media_status_t error, int32_t actionCode,
                           const char *detail) {
    (void)codec; (void)userdata; (void)actionCode;
    LOGE("decoder_async: error %d: %s", (int)error, detail ? detail : "unknown");
}

static void on_frame_rendered(AMediaCodec *codec, void *userdata,
                              int64_t media_time_us, int64_t system_nano) {
    (void)codec;
    DecoderContext *ctx = static_cast<DecoderContext*>(userdata);
    if (ctx->frame_rendered_fn) {
        ctx->frame_rendered_fn(ctx->output_userdata, media_time_us, system_nano);
    }
}

/* ---- Sync output thread ----
 * The only caller of dequeueOutputBuffer/releaseOutputBuffer in sync mode.
 * Blocks in dequeueOutputBuffer (no locks held), then takes every other
 * output that is already ready and keeps render-queue-depth = 1: older
 * outputs are released without rendering, the newest one is rendered. */
static void log_output_format(DecoderContext *ctx) {
    AMediaFormat *fmt = AMediaCodec_getOutputFormat(ctx->codec);
    if (fmt) {
        LOGI("decoder_output: output format changed: %s", AMediaFormat_toString(fmt));
        AMediaFormat_delete(fmt);
    }
}

static void* output_thread_func(void *arg) {
    DecoderContext *ctx = static_cast<DecoderContext*>(arg);
    setpriority(PRIO_PROCESS, 0, -8);
    LOGI("decoder_output: thread started");

    while (ctx->output_running.load(std::memory_order_acquire)) {
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info,
                                                      OUTPUT_WAIT_US);
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            log_output_format(ctx);
            continue;
        }
        if (idx < 0) continue;  /* TRY_AGAIN_LATER / BUFFERS_CHANGED */

        size_t last_idx = (size_t)idx;
        int64_t last_pts = info.presentationTimeUs;
        for (;;) {
            ssize_t next = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info, 0);
            if (next == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
                log_output_format(ctx);
                continue;
            }
            if (next == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
            if (next < 0) break;
            release_output(ctx, last_idx, last_pts, false);
            last_idx = (size_t)next;
            last_pts = info.presentationTimeUs;
        }
        release_output(ctx, last_idx, last_pts, true);
    }

    LOGI("decoder_output: thread exiting");
    return nullptr;
}

static void start_output_thread(DecoderContext *ctx) {
    ctx->output_running.store(true, std::memory_order_release);
    if (pthread_create(&ctx->output_thread, nullptr, output_thread_func, ctx) == 0) {
        ctx->output_thread_started = true;
    } else {
        ctx->output_running.store(false, std::memory_order_release);
        LOGE("decoder_configure: failed to start output thread");
    }
}

/* Stop flag, then join: the thread wakes from its bounded dequeue
 * (OUTPUT_WAIT_US) and exits. It takes no lock, so callers may hold theirs. */
static void stop_output_thread(DecoderContext *ctx) {
    if (!ctx->output_thread_started) return;
    ctx->output_running.store(false, std::memory_order_release);
    pthread_join(ctx->output_thread, nullptr);
    ctx->output_thread_started = false;
}

/* ---- Public API ---- */

static AMediaCodec* create_codec_instance(const char *mime,
                                          const char *codec_name) {
    AMediaCodec *codec = nullptr;
    if (codec_name && codec_name[0] != '\0') {
        codec = AMediaCodec_createCodecByName(codec_name);
        if (codec) {
            LOGI("decoder_create: using codec by name: %s", codec_name);
            return codec;
        }
        LOGW("decoder_create: createCodecByName(%s) failed, falling back to mime",
             codec_name);
    }

    codec = AMediaCodec_createDecoderByType(mime);
    if (codec) {
        LOGI("decoder_create: using decoder by mime: %s", mime);
    }
    return codec;
}

static bool recreate_codec(DecoderContext *ctx) {
    if (!ctx) return false;
    if (ctx->codec) {
        AMediaCodec_delete(ctx->codec);
        ctx->codec = nullptr;
    }
    ctx->codec = create_codec_instance(ctx->mime, ctx->codec_name);
    return ctx->codec != nullptr;
}

static void reset_async_queues(DecoderContext *ctx) {
    std::lock_guard<std::mutex> input_lock(ctx->input_mutex);
    std::lock_guard<std::mutex> output_lock(ctx->output_mutex);
    ctx->input_wr  = 0;
    ctx->input_rd  = 0;
    ctx->output_wr = 0;
    ctx->output_rd = 0;
}

static bool enable_async_callbacks(DecoderContext *ctx) {
    ctx->async_mode = false;
    if (ctx->hints.is_qcom_c2 || ctx->hints.is_qcom_omx) {
        LOGI("decoder_configure: using sync polling for Qualcomm decoder");
        return false;
    }
    if (!ctx->wakeup_fn) return false;

    AMediaCodecOnAsyncNotifyCallback cb;
    cb.onAsyncInputAvailable  = on_async_input;
    cb.onAsyncOutputAvailable = on_async_output;
    cb.onAsyncFormatChanged   = on_async_format;
    cb.onAsyncError           = on_async_error;

    media_status_t s = AMediaCodec_setAsyncNotifyCallback(ctx->codec, cb, ctx);
    if (s == AMEDIA_OK) {
        ctx->async_mode = true;
        LOGI("decoder_configure: async callbacks enabled (API %d)",
             android_get_device_api_level());
        return true;
    }

    LOGW("decoder_configure: async callbacks failed (%d), using sync", (int)s);
    return false;
}

DecoderContext* decoder_create(ANativeWindow *window,
                               const char *mime,
                               const char *codec_name,
                               const DecoderVendorHints *hints) {
    if (!mime || mime[0] == '\0') {
        mime = "video/avc";
    }

    auto *ctx = new DecoderContext();
    ctx->window      = window;
    ctx->configured  = false;
    ctx->async_mode  = false;
    ctx->render_mode = RENDER_MODE_LOWEST_LATENCY;
    ctx->wakeup_fn   = nullptr;
    ctx->released_fn = nullptr;
    ctx->frame_rendered_fn = nullptr;
    ctx->output_userdata = nullptr;
    ctx->output_thread_started = false;
    ctx->output_running.store(false, std::memory_order_relaxed);
    ctx->hints       = hints ? *hints : DecoderVendorHints{};
    snprintf(ctx->mime, sizeof(ctx->mime), "%s", mime);
    snprintf(ctx->codec_name, sizeof(ctx->codec_name), "%s",
             codec_name ? codec_name : "");

    ctx->codec = create_codec_instance(ctx->mime, ctx->codec_name);
    if (!ctx->codec) {
        LOGE("decoder_create: failed to create AMediaCodec for %s (%s)",
             ctx->mime, ctx->codec_name);
        delete ctx;
        return nullptr;
    }

    LOGI("decoder_create: codec ready mime=%s name=%s direct=%d ll=%d qcomC2=%d qcomOmx=%d",
         ctx->mime, ctx->codec_name, ctx->hints.direct_submit ? 1 : 0,
         ctx->hints.has_android_low_latency ? 1 : 0,
         ctx->hints.is_qcom_c2 ? 1 : 0, ctx->hints.is_qcom_omx ? 1 : 0);
    return ctx;
}

void decoder_set_wakeup(DecoderContext *ctx, decoder_wakeup_fn fn, void *userdata) {
    if (ctx) {
        ctx->wakeup_fn       = fn;
        ctx->wakeup_userdata = userdata;
    }
}

void decoder_set_output_callbacks(DecoderContext *ctx,
                                  decoder_released_fn released_fn,
                                  decoder_frame_rendered_fn frame_rendered_fn,
                                  void *userdata) {
    if (ctx) {
        ctx->released_fn = released_fn;
        ctx->frame_rendered_fn = frame_rendered_fn;
        ctx->output_userdata = userdata;
    }
}

void decoder_set_render_mode(DecoderContext *ctx, DecoderRenderMode mode) {
    if (ctx) {
        ctx->render_mode = mode;
        LOGI("decoder_set_render_mode: %s",
             mode == RENDER_MODE_LOWEST_LATENCY ? "LOWEST_LATENCY" : "SMOOTH");
    }
}

int decoder_configure(DecoderContext *ctx, int width, int height, int fps) {
    if (!ctx || !ctx->codec) {
        return -1;
    }

    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt > 0 && !recreate_codec(ctx)) {
            LOGE("decoder_configure: failed to recreate codec for retry %d",
                 attempt);
            return -1;
        }

        reset_async_queues(ctx);
        enable_async_callbacks(ctx);

        AMediaFormat *format = AMediaFormat_new();
        if (!format) {
            LOGE("decoder_configure: failed to create AMediaFormat");
            return -1;
        }

        AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, ctx->mime);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
        AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);
        if (fps > 0) {
            AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, fps);
            if (ctx->window) {
                int32_t fr_status = ANativeWindow_setFrameRate(
                    ctx->window, (float)fps,
                    ANATIVEWINDOW_FRAME_RATE_COMPATIBILITY_FIXED_SOURCE);
                LOGI("decoder_configure: surface frame rate hint %d -> %d",
                     fps, (int)fr_status);
            }
        }

        if (attempt <= 2) {
            AMediaFormat_setInt32(format, "low-latency", 1);
            AMediaFormat_setInt32(format, "priority", 0);
            AMediaFormat_setInt32(format, "operating-rate", 32767);
        }

        if (attempt == 0) {
            AMediaFormat_setInt32(format, "vdec-lowlatency", 1);
            if (!(ctx->hints.is_qcom_c2 || ctx->hints.is_qcom_omx)) {
                AMediaFormat_setInt32(format,
                                      "vendor.rtc-ext-dec-low-latency.enable", 1);
                AMediaFormat_setInt32(format, "vendor.low-latency.enable", 1);
            }
        }

        if (attempt <= 1 && (ctx->hints.is_qcom_c2 || ctx->hints.is_qcom_omx)) {
            int picture_order = ctx->hints.is_qcom_omx ? 0 : 1;
            AMediaFormat_setInt32(format,
                                  "vendor.qti-ext-dec-picture-order.enable",
                                  picture_order);
            AMediaFormat_setInt32(format,
                                  "vendor.qti-ext-dec-low-latency.enable", 1);
            LOGI("decoder_configure: qcom low-latency picture-order=%d attempt=%d",
                 picture_order, attempt);
        } else if (attempt == 2 &&
                   (ctx->hints.is_qcom_c2 || ctx->hints.is_qcom_omx)) {
            AMediaFormat_setInt32(format,
                                  "vendor.qti-ext-dec-low-latency.enable", 1);
        }

        media_status_t status = AMediaCodec_configure(
            ctx->codec, format, ctx->window, nullptr /* crypto */, 0 /* flags */);

        AMediaFormat_delete(format);

        if (status != AMEDIA_OK) {
            LOGW("decoder_configure: configure failed attempt=%d status=%d",
                 attempt, (int)status);
            continue;
        }

        /* Real render timestamps (API 33). Purely informational: a codec that
         * refuses it still decodes, it just reports no render samples. */
        media_status_t fr = AMediaCodec_setOnFrameRenderedCallback(
            ctx->codec, on_frame_rendered, ctx);
        if (fr != AMEDIA_OK) {
            LOGW("decoder_configure: setOnFrameRenderedCallback failed (%d)", (int)fr);
        }

        status = AMediaCodec_start(ctx->codec);
        if (status != AMEDIA_OK) {
            LOGW("decoder_configure: start failed attempt=%d status=%d",
                 attempt, (int)status);
            continue;
        }

        if (attempt <= 2) {
            AMediaFormat *params = AMediaFormat_new();
            if (params) {
                AMediaFormat_setInt32(params, "low-latency", 1);
                AMediaFormat_setInt32(params, "priority", 0);
                AMediaFormat_setInt32(params, "operating-rate", 32767);
                if (ctx->hints.is_qcom_c2 || ctx->hints.is_qcom_omx) {
                    AMediaFormat_setInt32(
                        params, "vendor.qti-ext-dec-low-latency.enable", 1);
                }
                media_status_t ps = AMediaCodec_setParameters(ctx->codec, params);
                LOGI("decoder_configure: runtime low-latency params attempt=%d -> %d",
                     attempt, (int)ps);
                AMediaFormat_delete(params);
            }
        }

        ctx->configured = true;
        if (!ctx->async_mode) {
            start_output_thread(ctx);
        }
        LOGI("decoder_configure: configured %dx%d@%d mime=%s name=%s attempt=%d mode=%s",
             width, height, fps, ctx->mime, ctx->codec_name, attempt,
             ctx->async_mode ? "ASYNC" : "SYNC");
        return 0;
    }

    LOGE("decoder_configure: all configuration attempts failed");
    return -1;
}

bool decoder_is_async(DecoderContext *ctx) {
    return ctx && ctx->async_mode;
}

int32_t decoder_dequeue_input(DecoderContext *ctx, int64_t timeout_us) {
    if (!ctx || !ctx->configured) {
        return -1;
    }

    ssize_t idx = AMediaCodec_dequeueInputBuffer(ctx->codec, timeout_us);
    if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        return -1;
    }
    if (idx < 0) {
        LOGW("decoder_dequeue_input: no input buffer available (idx=%zd)", idx);
        return -1;
    }
    return (int32_t)idx;
}

int decoder_feed_index(DecoderContext *ctx, int32_t index,
                       const uint8_t *nal_data, size_t nal_len,
                       int64_t timestamp_us, uint32_t flags) {
    if (!ctx || !ctx->configured || index < 0) {
        return -1;
    }

    size_t buf_size = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(ctx->codec, (size_t)index, &buf_size);
    if (!buf || buf_size < nal_len) {
        LOGE("decoder_feed_index: input buffer too small (%zu < %zu)",
             buf_size, nal_len);
        AMediaCodec_queueInputBuffer(ctx->codec, (size_t)index, 0, 0, 0, 0);
        return -1;
    }

    memcpy(buf, nal_data, nal_len);

    media_status_t status = AMediaCodec_queueInputBuffer(
        ctx->codec, (size_t)index, 0, nal_len, timestamp_us, flags);

    if (status != AMEDIA_OK) {
        LOGE("decoder_feed_index: queueInputBuffer failed: %d", (int)status);
        return -1;
    }

    return 0;
}

int32_t decoder_pop_input(DecoderContext *ctx) {
    if (!ctx) return -1;
    return input_queue_pop(ctx);
}

/* ---- Output release: render queue depth = 1 (Moonlight strategy) ---- */

static void release_output(DecoderContext *ctx, size_t index,
                           int64_t pts_us, bool render) {
    /* Rendered frames are released with an explicit timestamp of "now"
     * (present as soon as possible, same as render=true with our synthetic,
     * always-past PTS). It also matters for measurement: with plain
     * releaseOutputBuffer(true) Codec2 (POCO Pad, c2.qti) reports
     * onFrameRendered systemNano = presentationTimeUs * 1000, i.e. the
     * requested time, not the real one. With an explicit CLOCK_MONOTONIC
     * timestamp it reports the actual presentation (VSync-aligned).
     * RENDER_MODE_SMOOTH would present at the next VSync; both modes render
     * immediately today. */
    if (render) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        AMediaCodec_releaseOutputBufferAtTime(ctx->codec, index,
            (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec);
    } else {
        AMediaCodec_releaseOutputBuffer(ctx->codec, index, false);
    }
    if (ctx->released_fn) {
        ctx->released_fn(ctx->output_userdata, pts_us, render);
    }
}

int decoder_drain(DecoderContext *ctx) {
    if (!ctx || !ctx->configured || !ctx->async_mode) {
        return 0;
    }

    OutputEntry entry;
    bool have_last = false;
    OutputEntry last;

    while (output_queue_pop(ctx, &entry)) {
        if (have_last) {
            release_output(ctx, (size_t)last.index,
                           last.info.presentationTimeUs, false);
        }
        last = entry;
        have_last = true;
    }

    if (have_last) {
        release_output(ctx, (size_t)last.index, last.info.presentationTimeUs, true);
        return 1;
    }
    return 0;
}

void decoder_destroy(DecoderContext *ctx) {
    if (!ctx) {
        return;
    }

    /* The output thread uses ctx->codec: it must be gone before stop/delete. */
    stop_output_thread(ctx);

    if (ctx->codec) {
        if (ctx->configured) {
            AMediaCodec_stop(ctx->codec);
        }
        AMediaCodec_delete(ctx->codec);
    }

    delete ctx;
    LOGI("decoder_destroy: cleaned up");
}
