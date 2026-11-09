/*
 * DroidScreen - the one Android clock.
 *
 * Everything on the Android side (receive/feed/render timestamps, PONG,
 * input event timestamps) uses CLOCK_MONOTONIC. MediaCodec's onFrameRendered
 * systemNano is CLOCK_MONOTONIC too, so they compare directly.
 */

#ifndef DROIDSCREEN_DS_CLOCK_H
#define DROIDSCREEN_DS_CLOCK_H

#include <stdint.h>
#include <time.h>

static inline int64_t ds_now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000LL + ts.tv_nsec / 1000;
}

static inline uint32_t ds_now_ms32(void) {
    return (uint32_t)(ds_now_us() / 1000);
}

#endif /* DROIDSCREEN_DS_CLOCK_H */
