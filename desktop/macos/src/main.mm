/*
 * DroidScreen macOS - CLI entry point
 *
 * Captures the screen, encodes as H.264, and streams to an
 * Android tablet via TCP over USB (adb reverse).
 *
 * Usage: droidscreen_desktop [--port PORT] [--fps FPS]
 *                            [--bitrate KBPS] [--display INDEX]
 */

#import <Foundation/Foundation.h>

#include "sck_capturer.h"
#include "vt_encoder.h"
#include "droidscreen/pipeline.h"
#include "droidscreen/server.h"
#include "droidscreen/touch_injector.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <atomic>
#include <chrono>
#include <thread>

// ---------------------------------------------------------------------------
// Globals for signal handling
// ---------------------------------------------------------------------------

static std::atomic<bool> g_running{true};
static droidscreen::Pipeline* g_pipeline = nullptr;

static void signal_handler(int sig) {
    (void)sig;
    fprintf(stderr, "\n[main] caught signal, shutting down...\n");
    g_running.store(false);
    if (g_pipeline) {
        g_pipeline->stop();
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
};

static Options parse_args(int argc, const char* argv[]) {
    Options opts;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            opts.port = (uint16_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--fps") == 0 && i + 1 < argc) {
            opts.fps = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--bitrate") == 0 && i + 1 < argc) {
            opts.bitrate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--display") == 0 && i + 1 < argc) {
            opts.display = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [options]\n"
                "  --port PORT       TCP port (default: 38271)\n"
                "  --fps FPS         Target frame rate (default: 60)\n"
                "  --bitrate KBPS    Target bitrate in kbps (default: 15000)\n"
                "  --display INDEX   Display index (default: 0)\n",
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

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        Options opts = parse_args(argc, argv);

        fprintf(stderr, "[main] DroidScreen Desktop (macOS)\n");
        fprintf(stderr, "[main] port=%u fps=%u bitrate=%u kbps display=%u\n",
                opts.port, opts.fps, opts.bitrate, opts.display);

        // Set up adb reverse.
        if (!adb_reverse_setup(opts.port)) {
            return 1;
        }

        // Create components.
        droidscreen::SCKCapturer capturer;
        droidscreen::VTEncoder   encoder;
        droidscreen::NullTouchInjector touch;
        droidscreen::TCPClient   client;

        // Initialize the screen capturer.
        if (!capturer.init(opts.display)) {
            fprintf(stderr, "[main] screen capture init failed\n");
            adb_reverse_remove(opts.port);
            return 1;
        }

        fprintf(stderr, "[main] capture resolution: %ux%u\n",
                capturer.width(), capturer.height());

        // Connect to the Android device (via adb reverse).
        fprintf(stderr, "[main] connecting to 127.0.0.1:%u...\n", opts.port);
        if (!client.connect(opts.port)) {
            fprintf(stderr, "[main] TCP connect failed. "
                    "Is the Android app listening?\n");
            adb_reverse_remove(opts.port);
            return 1;
        }

        // Create and start the pipeline.
        droidscreen::Pipeline pipeline(&capturer, &encoder, &client, &touch);
        g_pipeline = &pipeline;

        // Install signal handlers.
        signal(SIGINT, signal_handler);
        signal(SIGTERM, signal_handler);

        if (!pipeline.start(capturer.width(), capturer.height(),
                            opts.fps, opts.bitrate, false)) {
            fprintf(stderr, "[main] pipeline start failed\n");
            client.close();
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

        // Remove adb reverse.
        adb_reverse_remove(opts.port);

        fprintf(stderr, "[main] done\n");
        return 0;
    }
}
