/*
 * DroidScreen - Deck / volume message builders
 */

#include "deck_sender.h"
#include "ds_clock.h"

#include <string.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/deck.h"
}

size_t deck_action_build(uint8_t *out, int action_type, int button_id) {
    ds_deck_action_t action;
    memset(&action, 0, sizeof(action));
    action.action       = (uint8_t)action_type;
    action.timestamp_ms = ds_now_ms32();
    action.button_id = (button_id >= 0 && button_id <= 255) ? (uint8_t)button_id : 0;

    ds_header_t header;
    header.type   = DS_MSG_DECK_ACTION;
    header.flags  = 0;
    header.length = DS_DECK_ACTION_SIZE;
    ds_header_serialize(out, &header);
    ds_deck_action_serialize(out + DS_HEADER_SIZE, &action);
    return DS_HEADER_SIZE + DS_DECK_ACTION_SIZE;
}

size_t volume_change_build(uint8_t *out, int volume, int muted) {
    ds_volume_state_t state;
    memset(&state, 0, sizeof(state));
    state.level  = (uint16_t)volume;
    state.muted  = (uint8_t)(muted ? 1 : 0);

    ds_header_t header;
    header.type   = DS_MSG_VOLUME_CHANGE;
    header.flags  = 0;
    header.length = DS_VOLUME_STATE_SIZE;
    ds_header_serialize(out, &header);
    ds_volume_state_serialize(out + DS_HEADER_SIZE, &state);
    return DS_HEADER_SIZE + DS_VOLUME_STATE_SIZE;
}
