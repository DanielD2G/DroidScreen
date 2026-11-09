/*
 * DroidScreen - outbound queue to the desktop (see outbound.h).
 */

#include "outbound.h"
#include "tcp_server.h"

#include <android/log.h>
#include <pthread.h>
#include <string.h>

#include <atomic>
#include <condition_variable>
#include <mutex>

#define TAG "DroidScreen"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)

namespace {

constexpr size_t kQueueCap = 512;  /* several seconds of 240 Hz pen input */

struct Slot {
    uint16_t len;
    uint8_t  data[DS_OUTBOUND_MAX_MSG];
};

std::mutex              g_mutex;
std::condition_variable g_cv;
Slot                    g_slots[kQueueCap];
size_t                  g_head = 0;     /* guarded: next slot to send */
size_t                  g_count = 0;    /* guarded */
int                     g_fd = -1;      /* guarded: live session socket */
uint64_t                g_session = 0;  /* guarded: bumps on every session change */
bool                    g_running = false;
pthread_t               g_thread;
std::atomic<bool>       g_connected{false};
uint64_t                g_dropped = 0;  /* guarded */

void* sender_thread(void*) {
    Slot slot;
    std::unique_lock<std::mutex> lock(g_mutex);
    while (true) {
        g_cv.wait(lock, [] { return !g_running || g_count > 0; });
        if (!g_running) break;

        slot.len = g_slots[g_head].len;
        memcpy(slot.data, g_slots[g_head].data, slot.len);
        g_head = (g_head + 1) % kQueueCap;
        g_count--;
        int fd = g_fd;
        uint64_t session = g_session;

        lock.unlock();
        bool ok = fd >= 0 && tcp_send_all(fd, slot.data, slot.len) == 0;
        lock.lock();

        if (!ok && fd >= 0 && session == g_session) {
            /* tcp_send_all already shut the socket down; the recv thread sees
             * it and ends the session. Nothing queued for it can be sent. */
            g_count = 0;
            g_head = 0;
            g_fd = -1;
            g_connected.store(false, std::memory_order_release);
        }
    }
    return nullptr;
}

}  // namespace

void outbound_start(void) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_running) return;
    g_running = true;
    g_head = 0;
    g_count = 0;
    g_fd = -1;
    g_connected.store(false, std::memory_order_release);
    pthread_create(&g_thread, nullptr, sender_thread, nullptr);
}

void outbound_stop(void) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running) return;
        g_running = false;
        g_fd = -1;
        g_count = 0;
        g_connected.store(false, std::memory_order_release);
    }
    g_cv.notify_all();
    pthread_join(g_thread, nullptr);
}

void outbound_set_session(int fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_fd = fd;
    g_session++;
    g_head = 0;
    g_count = 0;
    if (g_dropped > 0) {
        LOGW("outbound: %llu messages dropped last session (queue full)",
             (unsigned long long)g_dropped);
        g_dropped = 0;
    }
    g_connected.store(fd >= 0, std::memory_order_release);
}

bool outbound_connected(void) {
    return g_connected.load(std::memory_order_acquire);
}

bool outbound_enqueue(const uint8_t *msg, size_t len) {
    if (!msg || len == 0 || len > DS_OUTBOUND_MAX_MSG) return false;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_running || g_fd < 0) return false;
        if (g_count == kQueueCap) {
            if ((g_dropped++ % 256) == 0) {
                LOGW("outbound: queue full, dropping message (type=0x%02x)", msg[0]);
            }
            return false;
        }
        Slot &slot = g_slots[(g_head + g_count) % kQueueCap];
        slot.len = (uint16_t)len;
        memcpy(slot.data, msg, len);
        g_count++;
    }
    g_cv.notify_one();
    return true;
}
