/*
 * DroidScreen - H.264 hardware decoder using Android NDK MediaCodec
 */

#ifndef DROIDSCREEN_DECODER_H
#define DROIDSCREEN_DECODER_H

#include <android/native_window.h>
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Render mode controls how decoded frames are presented to the surface.
 */
typedef enum {
    RENDER_MODE_LOWEST_LATENCY = 0,  /* render ASAP — minimum latency */
    RENDER_MODE_SMOOTH         = 1,  /* VSync-aligned (future) */
} DecoderRenderMode;

typedef struct DecoderContext DecoderContext;

/*
 * Create a decoder context. Does NOT configure the codec yet.
 * The window is retained (ref-counted by caller).
 * Returns NULL on failure.
 */
DecoderContext* decoder_create(ANativeWindow *window);

/*
 * Configure the codec for the given resolution.
 * Sets up "video/avc" decoder with low-latency and priority=0,
 * outputting to the surface.
 * Returns 0 on success, -1 on error.
 */
int decoder_configure(DecoderContext *ctx, int width, int height);

/*
 * Feed a NAL unit to the decoder.
 * Dequeues an input buffer (5ms timeout), copies data, and queues it.
 * Returns 0 on success, -1 if no buffer available or error.
 */
int decoder_feed(DecoderContext *ctx, const uint8_t *nal_data, size_t nal_len,
                 int64_t timestamp_us, uint32_t flags);

/*
 * Set the render mode (default: RENDER_MODE_LOWEST_LATENCY).
 * Must be called before decoder_configure or between sessions.
 */
void decoder_set_render_mode(DecoderContext *ctx, DecoderRenderMode mode);

/*
 * Drain all available output buffers (timeout=0).
 *
 * Render queue depth = 1 policy: drains ALL pending output buffers,
 * drops every frame except the newest, and renders only that one.
 * This ensures the displayed frame is always the most recent decode.
 *
 * Returns number of frames rendered (0 or 1).
 */
int decoder_drain(DecoderContext *ctx);

/*
 * Stop and destroy the decoder. Releases all resources.
 */
void decoder_destroy(DecoderContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_DECODER_H */
