#include "win_media_controller.h"

#include <windows.h>

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Control.h>
#include <winrt/Windows.Security.Cryptography.h>
#include <winrt/Windows.Storage.Streams.h>
#include <winrt/base.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace droidscreen {

namespace {

using namespace winrt;
using namespace winrt::Windows::Media::Control;
using namespace winrt::Windows::Security::Cryptography;
using namespace winrt::Windows::Storage::Streams;

struct ScopedApartment {
  bool initialized = false;

  ScopedApartment() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      initialized = true;
    } catch (const winrt::hresult_error &e) {
      if (e.code() != RPC_E_CHANGED_MODE) {
        throw;
      }
    }
  }

  ~ScopedApartment() {
    if (initialized) {
      winrt::uninit_apartment();
    }
  }
};

std::string json_escape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      out += c;
      break;
    }
  }
  return out;
}

std::string build_media_json(bool playing, const std::string &title,
                             const std::string &artist, float progress,
                             int duration_sec, const std::string &artwork_b64) {
  std::string json = "{";
  json += "\"playing\":";
  json += playing ? "true" : "false";
  json += ",\"title\":\"" + json_escape(title) + "\"";
  json += ",\"artist\":\"" + json_escape(artist) + "\"";
  json += ",\"progress\":" + std::to_string(progress);
  json += ",\"duration_sec\":" + std::to_string(duration_sec);
  if (!artwork_b64.empty()) {
    json += ",\"album_art_b64\":\"" + artwork_b64 + "\"";
  }
  json += "}";
  return json;
}

std::string read_artwork_base64(
    const GlobalSystemMediaTransportControlsSessionMediaProperties &props) {
  auto thumb = props.Thumbnail();
  if (!thumb)
    return {};

  auto stream = thumb.OpenReadAsync().get();
  if (!stream)
    return {};

  uint64_t size64 = stream.Size();
  if (size64 == 0 || size64 > (16 * 1024 * 1024))
    return {};

  auto input = stream.GetInputStreamAt(0);
  DataReader reader(input);
  reader.LoadAsync(static_cast<uint32_t>(size64)).get();

  std::vector<uint8_t> bytes(static_cast<size_t>(size64));
  if (!bytes.empty()) {
    reader.ReadBytes(winrt::array_view<uint8_t>(bytes));
  }

  return to_string(CryptographicBuffer::EncodeToBase64String(
      CryptographicBuffer::CreateFromByteArray(
          winrt::array_view<const uint8_t>(bytes))));
}

struct Snapshot {
  std::string json;
  std::string track_key;
};

class MediaImpl {
public:
  std::mutex mutex;
  WinMediaController::StateCallback callback;
  GlobalSystemMediaTransportControlsSessionManager manager{nullptr};
  GlobalSystemMediaTransportControlsSession session{nullptr};
  event_token manager_current_token{};
  event_token manager_sessions_token{};
  event_token media_changed_token{};
  event_token playback_changed_token{};
  event_token timeline_changed_token{};
  std::thread poll_thread;
  std::atomic<bool> running{false};
  std::atomic<uint64_t> generation{0};
  std::string last_track_key;
  std::chrono::steady_clock::time_point last_emit_time{};
  static constexpr auto kEmitMinInterval = std::chrono::milliseconds(250);

  void unregister_session_locked() {
    if (session) {
      if (media_changed_token.value) {
        session.MediaPropertiesChanged(media_changed_token);
        media_changed_token = {};
      }
      if (playback_changed_token.value) {
        session.PlaybackInfoChanged(playback_changed_token);
        playback_changed_token = {};
      }
      if (timeline_changed_token.value) {
        session.TimelinePropertiesChanged(timeline_changed_token);
        timeline_changed_token = {};
      }
      session = nullptr;
    }
  }

