/*
 * DroidScreen Windows - System Tray Application
 *
 * A Win32 system-tray app that captures the screen via WGC, encodes with
 * NVENC, and streams H.264 to an Android tablet over USB (adb forward).
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

#include <windows.h>
#include <winsock2.h>
#include <shellapi.h>
#include <commctrl.h>
#include <dxgi1_2.h>
#include <wrl/client.h>
#include <shlobj.h>

#include "wgc_capturer.h"
#include "nvenc_encoder.h"
#include "touch_injector_win.h"
#include "droidscreen/pipeline.h"
#include "droidscreen/server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <io.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

// Required for WinRT initialization.
#include <winrt/base.h>

// ============================================================================
// Constants
// ============================================================================

static const wchar_t* kAppName       = L"DroidScreen";
static const wchar_t* kWindowClass   = L"DroidScreenHiddenWnd";
static const wchar_t* kRegistryKey   = L"Software\\DroidScreen";

// Message IDs.
static constexpr UINT WM_TRAYICON   = WM_APP + 1;
static constexpr UINT WM_UPDATE_UI  = WM_APP + 2;  // wParam = update type

// Timer IDs.
static constexpr UINT_PTR TIMER_STATS  = 1;
static constexpr UINT_PTR TIMER_DEVICE = 2;

// Menu item IDs.
static constexpr UINT IDM_TITLE      = 4000;
static constexpr UINT IDM_STATUS     = 4001;
static constexpr UINT IDM_SETTINGS   = 4002;
static constexpr UINT IDM_CONNECT    = 4003;
static constexpr UINT IDM_QUIT       = 4004;

// Settings dialog control IDs.
static constexpr UINT IDC_FPS_COMBO      = 5001;
static constexpr UINT IDC_BITRATE_COMBO  = 5002;
static constexpr UINT IDC_DISPLAY_COMBO  = 5003;
static constexpr UINT IDC_PORT_EDIT      = 5004;
static constexpr UINT IDC_TOUCH_CHECK    = 5005;
static constexpr UINT IDC_APPLY_BTN      = 5006;

// Bitrate presets (kbps).
static const int kBitrates[]     = { 5000, 10000, 15000, 20000, 25000, 30000 };
static const wchar_t* kBitrateLabels[] = {
    L"5 Mbps", L"10 Mbps", L"15 Mbps", L"20 Mbps", L"25 Mbps", L"30 Mbps"
};
static constexpr int kBitrateCount = sizeof(kBitrates) / sizeof(kBitrates[0]);

// ============================================================================
// Settings (persisted in Windows Registry)
// ============================================================================

struct Settings {
    uint32_t fps         = 60;
    uint32_t bitrate_kbps = 15000;
    uint32_t display     = 0;
    uint16_t port        = 38271;
    bool     touch       = true;
};

static Settings load_settings() {
    Settings s;
    HKEY hkey;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0,
                      KEY_READ, &hkey) == ERROR_SUCCESS) {
        DWORD val, sz;

        sz = sizeof(val);
        if (RegQueryValueExW(hkey, L"FPS", nullptr, nullptr,
                             (BYTE*)&val, &sz) == ERROR_SUCCESS)
            s.fps = val;

        sz = sizeof(val);
        if (RegQueryValueExW(hkey, L"Bitrate", nullptr, nullptr,
                             (BYTE*)&val, &sz) == ERROR_SUCCESS)
            s.bitrate_kbps = val;

        sz = sizeof(val);
        if (RegQueryValueExW(hkey, L"Display", nullptr, nullptr,
                             (BYTE*)&val, &sz) == ERROR_SUCCESS)
            s.display = val;

        sz = sizeof(val);
        if (RegQueryValueExW(hkey, L"Port", nullptr, nullptr,
                             (BYTE*)&val, &sz) == ERROR_SUCCESS)
            s.port = (uint16_t)val;

        sz = sizeof(val);
        if (RegQueryValueExW(hkey, L"Touch", nullptr, nullptr,
                             (BYTE*)&val, &sz) == ERROR_SUCCESS)
            s.touch = (val != 0);

        RegCloseKey(hkey);
    }
    return s;
}

static void save_settings(const Settings& s) {
    HKEY hkey;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegistryKey, 0, nullptr,
                        REG_OPTION_NON_VOLATILE, KEY_WRITE,
                        nullptr, &hkey, nullptr) == ERROR_SUCCESS) {
        DWORD val;

        val = s.fps;
        RegSetValueExW(hkey, L"FPS", 0, REG_DWORD, (BYTE*)&val, sizeof(val));

        val = s.bitrate_kbps;
        RegSetValueExW(hkey, L"Bitrate", 0, REG_DWORD, (BYTE*)&val, sizeof(val));

        val = s.display;
        RegSetValueExW(hkey, L"Display", 0, REG_DWORD, (BYTE*)&val, sizeof(val));

        val = (DWORD)s.port;
        RegSetValueExW(hkey, L"Port", 0, REG_DWORD, (BYTE*)&val, sizeof(val));

        val = s.touch ? 1 : 0;
        RegSetValueExW(hkey, L"Touch", 0, REG_DWORD, (BYTE*)&val, sizeof(val));

        RegCloseKey(hkey);
    }
}

// ============================================================================
// DXGI Display Enumeration
// ============================================================================

struct DisplayInfo {
    std::wstring name;
    uint32_t     width;
    uint32_t     height;
};

static std::vector<DisplayInfo> enumerate_displays() {
    std::vector<DisplayInfo> displays;

    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        return displays;
    }

    Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        Microsoft::WRL::ComPtr<IDXGIOutput> output;
        for (UINT oi = 0; adapter->EnumOutputs(oi, &output) != DXGI_ERROR_NOT_FOUND; ++oi) {
            DXGI_OUTPUT_DESC desc;
            if (SUCCEEDED(output->GetDesc(&desc))) {
                LONG w = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                LONG h = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;

                wchar_t label[256];
                _snwprintf_s(label, _TRUNCATE, L"Display %u: %s (%ldx%ld)",
                             (unsigned)displays.size(),
                             desc.DeviceName, w, h);

                displays.push_back({ label, (uint32_t)w, (uint32_t)h });
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

/// Run a process and capture stdout. Returns exit code, -1 on failure.
static int run_process(const std::string& cmd, std::string* output = nullptr) {
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE hReadPipe = nullptr, hWritePipe = nullptr;
    if (output) {
        if (!CreatePipe(&hReadPipe, &hWritePipe, &sa, 0)) return -1;
        SetHandleInformation(hReadPipe, HANDLE_FLAG_INHERIT, 0);
    }

    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    if (output) {
        si.dwFlags |= STARTF_USESTDHANDLES;
        si.hStdOutput = hWritePipe;
        si.hStdError  = hWritePipe;
    }

    PROCESS_INFORMATION pi = {};
    // CreateProcessA needs a mutable command line buffer.
    std::string cmd_buf = cmd;

    BOOL ok = CreateProcessA(
        nullptr, cmd_buf.data(), nullptr, nullptr,
        output ? TRUE : FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);

    if (output && hWritePipe) CloseHandle(hWritePipe);

    if (!ok) {
        if (output && hReadPipe) CloseHandle(hReadPipe);
        return -1;
    }

    if (output && hReadPipe) {
        output->clear();
        char buf[4096];
        DWORD bytesRead;
        while (ReadFile(hReadPipe, buf, sizeof(buf), &bytesRead, nullptr) && bytesRead > 0) {
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
static const std::string& adb_find_path() {
    if (!g_adb_path.empty()) return g_adb_path;

    // Check candidates.
    const char* candidates[] = {
        nullptr,  // placeholder for %LOCALAPPDATA%\Android\Sdk\platform-tools\adb.exe
        "C:\\Program Files\\Android\\Android Studio\\platform-tools\\adb.exe",
    };

    // Build %LOCALAPPDATA% path.
    char localAppData[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::string sdkAdb = std::string(localAppData) + "\\Android\\Sdk\\platform-tools\\adb.exe";
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
        std::string path = (nl != std::string::npos) ? output.substr(0, nl) : output;
        if (!path.empty() && GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
            g_adb_path = path;
            return g_adb_path;
        }
    }

    // Fallback: just try "adb" and hope it's on PATH.
    g_adb_path = "adb";
    return g_adb_path;
}

static int adb_run(const std::string& args, std::string* output = nullptr) {
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
    const char* prefix = "Physical size:";
    size_t pos = output.find(prefix);
    if (pos == std::string::npos) return {0, 0};

    pos += strlen(prefix);
    // Skip whitespace.
    while (pos < output.size() && (output[pos] == ' ' || output[pos] == '\t')) pos++;

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
    if (adb_run("devices", &output) != 0) return false;

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

// ============================================================================
// Debug Logging
// ============================================================================

static FILE* g_logFile = nullptr;

static void log_msg(const char* fmt, ...) {
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
    if (SUCCEEDED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData))) {
        std::string logDir = std::string(localAppData) + "\\DroidScreen";
        CreateDirectoryA(logDir.c_str(), nullptr);
        std::string logPath = logDir + "\\droidscreen.log";
        g_logFile = fopen(logPath.c_str(), "w");
        if (g_logFile) {
            // Redirect stderr to the log file so that fprintf(stderr, ...)
            // from sub-components (NVENC, WGC, pipeline, etc.) is captured.
            // In a /SUBSYSTEM:WINDOWS app, stderr goes nowhere by default.
            _dup2(_fileno(g_logFile), _fileno(stderr));
            setvbuf(stderr, nullptr, _IONBF, 0);
            log_msg("=== DroidScreen Debug Log ===");
        }
    }
}

// ============================================================================
// Application State
// ============================================================================

struct AppState {
    HWND         hwnd             = nullptr;
    HINSTANCE    hinstance        = nullptr;
    NOTIFYICONDATAW nid          = {};
    HWND         settingsDialog   = nullptr;

    // Streaming state.
    std::atomic<bool> isStreaming{false};
    std::atomic<bool> isBusy{false};
    std::atomic<bool> wantQuit{false};

    // Pipeline components.
    std::unique_ptr<droidscreen::WGCCapturer>     capturer;
    std::unique_ptr<droidscreen::NvencEncoder>     encoder;
    std::unique_ptr<droidscreen::TCPClient>        client;
    std::unique_ptr<droidscreen::WinTouchInjector> touch;
    std::unique_ptr<droidscreen::Pipeline>         pipeline;

    // Worker thread for streaming operations.
    std::thread  streamThread;
    std::mutex   streamMutex;

    // Current streaming info for status display.
    uint32_t streamWidth  = 0;
    uint32_t streamHeight = 0;
    uint32_t streamFPS    = 0;

    // Status text displayed in the tray menu.
    wchar_t statusText[256] = L"Disconnected";

    // Cached display list.
    std::vector<DisplayInfo> displays;
};

static AppState g_app;

// ============================================================================
// Forward Declarations
// ============================================================================

static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
static LRESULT CALLBACK SettingsWndProc(HWND, UINT, WPARAM, LPARAM);
static void setup_tray_icon(HWND hwnd);
static void remove_tray_icon();
static void show_tray_menu();
static void show_settings_dialog();
static void connect_async();
static void disconnect_async();
static void connect_sync();
static void disconnect_sync();
static void update_status(const wchar_t* text);

// ============================================================================
// System Tray Icon
// ============================================================================

static void setup_tray_icon(HWND hwnd) {
    NOTIFYICONDATAW& nid = g_app.nid;
    memset(&nid, 0, sizeof(nid));
    nid.cbSize = sizeof(nid);
    nid.hWnd   = hwnd;
    nid.uID    = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_TRAYICON;

    // Use a system application icon.
    nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32512)); // IDI_APPLICATION
    wcscpy_s(nid.szTip, L"DroidScreen");

    Shell_NotifyIconW(NIM_ADD, &nid);
}

static void remove_tray_icon() {
    Shell_NotifyIconW(NIM_DELETE, &g_app.nid);
}

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

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Connect / Disconnect toggle.
    bool streaming = g_app.isStreaming.load();
    bool busy = g_app.isBusy.load();
    const wchar_t* connectLabel = streaming ? L"Disconnect" : L"Connect";
    UINT connectFlags = MF_STRING;
    if (busy) connectFlags |= MF_GRAYED;
    AppendMenuW(menu, connectFlags, IDM_CONNECT, connectLabel);

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);

    // Quit.
    AppendMenuW(menu, MF_STRING, IDM_QUIT, L"Quit");

    // Show the menu at cursor position.
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_app.hwnd);
    TrackPopupMenu(menu, TPM_RIGHTALIGN | TPM_BOTTOMALIGN,
                   pt.x, pt.y, 0, g_app.hwnd, nullptr);
    PostMessage(g_app.hwnd, WM_NULL, 0, 0);  // Required per MSDN.

    DestroyMenu(menu);
}

// ============================================================================
// Status Update (thread-safe)
// ============================================================================

static void update_status(const wchar_t* text) {
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

static const wchar_t* kSettingsClass = L"DroidScreenSettingsWnd";

static void register_settings_class(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = SettingsWndProc;
    wc.hInstance      = hInst;
    wc.hbrBackground  = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName  = kSettingsClass;
    wc.hCursor        = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW = 32512
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
    const int dlgH = 340;

    // Center on screen.
    int screenW = GetSystemMetrics(SM_CXSCREEN);
    int screenH = GetSystemMetrics(SM_CYSCREEN);
    int x = (screenW - dlgW) / 2;
    int y = (screenH - dlgH) / 2;

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kSettingsClass, L"DroidScreen Settings",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        x, y, dlgW, dlgH,
        nullptr, nullptr, g_app.hinstance, nullptr);

    g_app.settingsDialog = hwnd;

    Settings s = load_settings();

    // Layout constants.
    const int leftMargin  = 20;
    const int labelWidth  = 80;
    const int ctrlLeft    = leftMargin + labelWidth + 10;
    const int ctrlWidth   = 260;
    const int rowHeight   = 30;
    int cy = 20;  // current Y position

    // Helper to create a label.
    auto makeLabel = [&](const wchar_t* text, int yPos) {
        CreateWindowExW(0, L"STATIC", text,
                        WS_CHILD | WS_VISIBLE | SS_RIGHT,
                        leftMargin, yPos + 4, labelWidth, 20,
                        hwnd, nullptr, g_app.hinstance, nullptr);
    };

    // --- FPS ---
    makeLabel(L"Frame Rate:", cy);
    HWND fpsCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        ctrlLeft, cy, ctrlWidth, 200,
        hwnd, (HMENU)(UINT_PTR)IDC_FPS_COMBO, g_app.hinstance, nullptr);
    SendMessageW(fpsCombo, CB_ADDSTRING, 0, (LPARAM)L"30 fps");
    SendMessageW(fpsCombo, CB_ADDSTRING, 0, (LPARAM)L"60 fps");
    SendMessageW(fpsCombo, CB_SETCURSEL, (s.fps == 30) ? 0 : 1, 0);
    cy += rowHeight + 8;

    // --- Bitrate ---
    makeLabel(L"Bitrate:", cy);
    HWND brCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        ctrlLeft, cy, ctrlWidth, 200,
        hwnd, (HMENU)(UINT_PTR)IDC_BITRATE_COMBO, g_app.hinstance, nullptr);
    int brSel = 2; // default 15 Mbps
    for (int i = 0; i < kBitrateCount; i++) {
        SendMessageW(brCombo, CB_ADDSTRING, 0, (LPARAM)kBitrateLabels[i]);
        if (kBitrates[i] == (int)s.bitrate_kbps) brSel = i;
    }
    SendMessageW(brCombo, CB_SETCURSEL, brSel, 0);
    cy += rowHeight + 8;

    // --- Display ---
    makeLabel(L"Display:", cy);
    HWND dispCombo = CreateWindowExW(0, L"COMBOBOX", nullptr,
        WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
        ctrlLeft, cy, ctrlWidth, 200,
        hwnd, (HMENU)(UINT_PTR)IDC_DISPLAY_COMBO, g_app.hinstance, nullptr);
    if (g_app.displays.empty()) {
        SendMessageW(dispCombo, CB_ADDSTRING, 0, (LPARAM)L"Display 0 (default)");
    } else {
        for (auto& d : g_app.displays) {
            SendMessageW(dispCombo, CB_ADDSTRING, 0, (LPARAM)d.name.c_str());
        }
    }
    SendMessageW(dispCombo, CB_SETCURSEL,
                 (s.display < (uint32_t)g_app.displays.size()) ? s.display : 0, 0);
    cy += rowHeight + 8;

    // --- Port ---
    makeLabel(L"Port:", cy);
    HWND portEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", nullptr,
        WS_CHILD | WS_VISIBLE | ES_NUMBER,
        ctrlLeft, cy, 100, 24,
        hwnd, (HMENU)(UINT_PTR)IDC_PORT_EDIT, g_app.hinstance, nullptr);
    {
        wchar_t portStr[16];
        _snwprintf_s(portStr, _TRUNCATE, L"%u", (unsigned)s.port);
        SetWindowTextW(portEdit, portStr);
    }
    cy += rowHeight + 8;

    // --- Touch ---
    HWND touchCheck = CreateWindowExW(0, L"BUTTON", L"Enable touch injection",
        WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
        ctrlLeft, cy + 2, ctrlWidth, 20,
        hwnd, (HMENU)(UINT_PTR)IDC_TOUCH_CHECK, g_app.hinstance, nullptr);
    SendMessageW(touchCheck, BM_SETCHECK, s.touch ? BST_CHECKED : BST_UNCHECKED, 0);
    makeLabel(L"Touch:", cy);
    cy += rowHeight + 16;

    // --- Apply Button ---
    CreateWindowExW(0, L"BUTTON", L"Apply",
        WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
        dlgW - 110 - 20, cy, 100, 32,
        hwnd, (HMENU)(UINT_PTR)IDC_APPLY_BTN, g_app.hinstance, nullptr);

    // Set font for all child controls.
    HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    EnumChildWindows(hwnd, [](HWND child, LPARAM lParam) -> BOOL {
        SendMessage(child, WM_SETFONT, (WPARAM)lParam, TRUE);
        return TRUE;
    }, (LPARAM)hFont);

    ShowWindow(hwnd, SW_SHOW);
    SetForegroundWindow(hwnd);
}

static void apply_settings_from_dialog(HWND dlg) {
    Settings s;

    // FPS.
    HWND fpsCombo = GetDlgItem(dlg, IDC_FPS_COMBO);
    int fpsSel = (int)SendMessageW(fpsCombo, CB_GETCURSEL, 0, 0);
    s.fps = (fpsSel == 0) ? 30 : 60;

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
    if (port < 1 || port > 65535) port = 38271;
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

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wParam) == IDC_APPLY_BTN && HIWORD(wParam) == BN_CLICKED) {
            apply_settings_from_dialog(hwnd);
            return 0;
        }
        break;

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

// ============================================================================
// Connect / Disconnect (runs on worker thread)
// ============================================================================

static void connect_sync() {
    if (g_app.isBusy.load() || g_app.isStreaming.load()) return;
    g_app.isBusy.store(true);

    Settings settings = load_settings();
    log_msg("[Stream] Connecting: display=%u %ufps %u kbps port=%u touch=%s",
            settings.display, settings.fps, settings.bitrate_kbps,
            settings.port, settings.touch ? "on" : "off");

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
        log_msg("[Stream] Could not detect device res, using fallback %ux%u", dev_w, dev_h);
    }
    log_msg("[Stream] Target device: %ux%u", dev_w, dev_h);

    // 3. Initialize WGC capturer.
    update_status(L"Initializing capture...");
    g_app.capturer = std::make_unique<droidscreen::WGCCapturer>();
    if (!g_app.capturer->init(settings.display)) {
        log_msg("[Stream] WGC capturer init failed");
        update_status(L"Screen capture init failed");
        g_app.capturer.reset();
        adb_forward_remove(settings.port);
        g_app.isBusy.store(false);
        return;
    }

    uint32_t cap_w = g_app.capturer->width();
    uint32_t cap_h = g_app.capturer->height();
    log_msg("[Stream] Capture resolution: %ux%u", cap_w, cap_h);

    // 4. Create encoder.
    update_status(L"Initializing encoder...");
    g_app.encoder = std::make_unique<droidscreen::NvencEncoder>();
    g_app.encoder->set_d3d_device(g_app.capturer->device(),
                                   g_app.capturer->context());

    if (!g_app.encoder->init(cap_w, cap_h, settings.fps, settings.bitrate_kbps)) {
        log_msg("[Stream] NVENC encoder init failed");
        update_status(L"NVENC encoder init failed");
        g_app.encoder.reset();
        g_app.capturer.reset();
        adb_forward_remove(settings.port);
        g_app.isBusy.store(false);
        return;
    }

    // 5. Create touch injector.
    g_app.touch = std::make_unique<droidscreen::WinTouchInjector>();
    if (settings.touch) {
        if (!g_app.touch->init(cap_w, cap_h)) {
            log_msg("[Stream] Touch injector init failed, continuing without touch");
            settings.touch = false;
        }
    }

    // 6. TCP connect.
    update_status(L"Connecting to device...");
    g_app.client = std::make_unique<droidscreen::TCPClient>();
    if (!g_app.client->connect(settings.port)) {
        log_msg("[Stream] TCP connect failed");
        update_status(L"Connection failed (is Android app running?)");
        g_app.encoder->shutdown();
        g_app.encoder.reset();
        g_app.capturer.reset();
        if (settings.touch) g_app.touch->shutdown();
        g_app.touch.reset();
        g_app.client.reset();
        adb_forward_remove(settings.port);
        g_app.isBusy.store(false);
        return;
    }

    // 7. Create and start pipeline.
    g_app.pipeline = std::make_unique<droidscreen::Pipeline>(
        g_app.capturer.get(), g_app.encoder.get(),
        g_app.client.get(), g_app.touch.get());

    if (!g_app.pipeline->start(cap_w, cap_h, settings.fps,
                                settings.bitrate_kbps, settings.touch)) {
        log_msg("[Stream] Pipeline start failed");
        update_status(L"Pipeline start failed");
        g_app.pipeline.reset();
        g_app.encoder->shutdown();
        g_app.encoder.reset();
        g_app.client->close();
        g_app.client.reset();
        g_app.capturer.reset();
        if (settings.touch) g_app.touch->shutdown();
        g_app.touch.reset();
        adb_forward_remove(settings.port);
        g_app.isBusy.store(false);
        return;
    }

    // Success.
    g_app.streamWidth  = cap_w;
    g_app.streamHeight = cap_h;
    g_app.streamFPS    = settings.fps;
    g_app.isStreaming.store(true);

    wchar_t statusBuf[256];
    _snwprintf_s(statusBuf, _TRUNCATE, L"Streaming %ux%u@%ufps",
                 cap_w, cap_h, settings.fps);
    update_status(statusBuf);

    log_msg("[Stream] Pipeline started: %ux%u@%ufps", cap_w, cap_h, settings.fps);

    // Start stats timer on main thread.
    PostMessage(g_app.hwnd, WM_UPDATE_UI, 1 /* start stats timer */, 0);

    g_app.isBusy.store(false);
}

