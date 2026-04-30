/*
 * DroidScreen Protocol - Video frame helpers
 */

#include "droidscreen/frame.h"
#include <string.h>

static void write_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void write_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void write_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        p[i] = (uint8_t)((v >> (i * 8)) & 0xFF);
    }
}

static uint16_t read_le16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= ((uint64_t)p[i]) << (i * 8);
    }
    return v;
}

size_t ds_frame_wrap(uint8_t *out_buf, size_t out_buf_size,
                     const uint8_t *nal_data, size_t nal_len,
                     int is_keyframe, int is_config)
{
    size_t total = DS_HEADER_SIZE + nal_len;
    if (out_buf_size < total) {
        return 0;
    }

    ds_header_t header;
    header.type  = DS_MSG_VIDEO_FRAME;
    header.flags = 0;
    if (is_keyframe) {
        header.flags |= DS_FLAG_KEYFRAME;
    }
    if (is_config) {
        header.flags |= DS_FLAG_CONFIG;
    }
    header.length = (uint32_t)nal_len;

    ds_header_serialize(out_buf, &header);
    memcpy(out_buf + DS_HEADER_SIZE, nal_data, nal_len);

    return total;
}

void ds_frame_parse_flags(uint8_t flags, int *is_keyframe, int *is_config)
{
    if (is_keyframe) {
        *is_keyframe = (flags & DS_FLAG_KEYFRAME) ? 1 : 0;
    }
    if (is_config) {
        *is_config = (flags & DS_FLAG_CONFIG) ? 1 : 0;
    }
}

size_t ds_frame_write_telemetry(uint8_t *buf, size_t buf_size,
                                const ds_video_telemetry_t *telemetry)
{
    if (!buf || !telemetry || buf_size < DS_VIDEO_TELEMETRY_SIZE) {
        return 0;
    }

    write_le32(buf + 0, DS_VIDEO_TELEMETRY_MAGIC);
    write_le16(buf + 4, DS_VIDEO_TELEMETRY_VERSION);
    write_le16(buf + 6, DS_VIDEO_TELEMETRY_SIZE);
    write_le64(buf + 8, telemetry->sequence);
    write_le64(buf + 16, (uint64_t)telemetry->capture_to_encode_us);
    write_le64(buf + 24, (uint64_t)telemetry->encode_to_send_us);
    write_le64(buf + 32, (uint64_t)telemetry->capture_to_send_us);
    write_le64(buf + 40, (uint64_t)telemetry->rtt_us);
    write_le32(buf + 48, telemetry->flags);
    write_le32(buf + 52, 0);
    return DS_VIDEO_TELEMETRY_SIZE;
}

int ds_frame_read_telemetry(const uint8_t *buf, size_t len,
                            ds_video_telemetry_t *telemetry,
                            size_t *header_size)
{
    if (header_size) {
        *header_size = 0;
    }
    if (!buf || len < DS_VIDEO_TELEMETRY_SIZE) {
        return 0;
    }
    if (read_le32(buf + 0) != DS_VIDEO_TELEMETRY_MAGIC) {
        return 0;
    }
    if (read_le16(buf + 4) != DS_VIDEO_TELEMETRY_VERSION) {
        return 0;
    }

    uint16_t size = read_le16(buf + 6);
    if (size < DS_VIDEO_TELEMETRY_SIZE || len < size) {
        return 0;
    }

    if (telemetry) {
        telemetry->sequence = read_le64(buf + 8);
        telemetry->capture_to_encode_us = (int64_t)read_le64(buf + 16);
        telemetry->encode_to_send_us = (int64_t)read_le64(buf + 24);
        telemetry->capture_to_send_us = (int64_t)read_le64(buf + 32);
        telemetry->rtt_us = (int64_t)read_le64(buf + 40);
        telemetry->flags = read_le32(buf + 48);
    }
    if (header_size) {
        *header_size = size;
    }
    return 1;
}
