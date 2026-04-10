/*
 * DroidScreen Protocol - Header serialization
 */

#include "droidscreen/protocol.h"

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

size_t ds_header_serialize(uint8_t *buf, const ds_header_t *header)
{
    buf[0] = header->type;
    buf[1] = header->flags;
    write_le32(buf + 2, header->length);
    return DS_HEADER_SIZE;
}

size_t ds_header_deserialize(const uint8_t *buf, ds_header_t *header)
{
    header->type  = buf[0];
    header->flags = buf[1];
    header->length = read_le32(buf + 2);
    return DS_HEADER_SIZE;
}
