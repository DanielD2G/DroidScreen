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
 * Drain all available output buffers (timeout=0).
 * Releases each with render=true so frames appear on the surface.
 * Returns number of frames rendered.
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
