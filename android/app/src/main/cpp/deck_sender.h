/*
 * DroidScreen - Deck / volume message builders
 */

#ifndef DROIDSCREEN_DECK_SENDER_H
#define DROIDSCREEN_DECK_SENDER_H

#include <stddef.h>
#include <stdint.h>

/* Complete messages into `out` (DS_OUTBOUND_MAX_MSG bytes). */
size_t deck_action_build(uint8_t *out, int action_type, int button_id);
size_t volume_change_build(uint8_t *out, int volume, int muted);

#endif /* DROIDSCREEN_DECK_SENDER_H */
