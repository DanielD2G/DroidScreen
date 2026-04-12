/*
 * DroidScreen Windows - System Tray Application
 *
 * A Win32 system-tray app that captures the screen via WGC, encodes with
 * FFmpeg (hw auto-detect + software fallback), and streams H.264 to an
 * Android tablet over USB (adb forward).
 *
 * Mirrors the macOS menu-bar app functionality:
 *   - System tray icon with right-click popup menu
 *   - Settings dialog (FPS, bitrate, display, port, touch toggle)
 *   - Background streaming thread
 *   - ADB device detection and auto-reconnect
 *   - Stats health monitoring via timers
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <dxgi1_2.h>
#include <shellapi.h>
#include <shlobj.h>
#include <wrl/client.h>

#include "droidscreen/deck_manager.h"
#include "droidscreen/pipeline.h"
#include "droidscreen/server.h"
#include "ffmpeg_encoder.h"
#include "mouse_injector_win.h"
#include "touch_injector_win.h"
#include "virtual_display_win.h"
#include "wgc_capturer.h"
#include "win_app_launcher.h"
#include "win_media_controller.h"
#include "win_volume_controller.h"

extern "C" {
#include "droidscreen/handshake.h"
#include "droidscreen/protocol.h"
}

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <io.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Required for WinRT initialization.
#include <winrt/Windows.Data.Json.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/base.h>

// ============================================================================
// Constants
// ============================================================================

static const wchar_t *kAppName = L"DroidScreen";
static const wchar_t *kWindowClass = L"DroidScreenHiddenWnd";
static const wchar_t *kRegistryKey = L"Software\\DroidScreen";

// Message IDs.
static constexpr UINT WM_TRAYICON = WM_APP + 1;
static constexpr UINT WM_UPDATE_UI = WM_APP + 2; // wParam = update type

// Timer IDs.
static constexpr UINT_PTR TIMER_STATS = 1;
static constexpr UINT_PTR TIMER_DEVICE = 2;

// Menu item IDs.
static constexpr UINT IDM_TITLE = 4000;
static constexpr UINT IDM_STATUS = 4001;
static constexpr UINT IDM_SETTINGS = 4002;
static constexpr UINT IDM_CONNECT = 4003;
static constexpr UINT IDM_QUIT = 4004;
static constexpr UINT IDM_DECK = 4005;

// Settings dialog control IDs.
static constexpr UINT IDC_FPS_COMBO = 5001;
static constexpr UINT IDC_BITRATE_COMBO = 5002;
static constexpr UINT IDC_DISPLAY_COMBO = 5003;
static constexpr UINT IDC_PORT_EDIT = 5004;
static constexpr UINT IDC_TOUCH_CHECK = 5005;
static constexpr UINT IDC_APPLY_BTN = 5006;
static constexpr UINT IDC_AUTODETECT_BTN = 5007;
static constexpr UINT IDC_DECK_LIST = 5100;
static constexpr UINT IDC_DECK_ADD = 5101;
static constexpr UINT IDC_DECK_EDIT = 5102;
static constexpr UINT IDC_DECK_REMOVE = 5103;
static constexpr UINT IDC_DECK_CLOSE = 5104;
static constexpr UINT IDC_EDITOR_LABEL = 5200;
static constexpr UINT IDC_EDITOR_KIND = 5201;
static constexpr UINT IDC_EDITOR_TARGET = 5202;
static constexpr UINT IDC_EDITOR_ARGS = 5203;
static constexpr UINT IDC_EDITOR_WORKDIR = 5204;
static constexpr UINT IDC_EDITOR_BROWSE = 5205;
static constexpr UINT IDC_EDITOR_SAVE = 5206;
static constexpr UINT IDC_EDITOR_CANCEL = 5207;

// Bitrate presets (kbps).
static const int kBitrates[] = {5000, 10000, 15000, 20000, 25000, 30000};
static const wchar_t *kBitrateLabels[] = {L"5 Mbps",  L"10 Mbps", L"15 Mbps",
                                          L"20 Mbps", L"25 Mbps", L"30 Mbps"};
static constexpr int kBitrateCount = sizeof(kBitrates) / sizeof(kBitrates[0]);

// ============================================================================
// Settings (persisted in Windows Registry)
// ============================================================================

struct Settings {
  uint32_t fps = 30;
  uint32_t bitrate_kbps = 15000;
  uint32_t display = 0;
  uint16_t port = 38271;
  bool touch = true;
};

struct DeckAppEntry {
  std::string id;
  std::string label;
  std::string launch_kind;
  std::string launch_target;
  std::string launch_args;
  std::string working_dir;
  std::string icon_png_b64;
};

struct DisplayInfo {
  std::wstring name;
  uint32_t width;
  uint32_t height;
};

struct AppState {
  HWND hwnd = nullptr;
  HINSTANCE hinstance = nullptr;
  NOTIFYICONDATAW nid = {};
  HWND settingsDialog = nullptr;
  HWND deckDialog = nullptr;
  HWND deckEditorDialog = nullptr;

  // Streaming state.
  std::atomic<bool> isStreaming{false};
  std::atomic<bool> isBusy{false};
  std::atomic<bool> wantQuit{false};
  std::atomic<bool> userDisconnected{false};

  // Pipeline components.
  std::unique_ptr<droidscreen::VirtualDisplayWin> vdisplay;
  std::unique_ptr<droidscreen::WGCCapturer> capturer;
  std::unique_ptr<droidscreen::FFmpegEncoder> encoder;
  std::unique_ptr<droidscreen::TCPClient> client;
  std::unique_ptr<droidscreen::WinMouseInjector> mouse;
  std::unique_ptr<droidscreen::WinTouchInjector> touch;
  std::unique_ptr<droidscreen::DeckManager> deckManager;
  std::unique_ptr<droidscreen::Pipeline> pipeline;
  std::unique_ptr<droidscreen::WinVolumeController> volumeController;
  std::unique_ptr<droidscreen::WinMediaController> mediaController;

  // Worker thread for streaming operations.
  std::thread streamThread;
  std::mutex streamMutex;

  // Current streaming info for status display.
  uint32_t streamWidth = 0;
  uint32_t streamHeight = 0;
  uint32_t streamFPS = 0;

  // Status text displayed in the tray menu.
  wchar_t statusText[256] = L"Disconnected";

  // Cached display list and deck state.
  std::vector<DisplayInfo> displays;
  std::vector<DeckAppEntry> deckApps;
  bool deckAppsLoaded = false;
};

static AppState g_app;
static void log_msg(const char *fmt, ...);

static Settings load_settings() {
  Settings s;
  HKEY hkey;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &hkey) ==
      ERROR_SUCCESS) {
    DWORD val, sz;

    sz = sizeof(val);
    if (RegQueryValueExW(hkey, L"FPS", nullptr, nullptr, (BYTE *)&val, &sz) ==
        ERROR_SUCCESS)
      s.fps = val;

    sz = sizeof(val);
    if (RegQueryValueExW(hkey, L"Bitrate", nullptr, nullptr, (BYTE *)&val,
                         &sz) == ERROR_SUCCESS)
      s.bitrate_kbps = val;

    sz = sizeof(val);
    if (RegQueryValueExW(hkey, L"Display", nullptr, nullptr, (BYTE *)&val,
                         &sz) == ERROR_SUCCESS)
      s.display = val;

    sz = sizeof(val);
    if (RegQueryValueExW(hkey, L"Port", nullptr, nullptr, (BYTE *)&val, &sz) ==
        ERROR_SUCCESS)
      s.port = (uint16_t)val;

    sz = sizeof(val);
    if (RegQueryValueExW(hkey, L"Touch", nullptr, nullptr, (BYTE *)&val, &sz) ==
        ERROR_SUCCESS)
      s.touch = (val != 0);

    RegCloseKey(hkey);
  }
  return s;
}

static void save_settings(const Settings &s) {
  HKEY hkey;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr,
                      REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hkey,
                      nullptr) == ERROR_SUCCESS) {
    DWORD val;

    val = s.fps;
    RegSetValueExW(hkey, L"FPS", 0, REG_DWORD, (BYTE *)&val, sizeof(val));

    val = s.bitrate_kbps;
    RegSetValueExW(hkey, L"Bitrate", 0, REG_DWORD, (BYTE *)&val, sizeof(val));

    val = s.display;
    RegSetValueExW(hkey, L"Display", 0, REG_DWORD, (BYTE *)&val, sizeof(val));

    val = (DWORD)s.port;
    RegSetValueExW(hkey, L"Port", 0, REG_DWORD, (BYTE *)&val, sizeof(val));

    val = s.touch ? 1 : 0;
    RegSetValueExW(hkey, L"Touch", 0, REG_DWORD, (BYTE *)&val, sizeof(val));

    RegCloseKey(hkey);
  }
}

static std::string local_app_data_dir() {
  char localAppData[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                 localAppData))) {
    std::string dir = std::string(localAppData) + "\\DroidScreen";
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
  }
  return ".";
}

static std::string deck_config_path() {
  return local_app_data_dir() + "\\deck.json";
}

static std::wstring utf8_to_wstring(const std::string &value) {
  if (value.empty())
    return {};
  int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
  if (len <= 1)
    return {};
  std::wstring out(static_cast<size_t>(len), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, out.data(), len);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

static std::string wstring_to_utf8(const std::wstring &value) {
  if (value.empty())
    return {};
  int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0,
                                nullptr, nullptr);
  if (len <= 1)
    return {};
  std::string out(static_cast<size_t>(len), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), len, nullptr,
                      nullptr);
  out.resize(static_cast<size_t>(len - 1));
  return out;
}

static std::string derive_deck_label(const DeckAppEntry &entry) {
  if (!entry.label.empty())
    return entry.label;
  if (entry.launch_kind == "protocol" &&
      entry.launch_target == "ms-settings:") {
    return "Settings";
  }

  std::wstring target = utf8_to_wstring(entry.launch_target);
  size_t slash = target.find_last_of(L"\\/");
  std::wstring base =
      (slash == std::wstring::npos) ? target : target.substr(slash + 1);
  size_t dot = base.find_last_of(L'.');
  if (dot != std::wstring::npos)
    base = base.substr(0, dot);
  std::string utf8 = wstring_to_utf8(base);
  return utf8.empty() ? "Shortcut" : utf8;
}

static std::string make_deck_tile_id(const std::string &label) {
  std::string slug = "app_";
  for (char c : label) {
    if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
      slug.push_back(c);
    } else if (c >= 'A' && c <= 'Z') {
      slug.push_back(static_cast<char>(c - 'A' + 'a'));
    } else if (c == ' ' || c == '-' || c == '_') {
      if (!slug.empty() && slug.back() != '_')
        slug.push_back('_');
    }
  }
  if (slug == "app_" || slug.empty()) {
    slug += "shortcut";
  }
  return slug;
}

static bool file_exists_utf8(const std::string &path) {
  std::wstring wide = utf8_to_wstring(path);
  return !wide.empty() &&
         GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

static std::string get_known_folder_utf8(int csidl) {
  char buffer[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, csidl, nullptr, 0, buffer))) {
    return buffer;
  }
  return {};
}

static std::string detect_spotify_target() {
  std::string roaming = get_known_folder_utf8(CSIDL_APPDATA);
  if (!roaming.empty()) {
    std::string path = roaming + "\\Spotify\\Spotify.exe";
    if (file_exists_utf8(path))
      return path;
  }
  return {};
}

static std::string detect_edge_target() {
  const char *envs[] = {"ProgramFiles(x86)", "ProgramFiles"};
  for (const char *env_name : envs) {
    char buffer[MAX_PATH] = {};
    DWORD len = GetEnvironmentVariableA(env_name, buffer, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
      continue;
    std::string path =
        std::string(buffer) + "\\Microsoft\\Edge\\Application\\msedge.exe";
    if (file_exists_utf8(path))
      return path;
  }
  return {};
}

static std::vector<DeckAppEntry> default_deck_apps() {
  std::vector<DeckAppEntry> apps;

  DeckAppEntry explorer;
  explorer.label = "Explorer";
  explorer.launch_kind = "exe";
  explorer.launch_target = "C:\\Windows\\explorer.exe";
  apps.push_back(explorer);

  DeckAppEntry terminal;
  terminal.label = "Terminal";
  terminal.launch_kind = "exe";
  terminal.launch_target = "wt.exe";
  apps.push_back(terminal);

  DeckAppEntry settings;
  settings.label = "Settings";
  settings.launch_kind = "protocol";
  settings.launch_target = "ms-settings:";
  apps.push_back(settings);

  DeckAppEntry fourth;
  std::string spotify = detect_spotify_target();
  if (!spotify.empty()) {
    fourth.label = "Spotify";
    fourth.launch_kind = "exe";
    fourth.launch_target = spotify;
  } else {
    fourth.label = "Edge";
    fourth.launch_kind = "exe";
    fourth.launch_target = detect_edge_target();
    if (fourth.launch_target.empty()) {
      fourth.launch_target = "msedge.exe";
    }
  }
  apps.push_back(fourth);

  return apps;
}

static void normalize_deck_app(DeckAppEntry &entry) {
  entry.label = derive_deck_label(entry);
  if (entry.id.empty()) {
    entry.id = make_deck_tile_id(entry.label);
  }
  if (entry.icon_png_b64.empty()) {
    entry.icon_png_b64 = droidscreen::WinAppLauncher::icon_png_base64(
        entry.launch_kind, entry.launch_target);
  }
}

static bool save_deck_apps() {
  using namespace winrt::Windows::Data::Json;

  JsonObject root;
  JsonArray apps_json;
  for (const auto &app : g_app.deckApps) {
    JsonObject obj;
    obj.SetNamedValue(L"id",
                      JsonValue::CreateStringValue(winrt::to_hstring(app.id)));
    obj.SetNamedValue(
        L"label", JsonValue::CreateStringValue(winrt::to_hstring(app.label)));
    obj.SetNamedValue(L"launch_kind", JsonValue::CreateStringValue(
                                          winrt::to_hstring(app.launch_kind)));
    obj.SetNamedValue(
        L"launch_target",
        JsonValue::CreateStringValue(winrt::to_hstring(app.launch_target)));
    obj.SetNamedValue(L"launch_args", JsonValue::CreateStringValue(
                                          winrt::to_hstring(app.launch_args)));
    obj.SetNamedValue(L"working_dir", JsonValue::CreateStringValue(
                                          winrt::to_hstring(app.working_dir)));
    obj.SetNamedValue(
        L"icon_png_b64",
        JsonValue::CreateStringValue(winrt::to_hstring(app.icon_png_b64)));
    apps_json.Append(obj);
  }
  root.SetNamedValue(L"apps", apps_json);

  std::ofstream out(deck_config_path(), std::ios::binary | std::ios::trunc);
  if (!out.is_open())
    return false;

  std::string json = winrt::to_string(root.Stringify());
  out.write(json.data(), static_cast<std::streamsize>(json.size()));
  return out.good();
}

static void load_deck_apps() {
  if (g_app.deckAppsLoaded)
    return;
  g_app.deckAppsLoaded = true;
  g_app.deckApps.clear();

  std::ifstream in(deck_config_path(), std::ios::binary);
  if (in.is_open()) {
    std::string json((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());
    if (!json.empty()) {
      try {
        using namespace winrt::Windows::Data::Json;
        JsonObject root = JsonObject::Parse(winrt::to_hstring(json));
        if (root.HasKey(L"apps")) {
          JsonArray apps = root.GetNamedArray(L"apps");
          for (uint32_t i = 0; i < apps.Size(); ++i) {
            JsonObject obj = apps.GetObjectAt(i);
            DeckAppEntry entry;
            entry.id = winrt::to_string(obj.GetNamedString(L"id", L""));
            entry.label = winrt::to_string(obj.GetNamedString(L"label", L""));
            entry.launch_kind =
                winrt::to_string(obj.GetNamedString(L"launch_kind", L"exe"));
            entry.launch_target =
                winrt::to_string(obj.GetNamedString(L"launch_target", L""));
            entry.launch_args =
                winrt::to_string(obj.GetNamedString(L"launch_args", L""));
            entry.working_dir =
                winrt::to_string(obj.GetNamedString(L"working_dir", L""));
            entry.icon_png_b64 =
                winrt::to_string(obj.GetNamedString(L"icon_png_b64", L""));
            normalize_deck_app(entry);
            if (!entry.launch_target.empty()) {
              g_app.deckApps.push_back(std::move(entry));
            }
          }
        }
      } catch (const winrt::hresult_error &e) {
        log_msg("[deck] Failed to parse deck.json: %ls", e.message().c_str());
      }
    }
  }

  if (g_app.deckApps.empty()) {
    g_app.deckApps = default_deck_apps();
    for (auto &app : g_app.deckApps) {
      normalize_deck_app(app);
    }
    save_deck_apps();
  }
}

static droidscreen::DeckLayout build_windows_deck_layout() {
  load_deck_apps();

  droidscreen::DeckLayout layout;
  layout.grid_cols = 4;
  layout.grid_rows = 2;

  droidscreen::DeckTile media;
  media.id = "media";
  media.type = "media";
  media.label = "Now Playing";
  media.row = 0;
  media.col = 0;
  media.col_span = 2;
  layout.tiles.push_back(std::move(media));

  droidscreen::DeckTile volume;
  volume.id = "volume";
  volume.type = "volume";
  volume.label = "Volume";
  volume.row = 0;
  volume.col = 2;
  volume.col_span = 1;
  layout.tiles.push_back(std::move(volume));

  for (size_t i = 0; i < g_app.deckApps.size(); ++i) {
    const auto &app = g_app.deckApps[i];
    droidscreen::DeckTile tile;
    tile.id = app.id;
    tile.type = "app";
    tile.label = app.label;
    tile.icon_b64 = app.icon_png_b64;
    tile.launch_kind = app.launch_kind;
    tile.launch_target = app.launch_target;
    tile.launch_args = app.launch_args;
    tile.working_dir = app.working_dir;
    tile.row = 1 + static_cast<int>(i / 4);
    tile.col = static_cast<int>(i % 4);
    tile.col_span = 1;
    layout.tiles.push_back(std::move(tile));
  }

  int app_rows = static_cast<int>((g_app.deckApps.size() + 3) / 4);
  layout.grid_rows = std::max(1, 1 + app_rows);
  return layout;
}

static void rebuild_deck_layout() {
  if (!g_app.deckManager)
    return;
  g_app.deckManager->set_layout(build_windows_deck_layout());
  if (g_app.pipeline) {
    g_app.pipeline->send_deck_config();
  }
}

struct ScopedWinrtApartment {
  bool initialized = false;

  ScopedWinrtApartment() {
    try {
      winrt::init_apartment(winrt::apartment_type::multi_threaded);
      initialized = true;
    } catch (const winrt::hresult_error &e) {
      if (e.code() != RPC_E_CHANGED_MODE) {
        throw;
      }
    }
  }

  ~ScopedWinrtApartment() {
    if (initialized) {
      winrt::uninit_apartment();
    }
  }
};

static std::vector<DisplayInfo> enumerate_displays() {
  std::vector<DisplayInfo> displays;

  Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
    return displays;
  }

  Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
  for (UINT ai = 0;
       factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
    Microsoft::WRL::ComPtr<IDXGIOutput> output;
    for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND;
         ++oi) {
      DXGI_OUTPUT_DESC desc;
      if (SUCCEEDED(output->GetDesc(&desc))) {
        LONG w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
        LONG h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

        wchar_t label[256];
        _snwprintf_s(label, _TRUNCATE, L"Display %u: %s (%ldx%ld)",
                     (unsigned)displays.size(), desc.DeviceName, w, h);

        displays.push_back({label, (uint32_t)w, (uint32_t)h});
      }
      output.Reset();
    }
    adapter.Reset();
  }

  return displays;
}

// ============================================================================
// ADB Helpers (using CreateProcess)
// ============================================================================

static std::string g_adb_path;
static HANDLE g_singleInstanceMutex = nullptr;

/// Run a process and capture stdout. Returns exit code, -1 on failure.
static int run_process(const std::string &cmd, std::string *output = nullptr) {
  SECURITY_ATTRIBUTES sa = {};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = TRUE;

  HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
  if (output) {
    if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0))
      return -1;
    SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
  }

  STARTUPINFOA si = {};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  if (output) {
    si.dwFlags |= STARTF_USESTDHANDLES;
    si.hStdOutput = hWritePipe;
    si.hStdError = hWritePipe;
  }

  PROCESS_INFORMATION pi = {};
  // CreateProcessA needs a mutable command line buffer.
  std::string cmd_buf = cmd;

  BOOL ok = CreateProcessA(nullptr, cmd_buf.data(), nullptr, nullptr,
                           output ? TRUE : FALSE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &si, &pi);

  if (output && hWritePipe)
    CloseHandle(hWritePipe);

  if (!ok) {
    if (output && hReadPipe)
      CloseHandle(hReadPipe);
    return -1;
  }

  if (output && hReadPipe) {
    output->clear();
    char buf[4096];
    DWORD bytesRead;
    while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) &&
           bytesRead > 0) {
      output->append(buf, bytesRead);
    }
    CloseHandle(hReadPipe);
  }

  WaitForSingleObject(pi.hProcess, 10000);
  DWORD exitCode = 0;
  GetExitCodeProcess(pi.hProcess, &exitCode);
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return (int)exitCode;
}

/// Locate the adb binary. Caches result.
static const std::string &adb_find_path() {
  if (!g_adb_path.empty())
    return g_adb_path;

  // 1. Bundled ADB in the NSIS install dir (reads InstallDir from registry).
  //    The NSIS installer writes: HKCU\Software\DroidScreen "InstallDir"
  //    and installs adb to: <InstallDir>\adb\adb.exe
  {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, KEY_READ, &hk) ==
        ERROR_SUCCESS) {
      wchar_t installDir[MAX_PATH] = {};
      DWORD sz = sizeof(installDir);
      if (RegQueryValueExW(hk, L"InstallDir", nullptr, nullptr,
                           reinterpret_cast<BYTE *>(installDir), &sz) ==
          ERROR_SUCCESS) {
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_ACP, 0, installDir, -1, narrow, MAX_PATH,
                            nullptr, nullptr);
        std::string bundledAdb = std::string(narrow) + "\\adb\\adb.exe";
        if (GetFileAttributesA(bundledAdb.c_str()) != INVALID_FILE_ATTRIBUTES) {
          g_adb_path = bundledAdb;
          RegCloseKey(hk);
          return g_adb_path;
        }
      }
      RegCloseKey(hk);
    }
  }

  // Check candidates.
  const char *candidates[] = {
      nullptr, // placeholder for
               // %LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
      "C:\\Program Files\\Android\\Android Studio\\platform-tools\\adb.exe",
  };

  // Build %LOCALAPPDATA% path.
  char localAppData[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                 localAppData))) {
    std::string sdkAdb =
        std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
    if (GetFileAttributesA(sdkAdb.c_str()) != INVALID_FILE_ATTRIBUTES) {
      g_adb_path = sdkAdb;
      return g_adb_path;
    }
  }

  for (int i = 1; i < (int)(sizeof(candidates) / sizeof(candidates[0])); i++) {
    if (GetFileAttributesA(candidates[i]) != INVALID_FILE_ATTRIBUTES) {
      g_adb_path = candidates[i];
      return g_adb_path;
    }
  }

  // Try PATH via `where adb`.
  std::string output;
  if (run_process("where adb", &output) == 0) {
    // First line is the path.
    size_t nl = output.find_first_of("\r\n");
    std::string path =
        (nl != std::string::npos) ? output.substr(0, nl) : output;
    if (!path.empty() &&
        GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
      g_adb_path = path;
      return g_adb_path;
    }
  }

  // Fallback: just try "adb" and hope it's on PATH.
  g_adb_path = "adb";
  return g_adb_path;
}

static int adb_run(const std::string &args, std::string *output = nullptr) {
  std::string cmd = "\"" + adb_find_path() + "\" " + args;
  return run_process(cmd, output);
}

static bool adb_forward_setup(uint16_t port) {
  char args[128];
  snprintf(args, sizeof(args), "forward tcp:%u tcp:%u", port, port);
  int ret = adb_run(args);
  if (ret != 0) {
    OutputDebugStringA("[ADB] adb forward failed\n");
    return false;
  }
  return true;
}

static void adb_forward_remove(uint16_t port) {
  char args[128];
  snprintf(args, sizeof(args), "forward --remove tcp:%u", port);
  adb_run(args);
}

/// Parse "Physical size: WxH" from `adb shell wm size`.
/// Returns {width, height} in landscape orientation, or {0,0} on failure.
static std::pair<uint32_t, uint32_t> adb_device_resolution() {
  std::string output;
  if (adb_run("shell wm size", &output) != 0) {
    return {0, 0};
  }

  // Look for "Physical size: WxH"
  const char *prefix = "Physical size:";
  size_t pos = output.find(prefix);
  if (pos == std::string::npos)
    return {0, 0};

  pos += strlen(prefix);
  // Skip whitespace.
  while (pos < output.size() && (output[pos] == ' ' || output[pos] == '\t'))
    pos++;

  int a = 0, b = 0;
  if (sscanf(output.c_str() + pos, "%dx%d", &a, &b) != 2 || a <= 0 || b <= 0) {
    return {0, 0};
  }

  // Return as landscape (wider first).
  uint32_t w = (a >= b) ? (uint32_t)a : (uint32_t)b;
  uint32_t h = (a >= b) ? (uint32_t)b : (uint32_t)a;
  return {w, h};
}

/// Returns true if `adb devices` shows at least one attached device.
static bool adb_device_connected() {
  std::string output;
  if (adb_run("devices", &output) != 0)
    return false;

  // Look for lines ending with "\tdevice"
  size_t pos = 0;
  while ((pos = output.find("\tdevice", pos)) != std::string::npos) {
    // Make sure this isn't part of the "List of devices attached" header.
    // A real device line has a serial before \tdevice.
    if (pos > 0 && output[pos - 1] != '\n' && output[pos - 1] != '\r') {
      return true;
    }
    pos++;
  }
  return false;
}

/// Returns true if the DroidScreen Android app is currently running on the
/// device.
static bool adb_droidscreen_running() {
  std::string output;
  if (adb_run("shell pidof com.droidscreen.app", &output) != 0)
    return false;
  // pidof returns the PID (a number) if the process is running, empty
  // otherwise.
  for (char c : output) {
    if (c >= '0' && c <= '9')
      return true;
  }
  return false;
}

// ============================================================================
// Debug Logging
// ============================================================================

static FILE *g_logFile = nullptr;

static void log_msg(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);

  char buf[2048];
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  OutputDebugStringA(buf);
  OutputDebugStringA("\n");

  if (g_logFile) {
    fprintf(g_logFile, "%s\n", buf);
    fflush(g_logFile);
  }
}

static void setup_logging() {
  // Log to %LOCALAPPDATA%\DroidScreen\droidscreen.log
  char localAppData[MAX_PATH] = {};
  if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0,
                                 localAppData))) {
    std::string logDir = std::string(localAppData) + "\\DroidScreen";
    CreateDirectoryA(logDir.c_str(), nullptr);
    std::string logPath = logDir + "\\droidscreen.log";
    g_logFile = fopen(logPath.c_str(), "w");
    if (g_logFile) {
      log_msg("=== DroidScreen Debug Log ===");

      // Also redirect stderr to the same log file so fprintf(stderr,...)
      // from sub-components (NVENC, WGC, pipeline, etc.) is captured.
      // In a /SUBSYSTEM:WINDOWS app, stderr goes nowhere by default.
      // Use freopen which is more reliable than _dup2 for GUI apps.
      std::string stderrPath = logDir + "\\stderr.log";
      freopen(stderrPath.c_str(), "w", stderr);
      setvbuf(stderr, nullptr, _IONBF, 0); // unbuffered
    }
  }
}

// ============================================================================
// Forward Declarations
// ============================================================================

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK DeckWndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK DeckEditorWndProc(HWND, UINT, WPARAM, LPARAM);
static void setup_tray_icon(HWND hwnd);
static void remove_tray_icon();
static void show_tray_menu();
static void show_settings_dialog();
static void show_deck_dialog();
static void connect_async();
static void disconnect_async();
static void connect_sync();
static void disconnect_sync();
static void update_status(const wchar_t *text);

// ============================================================================
// System Tray Icon
// ============================================================================

static void setup_tray_icon(HWND hwnd) {
  NOTIFYICONDATAW &nid = g_app.nid;
  memset(&nid, 0, sizeof(nid));
  nid.cbSize = sizeof(nid);
  nid.hWnd = hwnd;
  nid.uID = 1;
  nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  nid.uCallbackMessage = WM_TRAYICON;

  // Use a system application icon.
  nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
  wcscpy_s(nid.szTip, L"DroidScreen");

  Shell_NotifyIconW(NIM_ADD, &nid);
}

static void remove_tray_icon() { Shell_NotifyIconW(NIM_DELETE, &g_app.nid); }

static void show_tray_menu() {
  HMENU menu = CreatePopupMenu();

  // Title (grayed).
  AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_TITLE, L"DroidScreen");

  // Status.
  wchar_t statusLine[300];
  _snwprintf_s(statusLine, _TRUNCATE, L"Status: %s", g_app.statusText);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, IDM_STATUS, statusLine);

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  // Settings.
  AppendMenuW(menu, MF_STRING, IDM_SETTINGS, L"Settings...");

  // Deck Config.
  AppendMenuW(menu, MF_STRING, IDM_DECK, L"Deck Config...");

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  // Connect / Disconnect toggle.
  bool streaming = g_app.isStreaming.load();
  bool busy = g_app.isBusy.load();
  const wchar_t *connectLabel = streaming ? L"Disconnect" : L"Connect";
  UINT connectFlags = MF_STRING;
  if (busy)
    connectFlags |= MF_GRAYED;
  AppendMenuW(menu, connectFlags, IDM_CONNECT, connectLabel);

  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

  // Quit.
  AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

  // Show the menu at cursor position.
  POINT pt;
  GetCursorPos(&pt);
  SetForegroundWindow(g_app.hwnd);
  TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                 g_app.hwnd, nullptr);
  PostMessage(g_app.hwnd, WM_NULL, 0, 0); // Required per MSDN.

  DestroyMenu(menu);
}

// ============================================================================
// Status Update (thread-safe)
// ============================================================================

static void update_status(const wchar_t *text) {
  wcsncpy_s(g_app.statusText, text, _TRUNCATE);
  // Update tray tooltip.
  wchar_t tip[128];
  _snwprintf_s(tip, _TRUNCATE, L"DroidScreen - %s", text);
  wcscpy_s(g_app.nid.szTip, tip);
  Shell_NotifyIconW(NIM_MODIFY, &g_app.nid);
}

// ============================================================================
// Settings Dialog
// ============================================================================

static const wchar_t *kSettingsClass = L"DroidScreenSettingsWnd";
static const wchar_t *kDeckClass = L"DroidScreenDeckWnd";
static const wchar_t *kDeckEditorClass = L"DroidScreenDeckEditorWnd";

static void register_settings_class(HINSTANCE hInst) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = SettingsWndProc;
  wc.hInstance = hInst;
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kSettingsClass;
  wc.hCursor =
      LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW = 32512
  RegisterClassExW(&wc);
}

static void register_deck_classes(HINSTANCE hInst) {
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = DeckWndProc;
  wc.hInstance = hInst;
  wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
  wc.lpszClassName = kDeckClass;
  wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
  RegisterClassExW(&wc);

  wc.lpfnWndProc = DeckEditorWndProc;
  wc.lpszClassName = kDeckEditorClass;
  RegisterClassExW(&wc);
}

static void show_settings_dialog() {
  // If already open, bring to front.
  if (g_app.settingsDialog && IsWindow(g_app.settingsDialog)) {
    SetForegroundWindow(g_app.settingsDialog);
    return;
  }

  // Refresh display list.
  g_app.displays = enumerate_displays();

  const int dlgW = 420;
  const int dlgH = 380;

  // Center on screen.
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int x = (screenW - dlgW) / 2;
  int y = (screenH - dlgH) / 2;

  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSettingsClass, L"DroidScreen Settings",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, dlgW, dlgH, nullptr,
      nullptr, g_app.hinstance, nullptr);

  g_app.settingsDialog = hwnd;

  Settings s = load_settings();

  // Layout constants.
  const int leftMargin = 20;
  const int labelWidth = 80;
  const int ctrlLeft = leftMargin + labelWidth + 10;
  const int ctrlWidth = 260;
  const int rowHeight = 30;
  int cy = 20; // current Y position

  // Helper to create a label.
  auto makeLabel = [&](const wchar_t *text, int yPos) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                    leftMargin, yPos + 4, labelWidth, 20, hwnd, nullptr,
                    g_app.hinstance, nullptr);
  };

  // --- FPS ---
  makeLabel(L"Frame Rate:", cy);
  HWND fpsCombo =
      CreateWindowExW(0, L"COMBOBOX", nullptr,
                      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                      ctrlLeft, cy, ctrlWidth, 200, hwnd,
                      (HMENU)(UINT_PTR)IDC_FPS_COMBO, g_app.hinstance, nullptr);
  SendMessageW(fpsCombo, CB_ADDSTRING, 0, (LPARAM)L"30 fps");
  SendMessageW(fpsCombo, CB_ADDSTRING, 0, (LPARAM)L"60 fps");
  SendMessageW(fpsCombo, CB_ADDSTRING, 0, (LPARAM)L"120 fps");
  int fpsSel = (s.fps == 30) ? 0 : (s.fps == 120) ? 2 : 1;
  SendMessageW(fpsCombo, CB_SETCURSEL, fpsSel, 0);
  cy += rowHeight + 8;

  // --- Bitrate ---
  makeLabel(L"Bitrate:", cy);
  HWND brCombo = CreateWindowExW(
      0, L"COMBOBOX", nullptr,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, ctrlLeft, cy, 175,
      200, hwnd, (HMENU)(UINT_PTR)IDC_BITRATE_COMBO, g_app.hinstance, nullptr);
  int brSel = 2; // default 15 Mbps
  for (int i = 0; i < kBitrateCount; i++) {
    SendMessageW(brCombo, CB_ADDSTRING, 0, (LPARAM)kBitrateLabels[i]);
    if (kBitrates[i] == (int)s.bitrate_kbps)
      brSel = i;
  }
  SendMessageW(brCombo, CB_SETCURSEL, brSel, 0);
  // Auto-detect bitrate button.
  CreateWindowExW(0, L"BUTTON", L"Auto", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  ctrlLeft + 185, cy, 70, 26, hwnd,
                  (HMENU)(UINT_PTR)IDC_AUTODETECT_BTN, g_app.hinstance,
                  nullptr);
  cy += rowHeight + 8;

  // --- Display ---
  makeLabel(L"Display:", cy);
  HWND dispCombo = CreateWindowExW(
      0, L"COMBOBOX", nullptr,
      WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, ctrlLeft, cy,
      ctrlWidth, 200, hwnd, (HMENU)(UINT_PTR)IDC_DISPLAY_COMBO, g_app.hinstance,
      nullptr);
  if (g_app.displays.empty()) {
    SendMessageW(dispCombo, CB_ADDSTRING, 0, (LPARAM)L"Display 0 (default)");
  } else {
    for (auto &d : g_app.displays) {
      SendMessageW(dispCombo, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
    }
  }
  SendMessageW(dispCombo, CB_SETCURSEL,
               (s.display < (uint32_t)g_app.displays.size()) ? s.display : 0,
               0);
  cy += rowHeight + 8;

  // --- Port ---
  makeLabel(L"Port:", cy);
  HWND portEdit = CreateWindowExW(
      WS_EX_CLIENTEDGE, L"EDIT", nullptr, WS_CHILD | WS_VISIBLE | ES_NUMBER,
      ctrlLeft, cy, 100, 24, hwnd, (HMENU)(UINT_PTR)IDC_PORT_EDIT,
      g_app.hinstance, nullptr);
  {
    wchar_t portStr[16];
    _snwprintf_s(portStr, _TRUNCATE, L"%u", (unsigned)s.port);
    SetWindowTextW(portEdit, portStr);
  }
  cy += rowHeight + 8;

  // --- Touch ---
  HWND touchCheck = CreateWindowExW(
      0, L"BUTTON", L"Enable touch injection",
      WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, ctrlLeft, cy + 2, ctrlWidth, 20,
      hwnd, (HMENU)(UINT_PTR)IDC_TOUCH_CHECK, g_app.hinstance, nullptr);
  SendMessageW(touchCheck, BM_SETCHECK, s.touch ? BST_CHECKED : BST_UNCHECKED,
               0);
  makeLabel(L"Touch:", cy);
  cy += rowHeight + 16;

  // --- Apply Button ---
  CreateWindowExW(0, L"BUTTON", L"Apply", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  dlgW - 110 - 20, cy, 100, 32, hwnd,
                  (HMENU)(UINT_PTR)IDC_APPLY_BTN, g_app.hinstance, nullptr);

  // Set font for all child controls.
  HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  EnumChildWindows(
      hwnd,
      [](HWND child, LPARAM lParam) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
      },
      (LPARAM)hFont);

  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
}

static void apply_settings_from_dialog(HWND dlg) {
  Settings s;

  // FPS.
  HWND fpsCombo = GetDlgItem(dlg, IDC_FPS_COMBO);
  int fpsSel = (int)SendMessageW(fpsCombo, CB_GETCURSEL, 0, 0);
  switch (fpsSel) {
  case 0:
    s.fps = 30;
    break;
  case 2:
    s.fps = 120;
    break;
  default:
    s.fps = 60;
    break;
  }

  // Bitrate.
  HWND brCombo = GetDlgItem(dlg, IDC_BITRATE_COMBO);
  int brSel = (int)SendMessageW(brCombo, CB_GETCURSEL, 0, 0);
  if (brSel >= 0 && brSel < kBitrateCount)
    s.bitrate_kbps = kBitrates[brSel];
  else
    s.bitrate_kbps = 15000;

  // Display.
  HWND dispCombo = GetDlgItem(dlg, IDC_DISPLAY_COMBO);
  s.display = (uint32_t)SendMessageW(dispCombo, CB_GETCURSEL, 0, 0);

  // Port.
  HWND portEdit = GetDlgItem(dlg, IDC_PORT_EDIT);
  wchar_t portBuf[16] = {};
  GetWindowTextW(portEdit, portBuf, 16);
  int port = _wtoi(portBuf);
  if (port < 1 || port > 65535)
    port = 38271;
  s.port = (uint16_t)port;

  // Touch.
  HWND touchCheck = GetDlgItem(dlg, IDC_TOUCH_CHECK);
  s.touch = (SendMessageW(touchCheck, BM_GETCHECK, 0, 0) == BST_CHECKED);

  save_settings(s);
  log_msg("[Settings] Saved: fps=%u bitrate=%u display=%u port=%u touch=%d",
          s.fps, s.bitrate_kbps, s.display, s.port, s.touch);

  // If streaming, restart with new settings.
  if (g_app.isStreaming.load()) {
    log_msg("[Settings] Restarting pipeline with new settings...");
    std::thread([]() {
      disconnect_sync();
      connect_sync();
    }).detach();
  }

  DestroyWindow(dlg);
  g_app.settingsDialog = nullptr;
}

/// Perform a handshake with the given TCPClient (used by speed test).
static bool speed_test_handshake(droidscreen::TCPClient *client) {
  ds_handshake_req_t req{};
  req.protocol_version = DS_PROTOCOL_VERSION;
  req.width = 1920;
  req.height = 1200;
  req.fps = 60;
  req.codec = DS_CODEC_H264;
  req.max_bitrate_kbps = 15000;
  req.touch_enabled = 0;
  req.frame_interval_us = 16667; /* 60 fps */

  uint8_t req_buf[DS_HANDSHAKE_REQ_SIZE];
  ds_handshake_req_serialize(req_buf, &req);
  if (!client->send_message(DS_MSG_HANDSHAKE_REQ, 0, req_buf,
                            DS_HANDSHAKE_REQ_SIZE)) {
    return false;
  }

  ds_header_t hdr;
  if (!client->recv_header(&hdr))
    return false;
  if (hdr.type != DS_MSG_HANDSHAKE_RESP || hdr.length != DS_HANDSHAKE_RESP_SIZE)
    return false;

  uint8_t resp_buf[DS_HANDSHAKE_RESP_SIZE];
  return client->recv_exact(resp_buf, DS_HANDSHAKE_RESP_SIZE);
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                        LPARAM lParam) {
  switch (msg) {
  case WM_COMMAND:
    if (LOWORD(wParam) == IDC_APPLY_BTN && HIWORD(wParam) == BN_CLICKED) {
      apply_settings_from_dialog(hwnd);
      return 0;
    }
    if (LOWORD(wParam) == IDC_AUTODETECT_BTN && HIWORD(wParam) == BN_CLICKED) {
      HWND btn = GetDlgItem(hwnd, IDC_AUTODETECT_BTN);
      EnableWindow(btn, FALSE);
      SetWindowTextW(btn, L"...");

      HWND dlgCapture = hwnd;
      std::thread([dlgCapture, btn]() {
        Settings settings = load_settings();

        // Set up ADB forward.
        if (!adb_forward_setup(settings.port)) {
          PostMessage(dlgCapture, WM_APP + 10, 0, 0);
          return;
        }

        // Connect TCP.
        auto client = std::make_unique<droidscreen::TCPClient>();
        if (!client->connect(settings.port)) {
          PostMessage(dlgCapture, WM_APP + 10, 0, 0);
          return;
        }

        // Handshake.
        if (!speed_test_handshake(client.get())) {
          client->close();
          PostMessage(dlgCapture, WM_APP + 10, 0, 0);
          return;
        }

        // Run speed test (2 seconds).
        uint32_t throughput_kbps =
            droidscreen::run_speed_test(client.get(), 2000);
        client->close();

        // Select best bitrate at ~70%.
        uint32_t target_kbps = throughput_kbps * 70 / 100;
        int bestIdx = 0;
        for (int i = kBitrateCount - 1; i >= 0; i--) {
          if (kBitrates[i] <= (int)target_kbps) {
            bestIdx = i;
            break;
          }
        }

        log_msg(
            "[SpeedTest] throughput=%u kbps, target=%u kbps, selected=%d kbps",
            throughput_kbps, target_kbps, kBitrates[bestIdx]);

        // Post result back to UI thread.
        PostMessage(dlgCapture, WM_APP + 11, (WPARAM)bestIdx, 0);
      }).detach();
      return 0;
    }
    break;

  // Speed test failure — re-enable button.
  case WM_APP + 10: {
    HWND btn = GetDlgItem(hwnd, IDC_AUTODETECT_BTN);
    SetWindowTextW(btn, L"Fail");
    EnableWindow(btn, TRUE);
    return 0;
  }

  // Speed test success — update bitrate combo and re-enable button.
  case WM_APP + 11: {
    HWND brCombo = GetDlgItem(hwnd, IDC_BITRATE_COMBO);
    SendMessageW(brCombo, CB_SETCURSEL, wParam, 0);
    HWND btn = GetDlgItem(hwnd, IDC_AUTODETECT_BTN);
    SetWindowTextW(btn, L"Auto");
    EnableWindow(btn, TRUE);
    return 0;
  }

  case WM_CLOSE:
    DestroyWindow(hwnd);
    g_app.settingsDialog = nullptr;
    return 0;

  case WM_DESTROY:
    g_app.settingsDialog = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

struct DeckEditorState {
  int index = -1;
};

static void refresh_deck_list(HWND hwnd) {
  HWND list = GetDlgItem(hwnd, IDC_DECK_LIST);
  if (!list)
    return;

  SendMessageW(list, LB_RESETCONTENT, 0, 0);
  load_deck_apps();
  for (const auto &app : g_app.deckApps) {
    std::wstring row = utf8_to_wstring(app.label + " [" + app.launch_kind +
                                       "] " + app.launch_target);
    SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(row.c_str()));
  }
}

static void sync_editor_browse_button(HWND hwnd) {
  HWND kind_combo = GetDlgItem(hwnd, IDC_EDITOR_KIND);
  int sel = static_cast<int>(SendMessageW(kind_combo, CB_GETCURSEL, 0, 0));
  EnableWindow(GetDlgItem(hwnd, IDC_EDITOR_BROWSE), sel == 0);
}

static void populate_deck_editor(HWND hwnd, const DeckAppEntry *entry) {
  SetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_LABEL),
                 utf8_to_wstring(entry ? entry->label : "").c_str());
  SetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_TARGET),
                 utf8_to_wstring(entry ? entry->launch_target : "").c_str());
  SetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_ARGS),
                 utf8_to_wstring(entry ? entry->launch_args : "").c_str());
  SetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_WORKDIR),
                 utf8_to_wstring(entry ? entry->working_dir : "").c_str());

  HWND kind_combo = GetDlgItem(hwnd, IDC_EDITOR_KIND);
  const std::string kind = entry ? entry->launch_kind : "exe";
  int index = (kind == "protocol") ? 1 : (kind == "uwp") ? 2 : 0;
  SendMessageW(kind_combo, CB_SETCURSEL, index, 0);
  sync_editor_browse_button(hwnd);
}

