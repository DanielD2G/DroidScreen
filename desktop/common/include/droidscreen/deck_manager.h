/*
 * DroidScreen Desktop - Stream Deck Manager
 *
 * Manages the deck layout configuration and handles incoming
 * deck actions and volume changes from the Android device.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace droidscreen {

struct DeckTile {
  std::string id;
  std::string type; // "app", "media", "volume"
  std::string label;
  std::string icon_b64;  // base64-encoded PNG icon (64x64)
  std::string app_path;  // macOS app path (e.g. "/Applications/Safari.app")
  std::string bundle_id; // macOS bundle identifier (e.g. "com.apple.Safari")
  std::string
      launch_kind; // desktop-only launch type ("exe", "protocol", "uwp")
  std::string launch_target; // desktop-only launch target path / URI / AUMID
  std::string launch_args;   // desktop-only launch arguments
  std::string working_dir;   // desktop-only working directory
  int row = 0;
  int col = 0;
  int col_span = 1;
};

struct DeckLayout {
  std::vector<DeckTile> tiles;
  int grid_cols = 3;
  int grid_rows = 1;
};

class DeckManager {
public:
  using ActionCallback =
      std::function<void(uint8_t action, const std::string &tile_id)>;
  using VolumeCallback = std::function<void(uint16_t level, bool muted)>;

  DeckManager();
  ~DeckManager();

  // Set callbacks for incoming events from Android.
  void set_action_callback(ActionCallback cb);
  void set_volume_callback(VolumeCallback cb);

  // Get/set the deck layout.
  const DeckLayout &layout() const;
  void set_layout(const DeckLayout &layout);

  // Create a default layout with media, volume, and app tiles.
  static DeckLayout default_layout();

  // Serialize layout to JSON string for DS_MSG_DECK_CONFIG.
  std::string serialize_config() const;

  // Handle incoming DS_MSG_DECK_ACTION from Android.
  void handle_deck_action(const uint8_t *data, size_t len);

  // Handle incoming DS_MSG_VOLUME_CHANGE from Android.
  void handle_volume_change(const uint8_t *data, size_t len);

private:
  DeckLayout layout_;
  ActionCallback action_cb_;
  VolumeCallback volume_cb_;
};

} // namespace droidscreen