  void register_session_locked() {
    unregister_session_locked();
    if (!manager)
      return;

    session = manager.GetCurrentSession();
    if (!session)
      return;

    media_changed_token = session.MediaPropertiesChanged(
        [this](auto &&, auto &&) { emit_state(true); });
    playback_changed_token = session.PlaybackInfoChanged(
        [this](auto &&, auto &&) { emit_state_throttled(false); });
    timeline_changed_token = session.TimelinePropertiesChanged(
        [this](auto &&, auto &&) { emit_state_throttled(false); });
  }

  Snapshot build_snapshot(bool include_artwork) {
    ScopedApartment apartment;
    GlobalSystemMediaTransportControlsSession active_session{nullptr};
    {
      std::lock_guard<std::mutex> lock(mutex);
      active_session = session;
    }

    if (!active_session) {
      return {build_media_json(false, "", "", 0.0f, 0, ""), ""};
    }

    try {
      auto props = active_session.TryGetMediaPropertiesAsync().get();
      auto timeline = active_session.GetTimelineProperties();
      auto playback = active_session.GetPlaybackInfo();

      std::string title = to_string(props.Title());
      std::string artist = to_string(props.Artist());

      auto start = timeline.StartTime();
      auto end = timeline.EndTime();
      auto position = timeline.Position();

      int duration_sec = 0;
      float progress = 0.0f;
      if (end > start) {
        auto duration = end - start;
        duration_sec = static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(duration).count());
        auto played = position - start;
        double denom = static_cast<double>(duration.count());
        if (denom > 0.0) {
          progress = static_cast<float>(std::clamp(
              static_cast<double>(played.count()) / denom, 0.0, 1.0));
        }
      }

      bool playing =
          playback.PlaybackStatus() ==
          GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;

      std::string track_key = to_string(active_session.SourceAppUserModelId()) +
                              "|" + title + "|" + artist + "|" +
                              std::to_string(duration_sec);

      std::string artwork_b64;
      {
        std::lock_guard<std::mutex> lock(mutex);
        if (include_artwork || track_key != last_track_key) {
          artwork_b64 = read_artwork_base64(props);
        }
      }

      return {build_media_json(playing, title, artist, progress, duration_sec,
                               artwork_b64),
              std::move(track_key)};
    } catch (const hresult_error &e) {
      fprintf(stderr, "[media] snapshot failed: %ls\n", e.message().c_str());
    } catch (...) {
      fprintf(stderr, "[media] snapshot failed with unknown exception\n");
    }

    return {build_media_json(false, "", "", 0.0f, 0, ""), ""};
  }

  void emit_state(bool include_artwork) {
    auto snapshot = build_snapshot(include_artwork);
    WinMediaController::StateCallback cb;
    {
      std::lock_guard<std::mutex> lock(mutex);
      last_track_key = snapshot.track_key;
      last_emit_time = std::chrono::steady_clock::now();
      cb = callback;
    }
    if (cb) {
      cb(snapshot.json);
    }
  }

  void emit_state_throttled(bool include_artwork) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      auto now = std::chrono::steady_clock::now();
      if (now - last_emit_time < kEmitMinInterval)
        return;
    }
    emit_state(include_artwork);
  }

  void schedule_refresh_burst() {
    emit_state(false);

    const uint64_t gen = generation.load();
    std::thread([this, gen]() {
      ScopedApartment apartment;
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      if (!running.load() || generation.load() != gen)
        return;
      emit_state(false);
      std::this_thread::sleep_for(std::chrono::milliseconds(350));
      if (!running.load() || generation.load() != gen)
        return;
      emit_state(false);
    }).detach();
  }
};

} // namespace

struct WinMediaController::Impl : MediaImpl {};

WinMediaController::WinMediaController() : impl_(std::make_unique<Impl>()) {}

WinMediaController::~WinMediaController() { shutdown(); }

