#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

static inline bool audio_rate_supported(uint32_t rate)
{
    return rate == 44100 || rate == 48000 || rate == 88200 || rate == 96000;
}

static inline uint32_t audio_prefill(uint32_t rate, uint32_t milliseconds)
{
    uint32_t frames = (rate * milliseconds + 999) / 1000;
    // SPI delivers 32-frame bursts. Keep at least two real blocks.
    frames = (frames + 31) & ~31u;
    return frames < 64 ? 64 : frames;
}

static inline unsigned audio_next_packet(uint32_t rate, uint32_t packets_per_second,
                                         uint32_t *phase, uint32_t occupancy,
                                         uint32_t target, int *mode)
{
    if (*mode < 0 && occupancy >= target - 8) *mode = 0;
    if (*mode > 0 && occupancy <= target + 8) *mode = 0;
    if (occupancy + 16 < target) *mode = -1;
    if (occupancy > target + 16) *mode = 1;
    uint32_t total = *phase + rate;
    unsigned frames = total / packets_per_second;
    *phase = total % packets_per_second;
    return frames + *mode;
}

static inline void audio_pack_pcm(void *destination, const int32_t *source,
                                  size_t samples, unsigned subslot)
{
    if (subslot == 4) { memcpy(destination, source, samples * 4); return; }
    uint8_t *out = (uint8_t *)destination;
    for (size_t i = 0; i < samples; ++i) {
        uint32_t value = (uint32_t)source[i];
        if (subslot == 3) {
            // SPI/API is PCM24 left-aligned; USB is packed signed PCM24 LE.
            out[3 * i] = (uint8_t)(value >> 8);
            out[3 * i + 1] = (uint8_t)(value >> 16);
            out[3 * i + 2] = (uint8_t)(value >> 24);
        } else {
            out[2 * i] = (uint8_t)(value >> 16);
            out[2 * i + 1] = (uint8_t)(value >> 24);
        }
    }
}

static inline int32_t audio_unpack_sample(const uint8_t *source, unsigned subslot)
{
    uint32_t value;
    if (subslot == 4) { memcpy(&value, source, 4); return (int32_t)(value & ~255u); }
    if (subslot == 3) {
        value = ((uint32_t)source[0] << 8) | ((uint32_t)source[1] << 16) |
                ((uint32_t)source[2] << 24);
        return (int32_t)value; // sign bit already at bit 31; low byte stays zero
    }
    value = ((uint32_t)source[0] << 16) | ((uint32_t)source[1] << 24);
    return (int32_t)value;
}

static inline bool audio_format_self_test(void)
{
    const uint32_t rates[] = {44100, 48000, 88200, 96000};
    for (unsigned r = 0; r < 4; ++r) {
        uint32_t phase = 0, total = 0;
        int mode = 0;
        for (unsigned i = 0; i < 2000; ++i)
            total += audio_next_packet(rates[r], 2000, &phase, 96, 96, &mode);
        if (total != rates[r] || phase || !audio_rate_supported(rates[r])) return false;
    }
    const int32_t in[] = {0, 0x7fffff00, (int32_t)0x80000000u,
                         (int32_t)0xffff0000u, 0x12345600, (int32_t)0xedcbaa00u};
    enum { SAMPLES = sizeof(in) / sizeof(in[0]) };
    uint8_t packed[sizeof(in) + 2];
    audio_pack_pcm(packed, in, SAMPLES, 2);
    for (unsigned i = 0; i < SAMPLES; ++i)
        if ((uint32_t)audio_unpack_sample(packed + i * 2, 2) != ((uint32_t)in[i] & 0xffff0000u)) return false;
    audio_pack_pcm(packed, in, SAMPLES, 4);
    if (memcmp(packed, in, sizeof(in)) || audio_rate_supported(192000)) return false;
    const uint8_t expected24[] = {0, 0, 0, 0xff, 0xff, 0x7f, 0, 0, 0x80,
                                 0, 0xff, 0xff, 0x56, 0x34, 0x12, 0xaa, 0xcb, 0xed};
    memset(packed, 0xa5, sizeof(packed));
    audio_pack_pcm(packed + 1, in, SAMPLES, 3);
    if (memcmp(packed + 1, expected24, sizeof(expected24)) ||
        packed[0] != 0xa5 || packed[1 + sizeof(expected24)] != 0xa5) return false;
    for (unsigned i = 0; i < SAMPLES; ++i)
        if (audio_unpack_sample(packed + 1 + i * 3, 3) != in[i]) return false;
    uint32_t random = 0x12345678u;
    for (unsigned i = 0; i < 1024; ++i) {
        random = random * 1664525u + 1013904223u;
        int32_t sample = (int32_t)(random & ~255u);
        audio_pack_pcm(packed + 1, &sample, 1, 3);
        if (audio_unpack_sample(packed + 1, 3) != sample) return false;
    }
    return audio_prefill(44100, 1) == 64 && audio_prefill(96000, 1) == 96;
}
