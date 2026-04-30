/*
 * DroidScreen Protocol - Video frame helpers
 */

#ifndef DROIDSCREEN_FRAME_H
#define DROIDSCREEN_FRAME_H

#include <stdint.h>
#include <stddef.h>
#include "droidscreen/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Wrap NAL data into a protocol message (header + payload).
 *
 * Writes a DS_MSG_VIDEO_FRAME header followed by the raw NAL data
 * into out_buf.
 *
 * out_buf:      destination buffer
 * out_buf_size: capacity of out_buf in bytes
 * nal_data:     raw NAL unit data
 * nal_len:      length of nal_data in bytes
 * is_keyframe:  non-zero if this is a keyframe
 * is_config:    non-zero if this is SPS/PPS configuration data
 *
 * Returns total bytes written (DS_HEADER_SIZE + nal_len), or 0 if
 * out_buf is too small.
 */
size_t ds_frame_wrap(uint8_t *out_buf, size_t out_buf_size,
                     const uint8_t *nal_data, size_t nal_len,
                     int is_keyframe, int is_config);

/*
 * Parse video frame flags from a header flags byte.
 *
 * flags:       the flags byte from ds_header_t
 * is_keyframe: output, non-zero if keyframe flag is set
 * is_config:   output, non-zero if config (SPS/PPS) flag is set
 */
void ds_frame_parse_flags(uint8_t flags, int *is_keyframe, int *is_config);

#define DS_VIDEO_TELEMETRY_MAGIC 0x544C5344u /* "DSLT" little-endian */
#define DS_VIDEO_TELEMETRY_VERSION 2u
#define DS_VIDEO_TELEMETRY_SIZE 56u
#define DS_VIDEO_TELEMETRY_FLAG_IDLE 0x00000001u

typedef struct {
    uint64_t sequence;
    int64_t capture_to_encode_us;
    int64_t encode_to_send_us;
    int64_t capture_to_send_us;
    int64_t rtt_us;
    uint32_t flags;
} ds_video_telemetry_t;

size_t ds_frame_write_telemetry(uint8_t *buf, size_t buf_size,
                                const ds_video_telemetry_t *telemetry);

int ds_frame_read_telemetry(const uint8_t *buf, size_t len,
                            ds_video_telemetry_t *telemetry,
                            size_t *header_size);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_FRAME_H */