static bool read_deck_editor(HWND hwnd, DeckAppEntry *out) {
  wchar_t label[256] = {};
  wchar_t target[1024] = {};
  wchar_t args[1024] = {};
  wchar_t workdir[1024] = {};

  GetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_LABEL), label, 256);
  GetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_TARGET), target, 1024);
  GetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_ARGS), args, 1024);
  GetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_WORKDIR), workdir, 1024);

  if (target[0] == L'\0') {
    MessageBoxW(hwnd, L"Launch target is required.", L"DroidScreen",
                MB_OK | MB_ICONWARNING);
    return false;
  }

  HWND kind_combo = GetDlgItem(hwnd, IDC_EDITOR_KIND);
  int kind_index =
      static_cast<int>(SendMessageW(kind_combo, CB_GETCURSEL, 0, 0));

  out->label = wstring_to_utf8(label);
  out->launch_kind = (kind_index == 1)   ? "protocol"
                     : (kind_index == 2) ? "uwp"
                                         : "exe";
  out->launch_target = wstring_to_utf8(target);
  out->launch_args = wstring_to_utf8(args);
  out->working_dir = wstring_to_utf8(workdir);
  out->icon_png_b64.clear();
  normalize_deck_app(*out);
  return true;
}

static void show_deck_editor_dialog(int index) {
  if (g_app.deckEditorDialog && IsWindow(g_app.deckEditorDialog)) {
    SetForegroundWindow(g_app.deckEditorDialog);
    return;
  }

  const int dlgW = 520;
  const int dlgH = 320;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int x = (screenW - dlgW) / 2;
  int y = (screenH - dlgH) / 2;

  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kDeckEditorClass,
      (index >= 0) ? L"Edit Deck Shortcut" : L"Add Deck Shortcut",
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x, y, dlgW, dlgH, nullptr,
      nullptr, g_app.hinstance, nullptr);

  auto *state = new DeckEditorState();
  state->index = index;
  SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
  g_app.deckEditorDialog = hwnd;

  const int leftMargin = 20;
  const int labelWidth = 90;
  const int ctrlLeft = leftMargin + labelWidth + 10;
  const int ctrlWidth = 360;
  const int rowHeight = 30;
  int cy = 20;

  auto makeLabel = [&](const wchar_t *text, int yPos) {
    CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_RIGHT,
                    leftMargin, yPos + 4, labelWidth, 20, hwnd, nullptr,
                    g_app.hinstance, nullptr);
  };

  makeLabel(L"Label:", cy);
  CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, ctrlLeft, cy,
                  ctrlWidth, 24, hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_LABEL,
                  g_app.hinstance, nullptr);
  cy += rowHeight + 8;

  makeLabel(L"Type:", cy);
  HWND kindCombo = CreateWindowExW(
      0, L"COMBOBOX", nullptr, WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST,
      ctrlLeft, cy, 160, 200, hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_KIND,
      g_app.hinstance, nullptr);
  SendMessageW(kindCombo, CB_ADDSTRING, 0, (LPARAM)L"Executable");
  SendMessageW(kindCombo, CB_ADDSTRING, 0, (LPARAM)L"URL / Protocol");
  SendMessageW(kindCombo, CB_ADDSTRING, 0, (LPARAM)L"UWP / AUMID");
  cy += rowHeight + 8;

  makeLabel(L"Target:", cy);
  CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, ctrlLeft, cy,
                  ctrlWidth - 90, 24, hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_TARGET,
                  g_app.hinstance, nullptr);
  CreateWindowExW(0, L"BUTTON", L"Browse...",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  ctrlLeft + ctrlWidth - 80, cy, 80, 24, hwnd,
                  (HMENU)(UINT_PTR)IDC_EDITOR_BROWSE, g_app.hinstance, nullptr);
  cy += rowHeight + 8;

  makeLabel(L"Args:", cy);
  CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, ctrlLeft, cy,
                  ctrlWidth, 24, hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_ARGS,
                  g_app.hinstance, nullptr);
  cy += rowHeight + 8;

  makeLabel(L"Work Dir:", cy);
  CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
                  WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, ctrlLeft, cy,
                  ctrlWidth, 24, hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_WORKDIR,
                  g_app.hinstance, nullptr);
  cy += rowHeight + 18;

  CreateWindowExW(0, L"BUTTON", L"Save", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  dlgW - 200, cy, 80, 28, hwnd,
                  (HMENU)(UINT_PTR)IDC_EDITOR_SAVE, g_app.hinstance, nullptr);
  CreateWindowExW(0, L"BUTTON", L"Cancel",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, dlgW - 110, cy, 80, 28,
                  hwnd, (HMENU)(UINT_PTR)IDC_EDITOR_CANCEL, g_app.hinstance,
                  nullptr);

  HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  EnumChildWindows(
      hwnd,
      [](HWND child, LPARAM lParam) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
      },
      (LPARAM)hFont);

  load_deck_apps();
  populate_deck_editor(hwnd, (index >= 0 && index < (int)g_app.deckApps.size())
                                 ? &g_app.deckApps[index]
                                 : nullptr);

  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
}

