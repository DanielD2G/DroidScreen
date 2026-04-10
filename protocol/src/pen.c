/*
 * DroidScreen Protocol - Pen event serialization
 */

#include "droidscreen/pen.h"

static void write_le16(uint8_t *buf, uint16_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
}

static uint16_t read_le16(const uint8_t *buf)
{
    return (uint16_t)buf[0]
         | ((uint16_t)buf[1] << 8);
}

static void write_le32(uint8_t *buf, uint32_t val)
{
    buf[0] = (uint8_t)(val & 0xFF);
    buf[1] = (uint8_t)((val >> 8) & 0xFF);
    buf[2] = (uint8_t)((val >> 16) & 0xFF);
    buf[3] = (uint8_t)((val >> 24) & 0xFF);
}

static uint32_t read_le32(const uint8_t *buf)
{
    return (uint32_t)buf[0]
         | ((uint32_t)buf[1] << 8)
         | ((uint32_t)buf[2] << 16)
         | ((uint32_t)buf[3] << 24);
}

size_t ds_pen_serialize(uint8_t *buf, const ds_pen_event_t *event)
{
    buf[0] = event->action;
    buf[1] = event->pointer_id;
    buf[2] = event->tool_type;
    buf[3] = 0;
    write_le16(buf + 4, event->x_frac);
    write_le16(buf + 6, event->y_frac);
    write_le16(buf + 8, event->pressure);
    write_le16(buf + 10, event->distance);
    write_le16(buf + 12, event->tilt);
    write_le16(buf + 14, event->rotation);
    write_le32(buf + 16, event->buttons);
    write_le32(buf + 20, event->timestamp_ms);
    memset(buf + 24, 0, 8);
    return DS_PEN_EVENT_SIZE;
}

size_t ds_pen_deserialize(const uint8_t *buf, ds_pen_event_t *event)
{
    event->action = buf[0];
    event->pointer_id = buf[1];
    event->tool_type = buf[2];
    event->reserved0 = buf[3];
    event->x_frac = read_le16(buf + 4);
    event->y_frac = read_le16(buf + 6);
    event->pressure = read_le16(buf + 8);
    event->distance = read_le16(buf + 10);
    event->tilt = read_le16(buf + 12);
    event->rotation = read_le16(buf + 14);
    event->buttons = read_le32(buf + 16);
    event->timestamp_ms = read_le32(buf + 20);
    memcpy(event->reserved, buf + 24, 8);
    return DS_PEN_EVENT_SIZE;
}
