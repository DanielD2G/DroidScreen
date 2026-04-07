#pragma once

#include <functional>
#include <memory>
#include <string>

namespace droidscreen {

class WinMediaController {
public:
  using StateCallback = std::function<void(const std::string &json)>;

  WinMediaController();
  ~WinMediaController();

  bool init();
  void shutdown();

  void set_state_callback(StateCallback cb);
  void push_current_state(bool include_artwork);
  void schedule_refresh_burst();

  bool toggle_play_pause();
  bool skip_previous();
  bool skip_next();

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace droidscreen
