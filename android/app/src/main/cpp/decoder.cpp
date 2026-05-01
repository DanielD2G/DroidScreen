/*
 * DroidScreen - H.264 hardware decoder implementation
 *
 * Uses the Android NDK AMediaCodec API to decode H.264 NAL units
 * and render directly to an ANativeWindow (SurfaceView).
 *
 * Two operation modes:
 *   - Sync (API 26+): polling with dequeueInputBuffer/dequeueOutputBuffer
 *   - Async (API 28+): AMediaCodec async callbacks, event-driven
 *
 * Vendor-specific low-latency flags sourced from Moonlight's
 * MediaCodecHelper.java — unrecognized keys are silently ignored.
 */

#include "decoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <android/api-level.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <mutex>

/* ---- Runtime binding for API 28+ async callback ----
 * We dlsym() the function so we can compile against minSdk 26
 * while using async callbacks on API 28+ devices at runtime. */
typedef media_status_t (*PFN_setAsyncNotifyCallback)(
    AMediaCodec*, AMediaCodecOnAsyncNotifyCallback, void*);

static PFN_setAsyncNotifyCallback g_pfn_setAsync = nullptr;
static bool g_async_resolved = false;

typedef int32_t (*PFN_setFrameRate)(ANativeWindow*, float, int8_t);
static PFN_setFrameRate g_pfn_setFrameRate = nullptr;
static bool g_frame_rate_resolved = false;

static PFN_setAsyncNotifyCallback resolve_async_callback() {
    if (!g_async_resolved) {
        g_async_resolved = true;
        if (android_get_device_api_level() >= 28) {
            g_pfn_setAsync = (PFN_setAsyncNotifyCallback)
                dlsym(RTLD_DEFAULT, "AMediaCodec_setAsyncNotifyCallback");
        }
    }
    return g_pfn_setAsync;
}

static PFN_setFrameRate resolve_set_frame_rate() {
    if (!g_frame_rate_resolved) {
        g_frame_rate_resolved = true;
        if (android_get_device_api_level() >= 30) {
            g_pfn_setFrameRate = (PFN_setFrameRate)
                dlsym(RTLD_DEFAULT, "ANativeWindow_setFrameRate");
        }
    }
    return g_pfn_setFrameRate;
}

#define TAG "DroidScreen"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Input buffer dequeue timeout (sync mode only).
 * A small wait dramatically reduces dropped NALs under load without
 * adding noticeable end-to-end latency. */
#define INPUT_TIMEOUT_US  1000
/* Output buffer dequeue timeout (0 = non-blocking poll, sync mode only) */
#define OUTPUT_TIMEOUT_US 0

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
    decoder_rendered_fn rendered_fn;
    void               *rendered_userdata;
    bool                render_in_callback;

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

static void render_frame(DecoderContext *ctx, size_t index);

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
    if (ctx->render_in_callback &&
            ctx->render_mode == RENDER_MODE_LOWEST_LATENCY) {
        render_frame(ctx, (size_t)index);
        if (ctx->rendered_fn) {
            ctx->rendered_fn(ctx->rendered_userdata,
                             bufferInfo->presentationTimeUs);
        }
        return;
    }
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
    PFN_setAsyncNotifyCallback pfn = ctx->wakeup_fn ? resolve_async_callback()
                                                     : nullptr;
    if (!pfn) return false;

    AMediaCodecOnAsyncNotifyCallback cb;
    cb.onAsyncInputAvailable  = on_async_input;
    cb.onAsyncOutputAvailable = on_async_output;
    cb.onAsyncFormatChanged   = on_async_format;
    cb.onAsyncError           = on_async_error;

    media_status_t s = pfn(ctx->codec, cb, ctx);
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
    ctx->rendered_fn = nullptr;
    ctx->render_in_callback = false;
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

