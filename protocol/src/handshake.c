/*
 * DroidScreen Protocol - Handshake serialization
 */

#include "droidscreen/handshake.h"

/* Write a uint16 in little-endian byte order */
static void write_le16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

/* Read a uint16 from little-endian byte order */
static uint16_t read_le16(const uint8_t *buf)
{
    return (uint16_t)buf[0]
         | ((uint16_t)buf[1] << 8);
}

/* Write a uint32 in little-endian byte order */
static void write_le32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

/* Read a uint32 from little-endian byte order */
static uint32_t read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

size_t ds_handshake_req_serialize(uint8_t *buf, const ds_handshake_req_t *req)
{
    write_le16(buf + 0, req->protocol_version);
    write_le16(buf + 2, req->width);
    write_le16(buf + 4, req->height);
    buf[6] = req->fps;
    buf[7] = req->codec;
    write_le32(buf + 8, req->max_bitrate_kbps);
    buf[12] = req->touch_enabled;
    write_le32(buf + 13, req->frame_interval_us);
    memcpy(buf + 17, req->reserved, 3);
    return DS_HANDSHAKE_REQ_SIZE;
}

size_t ds_handshake_req_deserialize(const uint8_t *buf, ds_handshake_req_t *req)
{
    req->protocol_version  = read_le16(buf + 0);
    req->width             = read_le16(buf + 2);
    req->height            = read_le16(buf + 4);
    req->fps               = buf[6];
    req->codec             = buf[7];
    req->max_bitrate_kbps  = read_le32(buf + 8);
    req->touch_enabled     = buf[12];
    req->frame_interval_us = read_le32(buf + 13);
    memcpy(req->reserved, buf + 17, 3);
    return DS_HANDSHAKE_REQ_SIZE;
}

size_t ds_handshake_resp_serialize(uint8_t *buf, const ds_handshake_resp_t *resp)
{
    write_le16(buf + 0, resp->protocol_version);
    write_le16(buf + 2, resp->accepted_width);
    write_le16(buf + 4, resp->accepted_height);
    buf[6] = resp->accepted_fps;
    buf[7] = resp->accepted_codec;
    write_le32(buf + 8, resp->decoder_max_bitrate);
    buf[12] = resp->touch_supported;
    memcpy(buf + 13, resp->reserved, 3);
    return DS_HANDSHAKE_RESP_SIZE;
}

size_t ds_handshake_resp_deserialize(const uint8_t *buf, ds_handshake_resp_t *resp)
{
    resp->protocol_version  = read_le16(buf + 0);
    resp->accepted_width    = read_le16(buf + 2);
    resp->accepted_height   = read_le16(buf + 4);
    resp->accepted_fps      = buf[6];
    resp->accepted_codec    = buf[7];
    resp->decoder_max_bitrate = read_le32(buf + 8);
    resp->touch_supported   = buf[12];
    memcpy(resp->reserved, buf + 13, 3);
    return DS_HANDSHAKE_RESP_SIZE;
}
