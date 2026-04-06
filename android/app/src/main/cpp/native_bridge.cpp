/*
 * DroidScreen - JNI bridge between Kotlin and native C/C++ code.
 *
 * Manages TCP server, receive/decode threads, and touch forwarding.
 * Supports reconnection: recv_thread loops back to accept() after
 * a client disconnects, resetting the decoder and ring buffer.
 */

#include <jni.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/log.h>

#include <atomic>
#include <cstring>
#include <cstdlib>
#include <pthread.h>
#include <unistd.h>
#include <sched.h>
#include <sys/resource.h>

#include "tcp_server.h"
#include "decoder.h"
#include "ring_buffer.h"
#include "touch_sender.h"
#include "sps_patch.h"

extern "C" {
#include "droidscreen/protocol.h"
#include "droidscreen/handshake.h"
#include "droidscreen/frame.h"
#include "droidscreen/touch.h"
}

#define TAG "DroidScreen"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Ring buffer capacity: 4 MB */
#define RING_BUFFER_CAPACITY (4 * 1024 * 1024)

/* Max single frame payload size: 2 MB */
#define MAX_FRAME_SIZE (2 * 1024 * 1024)

/* Status constants — must match MainActivity.kt companion object */
#define STATUS_WAITING      0
#define STATUS_CONNECTED    1
#define STATUS_DISCONNECTED 2
#define STATUS_ERROR        3

/* ---- Global state ---- */
static std::atomic<bool> g_running{false};
static std::atomic<bool> g_decoder_configured{false};
static int               g_server_fd = -1;
static int               g_client_fd = -1;
static ANativeWindow*    g_window    = nullptr;
static ring_buffer*      g_ring_buf  = nullptr;
static pthread_t         g_recv_thread;
static pthread_t         g_decode_thread;
static DecoderContext*   g_decoder   = nullptr;

/* ---- JNI callback state ---- */
static JavaVM*           g_jvm      = nullptr;
static jobject           g_activity = nullptr;   /* global ref */
static jmethodID         g_onStatusChanged = nullptr;

/**
 * Notify Java about native connection status change.
 * Safe to call from any thread — attaches/detaches JNI as needed.
 */
static void notify_status(int status) {
    if (!g_jvm || !g_activity || !g_onStatusChanged) return;

    JNIEnv* env = nullptr;
    bool did_attach = false;

    jint result = g_jvm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    if (result == JNI_EDETACHED) {
        if (g_jvm->AttachCurrentThread(&env, nullptr) != JNI_OK) {
            LOGE("notify_status: AttachCurrentThread failed");
            return;
        }
        did_attach = true;
    } else if (result != JNI_OK) {
        LOGE("notify_status: GetEnv failed: %d", result);
        return;
    }

    env->CallVoidMethod(g_activity, g_onStatusChanged, (jint)status);

    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }

    if (did_attach) {
        g_jvm->DetachCurrentThread();
    }
}

/* ---- Receive thread ----
 * Accepts connections in a loop. For each connection:
 *   1. Performs handshake
 *   2. Configures decoder
 *   3. Runs message loop (video frames → ring buffer)
 *   4. On disconnect: cleans up and loops back to accept
 */
