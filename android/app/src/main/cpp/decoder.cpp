/*
 * DroidScreen - H.264 hardware decoder implementation
 *
 * Uses the Android NDK AMediaCodec API to decode H.264 NAL units
 * and render directly to an ANativeWindow (SurfaceView).
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

/* Input buffer dequeue timeout in microseconds */
#define INPUT_TIMEOUT_US  5000   /* 5ms */
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

    /* Priority 0 = real-time (API 28+). Use string literal for compat. */
    AMediaFormat_setInt32(format, "priority", 0);

    /* Request low latency decoding (API 30+, ignored on older) */
    AMediaFormat_setInt32(format, "low-latency", 1);

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
            /* Deprecated but may still be sent */
            continue;
        }

        if (idx < 0) {
            LOGW("decoder_drain: unexpected dequeue result: %zd", idx);
            break;
        }

        /* Release output buffer to surface (render = true) */
        AMediaCodec_releaseOutputBuffer(ctx->codec, (size_t)idx, true /* render */);
        rendered++;
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