static void show_deck_dialog() {
  if (g_app.deckDialog && IsWindow(g_app.deckDialog)) {
    SetForegroundWindow(g_app.deckDialog);
    return;
  }

  load_deck_apps();

  const int dlgW = 620;
  const int dlgH = 420;
  int screenW = GetSystemMetrics(SM_CXSCREEN);
  int screenH = GetSystemMetrics(SM_CYSCREEN);
  int x = (screenW - dlgW) / 2;
  int y = (screenH - dlgH) / 2;

  HWND hwnd = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kDeckClass,
      L"DroidScreen Deck Shortcuts", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, x,
      y, dlgW, dlgH, nullptr, nullptr, g_app.hinstance, nullptr);

  g_app.deckDialog = hwnd;

  CreateWindowExW(0, L"STATIC", L"Configured shortcuts", WS_CHILD | WS_VISIBLE,
                  20, 20, 300, 20, hwnd, nullptr, g_app.hinstance, nullptr);

  CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", nullptr,
                  WS_CHILD | WS_VISIBLE | LBS_NOTIFY | WS_VSCROLL, 20, 50, 450,
                  280, hwnd, (HMENU)(UINT_PTR)IDC_DECK_LIST, g_app.hinstance,
                  nullptr);

  CreateWindowExW(0, L"BUTTON", L"Add...",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 490, 50, 100, 28, hwnd,
                  (HMENU)(UINT_PTR)IDC_DECK_ADD, g_app.hinstance, nullptr);
  CreateWindowExW(0, L"BUTTON", L"Edit...",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 490, 86, 100, 28, hwnd,
                  (HMENU)(UINT_PTR)IDC_DECK_EDIT, g_app.hinstance, nullptr);
  CreateWindowExW(0, L"BUTTON", L"Remove",
                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 490, 122, 100, 28,
                  hwnd, (HMENU)(UINT_PTR)IDC_DECK_REMOVE, g_app.hinstance,
                  nullptr);
  CreateWindowExW(0, L"BUTTON", L"Close", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                  490, 302, 100, 28, hwnd, (HMENU)(UINT_PTR)IDC_DECK_CLOSE,
                  g_app.hinstance, nullptr);

  HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  EnumChildWindows(
      hwnd,
      [](HWND child, LPARAM lParam) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
      },
      (LPARAM)hFont);

  refresh_deck_list(hwnd);
  ShowWindow(hwnd, SW_SHOW);
  SetForegroundWindow(hwnd);
}