void decoder_set_rendered_callback(DecoderContext *ctx,
                                   decoder_rendered_fn fn,
                                   void *userdata,
                                   bool render_in_callback) {
    if (ctx) {
        ctx->rendered_fn = fn;
        ctx->rendered_userdata = userdata;
        ctx->render_in_callback = render_in_callback;
        LOGI("decoder_set_rendered_callback: render_in_callback=%d",
             render_in_callback ? 1 : 0);
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
            PFN_setFrameRate set_frame_rate = resolve_set_frame_rate();
            if (set_frame_rate && ctx->window) {
                int32_t fr_status = set_frame_rate(ctx->window, (float)fps, 1);
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

        status = AMediaCodec_start(ctx->codec);
        if (status != AMEDIA_OK) {
            LOGW("decoder_configure: start failed attempt=%d status=%d",
                 attempt, (int)status);
            continue;
        }

        if (android_get_device_api_level() >= 30 && attempt <= 2) {
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

int decoder_feed(DecoderContext *ctx, const uint8_t *nal_data, size_t nal_len,
                 int64_t timestamp_us, uint32_t flags) {
    if (!ctx || !ctx->configured) {
        return -1;
    }

    int32_t idx = decoder_dequeue_input(ctx, INPUT_TIMEOUT_US);
    if (idx < 0) {
        return -1;
    }

    return decoder_feed_index(ctx, idx, nal_data, nal_len, timestamp_us, flags);
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

/* ---- Drain: render queue depth = 1 (Moonlight strategy) ---- */

static void render_frame(DecoderContext *ctx, size_t index) {
    if (ctx->render_mode == RENDER_MODE_LOWEST_LATENCY) {
        AMediaCodec_releaseOutputBuffer(ctx->codec, index, true);
    } else {
        /* RENDER_MODE_SMOOTH: present at next VSync via releaseOutputBuffer. */
        AMediaCodec_releaseOutputBuffer(ctx->codec, index, true);
    }
}

int decoder_drain(DecoderContext *ctx, int64_t *rendered_pts_us) {
    if (!ctx || !ctx->configured) {
        return 0;
    }
    if (rendered_pts_us) {
        *rendered_pts_us = -1;
    }

    if (ctx->async_mode) {
        /* ---- Async path: drain from the output queue ---- */
        OutputEntry entry;
        int32_t last_idx = -1;

        while (output_queue_pop(ctx, &entry)) {
            if (last_idx >= 0) {
                AMediaCodec_releaseOutputBuffer(ctx->codec, (size_t)last_idx, false);
            }
            last_idx = entry.index;
            if (rendered_pts_us) {
                *rendered_pts_us = entry.info.presentationTimeUs;
            }
        }

        if (last_idx >= 0) {
            render_frame(ctx, (size_t)last_idx);
            return 1;
        }
        return 0;
    }

    /* ---- Sync path: poll with dequeueOutputBuffer ---- */
    AMediaCodecBufferInfo info;
    ssize_t last_idx = -1;

    for (;;) {
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(
            ctx->codec, &info, OUTPUT_TIMEOUT_US);

        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER)       break;
        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(ctx->codec);
            if (fmt) {
                LOGI("decoder_drain: output format changed: %s",
                     AMediaFormat_toString(fmt));
                AMediaFormat_delete(fmt);
            }
            continue;
        }
        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) continue;
        if (idx < 0) break;

        if (last_idx >= 0) {
            AMediaCodec_releaseOutputBuffer(ctx->codec, (size_t)last_idx, false);
        }
        last_idx = idx;
    }

    if (last_idx >= 0) {
        if (rendered_pts_us) {
            *rendered_pts_us = info.presentationTimeUs;
        }
        render_frame(ctx, (size_t)last_idx);
        return 1;
    }

    return 0;
}

void decoder_destroy(DecoderContext *ctx) {
    if (!ctx) {
        return;
    }

    if (ctx->codec) {
        if (ctx->configured) {
            AMediaCodec_stop(ctx->codec);
        }
        AMediaCodec_delete(ctx->codec);
    }

    delete ctx;
    LOGI("decoder_destroy: cleaned up");
}
