/*
 * DroidScreen Protocol - Mouse event serialization
 */

#include "droidscreen/mouse.h"

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

size_t ds_mouse_serialize(uint8_t *buf, const ds_mouse_event_t *event)
{
    buf[0] = event->action;
    buf[1] = event->buttons;
    buf[2] = 0;
    buf[3] = 0;
    write_le16(buf + 4, event->x_frac);
    write_le16(buf + 6, event->y_frac);
    write_le32(buf + 8, event->timestamp_ms);
    memset(buf + 12, 0, 4);
    return DS_MOUSE_EVENT_SIZE;
}

size_t ds_mouse_deserialize(const uint8_t *buf, ds_mouse_event_t *event)
{
    event->action = buf[0];
    event->buttons = buf[1];
    memcpy(event->reserved0, buf + 2, 2);
    event->x_frac = read_le16(buf + 4);
    event->y_frac = read_le16(buf + 6);
    event->timestamp_ms = read_le32(buf + 8);
    memcpy(event->reserved, buf + 12, 4);
    return DS_MOUSE_EVENT_SIZE;
}