static void disconnect_sync() {
    if (!g_app.isStreaming.load()) return;
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
    if (g_app.client) {
        g_app.client->close();
        g_app.client.reset();
    }
    g_app.capturer.reset();

    adb_forward_remove(settings.port);

    g_app.isStreaming.store(false);
    update_status(L"Disconnected");

    log_msg("[Stream] Disconnected");
}

static void connect_async() {
    if (g_app.isBusy.load()) return;
    std::thread([]() {
        connect_sync();
    }).detach();
}

static void disconnect_async() {
    if (!g_app.isStreaming.load()) return;
    std::thread([]() {
        disconnect_sync();
    }).detach();
}

// ============================================================================
// Stats Timer Callback
// ============================================================================

static void on_stats_timer() {
    if (!g_app.isStreaming.load()) return;

    droidscreen::Pipeline* pl = g_app.pipeline.get();
    if (!pl) return;

    // Check if pipeline stopped unexpectedly.
    if (!pl->is_running()) {
        log_msg("[Stream] Pipeline stopped unexpectedly, disconnecting...");
        disconnect_async();
        return;
    }

    uint64_t enc = pl->frames_encoded();
    uint64_t byt = pl->bytes_sent();
    int64_t  rtt = pl->last_rtt_us();

    log_msg("[Stats] encoded=%llu bytes=%llu rtt=%lld us",
            (unsigned long long)enc, (unsigned long long)byt, (long long)rtt);
}

