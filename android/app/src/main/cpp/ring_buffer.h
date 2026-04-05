/*
 * DroidScreen - Lock-free SPSC (single-producer, single-consumer) ring buffer
 *
 * Header-only C implementation. 4MB default capacity.
 * Each message is framed as [u32 length][data].
 */

#ifndef DROIDSCREEN_RING_BUFFER_H
#define DROIDSCREEN_RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ring_buffer {
    uint8_t       *data;
    size_t         capacity;
    _Atomic size_t write_pos;
    char           _pad1[56];  /* Prevent false sharing */
    _Atomic size_t read_pos;
    char           _pad2[56];  /* Prevent false sharing */
} ring_buffer;

/*
 * Create a ring buffer with the given capacity in bytes.
 * Returns NULL on allocation failure.
 */
static inline ring_buffer* ring_buffer_create(size_t capacity) {
    /* Capacity must be a power of 2 for bitmask indexing */
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
    atomic_store_explicit(&rb->write_pos, 0, memory_order_relaxed);
    atomic_store_explicit(&rb->read_pos, 0, memory_order_relaxed);
    return rb;
}

/*
 * Destroy the ring buffer and free memory.
 */
static inline void ring_buffer_destroy(ring_buffer *rb) {
    if (rb) {
        free(rb->data);
        free(rb);
    }
}

/*
 * Return the number of bytes available for reading.
 */
static inline size_t ring_buffer_available_read(ring_buffer *rb) {
    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    return w - r;
}

/*
 * Return the number of bytes of free space for writing.
 */
static inline size_t ring_buffer_available_write(ring_buffer *rb) {
    return rb->capacity - ring_buffer_available_read(rb);
}

/*
 * Write a framed message [u32 length][data] into the ring buffer.
 * Single producer only.
 *
 * Returns 0 on success, -1 if there is not enough space.
 */
static inline int ring_buffer_write_message(ring_buffer *rb,
                                            const uint8_t *data, size_t len) {
    size_t msg_size = sizeof(uint32_t) + len;
    if (ring_buffer_available_write(rb) < msg_size) {
        return -1;
    }

    size_t w = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    size_t mask = rb->capacity - 1;

    /* Write length header (little-endian u32) */
    uint32_t le_len = (uint32_t)len;
    uint8_t len_bytes[4];
    len_bytes[0] = (uint8_t)(le_len & 0xFF);
    len_bytes[1] = (uint8_t)((le_len >> 8) & 0xFF);
    len_bytes[2] = (uint8_t)((le_len >> 16) & 0xFF);
    len_bytes[3] = (uint8_t)((le_len >> 24) & 0xFF);

    for (size_t i = 0; i < 4; i++) {
        rb->data[(w + i) & mask] = len_bytes[i];
    }
    w += 4;

    /* Write payload data */
    for (size_t i = 0; i < len; i++) {
        rb->data[(w + i) & mask] = data[i];
    }
    w += len;

    /* Publish write position */
    atomic_store_explicit(&rb->write_pos, w, memory_order_release);
    return 0;
}

/*
 * Read one framed message from the ring buffer.
 * Single consumer only.
 *
 * Copies up to max_len bytes of payload into buf.
 * Returns the payload length (may be > max_len if truncated), or 0 if no message available.
 */
static inline size_t ring_buffer_read_message(ring_buffer *rb,
                                              uint8_t *buf, size_t max_len) {
    size_t avail = ring_buffer_available_read(rb);
    if (avail < sizeof(uint32_t)) {
        return 0;
    }

    size_t r = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    size_t mask = rb->capacity - 1;

    /* Read length header */
    uint8_t len_bytes[4];
    for (size_t i = 0; i < 4; i++) {
        len_bytes[i] = rb->data[(r + i) & mask];
    }

    uint32_t msg_len = (uint32_t)len_bytes[0]
                     | ((uint32_t)len_bytes[1] << 8)
                     | ((uint32_t)len_bytes[2] << 16)
                     | ((uint32_t)len_bytes[3] << 24);

    size_t total = sizeof(uint32_t) + msg_len;
    if (avail < total) {
        /* Incomplete message, wait for more data */
        return 0;
    }

    r += 4;

    /* Copy payload */
    size_t copy_len = msg_len < max_len ? msg_len : max_len;
    for (size_t i = 0; i < copy_len; i++) {
        buf[i] = rb->data[(r + i) & mask];
    }
    r += msg_len;

    /* Publish read position */
    atomic_store_explicit(&rb->read_pos, r, memory_order_release);

    return (size_t)msg_len;
}

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_RING_BUFFER_H */
