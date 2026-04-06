/*
 * DroidScreen macOS - CLI entry point
 *
 * Creates a virtual display at the tablet's resolution, captures it,
 * encodes as H.264, and streams to an Android tablet via USB.
 */

#import <Foundation/Foundation.h>

#include "sck_capturer.h"
#include "vt_encoder.h"
#include "virtual_display.h"
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
    uint32_t display  = 0;      // fallback display index if no virtual display
    uint32_t width    = 2560;   // virtual display width (tablet landscape)
    uint32_t height   = 1600;   // virtual display height (tablet landscape)
    bool     no_vd    = false;  // disable virtual display (capture real display)
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
        } else if (strcmp(argv[i], "--width") == 0 && i + 1 < argc) {
            opts.width = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--height") == 0 && i + 1 < argc) {
            opts.height = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-vd") == 0) {
            opts.no_vd = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            fprintf(stderr,
                "Usage: %s [options]\n"
                "  --port PORT       TCP port (default: 38271)\n"
                "  --fps FPS         Target frame rate (default: 60)\n"
                "  --bitrate KBPS    Target bitrate in kbps (default: 15000)\n"
                "  --width W         Virtual display width (default: 2560)\n"
                "  --height H        Virtual display height (default: 1600)\n"
                "  --display INDEX   Physical display index if --no-vd (default: 0)\n"
                "  --no-vd           Capture real display instead of virtual\n",
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

static bool adb_forward_setup(uint16_t port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb forward tcp:%u tcp:%u", port, port);
    fprintf(stderr, "[main] running: %s\n", cmd);
    int ret = system(cmd);
    if (ret != 0) {
        fprintf(stderr, "[main] adb forward failed (exit %d). "
                "Is the device connected?\n", ret);
        return false;
    }
    return true;
}

static void adb_forward_remove(uint16_t port) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "adb forward --remove tcp:%u", port);
    system(cmd);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, const char* argv[]) {
    @autoreleasepool {
        Options opts = parse_args(argc, argv);

        fprintf(stderr, "[main] DroidScreen Desktop (macOS)\n");
        fprintf(stderr, "[main] port=%u fps=%u bitrate=%u kbps\n",
                opts.port, opts.fps, opts.bitrate);

        // Set up adb forward.
        if (!adb_forward_setup(opts.port)) {
            return 1;
        }

        // Create virtual display (unless --no-vd).
        droidscreen::VirtualDisplay vdisplay;
        bool using_vd = false;

        if (!opts.no_vd) {
            fprintf(stderr, "[main] creating virtual display %ux%u@%uHz\n",
                    opts.width, opts.height, opts.fps);

            if (vdisplay.create(opts.width, opts.height, opts.fps)) {
                using_vd = true;
                fprintf(stderr, "[main] virtual display created (ID=%u)\n",
                        vdisplay.display_id());
                // Give macOS a moment to register the new display.
                std::this_thread::sleep_for(std::chrono::seconds(2));
            } else {
                fprintf(stderr, "[main] virtual display failed, "
                        "falling back to real display %u\n", opts.display);
            }
        }

        // Create components.
        droidscreen::SCKCapturer capturer;
        droidscreen::VTEncoder   encoder;
        droidscreen::NullTouchInjector touch;
        droidscreen::TCPClient   client;

        // Initialize the screen capturer.
        bool cap_ok = false;
        if (using_vd) {
            cap_ok = capturer.init_with_display_id(vdisplay.display_id());
        } else {
            cap_ok = capturer.init(opts.display);
        }

        if (!cap_ok) {
            fprintf(stderr, "[main] screen capture init failed\n");
            vdisplay.destroy();
            adb_forward_remove(opts.port);
            return 1;
        }

        fprintf(stderr, "[main] capture resolution: %ux%u\n",
                capturer.width(), capturer.height());

        // Connect to the Android device.
        fprintf(stderr, "[main] connecting to 127.0.0.1:%u...\n", opts.port);
        if (!client.connect(opts.port)) {
            fprintf(stderr, "[main] TCP connect failed. "
                    "Is the Android app running?\n");
            vdisplay.destroy();
            adb_forward_remove(opts.port);
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
            vdisplay.destroy();
            adb_forward_remove(opts.port);
            return 1;
        }

        fprintf(stderr, "[main] streaming %ux%u@%ufps... press Ctrl+C to stop\n",
                capturer.width(), capturer.height(), opts.fps);

        // Log detailed stats every second.
        auto last_log = std::chrono::steady_clock::now();
        uint64_t prev_encoded = 0, prev_captured = 0, prev_bytes = 0;

        while (g_running.load() && pipeline.is_running()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - last_log).count();
            if (dt < 0.5) continue;

            uint64_t enc  = pipeline.frames_encoded();
            uint64_t cap  = pipeline.frames_captured();
            uint64_t drop = pipeline.frames_dropped();
            uint64_t idle = pipeline.frames_idle();
            uint64_t byt  = pipeline.bytes_sent();

            double cap_fps = (cap - prev_captured) / dt;
            double enc_fps = (enc - prev_encoded)  / dt;
            double mbps    = (byt - prev_bytes) * 8.0 / dt / 1e6;

            fprintf(stderr,
                "[stats] cap=%.0f fps | enc=%.0f fps | "
                "%.1f Mbps | enc=%lld us | send=%lld us | "
                "rtt=%lld us | drop=%llu idle=%llu | "
                "cq=%zu sq=%zu\n",
                cap_fps, enc_fps, mbps,
                (long long)pipeline.last_encode_us(),
                (long long)pipeline.last_send_us(),
                (long long)pipeline.last_rtt_us(),
                (unsigned long long)drop,
                (unsigned long long)idle,
                pipeline.capture_queue_depth(),
                pipeline.send_queue_depth());

            prev_encoded  = enc;
            prev_captured = cap;
            prev_bytes    = byt;
            last_log      = now;
        }

        // Clean shutdown.
        fprintf(stderr, "[main] shutting down...\n");
        pipeline.stop();
        g_pipeline = nullptr;
        client.close();

        // Destroy virtual display.
        vdisplay.destroy();

        // Remove adb forward.
        adb_forward_remove(opts.port);

        fprintf(stderr, "[main] done\n");
        return 0;
    }
}
