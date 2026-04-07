/*
 * DroidScreen Protocol - Stream Deck messages
 */

#ifndef DROIDSCREEN_DECK_H
#define DROIDSCREEN_DECK_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Deck action types
 */
typedef enum {
    DS_DECK_ACTION_TAP     = 0,
    DS_DECK_ACTION_HOLD    = 1,
    DS_DECK_ACTION_RELEASE = 2
} ds_deck_action_type_t;

/*
 * Deck action (16 bytes)
 *
 * Represents a button action on the stream deck.
 * button_id identifies which button was pressed.
 * action specifies the type of interaction.
 * value is an action-specific parameter.
 */
#define DS_DECK_ACTION_SIZE  16
#pragma pack(push, 1)
typedef struct {
    uint8_t  action;
    uint8_t  button_id;
    uint16_t value;
    uint32_t timestamp_ms;
    uint8_t  reserved[8];
} ds_deck_action_t;
#pragma pack(pop)

/*
 * Volume state (4 bytes)
 *
 * Represents the current volume level and mute state.
 * level is 0..65535 representing 0%..100%.
 */
#define DS_VOLUME_STATE_SIZE  4
#pragma pack(push, 1)
typedef struct {
    uint16_t level;
    uint8_t  muted;
    uint8_t  reserved;
} ds_volume_state_t;
#pragma pack(pop)

/*
 * Serialize deck action to buffer.
 * buf must be at least DS_DECK_ACTION_SIZE bytes.
 * Returns DS_DECK_ACTION_SIZE on success.
 */
size_t ds_deck_action_serialize(uint8_t *buf, const ds_deck_action_t *action);

/*
 * Deserialize deck action from buffer.
 * buf must contain at least DS_DECK_ACTION_SIZE bytes.
 * Returns DS_DECK_ACTION_SIZE on success.
 */
size_t ds_deck_action_deserialize(const uint8_t *buf, ds_deck_action_t *action);

/*
 * Serialize volume state to buffer.
 * buf must be at least DS_VOLUME_STATE_SIZE bytes.
 * Returns DS_VOLUME_STATE_SIZE on success.
 */
size_t ds_volume_state_serialize(uint8_t *buf, const ds_volume_state_t *state);

/*
 * Deserialize volume state from buffer.
 * buf must contain at least DS_VOLUME_STATE_SIZE bytes.
 * Returns DS_VOLUME_STATE_SIZE on success.
 */
size_t ds_volume_state_deserialize(const uint8_t *buf, ds_volume_state_t *state);

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_DECK_H */