static void* recv_thread_func(void* /*arg*/) {
    /* Set high priority for low latency */
    setpriority(PRIO_PROCESS, 0, -10);

    /* Use a static thread-local buffer to avoid per-connection malloc. */
    static uint8_t payload_buf[MAX_FRAME_SIZE];
    uint8_t hdr_buf[DS_HEADER_SIZE];
    ds_header_t hdr;

    /* ---- Outer reconnection loop ---- */
    while (g_running.load(std::memory_order_acquire)) {

        /* Notify Java: waiting for connection */
        notify_status(STATUS_WAITING);
        LOGI("recv_thread: waiting for connection on fd=%d", g_server_fd);

        /* Accept a client connection (blocking). */
        g_client_fd = tcp_server_accept(g_server_fd);
        if (g_client_fd < 0) {
            if (g_running.load(std::memory_order_acquire)) {
                LOGW("recv_thread: accept failed, retrying in 500ms...");
                usleep(500000);
            }
            continue;
        }
        LOGI("recv_thread: client connected, fd=%d", g_client_fd);

        /* ---- Handshake ---- */
        if (tcp_recv_exact(g_client_fd, hdr_buf, DS_HEADER_SIZE) != 0) {
            LOGE("recv_thread: failed to read handshake header");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        ds_header_deserialize(hdr_buf, &hdr);

        if (hdr.type != DS_MSG_HANDSHAKE_REQ || hdr.length != DS_HANDSHAKE_REQ_SIZE) {
            LOGE("recv_thread: unexpected first message type=0x%02x len=%u",
                 hdr.type, hdr.length);
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        uint8_t hs_buf[DS_HANDSHAKE_REQ_SIZE];
        if (tcp_recv_exact(g_client_fd, hs_buf, DS_HANDSHAKE_REQ_SIZE) != 0) {
            LOGE("recv_thread: failed to read handshake payload");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }

        ds_handshake_req_t req;
        ds_handshake_req_deserialize(hs_buf, &req);
        LOGI("recv_thread: handshake req: %ux%u @ %u fps, codec=%u",
             req.width, req.height, req.fps, req.codec);

        /* Configure decoder with the negotiated resolution */
        if (g_decoder) {
            decoder_configure(g_decoder, req.width, req.height);
            g_decoder_configured.store(true, std::memory_order_release);
        }

        /* Send handshake response */
        ds_handshake_resp_t resp;
        memset(&resp, 0, sizeof(resp));
        resp.protocol_version   = DS_PROTOCOL_VERSION;
        resp.accepted_width     = req.width;
        resp.accepted_height    = req.height;
        resp.accepted_fps       = req.fps;
        resp.accepted_codec     = req.codec;
        resp.decoder_max_bitrate = req.max_bitrate_kbps;
        resp.touch_supported    = 1;

        uint8_t resp_hdr[DS_HEADER_SIZE];
        ds_header_t resp_header;
        resp_header.type   = DS_MSG_HANDSHAKE_RESP;
        resp_header.flags  = 0;
        resp_header.length = DS_HANDSHAKE_RESP_SIZE;
        ds_header_serialize(resp_hdr, &resp_header);

        uint8_t resp_payload[DS_HANDSHAKE_RESP_SIZE];
        ds_handshake_resp_serialize(resp_payload, &resp);

        if (tcp_send_all(g_client_fd, resp_hdr, DS_HEADER_SIZE) != 0 ||
            tcp_send_all(g_client_fd, resp_payload, DS_HANDSHAKE_RESP_SIZE) != 0) {
            LOGE("recv_thread: failed to send handshake response");
            tcp_close(g_client_fd);
            g_client_fd = -1;
            continue;
        }
        LOGI("recv_thread: handshake complete");

        /* Notify Java: connected and streaming */
        notify_status(STATUS_CONNECTED);

        /* ---- Message loop ---- */
        while (g_running.load(std::memory_order_acquire)) {
            /* Read message header */
            if (tcp_recv_exact(g_client_fd, hdr_buf, DS_HEADER_SIZE) != 0) {
                LOGW("recv_thread: connection closed or error reading header");
                break;
            }
            ds_header_deserialize(hdr_buf, &hdr);

            if (hdr.length > MAX_FRAME_SIZE) {
                LOGE("recv_thread: payload too large: %u", hdr.length);
                break;
            }

            /* Read payload */
            if (hdr.length > 0) {
                if (tcp_recv_exact(g_client_fd, payload_buf, hdr.length) != 0) {
                    LOGW("recv_thread: connection closed or error reading payload");
                    break;
                }
            }

            switch (hdr.type) {
                case DS_MSG_VIDEO_FRAME: {
                    /* Write NAL data to ring buffer for decode thread */
                    if (ring_buffer_write_message(g_ring_buf, payload_buf, hdr.length) != 0) {
                        LOGW("recv_thread: ring buffer full, dropping frame");
                    }
                    break;
                }

                case DS_MSG_PING: {
                    /* Respond with PONG */
                    ds_header_t pong;
                    pong.type   = DS_MSG_PONG;
                    pong.flags  = 0;
                    pong.length = 0;
                    uint8_t pong_buf[DS_HEADER_SIZE];
                    ds_header_serialize(pong_buf, &pong);
                    tcp_send_all(g_client_fd, pong_buf, DS_HEADER_SIZE);
                    break;
                }

                case DS_MSG_CONTROL: {
                    if (hdr.length > 0 && payload_buf[0] == DS_CTRL_DISCONNECT) {
                        LOGI("recv_thread: received disconnect control");
                        goto end_message_loop;
                    }
                    break;
                }

                default:
                    LOGD("recv_thread: ignoring message type=0x%02x", hdr.type);
                    break;
            }
        }

    end_message_loop:

        /* ---- Cleanup between sessions ---- */
        LOGI("recv_thread: client disconnected, cleaning up for reconnection");

        /* Close client socket */
        if (g_client_fd >= 0) {
            tcp_close(g_client_fd);
            g_client_fd = -1;
        }

        /* Signal decode thread to pause */
        g_decoder_configured.store(false, std::memory_order_release);

        /* Give decode thread time to notice and stop touching the decoder */
        usleep(5000);  /* 5ms — decode thread polls at 100us */

        /* Reset ring buffer to purge stale video data */
        if (g_ring_buf) {
            ring_buffer_reset(g_ring_buf);
        }

        /* Destroy and recreate decoder for clean state */
        if (g_decoder) {
            decoder_destroy(g_decoder);
            g_decoder = nullptr;
        }
        if (g_window) {
            g_decoder = decoder_create(g_window);
            if (!g_decoder) {
                LOGE("recv_thread: failed to recreate decoder");
                notify_status(STATUS_ERROR);
                break;  /* Fatal — exit thread */
            }
        }

        /* Notify Java: disconnected (will show "reconnecting..." UI) */
        if (g_running.load(std::memory_order_acquire)) {
            notify_status(STATUS_DISCONNECTED);
            /* Brief debounce before re-accepting */
            usleep(200000);  /* 200ms */
        }
    }

    LOGI("recv_thread: exiting");
    return nullptr;
}

/* ---- Decode thread ----
 * Reads NAL units from ring buffer and feeds them to MediaCodec decoder.
 * Survives reconnections — pauses when decoder is unconfigured.
 */
static void* decode_thread_func(void* /*arg*/) {
    /* Set high priority */
    setpriority(PRIO_PROCESS, 0, -8);

    /* Try to pin decode thread to big cores (cores 4-7 on typical ARM big.LITTLE). */
    {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        int ncpus = sysconf(_SC_NPROCESSORS_ONLN);
        int first_big = ncpus > 4 ? ncpus / 2 : 0;
        for (int i = first_big; i < ncpus; i++) {
            CPU_SET(i, &cpuset);
        }
        if (sched_setaffinity(0, sizeof(cpuset), &cpuset) == 0) {
            LOGI("decode_thread: pinned to cores %d-%d", first_big, ncpus - 1);
        }
    }

    LOGI("decode_thread: started, waiting for decoder configuration...");

    auto* nal_buf = static_cast<uint8_t*>(malloc(MAX_FRAME_SIZE));
    if (!nal_buf) {
        LOGE("decode_thread: failed to allocate NAL buffer");
        return nullptr;
    }

    int64_t pts_us = 0;
    uint32_t frames_fed = 0;
    uint32_t frames_rendered = 0;
    uint32_t feed_errors = 0;
    struct timespec ts_start, ts_now;
    clock_gettime(CLOCK_MONOTONIC, &ts_start);

    while (g_running.load(std::memory_order_acquire)) {
        /* Wait for decoder to be configured (pauses between reconnections) */
        if (!g_decoder_configured.load(std::memory_order_acquire)) {
            usleep(1000); /* 1ms spin wait */
            /* Reset stats on reconnection */
            frames_fed = 0;
            frames_rendered = 0;
            feed_errors = 0;
            pts_us = 0;
            clock_gettime(CLOCK_MONOTONIC, &ts_start);
            continue;
        }

        /* Always try to drain first — output may be ready even without new input */
        int r = decoder_drain(g_decoder);
        if (r > 0) frames_rendered += r;

        /* Try to read a NAL unit from the ring buffer */
        size_t nal_len = ring_buffer_read_message(g_ring_buf, nal_buf, MAX_FRAME_SIZE);
        if (nal_len == 0) {
            usleep(100); /* 100us — 5x faster polling than before */
            continue;
        }

        /* Identify NAL type. Patch SPS for low-latency (Moonlight trick).
         * SPS=7, PPS=8 → BUFFER_FLAG_CODEC_CONFIG. IDR=5 → keyframe. */
        uint32_t flags = 0;
        uint8_t first_nal_type = 0;

        if (nal_len >= 5 && nal_buf[0] == 0 && nal_buf[1] == 0 &&
            nal_buf[2] == 0 && nal_buf[3] == 1) {
            first_nal_type = nal_buf[4] & 0x1F;
        } else if (nal_len > 0) {
            first_nal_type = nal_buf[0] & 0x1F;
        }

        if (first_nal_type == 7 || first_nal_type == 8) {
            flags = 2; /* BUFFER_FLAG_CODEC_CONFIG */

            /* Patch SPS constraint flags to signal no reordering needed. */
            if (first_nal_type == 7) {
                sps_patch_constraints(nal_buf, nal_len);
            }
        }

        /* Feed to decoder (non-blocking — timeout 0) */
        int ret = decoder_feed(g_decoder, nal_buf, nal_len, pts_us, flags);
        if (ret == 0) {
            frames_fed++;
        } else {
            feed_errors++;
        }
        pts_us += 16667; /* ~60fps timestamp increment */

        /* Immediately drain again after feeding */
        r = decoder_drain(g_decoder);
        if (r > 0) frames_rendered += r;

        /* Log stats every ~2 seconds */
        if ((frames_fed % 120) == 0 && frames_fed > 0) {
            clock_gettime(CLOCK_MONOTONIC, &ts_now);
            double elapsed = (ts_now.tv_sec - ts_start.tv_sec)
                           + (ts_now.tv_nsec - ts_start.tv_nsec) / 1e9;
            size_t ring_used = ring_buffer_available_read(g_ring_buf);
            LOGI("decode: fed=%u rendered=%u err=%u | "
                 "%.1f fed/s %.1f render/s | ring=%zu bytes",
                 frames_fed, frames_rendered, feed_errors,
                 frames_fed / elapsed, frames_rendered / elapsed,
                 ring_used);
        }
    }

    free(nal_buf);
    LOGI("decode_thread: exiting");
    return nullptr;
}

/* ---- JNI functions ---- */

extern "C" {

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeInit(
        JNIEnv* env, jobject thiz, jobject surface, jint port) {

    if (g_running.load()) {
        LOGW("nativeInit: already running, ignoring");
        return;
    }

    LOGI("nativeInit: starting with port=%d", port);

    /* Cache JVM and Activity reference for callbacks */
    env->GetJavaVM(&g_jvm);
    g_activity = env->NewGlobalRef(thiz);

    jclass clazz = env->GetObjectClass(thiz);
    g_onStatusChanged = env->GetMethodID(clazz, "onNativeStatusChanged", "(I)V");
    if (!g_onStatusChanged) {
        LOGE("nativeInit: could not find onNativeStatusChanged(I)V method");
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Get native window from Surface */
    g_window = ANativeWindow_fromSurface(env, surface);
    if (!g_window) {
        LOGE("nativeInit: failed to get ANativeWindow");
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Create ring buffer */
    g_ring_buf = ring_buffer_create(RING_BUFFER_CAPACITY);
    if (!g_ring_buf) {
        LOGE("nativeInit: failed to create ring buffer");
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Create decoder */
    g_decoder = decoder_create(g_window);
    if (!g_decoder) {
        LOGE("nativeInit: failed to create decoder");
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    /* Start TCP server */
    g_server_fd = tcp_server_start(port);
    if (g_server_fd < 0) {
        LOGE("nativeInit: failed to start TCP server on port %d", port);
        decoder_destroy(g_decoder);
        g_decoder = nullptr;
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
        ANativeWindow_release(g_window);
        g_window = nullptr;
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
        g_jvm = nullptr;
        return;
    }

    g_running.store(true, std::memory_order_release);
    g_decoder_configured.store(false, std::memory_order_release);

    /* Spawn threads */
    pthread_create(&g_recv_thread, nullptr, recv_thread_func, nullptr);
    pthread_create(&g_decode_thread, nullptr, decode_thread_func, nullptr);

    LOGI("nativeInit: started successfully");
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeStop(
        JNIEnv* env, jobject /*thiz*/) {

    if (!g_running.load()) {
        LOGW("nativeStop: not running");
        return;
    }

    LOGI("nativeStop: stopping...");
    g_running.store(false, std::memory_order_release);

    /* Close sockets to unblock recv/accept */
    if (g_client_fd >= 0) {
        tcp_close(g_client_fd);
        g_client_fd = -1;
    }
    if (g_server_fd >= 0) {
        tcp_close(g_server_fd);
        g_server_fd = -1;
    }

    /* Join threads */
    pthread_join(g_recv_thread, nullptr);
    pthread_join(g_decode_thread, nullptr);

    /* Cleanup decoder */
    if (g_decoder) {
        decoder_destroy(g_decoder);
        g_decoder = nullptr;
    }

    /* Cleanup ring buffer */
    if (g_ring_buf) {
        ring_buffer_destroy(g_ring_buf);
        g_ring_buf = nullptr;
    }

    /* Release native window */
    if (g_window) {
        ANativeWindow_release(g_window);
        g_window = nullptr;
    }

    g_decoder_configured.store(false, std::memory_order_release);

    /* Cleanup JNI refs */
    if (g_activity) {
        env->DeleteGlobalRef(g_activity);
        g_activity = nullptr;
    }
    g_onStatusChanged = nullptr;
    g_jvm = nullptr;

    LOGI("nativeStop: stopped");
}

JNIEXPORT void JNICALL
Java_com_droidscreen_app_MainActivity_nativeSendTouch(
        JNIEnv* /*env*/, jobject /*thiz*/,
        jint action, jint pointerId, jint xFrac, jint yFrac, jint pressure) {

    if (g_client_fd < 0) {
        return;
    }

    touch_sender_send(g_client_fd, action, pointerId, xFrac, yFrac, pressure);
}

} /* extern "C" */
