/*
 * DroidScreen - H.264/HEVC hardware decoder using Android NDK MediaCodec
 *
 * Two operation modes, chosen per codec (never mixed):
 *   - Sync (Qualcomm): the feed thread dequeues input buffers; a dedicated
 *     output thread owned by the decoder is the only caller of
 *     dequeueOutputBuffer/releaseOutputBuffer.
 *   - Async (others): MediaCodec callbacks; the decode thread drains outputs.
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

/*
 * Called for every output buffer the decoder releases, in release order:
 * rendered = true for the one sent to the surface, false for older outputs
 * dropped by the render-queue-depth=1 policy. Sync mode: output thread.
 * Async mode: decode thread.
 */
typedef void (*decoder_released_fn)(void *userdata, int64_t pts_us, bool rendered);

/*
 * MediaCodec onFrameRendered (API 33): the frame with this PTS reached the
 * display at system_nano (CLOCK_MONOTONIC). Fired on an NDK internal thread;
 * can be batched, delayed or missing for some frames.
 */
typedef void (*decoder_frame_rendered_fn)(void *userdata, int64_t pts_us,
                                          int64_t system_nano);

typedef struct DecoderContext DecoderContext;

typedef struct {
    int  codec_id;
    bool direct_submit;
    bool has_android_low_latency;
    bool is_qcom_c2;
    bool is_qcom_omx;
} DecoderVendorHints;

/*
 * Create a decoder context. Does NOT configure the codec yet.
 * The window is retained (ref-counted by caller).
 * Returns NULL on failure.
 */
DecoderContext* decoder_create(ANativeWindow *window,
                               const char *mime,
                               const char *codec_name,
                               const DecoderVendorHints *hints);

/*
 * Set a wakeup callback. Must be called BEFORE decoder_configure.
 * The callback is invoked from MediaCodec's internal thread when
 * input or output buffers become available (async mode only).
 */
void decoder_set_wakeup(DecoderContext *ctx, decoder_wakeup_fn fn, void *userdata);

/*
 * Output observers. Must be called BEFORE decoder_configure.
 */
void decoder_set_output_callbacks(DecoderContext *ctx,
                                  decoder_released_fn released_fn,
                                  decoder_frame_rendered_fn frame_rendered_fn,
                                  void *userdata);

/*
 * Set the render mode (default: RENDER_MODE_LOWEST_LATENCY).
 * Must be called before decoder_configure or between sessions.
 */
void decoder_set_render_mode(DecoderContext *ctx, DecoderRenderMode mode);

/*
 * Configure and start the codec for the given resolution. Async callbacks
 * for non-Qualcomm codecs; sync mode starts the output thread.
 * Returns 0 on success, -1 on error.
 */
int decoder_configure(DecoderContext *ctx, int width, int height, int fps);

/*
 * Returns true if the decoder is operating in async callback mode.
 */
bool decoder_is_async(DecoderContext *ctx);

/*
 * Sync mode: dequeue an input buffer, waiting up to timeout_us (the output
 * thread frees buffers concurrently). Blocking MediaCodec call: never call it
 * while holding a lock other threads need.
 * Returns the index, or -1 if none became available.
 */
int32_t decoder_dequeue_input(DecoderContext *ctx, int64_t timeout_us);

/*
 * Copy a NAL unit into a dequeued input buffer and queue it (both modes;
 * async indices come from onAsyncInputAvailable via decoder_pop_input).
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
 * Async mode only: drain the output queue filled by the callbacks.
 * Render queue depth = 1: every output but the newest is released without
 * rendering, the newest is rendered. Returns 1 if a frame was rendered.
 * Sync mode returns 0 (the output thread owns the outputs).
 */
int decoder_drain(DecoderContext *ctx);

/*
 * Stop and destroy the decoder. Stops, wakes and joins the sync output thread
 * first, then stops and deletes the codec. The caller must make sure no other
 * thread is still inside a decoder_* call.
 */
void decoder_destroy(DecoderContext *ctx);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_DECODER_H */
