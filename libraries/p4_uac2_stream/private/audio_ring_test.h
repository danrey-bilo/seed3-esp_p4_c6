#pragma once
#include "audio_ring.h"
// Uses the capture ring only before USB starts; no extra permanent scratch ring.
static inline bool audio_ring_self_test(audio_ring_t *ring) {
    int32_t in[2 * 49], out[2 * 49];
    memset(ring, 0, sizeof(*ring));
    ring->head = ring->tail = UINT32_MAX - 20u;
    for (unsigned n = 0; n < 100; ++n) {
        for (unsigned i = 0; i < 98; ++i) in[i] = (int32_t)((n * 98 + i) << 8);
        if (audio_ring_write(ring, in, 49) != 49 || audio_ring_fill(ring) != 49) return false;
        if (audio_ring_read(ring, out, 17) != 17 || memcmp(in, out, 17 * 8)) return false;
        if (audio_ring_read(ring, out, 49) != 32 || memcmp(in + 34, out, 32 * 8)) return false;
    }
    for (unsigned n = 0; n < 10; ++n) if (audio_ring_write(ring, in, 49) != 49) return false;
    if (audio_ring_write(ring, in, 49) != 22 || audio_ring_write(ring, in, 1) != 0) return false;
    if (audio_ring_discard(ring) != 512 || audio_ring_fill(ring) != 0) return false;
    if (audio_ring_read(ring, out, 49) != 0) return false;
    int mode = 0;
    if (audio_packet_frames(96, &mode) != 48 || audio_packet_frames(79, &mode) != 47 ||
        audio_packet_frames(87, &mode) != 47 || audio_packet_frames(88, &mode) != 48 ||
        audio_packet_frames(113, &mode) != 49 || audio_packet_frames(105, &mode) != 49 ||
        audio_packet_frames(104, &mode) != 48) return false;
    return true;
}
