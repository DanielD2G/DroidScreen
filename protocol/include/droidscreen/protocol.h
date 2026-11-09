/*
 * DroidScreen Protocol - Wire protocol definitions
 *
 * Binary wire protocol between desktop and Android applications.
 * Header format: [type:u8][flags:u8][length:u32 LE] = 6 bytes
 */

#ifndef DROIDSCREEN_PROTOCOL_H
#define DROIDSCREEN_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Protocol version */
#define DS_PROTOCOL_VERSION  10

/* Default TCP port */
#define DS_DEFAULT_PORT      38271

/* Header size in bytes */
#define DS_HEADER_SIZE       6

/*
 * Message types
 */
typedef enum {
    DS_MSG_HANDSHAKE_REQ  = 0x01,
    DS_MSG_HANDSHAKE_RESP = 0x02,
    DS_MSG_HELLO          = 0x03,  /* wireless only, see hello.h */
    DS_MSG_HELLO_RESP     = 0x04,
    DS_MSG_VIDEO_FRAME    = 0x10,
    DS_MSG_TOUCH_EVENT    = 0x20,
    DS_MSG_PEN_EVENT      = 0x21,
    DS_MSG_MOUSE_EVENT    = 0x22,
    DS_MSG_KEY_EVENT      = 0x23,  /* soft keyboard text/keys, see keyboard.h */
    DS_MSG_CONTROL        = 0x30,
    DS_MSG_DECK_CONFIG    = 0x40,
    DS_MSG_DECK_ACTION    = 0x41,
    DS_MSG_MEDIA_STATE    = 0x42,
    DS_MSG_VOLUME_STATE   = 0x43,
    DS_MSG_VOLUME_CHANGE  = 0x44,
    DS_MSG_PING           = 0xF0,
    DS_MSG_PONG           = 0xF1
} ds_msg_type_t;

/*
 * Video frame flags (stored in header flags field)
 */
#define DS_FLAG_KEYFRAME  0x01
#define DS_FLAG_CONFIG    0x02  /* SPS/PPS data */

/*
 * Control sub-types (first byte of control message payload)
 */
typedef enum {
    DS_CTRL_RESOLUTION_CHANGE  = 0x01,
    DS_CTRL_BITRATE_CHANGE     = 0x02,
    DS_CTRL_REQUEST_KEYFRAME   = 0x03,
    DS_CTRL_DISCONNECT         = 0x04,
    DS_CTRL_DISPLAY_OFF        = 0x05,
    DS_CTRL_DISPLAY_ON         = 0x06,
    DS_CTRL_SPEED_TEST         = 0x07,
    /* Android -> desktop, ~10 Hz: receive backlog, see DS_BACKLOG_* below. */
    DS_CTRL_RECEIVER_BACKLOG   = 0x08
} ds_ctrl_type_t;

/*
 * DS_CTRL_RECEIVER_BACKLOG payload (little-endian, after the ctrl byte):
 *   u32 ring_bytes      compressed video waiting in the Android ring buffer
 *   u32 ring_frames     video messages waiting in the ring buffer
 *   u32 decoder_frames  frames fed to MediaCodec but not yet released
 */
#define DS_BACKLOG_PAYLOAD_SIZE 13u  /* 1 ctrl byte + 3 x u32 */

/*
 * PING / PONG payloads (little-endian), used for RTT and for the
 * desktop<->Android clock offset:
 *   PING: u64 desktop_send_us   (desktop steady clock)
 *   PONG: u64 desktop_send_us   (echoed from the PING)
 *         u64 android_now_us    (Android CLOCK_MONOTONIC when answering)
 */
#define DS_PING_PAYLOAD_SIZE 8u
#define DS_PONG_PAYLOAD_SIZE 16u

/*
 * Wire header: 6 bytes, little-endian
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  type;
    uint8_t  flags;
    uint32_t length;
} ds_header_t;
#pragma pack(pop)

/*
 * Serialize header to buffer.
 * buf must be at least DS_HEADER_SIZE bytes.
 * Returns DS_HEADER_SIZE on success.
 */
size_t ds_header_serialize(uint8_t *buf, const ds_header_t *header);

/*
 * Deserialize header from buffer.
 * buf must contain at least DS_HEADER_SIZE bytes.
 * Returns DS_HEADER_SIZE on success.
 */
size_t ds_header_deserialize(const uint8_t *buf, ds_header_t *header);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_PROTOCOL_H */
