/*
 * DroidScreen - Lock-free SPSC (single-producer, single-consumer) ring buffer
 *
 * Header-only implementation compatible with both C and C++.
 * 4MB default capacity. Each message framed as [u32 length][data].
 */

#ifndef DROIDSCREEN_RING_BUFFER_H
#define DROIDSCREEN_RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
#include <atomic>
#define RB_ATOMIC_SIZE_T      std::atomic<size_t>
#define RB_ATOMIC_STORE(p,v,o) (p)->store((v), (o))
#define RB_ATOMIC_LOAD(p,o)    (p)->load((o))
#define RB_MO_RELAXED         std::memory_order_relaxed
#define RB_MO_ACQUIRE         std::memory_order_acquire
#define RB_MO_RELEASE         std::memory_order_release
extern "C" {
#else
#include <stdatomic.h>
#define RB_ATOMIC_SIZE_T      _Atomic size_t
#define RB_ATOMIC_STORE(p,v,o) atomic_store_explicit((p), (v), (o))
#define RB_ATOMIC_LOAD(p,o)    atomic_load_explicit((p), (o))
#define RB_MO_RELAXED         memory_order_relaxed
#define RB_MO_ACQUIRE         memory_order_acquire
#define RB_MO_RELEASE         memory_order_release
#endif

typedef struct ring_buffer {
    uint8_t           *data;
    size_t             capacity;
    RB_ATOMIC_SIZE_T   write_pos;
    char               _pad1[64 - sizeof(RB_ATOMIC_SIZE_T)];
    RB_ATOMIC_SIZE_T   read_pos;
    char               _pad2[64 - sizeof(RB_ATOMIC_SIZE_T)];
} ring_buffer;

static inline ring_buffer* ring_buffer_create(size_t capacity) {
    if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
        return NULL;
    }

    ring_buffer *rb = (ring_buffer*)calloc(1, sizeof(ring_buffer));
    if (!rb) return NULL;

    rb->data = (uint8_t*)malloc(capacity);
    if (!rb->data) {
        free(rb);
        return NULL;
    }

    rb->capacity = capacity;
    RB_ATOMIC_STORE(&rb->write_pos, (size_t)0, RB_MO_RELAXED);
    RB_ATOMIC_STORE(&rb->read_pos, (size_t)0, RB_MO_RELAXED);
    return rb;
}

static inline void ring_buffer_destroy(ring_buffer *rb) {
    if (rb) {
        free(rb->data);
        free(rb);
    }
}

static inline size_t ring_buffer_available_read(ring_buffer *rb) {
    size_t w = RB_ATOMIC_LOAD(&rb->write_pos, RB_MO_ACQUIRE);
    size_t r = RB_ATOMIC_LOAD(&rb->read_pos, RB_MO_ACQUIRE);
    return w - r;
}

static inline size_t ring_buffer_available_write(ring_buffer *rb) {
    return rb->capacity - ring_buffer_available_read(rb);
}

static inline int ring_buffer_write_message(ring_buffer *rb,
                                            const uint8_t *data, size_t len) {
    size_t msg_size = sizeof(uint32_t) + len;
    if (ring_buffer_available_write(rb) < msg_size) {
        return -1;
    }

    size_t w = RB_ATOMIC_LOAD(&rb->write_pos, RB_MO_RELAXED);
    size_t mask = rb->capacity - 1;

    uint8_t len_bytes[4];
    uint32_t le_len = (uint32_t)len;
    len_bytes[0] = (uint8_t)(le_len & 0xFF);
    len_bytes[1] = (uint8_t)((le_len >> 8) & 0xFF);
    len_bytes[2] = (uint8_t)((le_len >> 16) & 0xFF);
    len_bytes[3] = (uint8_t)((le_len >> 24) & 0xFF);

    size_t pos = w & mask;
    size_t first = rb->capacity - pos;
    if (first > 4) first = 4;
    memcpy(rb->data + pos, len_bytes, first);
    if (first < 4) {
        memcpy(rb->data, len_bytes + first, 4 - first);
    }
    w += 4;

    pos = w & mask;
    first = rb->capacity - pos;
    if (first > len) first = len;
    memcpy(rb->data + pos, data, first);
    if (first < len) {
        memcpy(rb->data, data + first, len - first);
    }
    w += len;

    RB_ATOMIC_STORE(&rb->write_pos, w, RB_MO_RELEASE);
    return 0;
}

static inline size_t ring_buffer_read_message(ring_buffer *rb,
                                              uint8_t *buf, size_t max_len) {
    size_t avail = ring_buffer_available_read(rb);
    if (avail < sizeof(uint32_t)) {
        return 0;
    }

    size_t r = RB_ATOMIC_LOAD(&rb->read_pos, RB_MO_RELAXED);
    size_t mask = rb->capacity - 1;

    uint8_t len_bytes[4];
    size_t pos = r & mask;
    size_t first = rb->capacity - pos;
    if (first > 4) first = 4;
    memcpy(len_bytes, rb->data + pos, first);
    if (first < 4) {
        memcpy(len_bytes + first, rb->data, 4 - first);
    }

    uint32_t msg_len = (uint32_t)len_bytes[0]
                     | ((uint32_t)len_bytes[1] << 8)
                     | ((uint32_t)len_bytes[2] << 16)
                     | ((uint32_t)len_bytes[3] << 24);

    size_t total = sizeof(uint32_t) + msg_len;
    if (avail < total) {
        return 0;
    }

    r += 4;

    size_t copy_len = msg_len < max_len ? msg_len : max_len;
    pos = r & mask;
    first = rb->capacity - pos;
    if (first > copy_len) first = copy_len;
    memcpy(buf, rb->data + pos, first);
    if (first < copy_len) {
        memcpy(buf + first, rb->data, copy_len - first);
    }
    r += msg_len;

    RB_ATOMIC_STORE(&rb->read_pos, r, RB_MO_RELEASE);

    return (size_t)msg_len;
}

/**
 * Return the number of complete messages available to read.
 * Walks the buffer counting [u32 length][payload] frames without consuming.
 * O(n) in the number of messages but each iteration is cheap (no data copy).
 */
static inline size_t ring_buffer_messages_pending(ring_buffer *rb) {
    size_t avail = ring_buffer_available_read(rb);
    if (avail < sizeof(uint32_t)) return 0;

    size_t r = RB_ATOMIC_LOAD(&rb->read_pos, RB_MO_ACQUIRE);
    size_t mask = rb->capacity - 1;
    size_t count = 0;
    size_t consumed = 0;

    while (consumed + sizeof(uint32_t) <= avail) {
        uint8_t len_bytes[4];
        for (size_t i = 0; i < 4; i++)
            len_bytes[i] = rb->data[(r + consumed + i) & mask];

        uint32_t msg_len = (uint32_t)len_bytes[0]
                         | ((uint32_t)len_bytes[1] << 8)
                         | ((uint32_t)len_bytes[2] << 16)
                         | ((uint32_t)len_bytes[3] << 24);

        size_t total = sizeof(uint32_t) + msg_len;
        if (consumed + total > avail) break;

        consumed += total;
        count++;
    }

    return count;
}

/**
 * Reset the ring buffer, discarding all data.
 * Only safe when no concurrent reads/writes are happening
 * (e.g., between client sessions when producer has stopped).
 */
static inline void ring_buffer_reset(ring_buffer *rb) {
    RB_ATOMIC_STORE(&rb->write_pos, (size_t)0, RB_MO_RELEASE);
    RB_ATOMIC_STORE(&rb->read_pos, (size_t)0, RB_MO_RELEASE);
}

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_RING_BUFFER_H */