// ============================================================================
// Device Detection Timer Callback
// ============================================================================

static void on_device_timer() {
    // Run device check on a background thread to avoid blocking the UI.
    std::thread([]() {
        bool connected = adb_device_connected();

        if (!connected && g_app.isStreaming.load() && !g_app.isBusy.load()) {
            log_msg("[AutoDetect] Device disconnected, stopping...");
            disconnect_sync();
        }
    }).detach();
}

// ============================================================================
// Hidden Window Procedure (message pump)
// ============================================================================

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
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
        case IDM_CONNECT:
            if (g_app.isStreaming.load()) {
                disconnect_async();
            } else {
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
    // Initialize WinRT apartment (required for Windows.Graphics.Capture).
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    // Set up debug logging.
    setup_logging();
    log_msg("[DroidScreen] Starting system tray app...");

    g_app.hinstance = hInstance;

    // Initialize common controls (for comboboxes, etc.).
    INITCOMMONCONTROLSEX icc = {};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icc);

    // Register the hidden window class.
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance      = hInstance;
    wc.lpszClassName  = kWindowClass;
    RegisterClassExW(&wc);

    // Register the settings window class.
    register_settings_class(hInstance);

    // Create the hidden message-only window.
    g_app.hwnd = CreateWindowExW(
        0, kWindowClass, kAppName,
        0,  // No visible style.
        0, 0, 0, 0,
        HWND_MESSAGE,  // Message-only window.
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

    return 0;
}
