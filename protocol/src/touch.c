/*
 * DroidScreen Protocol - Touch event serialization
 */

#include "droidscreen/touch.h"

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

size_t ds_touch_serialize(uint8_t *buf, const ds_touch_event_t *event)
{
    buf[0] = event->action;
    buf[1] = event->pointer_id;
    buf[2] = event->tool_type;
    buf[3] = 0;
    write_le16(buf + 4, event->x_frac);
    write_le16(buf + 6, event->y_frac);
    write_le16(buf + 8, event->pressure);
    write_le16(buf + 10, event->touch_major);
    write_le16(buf + 12, event->touch_minor);
    write_le16(buf + 14, event->orientation);
    write_le16(buf + 16, event->distance);
    write_le16(buf + 18, event->tilt);
    write_le32(buf + 20, event->buttons);
    write_le32(buf + 24, event->timestamp_ms);
    memset(buf + 28, 0, 4);
    return DS_TOUCH_EVENT_SIZE;
}

size_t ds_touch_deserialize(const uint8_t *buf, ds_touch_event_t *event)
{
    event->action       = buf[0];
    event->pointer_id   = buf[1];
    event->tool_type    = buf[2];
    event->reserved0    = buf[3];
    event->x_frac       = read_le16(buf + 4);
    event->y_frac       = read_le16(buf + 6);
    event->pressure     = read_le16(buf + 8);
    event->touch_major  = read_le16(buf + 10);
    event->touch_minor  = read_le16(buf + 12);
    event->orientation  = read_le16(buf + 14);
    event->distance     = read_le16(buf + 16);
    event->tilt         = read_le16(buf + 18);
    event->buttons      = read_le32(buf + 20);
    event->timestamp_ms = read_le32(buf + 24);
    memcpy(event->reserved, buf + 28, 4);
    return DS_TOUCH_EVENT_SIZE;
}

void ds_touch_to_frac(int pixel_x, int pixel_y,
                      int surface_width, int surface_height,
                      uint16_t *x_frac, uint16_t *y_frac)
{
    if (surface_width <= 0 || surface_height <= 0) {
        *x_frac = 0;
        *y_frac = 0;
        return;
    }

    /* Clamp to surface bounds */
    if (pixel_x < 0) pixel_x = 0;
    if (pixel_y < 0) pixel_y = 0;
    if (pixel_x >= surface_width)  pixel_x = surface_width - 1;
    if (pixel_y >= surface_height) pixel_y = surface_height - 1;

    *x_frac = (uint16_t)(((uint32_t)pixel_x * 65535) / (uint32_t)(surface_width - 1));
    *y_frac = (uint16_t)(((uint32_t)pixel_y * 65535) / (uint32_t)(surface_height - 1));
}

void ds_touch_from_frac(uint16_t x_frac, uint16_t y_frac,
                        int display_width, int display_height,
                        int *pixel_x, int *pixel_y)
{
    if (display_width <= 0 || display_height <= 0) {
        *pixel_x = 0;
        *pixel_y = 0;
        return;
    }

    *pixel_x = (int)(((uint32_t)x_frac * (uint32_t)(display_width - 1)) / 65535);
    *pixel_y = (int)(((uint32_t)y_frac * (uint32_t)(display_height - 1)) / 65535);
}
