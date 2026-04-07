/*
 * DroidScreen Desktop - Stream Deck Manager implementation
 *
 * Handles deck layout configuration, JSON serialization, and
 * incoming deck action / volume change messages from Android.
 */

#include "droidscreen/deck_manager.h"

#include <cstdio>
#include <cstring>

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/deck.h"
}

namespace droidscreen {

DeckManager::DeckManager()
    : layout_(default_layout())
{
}

DeckManager::~DeckManager() = default;

void DeckManager::set_action_callback(ActionCallback cb) {
    action_cb_ = std::move(cb);
}

void DeckManager::set_volume_callback(VolumeCallback cb) {
    volume_cb_ = std::move(cb);
}

const DeckLayout& DeckManager::layout() const {
    return layout_;
}

void DeckManager::set_layout(const DeckLayout& layout) {
    layout_ = layout;
}

DeckLayout DeckManager::default_layout() {
    DeckLayout layout;
    layout.grid_cols = 4;
    layout.grid_rows = 2;

    // Row 0: Media (2 cols) + Volume (1 col) + empty
    DeckTile media;
    media.id       = "media";
    media.type     = "media";
    media.label    = "Now Playing";
    media.row      = 0;
    media.col      = 0;
    media.col_span = 2;
    layout.tiles.push_back(std::move(media));

    DeckTile volume;
    volume.id       = "volume";
    volume.type     = "volume";
    volume.label    = "Volume";
    volume.row      = 0;
    volume.col      = 2;
    volume.col_span = 1;
    layout.tiles.push_back(std::move(volume));

    // Row 1: App shortcuts
    auto add_app = [&](const char* id, const char* label, int col) {
        DeckTile tile;
        tile.id       = id;
        tile.type     = "app";
        tile.label    = label;
        tile.row      = 1;
        tile.col      = col;
        tile.col_span = 1;
        layout.tiles.push_back(std::move(tile));
    };

    add_app("app_safari",   "Safari",   0);
    add_app("app_music",    "Music",    1);
    add_app("app_notes",    "Notes",    2);
    add_app("app_terminal", "Terminal", 3);

    return layout;
}

// Escape a string for JSON output (handles quotes and backslashes).
static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:   out += c;      break;
        }
    }
    return out;
}

std::string DeckManager::serialize_config() const {
    std::string json;
    json.reserve(512);

    char buf[64];

    json += "{";

    snprintf(buf, sizeof(buf), "\"grid_cols\":%d", layout_.grid_cols);
    json += buf;

    snprintf(buf, sizeof(buf), ",\"grid_rows\":%d", layout_.grid_rows);
    json += buf;

    json += ",\"tiles\":[";

    for (size_t i = 0; i < layout_.tiles.size(); i++) {
        const auto& tile = layout_.tiles[i];
        if (i > 0) json += ",";

        json += "{";
        json += "\"id\":\"" + json_escape(tile.id) + "\"";
        json += ",\"type\":\"" + json_escape(tile.type) + "\"";
        json += ",\"label\":\"" + json_escape(tile.label) + "\"";

        if (!tile.icon_b64.empty()) {
            json += ",\"icon_b64\":\"" + json_escape(tile.icon_b64) + "\"";
        }

        snprintf(buf, sizeof(buf), ",\"row\":%d", tile.row);
        json += buf;

        snprintf(buf, sizeof(buf), ",\"col\":%d", tile.col);
        json += buf;

        snprintf(buf, sizeof(buf), ",\"col_span\":%d", tile.col_span);
        json += buf;

        json += "}";
    }

    json += "]}";

    return json;
}

void DeckManager::handle_deck_action(const uint8_t* data, size_t len) {
    if (len < DS_DECK_ACTION_SIZE) {
        fprintf(stderr, "[deck] action payload too small (%zu < %d)\n",
                len, DS_DECK_ACTION_SIZE);
        return;
    }

    ds_deck_action_t action;
    ds_deck_action_deserialize(data, &action);

    // Resolve button_id (tile index) to the tile's string ID.
    std::string tile_id;
    if (action.button_id < layout_.tiles.size()) {
        tile_id = layout_.tiles[action.button_id].id;
    }

    fprintf(stderr, "[deck] action: type=%u button=%u tile=%s ts=%u\n",
            action.action, action.button_id, tile_id.c_str(), action.timestamp_ms);

    if (action_cb_) {
        action_cb_(action.action, tile_id);
    }
}

void DeckManager::handle_volume_change(const uint8_t* data, size_t len) {
    if (len < DS_VOLUME_STATE_SIZE) {
        fprintf(stderr, "[deck] volume change payload too small (%zu < %d)\n",
                len, DS_VOLUME_STATE_SIZE);
        return;
    }

    ds_volume_state_t state;
    ds_volume_state_deserialize(data, &state);

    fprintf(stderr, "[deck] volume change: level=%u muted=%u\n",
            state.level, state.muted);

    if (volume_cb_) {
        volume_cb_(state.level, state.muted != 0);
    }
}

} // namespace droidscreen
