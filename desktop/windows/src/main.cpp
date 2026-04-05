/*
 * DroidScreen Windows - CLI entry point
 *
 * Captures the screen, encodes as H.264 (NVENC), and streams to an
 * Android tablet via TCP over USB (adb reverse).
 *
 * Usage: droidscreen_desktop [--port PORT] [--fps FPS]
 *                            [--bitrate KBPS] [--display INDEX]
 */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <winsock2.h>

#include "wgc_capturer.h"
#include "nvenc_encoder.h"
#include "touch_injector_win.h"
#include "droidscreen/pipeline.h"
#include "droidscreen/server.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

// Required for WinRT initialization.
#include <winrt/base.h>

// ---------------------------------------------------------------------------
// Globals for signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};
static droidscreen::Pipeline* g_pipeline = nullptr;

static BOOL WINAPI console_ctrl_handler(DWORD ctrl_type) {
    switch (ctrl_type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
            fprintf(stderr, "\n[main] caught console event, shutting down...\n");
            g_running.store(false);
            if (g_pipeline) {
                g_pipeline->stop();
            }
            return TRUE;
        default:
            return FALSE;
    }
}

// ---------------------------------------------------------------------------
// Argument parsing
// ---------------------------------------------------------------------------

struct Options {
    uint16_t port     = 38271;
    uint32_t fps      = 60;
    uint32_t bitrate  = 15000;  // kbps
    uint32_t display  = 0;
    bool     touch    = true;
};

static Options parse_args(int argc, char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            opts.fps = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            opts.bitrate = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            opts.display = static_cast<uint32_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--no-touch") == 0) {
            opts.touch = false;
        } else if (strcmp(argv[i], "--help") == 0 ||
                   strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [options]\n"
                "  --port PORT       TCP port (default: 38271)\n"
                "  --fps FPS         Target frame rate (default: 60)\n"
                "  --bitrate KBPS    Target bitrate in kbps (default: 15000)\n"
                "  --display INDEX   Display index (default: 0)\n"
                "  --no-touch        Disable touch injection\n",
                argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "[main] unknown argument: %s\n", argv[i]);
        }
    }
    return opts;
}

// ---------------------------------------------------------------------------
// ADB helpers
// ---------------------------------------------------------------------------

static bool adb_reverse_setup(uint16_t port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "adb reverse tcp:%u tcp:%u", port, port);
    fprintf(stderr, "[main] running: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[main] adb reverse failed (exit %d). "
                "Is the device connected?\n", ret);
        return false;
    }
    return true;
}

static void adb_reverse_remove(uint16_t port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "adb reverse --remove tcp:%u", port);
    fprintf(stderr, "[main] running: %s\n", cmd);
    system(cmd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    // Initialize WinRT apartment (required for Windows.Graphics.Capture).
    winrt::init_apartment(winrt::apartment_type::multi_threaded);

    Options opts = parse_args(argc, argv);

    fprintf(stderr, "[main] DroidScreen Desktop (Windows)\n");
    fprintf(stderr, "[main] port=%u fps=%u bitrate=%u kbps display=%u "
            "touch=%s\n",
            opts.port, opts.fps, opts.bitrate, opts.display,
            opts.touch ? "on" : "off");

    // Set up adb reverse.
    if (!adb_reverse_setup(opts.port)) {
        return 1;
    }

    // Install the console control handler for Ctrl+C.
    SetConsoleCtrlHandler(console_ctrl_handler, TRUE);

    // Create the screen capturer.
    droidscreen::WGCCapturer capturer;
    if (!capturer.init(opts.display)) {
        fprintf(stderr, "[main] screen capture init failed\n");
        adb_reverse_remove(opts.port);
        return 1;
    }

    fprintf(stderr, "[main] capture resolution: %ux%u\n",
            capturer.width(), capturer.height());

    // Create the encoder. Share the D3D11 device from the capturer so
    // that texture copies between capture and encode happen on the same
    // device without cross-device synchronization.
    droidscreen::NvencEncoder encoder;
    encoder.set_d3d_device(capturer.device(), capturer.context());

    if (!encoder.init(capturer.width(), capturer.height(),
                      opts.fps, opts.bitrate)) {
        fprintf(stderr, "[main] NVENC encoder init failed. "
                "Check that an NVIDIA GPU with NVENC is available.\n");
        adb_reverse_remove(opts.port);
        return 1;
    }

    // Create the touch injector.
    droidscreen::WinTouchInjector touch;
    if (opts.touch) {
        if (!touch.init(capturer.width(), capturer.height())) {
            fprintf(stderr, "[main] touch injector init failed, "
                    "continuing without touch\n");
            opts.touch = false;
        }
    }

    // Connect to the Android device (via adb reverse).
    droidscreen::TCPClient client;
    fprintf(stderr, "[main] connecting to 127.0.0.1:%u...\n", opts.port);
    if (!client.connect(opts.port)) {
        fprintf(stderr, "[main] TCP connect failed. "
                "Is the Android app listening?\n");
        encoder.shutdown();
        adb_reverse_remove(opts.port);
        return 1;
    }

    // Create and start the pipeline.
    droidscreen::Pipeline pipeline(&capturer, &encoder, &client, &touch);
    g_pipeline = &pipeline;

    if (!pipeline.start(capturer.width(), capturer.height(),
                        opts.fps, opts.bitrate, opts.touch)) {
        fprintf(stderr, "[main] pipeline start failed\n");
        client.close();
        encoder.shutdown();
        adb_reverse_remove(opts.port);
        return 1;
    }

    fprintf(stderr, "[main] streaming... press Ctrl+C to stop\n");

    // Log stats periodically while running.
    auto last_log = std::chrono::steady_clock::now();
    while (g_running.load() && pipeline.is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            now - last_log);
        if (elapsed.count() >= 5) {
            fprintf(stderr, "[stats] frames=%llu bytes=%llu rtt=%lld us\n",
                static_cast<unsigned long long>(pipeline.frames_encoded()),
                static_cast<unsigned long long>(pipeline.bytes_sent()),
                static_cast<long long>(pipeline.last_rtt_us()));
            last_log = now;
        }
    }

    // Clean shutdown.
    fprintf(stderr, "[main] shutting down...\n");
    pipeline.stop();
    g_pipeline = nullptr;
    client.close();
    encoder.shutdown();
    if (opts.touch) {
        touch.shutdown();
    }

    // Remove adb reverse.
    adb_reverse_remove(opts.port);

    // Uninitialize WinRT.
    winrt::uninit_apartment();

    fprintf(stderr, "[main] done\n");
    return 0;
}
