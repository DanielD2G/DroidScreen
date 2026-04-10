/*
 * DroidScreen Protocol - Video frame helpers
 */

#include "droidscreen/frame.h"
#include <string.h>

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
