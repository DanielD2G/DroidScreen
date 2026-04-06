/*
 * DroidScreen - HEVC hardware decoder implementation
 *
 * Uses the Android NDK AMediaCodec API to decode HEVC NAL units
 * and render directly to an ANativeWindow (SurfaceView).
 *
 * Vendor-specific low-latency flags sourced from Moonlight's
 * MediaCodecHelper.java — unrecognized keys are silently ignored.
 */

#include "decoder.h"

#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <android/log.h>
#include <stdlib.h>
#include <string.h>

#define TAG "DroidScreen"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Input buffer dequeue timeout: 0 = non-blocking.
 * If no buffer is available the caller retries on the next iteration,
 * avoiding any stall on the decode thread. */
#define INPUT_TIMEOUT_US  0
/* Output buffer dequeue timeout (0 = non-blocking poll) */
#define OUTPUT_TIMEOUT_US 0

struct DecoderContext {
    AMediaCodec  *codec;
    ANativeWindow *window;
    bool          configured;
};

DecoderContext* decoder_create(ANativeWindow *window) {
    AMediaCodec *codec = AMediaCodec_createDecoderByType("video/avc");
    if (!codec) {
        LOGE("decoder_create: failed to create AMediaCodec for video/avc");
        return nullptr;
    }

    auto *ctx = static_cast<DecoderContext*>(calloc(1, sizeof(DecoderContext)));
    if (!ctx) {
        AMediaCodec_delete(codec);
        return nullptr;
    }

    ctx->codec      = codec;
    ctx->window     = window;
    ctx->configured = false;

    LOGI("decoder_create: codec created successfully");
    return ctx;
}

int decoder_configure(DecoderContext *ctx, int width, int height) {
    if (!ctx || !ctx->codec) {
        return -1;
    }

    AMediaFormat *format = AMediaFormat_new();
    if (!format) {
        LOGE("decoder_configure: failed to create AMediaFormat");
        return -1;
    }

    AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, "video/avc");
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, width);
    AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, height);

    /* Standard Android 11+ low-latency keys */
    AMediaFormat_setInt32(format, "low-latency", 1);
    AMediaFormat_setInt32(format, "priority", 0); /* real-time priority (API 28+) */

    /* Qualcomm vendor extensions */
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(format, "vendor.qti-ext-dec-picture-order.enable", 0);

    /* Samsung Exynos */
    AMediaFormat_setInt32(format, "vendor.rtc-ext-dec-low-latency.enable", 1);

    /* MediaTek */
    AMediaFormat_setInt32(format, "vdec-lowlatency", 1);

    /* Amlogic (Fire TV etc.) */
    AMediaFormat_setInt32(format, "vendor.low-latency.enable", 1);

    /* Force maximum decode speed (Moonlight uses this) */
    AMediaFormat_setInt32(format, "operating-rate", 32767); /* Short.MAX_VALUE */

    media_status_t status = AMediaCodec_configure(
        ctx->codec, format, ctx->window, nullptr /* crypto */, 0 /* flags */);

    AMediaFormat_delete(format);

    if (status != AMEDIA_OK) {
        LOGE("decoder_configure: AMediaCodec_configure failed: %d", (int)status);
        return -1;
    }

    status = AMediaCodec_start(ctx->codec);
    if (status != AMEDIA_OK) {
        LOGE("decoder_configure: AMediaCodec_start failed: %d", (int)status);
        return -1;
    }

    ctx->configured = true;
    LOGI("decoder_configure: configured %dx%d", width, height);
    return 0;
}

int decoder_feed(DecoderContext *ctx, const uint8_t *nal_data, size_t nal_len,
                 int64_t timestamp_us, uint32_t flags) {
    if (!ctx || !ctx->configured) {
        return -1;
    }

    ssize_t idx = AMediaCodec_dequeueInputBuffer(ctx->codec, INPUT_TIMEOUT_US);
    if (idx < 0) {
        LOGW("decoder_feed: no input buffer available (idx=%zd)", idx);
        return -1;
    }

    size_t buf_size = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(ctx->codec, (size_t)idx, &buf_size);
    if (!buf || buf_size < nal_len) {
        LOGE("decoder_feed: input buffer too small (%zu < %zu)", buf_size, nal_len);
        AMediaCodec_queueInputBuffer(ctx->codec, (size_t)idx, 0, 0, 0, 0);
        return -1;
    }

    memcpy(buf, nal_data, nal_len);

    media_status_t status = AMediaCodec_queueInputBuffer(
        ctx->codec, (size_t)idx, 0, nal_len, timestamp_us, flags);

    if (status != AMEDIA_OK) {
        LOGE("decoder_feed: queueInputBuffer failed: %d", (int)status);
        return -1;
    }

    return 0;
}

int decoder_drain(DecoderContext *ctx) {
    if (!ctx || !ctx->configured) {
        return 0;
    }

    int rendered = 0;
    AMediaCodecBufferInfo info;

    /* Moonlight strategy: drain ALL available output buffers.
     * Keep only the LATEST one for rendering — drop older frames
     * without rendering them (release with render=false).
     * This ensures we always display the newest decoded frame. */
    ssize_t last_idx = -1;

    for (;;) {
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(ctx->codec, &info, OUTPUT_TIMEOUT_US);

        if (idx == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
            break;
        }

        if (idx == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
            AMediaFormat *fmt = AMediaCodec_getOutputFormat(ctx->codec);
            if (fmt) {
                LOGI("decoder_drain: output format changed: %s",
                     AMediaFormat_toString(fmt));
                AMediaFormat_delete(fmt);
            }
            continue;
        }

        if (idx == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
            continue;
        }

        if (idx < 0) {
            break;
        }

        /* If we already have a pending frame, drop it (render=false) */
        if (last_idx >= 0) {
            AMediaCodec_releaseOutputBuffer(ctx->codec, (size_t)last_idx, false);
        }
        last_idx = idx;
    }

    /* Render only the newest frame */
    if (last_idx >= 0) {
        AMediaCodec_releaseOutputBuffer(ctx->codec, (size_t)last_idx, true);
        rendered = 1;
    }

    return rendered;
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

    free(ctx);
    LOGI("decoder_destroy: cleaned up");
}
