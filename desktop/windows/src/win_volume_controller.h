#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace droidscreen {

class WinVolumeController {
public:
  using StateCallback = std::function<void(uint16_t level, bool muted)>;

  WinVolumeController();
  ~WinVolumeController();

  bool init();
  void shutdown();

  void set_state_callback(StateCallback cb);
  void push_current_state();
  bool set_volume(uint16_t level, bool muted);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace droidscreen
