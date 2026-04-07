/*
 * DroidScreen Protocol - Stream Deck serialization
 */

#include "droidscreen/deck.h"

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

size_t ds_deck_action_serialize(uint8_t *buf, const ds_deck_action_t *action)
{
    buf[0] = action->action;
    buf[1] = action->button_id;
    write_le16(buf + 2, action->value);
    write_le32(buf + 4, action->timestamp_ms);
    memset(buf + 8, 0, 8);
    return DS_DECK_ACTION_SIZE;
}

size_t ds_deck_action_deserialize(const uint8_t *buf, ds_deck_action_t *action)
{
    action->action       = buf[0];
    action->button_id    = buf[1];
    action->value        = read_le16(buf + 2);
    action->timestamp_ms = read_le32(buf + 4);
    memcpy(action->reserved, buf + 8, 8);
    return DS_DECK_ACTION_SIZE;
}

size_t ds_volume_state_serialize(uint8_t *buf, const ds_volume_state_t *state)
{
    write_le16(buf, state->level);
    buf[2] = state->muted;
    buf[3] = 0;
    return DS_VOLUME_STATE_SIZE;
}

size_t ds_volume_state_deserialize(const uint8_t *buf, ds_volume_state_t *state)
{
    state->level    = read_le16(buf);
    state->muted    = buf[2];
    state->reserved = buf[3];
    return DS_VOLUME_STATE_SIZE;
}
