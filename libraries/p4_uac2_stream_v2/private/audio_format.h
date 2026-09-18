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
        out[2 * i] = (uint8_t)(value >> 16);
        out[2 * i + 1] = (uint8_t)(value >> 24);
    }
}

static inline int32_t audio_unpack_sample(const uint8_t *source, unsigned subslot)
{
    uint32_t value;
    if (subslot == 4) { memcpy(&value, source, 4); return (int32_t)(value & ~255u); }
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
    const int32_t in[] = {0, 0x7fffff00, (int32_t)0x80000000u, (int32_t)0xffff0000u};
    uint8_t packed[16];
    audio_pack_pcm(packed, in, 4, 2);
    for (unsigned i = 0; i < 4; ++i)
        if ((uint32_t)audio_unpack_sample(packed + i * 2, 2) != ((uint32_t)in[i] & 0xffff0000u)) return false;
    audio_pack_pcm(packed, in, 4, 4);
    if (memcmp(packed, in, sizeof(in)) || audio_rate_supported(192000)) return false;
    return audio_prefill(44100, 1) == 64 && audio_prefill(96000, 1) == 96;
}
