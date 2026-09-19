#pragma once

#include "seedfx_graph_protocol.h"

#include <cstddef>
#include <cstdint>

enum class SeedFxGraphResult : uint8_t
{
    Ok = 0,
    InvalidHeader,
    InvalidCrc,
    InvalidNode,
    InvalidEdge,
    Cycle,
    GraphBusy,
    CacheBusy,
    CacheBounds,
    CacheSequence,
    CacheCrc,
};

struct SeedFxGraphStats
{
    uint32_t active_revision;
    uint32_t active_nodes;
    uint32_t graph_commits;
    uint32_t graph_rejects;
    uint32_t cache_bytes_used;
    uint32_t cache_capacity;
    uint32_t cached_packages;
};

/* Call after DaisySeed::Init(), because that initializes external SDRAM. */
void seedfx_graph_init();

/* Foreground-only staging. A valid graph becomes active at the next audio
 * block boundary; the function never changes the graph currently executing. */
SeedFxGraphResult seedfx_graph_stage(const SeedFxGraphDefinition& graph);

/* Audio-callback-only, fixed-cost processing. input/output are planar stereo
 * arrays. The implementation allocates nothing and never locks. */
void seedfx_graph_process(const float* const input[2],
                          float* const output[2],
                          std::size_t frames,
                          uint32_t sample_rate,
                          bool enabled,
                          uint8_t output_mask = 3U);

/* Sequential package upload into the 62 MiB SDRAM arena. It is intentionally
 * separate from the graph swap: files/resources are completely verified
 * before any graph may refer to them. */
SeedFxGraphResult seedfx_cache_begin(uint32_t package_id,
                                     uint32_t total_bytes,
                                     uint32_t content_crc32);
SeedFxGraphResult seedfx_cache_write(uint32_t package_id,
                                     uint32_t offset,
                                     const uint8_t* data,
                                     uint32_t bytes);
SeedFxGraphResult seedfx_cache_end(uint32_t package_id);
void seedfx_cache_clear();

void seedfx_graph_get_stats(SeedFxGraphStats& stats);
const char* seedfx_graph_result_name(SeedFxGraphResult result);