bool WinMediaController::init() {
  if (!impl_)
    return false;

  try {
    impl_->manager =
        GlobalSystemMediaTransportControlsSessionManager::RequestAsync().get();
    if (!impl_->manager) {
      fprintf(stderr,
              "[media] GlobalSystemMediaTransportControlsSessionManager "
              "unavailable\n");
      return false;
    }

    impl_->manager_current_token =
        impl_->manager.CurrentSessionChanged([this](auto &&, auto &&) {
          {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->register_session_locked();
          }
          impl_->emit_state(true);
        });
    impl_->manager_sessions_token =
        impl_->manager.SessionsChanged([this](auto &&, auto &&) {
          {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->register_session_locked();
          }
          impl_->emit_state(true);
        });

    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      impl_->register_session_locked();
    }

    impl_->running.store(true);
    impl_->poll_thread = std::thread([this]() {
      ScopedApartment apartment;
      while (impl_->running.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        if (!impl_->running.load())
          break;
        impl_->emit_state(false);
      }
    });

    impl_->emit_state(true);
    return true;
  } catch (const hresult_error &e) {
    fprintf(stderr, "[media] init failed: %ls\n", e.message().c_str());
  } catch (...) {
    fprintf(stderr, "[media] init failed with unknown exception\n");
  }

  return false;
}

void WinMediaController::shutdown() {
  if (!impl_)
    return;

  impl_->running.store(false);
  impl_->generation.fetch_add(1);
  if (impl_->poll_thread.joinable()) {
    impl_->poll_thread.join();
  }

  std::lock_guard<std::mutex> lock(impl_->mutex);
  if (impl_->manager && impl_->manager_current_token.value) {
    impl_->manager.CurrentSessionChanged(impl_->manager_current_token);
    impl_->manager_current_token = {};
  }
  if (impl_->manager && impl_->manager_sessions_token.value) {
    impl_->manager.SessionsChanged(impl_->manager_sessions_token);
    impl_->manager_sessions_token = {};
  }
  impl_->unregister_session_locked();
  impl_->manager = nullptr;
  impl_->callback = nullptr;
  impl_->last_track_key.clear();
}

void WinMediaController::set_state_callback(StateCallback cb) {
  if (!impl_)
    return;
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->callback = std::move(cb);
}

void WinMediaController::push_current_state(bool include_artwork) {
  if (!impl_)
    return;
  impl_->emit_state(include_artwork);
}

void WinMediaController::schedule_refresh_burst() {
  if (!impl_)
    return;
  impl_->schedule_refresh_burst();
}

bool WinMediaController::toggle_play_pause() {
  if (!impl_)
    return false;
  try {
    ScopedApartment apartment;
    GlobalSystemMediaTransportControlsSession session{nullptr};
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      session = impl_->session;
    }
    if (!session)
      return false;
    bool ok = session.TryTogglePlayPauseAsync().get();
    if (ok)
      schedule_refresh_burst();
    return ok;
  } catch (const hresult_error &e) {
    fprintf(stderr, "[media] TryTogglePlayPauseAsync failed: %ls\n",
            e.message().c_str());
  }
  return false;
}

bool WinMediaController::skip_previous() {
  if (!impl_)
    return false;
  try {
    ScopedApartment apartment;
    GlobalSystemMediaTransportControlsSession session{nullptr};
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      session = impl_->session;
    }
    if (!session)
      return false;
    bool ok = session.TrySkipPreviousAsync().get();
    if (ok)
      schedule_refresh_burst();
    return ok;
  } catch (const hresult_error &e) {
    fprintf(stderr, "[media] TrySkipPreviousAsync failed: %ls\n",
            e.message().c_str());
  }
  return false;
}

bool WinMediaController::skip_next() {
  if (!impl_)
    return false;
  try {
    ScopedApartment apartment;
    GlobalSystemMediaTransportControlsSession session{nullptr};
    {
      std::lock_guard<std::mutex> lock(impl_->mutex);
      session = impl_->session;
    }
    if (!session)
      return false;
    bool ok = session.TrySkipNextAsync().get();
    if (ok)
      schedule_refresh_burst();
    return ok;
  } catch (const hresult_error &e) {
    fprintf(stderr, "[media] TrySkipNextAsync failed: %ls\n",
            e.message().c_str());
  }
  return false;
}

} // namespace droidscreen
