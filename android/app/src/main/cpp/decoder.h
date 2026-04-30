/*
 * DroidScreen - H.264 hardware decoder using Android NDK MediaCodec
 *
 * Supports two operation modes:
 *   - Sync mode (API 26+): polling-based dequeue (fallback)
 *   - Async mode (API 28+): callback-driven, event-based (preferred)
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

/*
 * Callback invoked when the decoder has work to do (input or output ready).
 * Used to wake the decode thread from its wait instead of polling.
 */
typedef void (*decoder_wakeup_fn)(void *userdata);

typedef struct DecoderContext DecoderContext;

/*
 * Create a decoder context. Does NOT configure the codec yet.
 * The window is retained (ref-counted by caller).
 * Returns NULL on failure.
 */
DecoderContext* decoder_create(ANativeWindow *window);

/*
 * Set a wakeup callback. Must be called BEFORE decoder_configure.
 * The callback is invoked from MediaCodec's internal thread when
 * input or output buffers become available (async mode only).
 */
void decoder_set_wakeup(DecoderContext *ctx, decoder_wakeup_fn fn, void *userdata);

/*
 * Set the render mode (default: RENDER_MODE_LOWEST_LATENCY).
 * Must be called before decoder_configure or between sessions.
 */
void decoder_set_render_mode(DecoderContext *ctx, DecoderRenderMode mode);

/*
 * Configure the codec for the given resolution and frame rate.
 * On API 28+, enables async callbacks if a wakeup function was set.
 * Returns 0 on success, -1 on error.
 */
int decoder_configure(DecoderContext *ctx, int width, int height, int fps);

/*
 * Returns true if the decoder is operating in async callback mode.
 */
bool decoder_is_async(DecoderContext *ctx);

/*
 * Feed a NAL unit to the decoder (sync mode).
 * Dequeues an input buffer (5ms timeout), copies data, and queues it.
 * Returns 0 on success, -1 if no buffer available or error.
 */
int decoder_feed(DecoderContext *ctx, const uint8_t *nal_data, size_t nal_len,
                 int64_t timestamp_us, uint32_t flags);

/*
 * Feed a NAL unit using a pre-dequeued input buffer index (async mode).
 * The index comes from the onAsyncInputAvailable callback.
 * Returns 0 on success, -1 on error.
 */
int decoder_feed_index(DecoderContext *ctx, int32_t index,
                       const uint8_t *nal_data, size_t nal_len,
                       int64_t timestamp_us, uint32_t flags);

/*
 * Pop an available input buffer index (async mode).
 * Returns a valid index >= 0, or -1 if none available.
 */
int32_t decoder_pop_input(DecoderContext *ctx);

/*
 * Drain all available output buffers.
 *
 * Sync mode: polls with dequeueOutputBuffer(timeout=0).
 * Async mode: drains from the internal output queue filled by callbacks.
 *
 * Render queue depth = 1 policy: drops every frame except the newest,
 * renders only that one.  Returns 0 or 1.
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
