/*
 * DroidScreen - Deck event sender implementation
 */

#include "deck_sender.h"
#include "tcp_server.h"

#include <android/log.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/deck.h"
}

#define TAG "DroidScreen"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static uint32_t get_timestamp_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

int deck_action_send(int fd, int action_type, const char* tile_id) {
    /* Build deck action */
    ds_deck_action_t action;
    memset(&action, 0, sizeof(action));
    action.action       = (uint8_t)action_type;
    action.timestamp_ms = get_timestamp_ms();

    /* tile_id is the tile index as a numeric string (e.g. "0", "3"). */
    action.button_id = tile_id ? (uint8_t)atoi(tile_id) : 0;

    /* Serialize payload */
    uint8_t payload[DS_DECK_ACTION_SIZE];
    ds_deck_action_serialize(payload, &action);

    /* Serialize header */
    ds_header_t header;
    header.type   = DS_MSG_DECK_ACTION;
    header.flags  = 0;
    header.length = DS_DECK_ACTION_SIZE;

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

    /* Send header + payload */
    uint8_t wire_buf[DS_HEADER_SIZE + DS_DECK_ACTION_SIZE];
    memcpy(wire_buf, hdr_buf, DS_HEADER_SIZE);
    memcpy(wire_buf + DS_HEADER_SIZE, payload, DS_DECK_ACTION_SIZE);

    if (tcp_send_all(fd, wire_buf, sizeof(wire_buf)) != 0) {
        LOGE("deck_action_send: failed to send deck action");
        return -1;
    }

    return 0;
}

int volume_change_send(int fd, int volume, int muted) {
    /* Build volume state */
    ds_volume_state_t state;
    memset(&state, 0, sizeof(state));
    state.level  = (uint16_t)volume;
    state.muted  = (uint8_t)(muted ? 1 : 0);

    /* Serialize payload */
    uint8_t payload[DS_VOLUME_STATE_SIZE];
    ds_volume_state_serialize(payload, &state);

    /* Serialize header */
    ds_header_t header;
    header.type   = DS_MSG_VOLUME_CHANGE;
    header.flags  = 0;
    header.length = DS_VOLUME_STATE_SIZE;

    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_serialize(hdr_buf, &header);

    /* Send header + payload */
    uint8_t wire_buf[DS_HEADER_SIZE + DS_VOLUME_STATE_SIZE];
    memcpy(wire_buf, hdr_buf, DS_HEADER_SIZE);
    memcpy(wire_buf + DS_HEADER_SIZE, payload, DS_VOLUME_STATE_SIZE);

    if (tcp_send_all(fd, wire_buf, sizeof(wire_buf)) != 0) {
        LOGE("volume_change_send: failed to send volume change");
        return -1;
    }

    return 0;
}
