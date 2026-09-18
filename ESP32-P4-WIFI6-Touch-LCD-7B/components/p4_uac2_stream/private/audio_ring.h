#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define AUDIO_RING_CAPACITY 512u
typedef struct {
    uint32_t head, tail;
    int32_t pcm[AUDIO_RING_CAPACITY * 2];
} audio_ring_t;

// Only producer writes head; only consumer writes tail, even on reset/overflow.
static inline uint32_t audio_ring_fill(const audio_ring_t *r) {
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    uint32_t n = h - t;
    return n <= AUDIO_RING_CAPACITY ? n : 0; // observer can straddle a reset
}
static inline size_t audio_ring_write(audio_ring_t *r, const int32_t *p, size_t n) {
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_RELAXED);
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_ACQUIRE);
    size_t available = AUDIO_RING_CAPACITY - (h - t);
    if (n > available) n = available;
    size_t first = AUDIO_RING_CAPACITY - (h & (AUDIO_RING_CAPACITY - 1));
    if (first > n) first = n;
    memcpy(r->pcm + 2 * (h & (AUDIO_RING_CAPACITY - 1)), p, first * 8);
    memcpy(r->pcm, p + first * 2, (n - first) * 8);
    __atomic_store_n(&r->head, h + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
}
static inline size_t audio_ring_read(audio_ring_t *r, int32_t *p, size_t n) {
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    if (n > h - t) n = h - t;
    size_t first = AUDIO_RING_CAPACITY - (t & (AUDIO_RING_CAPACITY - 1));
    if (first > n) first = n;
    memcpy(p, r->pcm + 2 * (t & (AUDIO_RING_CAPACITY - 1)), first * 8);
    memcpy(p + first * 2, r->pcm, (n - first) * 8);
    __atomic_store_n(&r->tail, t + (uint32_t)n, __ATOMIC_RELEASE);
    return n;
}
static inline uint32_t audio_ring_discard(audio_ring_t *r) {
    uint32_t t = __atomic_load_n(&r->tail, __ATOMIC_RELAXED);
    uint32_t h = __atomic_load_n(&r->head, __ATOMIC_ACQUIRE);
    __atomic_store_n(&r->tail, h, __ATOMIC_RELEASE);
    return h - t;
}
// Hysteretic packet controller. Changes packet LENGTH, never sample contents.
static inline unsigned audio_packet_frames(uint32_t fill, int *mode) {
    if (*mode < 0 && fill >= 88) *mode = 0;
    if (*mode > 0 && fill <= 104) *mode = 0;
    if (fill < 80) *mode = -1;
    if (fill > 112) *mode = 1;
    return (unsigned)(48 + *mode);
}
