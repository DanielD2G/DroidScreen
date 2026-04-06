/*
 * DroidScreen Protocol - Handshake messages
 */

#ifndef DROIDSCREEN_HANDSHAKE_H
#define DROIDSCREEN_HANDSHAKE_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Codec identifiers
 */
typedef enum {
    DS_CODEC_H264 = 0,
    DS_CODEC_HEVC = 1
} ds_codec_t;

#define DS_HANDSHAKE_REQ_SIZE   20
#define DS_HANDSHAKE_RESP_SIZE  16

/*
 * Handshake request (20 bytes)
 * Sent by the Android device to propose stream parameters.
 */
#pragma pack(push, 1)
typedef struct {
    uint16_t protocol_version;
    uint16_t width;
    uint16_t height;
    uint8_t  fps;
    uint8_t  codec;
    uint32_t max_bitrate_kbps;
    uint8_t  touch_enabled;
    uint8_t  reserved[7];
} ds_handshake_req_t;
#pragma pack(pop)

/*
 * Handshake response (16 bytes)
 * Sent by the desktop to confirm/negotiate parameters.
 */
#pragma pack(push, 1)
typedef struct {
    uint16_t protocol_version;
    uint16_t accepted_width;
    uint16_t accepted_height;
    uint8_t  accepted_fps;
    uint8_t  accepted_codec;
    uint32_t decoder_max_bitrate;
    uint8_t  touch_supported;
    uint8_t  reserved[3];
} ds_handshake_resp_t;
#pragma pack(pop)

/*
 * Serialize handshake request to buffer.
 * buf must be at least DS_HANDSHAKE_REQ_SIZE bytes.
 * Returns DS_HANDSHAKE_REQ_SIZE on success.
 */
size_t ds_handshake_req_serialize(uint8_t *buf, const ds_handshake_req_t *req);

/*
 * Deserialize handshake request from buffer.
 * buf must contain at least DS_HANDSHAKE_REQ_SIZE bytes.
 * Returns DS_HANDSHAKE_REQ_SIZE on success.
 */
size_t ds_handshake_req_deserialize(const uint8_t *buf, ds_handshake_req_t *req);

/*
 * Serialize handshake response to buffer.
 * buf must be at least DS_HANDSHAKE_RESP_SIZE bytes.
 * Returns DS_HANDSHAKE_RESP_SIZE on success.
 */
size_t ds_handshake_resp_serialize(uint8_t *buf, const ds_handshake_resp_t *resp);

/*
 * Deserialize handshake response from buffer.
 * buf must contain at least DS_HANDSHAKE_RESP_SIZE bytes.
 * Returns DS_HANDSHAKE_RESP_SIZE on success.
 */
size_t ds_handshake_resp_deserialize(const uint8_t *buf, ds_handshake_resp_t *resp);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_HANDSHAKE_H */