static LRESULT CALLBACK DeckWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                    LPARAM lParam) {
  switch (msg) {
  case WM_COMMAND: {
    switch (LOWORD(wParam)) {
    case IDC_DECK_ADD:
      if (HIWORD(wParam) == BN_CLICKED) {
        show_deck_editor_dialog(-1);
        return 0;
      }
      break;
    case IDC_DECK_EDIT:
      if (HIWORD(wParam) == BN_CLICKED) {
        int index = (int)SendMessageW(GetDlgItem(hwnd, IDC_DECK_LIST),
                                      LB_GETCURSEL, 0, 0);
        if (index != LB_ERR)
          show_deck_editor_dialog(index);
        return 0;
      }
      break;
    case IDC_DECK_REMOVE:
      if (HIWORD(wParam) == BN_CLICKED) {
        int index = (int)SendMessageW(GetDlgItem(hwnd, IDC_DECK_LIST),
                                      LB_GETCURSEL, 0, 0);
        if (index != LB_ERR && index >= 0 &&
            index < (int)g_app.deckApps.size()) {
          g_app.deckApps.erase(g_app.deckApps.begin() + index);
          save_deck_apps();
          refresh_deck_list(hwnd);
          rebuild_deck_layout();
        }
        return 0;
      }
      break;
    case IDC_DECK_CLOSE:
      DestroyWindow(hwnd);
      return 0;
    case IDC_DECK_LIST:
      if (HIWORD(wParam) == LBN_DBLCLK) {
        int index = (int)SendMessageW(GetDlgItem(hwnd, IDC_DECK_LIST),
                                      LB_GETCURSEL, 0, 0);
        if (index != LB_ERR)
          show_deck_editor_dialog(index);
        return 0;
      }
      break;
    }
    break;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY:
    g_app.deckDialog = nullptr;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static LRESULT CALLBACK DeckEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                          LPARAM lParam) {
  switch (msg) {
  case WM_COMMAND: {
    switch (LOWORD(wParam)) {
    case IDC_EDITOR_KIND:
      if (HIWORD(wParam) == CBN_SELCHANGE) {
        sync_editor_browse_button(hwnd);
        return 0;
      }
      break;
    case IDC_EDITOR_BROWSE:
      if (HIWORD(wParam) == BN_CLICKED) {
        wchar_t file[MAX_PATH] = {};
        OPENFILENAMEW ofn{};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = L"Executables\0*.exe\0All Files\0*.*\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
          SetWindowTextW(GetDlgItem(hwnd, IDC_EDITOR_TARGET), file);
        }
        return 0;
      }
      break;
    case IDC_EDITOR_SAVE:
      if (HIWORD(wParam) == BN_CLICKED) {
        auto *state = reinterpret_cast<DeckEditorState *>(
            GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (!state)
          return 0;

        DeckAppEntry entry;
        if (!read_deck_editor(hwnd, &entry)) {
          return 0;
        }

        if (state->index >= 0 && state->index < (int)g_app.deckApps.size()) {
          entry.id = g_app.deckApps[state->index].id;
          g_app.deckApps[state->index] = std::move(entry);
        } else {
          int suffix = (int)g_app.deckApps.size();
          std::string base_id = entry.id;
          bool unique = false;
          while (!unique) {
            unique = true;
            for (const auto &app : g_app.deckApps) {
              if (app.id == entry.id) {
                entry.id = base_id + "_" + std::to_string(++suffix);
                unique = false;
                break;
              }
            }
          }
          g_app.deckApps.push_back(std::move(entry));
        }

        save_deck_apps();
        if (g_app.deckDialog && IsWindow(g_app.deckDialog)) {
          refresh_deck_list(g_app.deckDialog);
        }
        rebuild_deck_layout();
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    case IDC_EDITOR_CANCEL:
      if (HIWORD(wParam) == BN_CLICKED) {
        DestroyWindow(hwnd);
        return 0;
      }
      break;
    }
    break;
  }
  case WM_CLOSE:
    DestroyWindow(hwnd);
    return 0;
  case WM_DESTROY: {
    auto *state = reinterpret_cast<DeckEditorState *>(
        GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    delete state;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
    g_app.deckEditorDialog = nullptr;
    return 0;
  }
  }
  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// Connect / Disconnect (runs on worker thread)
// ============================================================================

static void connect_sync() {
  ScopedWinrtApartment apartment;
  if (g_app.isBusy.load() || g_app.isStreaming.load())
    return;
  g_app.isBusy.store(true);

  Settings settings = load_settings();
  log_msg("[Stream] Connecting: display=%u %ufps %u kbps port=%u touch=%s",
          settings.display, settings.fps, settings.bitrate_kbps, settings.port,
          settings.touch ? "on" : "off");

  // 1. ADB forward.
  update_status(L"Setting up ADB...");
  if (!adb_forward_setup(settings.port)) {
    update_status(L"ADB forward failed");
    g_app.isBusy.store(false);
    return;
  }

  // 2. Detect Android device resolution.
  auto [dev_w, dev_h] = adb_device_resolution();
  if (dev_w == 0 || dev_h == 0) {
    dev_w = 2560;
    dev_h = 1600;
    log_msg("[Stream] Could not detect device res, using fallback %ux%u", dev_w,
            dev_h);
  }
  log_msg("[Stream] Target device: %ux%u", dev_w, dev_h);

  // 3. Create a virtual display matching the device resolution.
  update_status(L"Creating virtual display...");
  g_app.vdisplay = std::make_unique<droidscreen::VirtualDisplayWin>();

  if (!g_app.vdisplay->is_driver_installed()) {
    log_msg("[Stream] Parsec VDD: %s", g_app.vdisplay->last_error().c_str());
    // Show a user-friendly message box with download link.
    MessageBoxA(
        nullptr,
        "The Parsec Virtual Display Driver is required but not installed.\n\n"
        "Please download and install it from:\n"
        "https://github.com/nomi-san/parsec-vdd/releases\n\n"
        "After installing, restart DroidScreen and try again.",
        "DroidScreen - Driver Required", MB_OK | MB_ICONWARNING);
    update_status(L"Parsec VDD driver not installed");
    g_app.vdisplay.reset();
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }

  if (!g_app.vdisplay->create(dev_w, dev_h, settings.fps)) {
    log_msg("[Stream] Virtual display creation failed: %s",
            g_app.vdisplay->last_error().c_str());
    update_status(L"Virtual display creation failed");
    g_app.vdisplay.reset();
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }

  log_msg("[Stream] Virtual display created: %ux%u@%u HMONITOR=%p", dev_w,
          dev_h, settings.fps, g_app.vdisplay->monitor_handle());

  // 4. Initialize WGC capturer targeting the virtual display's HMONITOR.
  update_status(L"Initializing capture...");
  g_app.capturer = std::make_unique<droidscreen::WGCCapturer>();
  if (!g_app.capturer->init_with_monitor(g_app.vdisplay->monitor_handle())) {
    log_msg("[Stream] WGC capturer init failed for virtual display");
    update_status(L"Screen capture init failed");
    g_app.capturer.reset();
    g_app.vdisplay->destroy();
    g_app.vdisplay.reset();
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }
  g_app.capturer->set_target_fps(settings.fps);

  uint32_t cap_w = g_app.capturer->width();
  uint32_t cap_h = g_app.capturer->height();
  log_msg("[Stream] Capture resolution: %ux%u", cap_w, cap_h);

  // 5. Create encoder.
  update_status(L"Initializing encoder...");
  log_msg("[Stream] Creating FFmpeg encoder (D3D device=%p, context=%p)",
          g_app.capturer->device(), g_app.capturer->context());
  g_app.encoder = std::make_unique<droidscreen::FFmpegEncoder>();
  g_app.encoder->set_d3d_device(g_app.capturer->device(),
                                g_app.capturer->context(),
                                &g_app.capturer->d3d_mutex());

  // Flush stderr before init so any encoder messages are captured.
  fflush(stderr);
  if (!g_app.encoder->init(cap_w, cap_h, settings.fps, settings.bitrate_kbps)) {
    fflush(stderr);
    // Read back any stderr output that the encoder wrote.
    log_msg("[Stream] Encoder init failed for %ux%u@%ufps %ukbps", cap_w, cap_h,
            settings.fps, settings.bitrate_kbps);
    log_msg("[Stream] No suitable hardware H.264 encoder found "
            "(tried NVENC, QSV, AMF — D3D11VA zero-copy)");
    update_status(L"Encoder init failed");
    g_app.encoder.reset();
    g_app.capturer.reset();
    if (g_app.vdisplay) {
      g_app.vdisplay->destroy();
      g_app.vdisplay.reset();
    }
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }

  // 6. Create touch injector.
  g_app.mouse = std::make_unique<droidscreen::WinMouseInjector>();
  g_app.touch = std::make_unique<droidscreen::WinTouchInjector>();
  if (settings.touch) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(g_app.vdisplay->monitor_handle(), &mi)) {
      log_msg("[Stream] GetMonitorInfo failed for virtual display: %lu",
              GetLastError());
      log_msg("[Stream] Touch injector disabled");
      settings.touch = false;
    } else if (!g_app.mouse->init(cap_w, cap_h, mi.rcMonitor.left,
                                  mi.rcMonitor.top)) {
      log_msg("[Stream] Mouse injector init failed, continuing without input");
      settings.touch = false;
    } else if (!g_app.touch->init(cap_w, cap_h, mi.rcMonitor.left,
                                  mi.rcMonitor.top)) {
      log_msg("[Stream] Touch injector init failed, continuing without touch");
      settings.touch = false;
    } else {
      log_msg("[Stream] Touch target rect: %ls (%ld,%ld)-(%ld,%ld)",
              mi.szDevice, mi.rcMonitor.left, mi.rcMonitor.top,
              mi.rcMonitor.right, mi.rcMonitor.bottom);
    }
  }

  // 7. TCP connect.
  update_status(L"Connecting to device...");
  g_app.client = std::make_unique<droidscreen::TCPClient>();
  if (!g_app.client->connect(settings.port)) {
    log_msg("[Stream] TCP connect failed");
    update_status(L"Connection failed (is Android app running?)");
    g_app.encoder->shutdown();
    g_app.encoder.reset();
    g_app.capturer.reset();
    if (settings.touch) {
      g_app.touch->shutdown();
      g_app.mouse->shutdown();
    }
    g_app.touch.reset();
    g_app.mouse.reset();
    g_app.client.reset();
    if (g_app.vdisplay) {
      g_app.vdisplay->destroy();
      g_app.vdisplay.reset();
    }
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }

  // 8. Create deck manager, state controllers, and pipeline.
  g_app.deckManager = std::make_unique<droidscreen::DeckManager>();
  g_app.volumeController = std::make_unique<droidscreen::WinVolumeController>();
  g_app.mediaController = std::make_unique<droidscreen::WinMediaController>();

  g_app.deckManager->set_layout(build_windows_deck_layout());

  g_app.deckManager->set_action_callback(
      [](uint8_t action, const std::string &tile_id) {
        log_msg("[deck] action callback: action=%u tile=%s", action,
                tile_id.c_str());

        if (!g_app.deckManager)
          return;

        const auto &tiles = g_app.deckManager->layout().tiles;
        auto it = std::find_if(tiles.begin(), tiles.end(),
                               [&](const droidscreen::DeckTile &tile) {
                                 return tile.id == tile_id;
                               });
        if (it == tiles.end())
          return;

        if (it->type == "media") {
          bool ok = false;
          if (g_app.mediaController) {
            switch (action) {
            case 1:
              ok = g_app.mediaController->toggle_play_pause();
              break;
            case 2:
              ok = g_app.mediaController->skip_previous();
              break;
            case 3:
              ok = g_app.mediaController->skip_next();
              break;
            default:
              log_msg("[deck] Ignoring unknown media action %u", action);
              break;
            }
          }
          if (!ok) {
            log_msg("[deck] Media action failed or no active session");
          }
          return;
        }

        if (it->type == "app") {
          if (action != 0) {
            log_msg("[deck] Ignoring non-tap app action %u for %s", action,
                    tile_id.c_str());
            return;
          }

          std::string error;
          if (!droidscreen::WinAppLauncher::launch(
                  it->launch_kind, it->launch_target, it->launch_args,
                  it->working_dir, &error)) {
            log_msg("[deck] Failed to launch %s: %s", tile_id.c_str(),
                    error.c_str());
          }
        }
      });
  g_app.deckManager->set_volume_callback([](uint16_t level, bool muted) {
    log_msg("[deck] volume callback: level=%u muted=%d", level, muted ? 1 : 0);
    if (g_app.volumeController) {
      g_app.volumeController->set_volume(level, muted);
    }
  });

  g_app.pipeline = std::make_unique<droidscreen::Pipeline>(
      g_app.capturer.get(), g_app.encoder.get(), g_app.client.get(),
      g_app.touch.get(), g_app.mouse.get(), g_app.deckManager.get());

  if (!g_app.pipeline->start(cap_w, cap_h, settings.fps, settings.bitrate_kbps,
                             settings.touch)) {
    log_msg("[Stream] Pipeline start failed");
    update_status(L"Pipeline start failed");
    g_app.pipeline.reset();
    g_app.deckManager.reset();
    g_app.mediaController.reset();
    g_app.volumeController.reset();
    g_app.encoder->shutdown();
    g_app.encoder.reset();
    g_app.client->close();
    g_app.client.reset();
    g_app.capturer.reset();
    if (settings.touch) {
      g_app.touch->shutdown();
      g_app.mouse->shutdown();
    }
    g_app.touch.reset();
    g_app.mouse.reset();
    if (g_app.vdisplay) {
      g_app.vdisplay->destroy();
      g_app.vdisplay.reset();
    }
    adb_forward_remove(settings.port);
    g_app.isBusy.store(false);
    return;
  }

  // Success.
  g_app.streamWidth = cap_w;
  g_app.streamHeight = cap_h;
  g_app.streamFPS = settings.fps;
  g_app.isStreaming.store(true);

  wchar_t statusBuf[256];
  _snwprintf_s(statusBuf, _TRUNCATE, L"Streaming %ux%u@%ufps", cap_w, cap_h,
               settings.fps);
  update_status(statusBuf);

  log_msg("[Stream] Pipeline started: %ux%u@%ufps", cap_w, cap_h, settings.fps);

  if (g_app.volumeController) {
    g_app.volumeController->set_state_callback([](uint16_t level, bool muted) {
      if (g_app.pipeline) {
        g_app.pipeline->send_volume_state(level, muted);
      }
    });
    if (!g_app.volumeController->init()) {
      log_msg("[volume] Volume controller init failed");
      g_app.volumeController.reset();
    } else {
      g_app.volumeController->push_current_state();
    }
  }

  if (g_app.mediaController) {
    g_app.mediaController->set_state_callback([](const std::string &json) {
      if (g_app.pipeline) {
        g_app.pipeline->send_media_state(json);
      }
    });
    if (!g_app.mediaController->init()) {
      log_msg("[media] Media controller init failed");
      if (g_app.pipeline) {
        g_app.pipeline->send_media_state(
            "{\"playing\":false,\"title\":\"\",\"artist\":\"\",\"progress\":0."
            "0,\"duration_sec\":0}");
      }
      g_app.mediaController.reset();
    } else {
      g_app.mediaController->push_current_state(true);
    }
  }

  // Start stats timer on main thread.
  PostMessage(g_app.hwnd, WM_UPDATE_UI, 1 /* start stats timer */, 0);

  g_app.isBusy.store(false);
}

static void disconnect_sync() {
  ScopedWinrtApartment apartment;
  if (!g_app.isStreaming.load())
    return;

  // Mark not-streaming FIRST — prevents stats timer and device timer
  // from accessing objects we're about to destroy (race condition fix).
  g_app.isStreaming.store(false);

  log_msg("[Stream] Disconnecting...");

  // Stop stats timer on main thread.
  PostMessage(g_app.hwnd, WM_UPDATE_UI, 2 /* stop stats timer */, 0);

  Settings settings = load_settings();

  if (g_app.pipeline) {
    g_app.pipeline->stop();
    g_app.pipeline.reset();
  }
  if (g_app.encoder) {
    g_app.encoder->shutdown();
    g_app.encoder.reset();
  }
  if (g_app.touch) {
    g_app.touch->shutdown();
    g_app.touch.reset();
  }
  if (g_app.mouse) {
    g_app.mouse->shutdown();
    g_app.mouse.reset();
  }
  if (g_app.mediaController) {
    g_app.mediaController->shutdown();
    g_app.mediaController.reset();
  }
  if (g_app.volumeController) {
    g_app.volumeController->shutdown();
    g_app.volumeController.reset();
  }
  if (g_app.deckManager) {
    g_app.deckManager.reset();
  }
  if (g_app.client) {
    g_app.client->close();
    g_app.client.reset();
  }
  g_app.capturer.reset();

  // Destroy the virtual display (removes it from Windows).
  if (g_app.vdisplay) {
    g_app.vdisplay->destroy();
    g_app.vdisplay.reset();
  }

  adb_forward_remove(settings.port);

  update_status(L"Disconnected");

  log_msg("[Stream] Disconnected");
}

static void connect_async() {
  if (g_app.isBusy.load())
    return;
  std::thread([]() { connect_sync(); }).detach();
}

static void disconnect_async() {
  if (!g_app.isStreaming.load())
    return;
  std::thread([]() { disconnect_sync(); }).detach();
}

// ============================================================================
// Stats Timer Callback
// ============================================================================

static void on_stats_timer() {
  if (!g_app.isStreaming.load())
    return;

  droidscreen::Pipeline *pl = g_app.pipeline.get();
  if (!pl)
    return;

  // Check if pipeline stopped unexpectedly.
  if (!pl->is_running()) {
    log_msg("[Stream] Pipeline stopped unexpectedly, disconnecting...");
    disconnect_async();
    return;
  }

  uint64_t enc = pl->frames_encoded();
  uint64_t cap = pl->frames_captured();
  uint64_t drop = pl->frames_dropped();
  uint64_t idle = pl->frames_idle();
  uint64_t byt = pl->bytes_sent();
  int64_t rtt = pl->last_rtt_us();
  int64_t enc_us = pl->last_encode_us();
  int64_t send_us = pl->last_send_us();
  size_t cap_q = pl->capture_queue_depth();
  size_t send_q = pl->send_queue_depth();
  uint64_t rate_limited =
      g_app.capturer ? g_app.capturer->frames_rate_limited() : 0;

  log_msg(
      "[Stats] captured=%llu encoded=%llu dropped=%llu idle=%llu "
      "rate_limited=%llu capture_q=%zu send_q=%zu encode=%lld us "
      "send=%lld us bytes=%llu rtt=%lld us",
      (unsigned long long)cap, (unsigned long long)enc,
      (unsigned long long)drop, (unsigned long long)idle,
      (unsigned long long)rate_limited, cap_q, send_q, (long long)enc_us,
      (long long)send_us, (unsigned long long)byt, (long long)rtt);
}

// ============================================================================
// Device Detection Timer Callback
// ============================================================================

static void on_device_timer() {
  // Run device check on a background thread to avoid blocking the UI.
  std::thread([]() {
    bool connected = adb_device_connected();

    if (!connected) {
      // Device physically removed — reset the user-disconnect flag so
      // auto-connect kicks in when the device is plugged back in.
      g_app.userDisconnected.store(false);

      if (g_app.isStreaming.load() && !g_app.isBusy.load()) {
        log_msg("[AutoDetect] Device disconnected, stopping...");
        disconnect_sync();
      }
    } else if (connected && !g_app.isStreaming.load() && !g_app.isBusy.load() &&
               !g_app.userDisconnected.load()) {
      // Device connected but not streaming — check if DroidScreen app is
      // running.
      if (adb_droidscreen_running()) {
        log_msg("[AutoConnect] DroidScreen detected on device, connecting...");
        connect_sync();
      }
    }
  }).detach();
}

// ============================================================================
// Hidden Window Procedure (message pump)
// ============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam,
                                LPARAM lParam) {
  switch (msg) {
  case WM_TRAYICON:
    if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
      show_tray_menu();
    } else if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
      show_settings_dialog();
    }
    return 0;

  case WM_COMMAND:
    switch (LOWORD(wParam)) {
    case IDM_SETTINGS:
      show_settings_dialog();
      break;
    case IDM_DECK:
      show_deck_dialog();
      break;
    case IDM_CONNECT:
      if (g_app.isStreaming.load()) {
        g_app.userDisconnected.store(true);
        disconnect_async();
      } else {
        g_app.userDisconnected.store(false);
        connect_async();
      }
      break;
    case IDM_QUIT:
      // Disconnect if streaming.
      if (g_app.isStreaming.load()) {
        disconnect_sync();
      }
      remove_tray_icon();
      PostQuitMessage(0);
      break;
    }
    return 0;

  case WM_UPDATE_UI:
    if (wParam == 1) {
      // Start stats timer (2 second interval).
      SetTimer(hwnd, TIMER_STATS, 2000, nullptr);
    } else if (wParam == 2) {
      // Stop stats timer.
      KillTimer(hwnd, TIMER_STATS);
    }
    return 0;

  case WM_TIMER:
    if (wParam == TIMER_STATS) {
      on_stats_timer();
    } else if (wParam == TIMER_DEVICE) {
      on_device_timer();
    }
    return 0;

  case WM_DESTROY:
    KillTimer(hwnd, TIMER_STATS);
    KillTimer(hwnd, TIMER_DEVICE);
    remove_tray_icon();
    PostQuitMessage(0);
    return 0;
  }

  return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ============================================================================
// WinMain
// ============================================================================

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int) {
  // ── Single-instance guard ────────────────────────────────────────────────
  // Si ya hay una instancia corriendo, la nueva la mata antes de continuar.
  // La nueva instancia "gana": envía WM_CLOSE gracioso, espera 800ms,
  // luego force-kill si sigue vivo, y re-adquiere el mutex.
  {
    static const wchar_t *kMutexName = L"Global\\DroidScreenSingleInstance";
    HANDLE hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
      if (hMutex) {
        CloseHandle(hMutex);
        hMutex = nullptr;
      }
      // kWindowClass = L"DroidScreenHiddenWnd" (defined at line 73).
      // FindWindowW busca entre ventanas existentes — la nuestra aún no está
      // registrada, así que solo encuentra la instancia anterior.
      HWND hExisting = FindWindowW(kWindowClass, nullptr);
      if (hExisting) {
        DWORD existingPid = 0;
        GetWindowThreadProcessId(hExisting, &existingPid);
        PostMessageW(hExisting, WM_CLOSE, 0, 0); // shutdown gracioso
        Sleep(800);
        if (existingPid) {
          HANDLE hProc =
              OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, existingPid);
          if (hProc) {
            if (WaitForSingleObject(hProc, 0) != WAIT_OBJECT_0) {
              // Sigue vivo — force-kill.
              TerminateProcess(hProc, 0);
              WaitForSingleObject(hProc, 2000);
            }
            CloseHandle(hProc);
          }
        }
      } else {
        // Sin ventana (carrera): esperar a que el proceso anterior cierre.
        Sleep(800);
      }
      // Re-adquirir el mutex como único dueño.
      hMutex = CreateMutexW(nullptr, TRUE, kMutexName);
    }
    g_singleInstanceMutex = hMutex;
  }
  // ── End single-instance guard ────────────────────────────────────────────

  // Make the process DPI-aware so that all Win32 APIs (GetMonitorInfoW,
  // DXGI output descs, etc.) return physical pixel coordinates instead of
  // DPI-scaled logical coordinates.  Without this, a 2560x1600 monitor at
  // 125% scaling would report as 2048x1280, causing resolution mismatches
  // between the virtual display and the WGC capturer / encoder.
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

  // Initialize WinRT apartment (required for Windows.Graphics.Capture).
  winrt::init_apartment(winrt::apartment_type::multi_threaded);

  // Set up debug logging.
  setup_logging();
  log_msg("[DroidScreen] Starting system tray app...");

  g_app.hinstance = hInstance;

  // Initialize common controls (for comboboxes, etc.).
  INITCOMMONCONTROLSEX icc = {};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
  InitCommonControlsEx(&icc);

  // Register the hidden window class.
  WNDCLASSEXW wc = {};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = WndProc;
  wc.hInstance = hInstance;
  wc.lpszClassName = kWindowClass;
  RegisterClassExW(&wc);

  // Register the settings window class.
  register_settings_class(hInstance);
  register_deck_classes(hInstance);

  // Create the hidden message-only window.
  g_app.hwnd = CreateWindowExW(0, kWindowClass, kAppName,
                               0, // No visible style.
                               0, 0, 0, 0,
                               HWND_MESSAGE, // Message-only window.
                               nullptr, hInstance, nullptr);

  if (!g_app.hwnd) {
    log_msg("[FATAL] CreateWindowEx failed: %lu", GetLastError());
    return 1;
  }

  // Set up the system tray icon.
  setup_tray_icon(g_app.hwnd);

  // Start the device detection timer (3 second interval).
  SetTimer(g_app.hwnd, TIMER_DEVICE, 3000, nullptr);

  log_msg("[DroidScreen] System tray app ready");

  // Message loop.
  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0)) {
    // Allow settings dialog to process messages with IsDialogMessage
    // for proper tab navigation, etc.
    if (g_app.settingsDialog && IsWindow(g_app.settingsDialog) &&
        IsDialogMessage(g_app.settingsDialog, &msg)) {
      continue;
    }
    if (g_app.deckDialog && IsWindow(g_app.deckDialog) &&
        IsDialogMessage(g_app.deckDialog, &msg)) {
      continue;
    }
    if (g_app.deckEditorDialog && IsWindow(g_app.deckEditorDialog) &&
        IsDialogMessage(g_app.deckEditorDialog, &msg)) {
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Clean shutdown.
  if (g_app.isStreaming.load()) {
    disconnect_sync();
  }

  if (g_logFile) {
    log_msg("[DroidScreen] Exiting");
    fclose(g_logFile);
    g_logFile = nullptr;
  }

  winrt::uninit_apartment();

  if (g_singleInstanceMutex) {
    CloseHandle(g_singleInstanceMutex);
    g_singleInstanceMutex = nullptr;
  }

  return 0;
}
