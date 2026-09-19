#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stable, endian-explicit control format shared by ESP32-P4 and Seed3.
 * Audio still travels through spi_audio_protocol.h; this file is only for
 * infrequent graph/cache control messages over the diagnostic UART. */
enum {
    SEEDFX_GRAPH_MAGIC = 0x31474653U,   /* "SFG1", little-endian */
    SEEDFX_GRAPH_VERSION = 2U,
    SEEDFX_CONTROL_MAGIC = 0x43584653U, /* "SFXC", little-endian */
    SEEDFX_CONTROL_VERSION = 1U,
    SEEDFX_MAX_NODES = 12U,
    SEEDFX_MAX_EDGES = 48U,
    SEEDFX_MAX_PARAMS = 4U,
    SEEDFX_ENDPOINT_AUDIO = 0U,
    SEEDFX_CACHE_CHUNK_BYTES = 384U,
};

typedef enum {
    SEEDFX_EFFECT_BYPASS = 1,
    SEEDFX_EFFECT_GAIN = 2,
    SEEDFX_EFFECT_MUTE = 3,
    SEEDFX_EFFECT_POLARITY = 4,
    SEEDFX_EFFECT_SOFT_CLIP = 5,
    SEEDFX_EFFECT_HARD_CLIP = 6,
    SEEDFX_EFFECT_OVERDRIVE = 7,
    SEEDFX_EFFECT_FUZZ = 8,
    SEEDFX_EFFECT_WAVEFOLDER = 9,
    SEEDFX_EFFECT_BITCRUSHER = 10,
    SEEDFX_EFFECT_SAMPLE_RATE_REDUCER = 11,
    SEEDFX_EFFECT_LOWPASS = 12,
    SEEDFX_EFFECT_HIGHPASS = 13,
    SEEDFX_EFFECT_BANDPASS = 14,
    SEEDFX_EFFECT_NOTCH = 15,
    SEEDFX_EFFECT_BASS_BOOST = 16,
    SEEDFX_EFFECT_TREBLE_BOOST = 17,
    SEEDFX_EFFECT_COMPRESSOR = 18,
    SEEDFX_EFFECT_LIMITER = 19,
    SEEDFX_EFFECT_NOISE_GATE = 20,
    SEEDFX_EFFECT_TREMOLO = 21,
    SEEDFX_EFFECT_AUTO_PAN = 22,
    SEEDFX_EFFECT_RING_MOD = 23,
    SEEDFX_EFFECT_DELAY = 24,
    SEEDFX_EFFECT_PING_PONG_DELAY = 25,
    SEEDFX_EFFECT_SLAPBACK = 26,
    SEEDFX_EFFECT_CHORUS = 27,
    SEEDFX_EFFECT_FLANGER = 28,
    SEEDFX_EFFECT_VIBRATO = 29,
    SEEDFX_EFFECT_PHASER = 30,
    SEEDFX_EFFECT_AUTO_WAH = 31,
    SEEDFX_EFFECT_STEREO_WIDENER = 32,
    SEEDFX_EFFECT_MONO = 33,
    SEEDFX_EFFECT_CHANNEL_SWAP = 34,
    SEEDFX_EFFECT_DC_BLOCKER = 35,
    SEEDFX_EFFECT_REVERB = 36,
    SEEDFX_EFFECT_CABINET_SIM = 37,
    SEEDFX_EFFECT_EXCITER = 38,
    SEEDFX_EFFECT_DOUBLER = 39,
    SEEDFX_EFFECT_ROTARY = 40,
    SEEDFX_EFFECT_SPLITTER = 41,
    SEEDFX_EFFECT_MIXER = 42,
    SEEDFX_EFFECT_LAST = SEEDFX_EFFECT_MIXER,
} SeedFxEffectType;

typedef enum {
    SEEDFX_SHAPE_RECTANGLE = 0,
    SEEDFX_SHAPE_ROUNDED = 1,
    SEEDFX_SHAPE_PILL = 2,
} SeedFxNodeShape;

enum {
    SEEDFX_NODE_ENABLED = 1U << 0,
    SEEDFX_GRAPH_ENABLED = 1U << 0,
};

typedef struct __attribute__((packed, aligned(4))) {
    uint16_t id;          /* 1..65535; zero is the physical stereo endpoint. */
    uint16_t effect_type; /* SeedFxEffectType. */
    uint32_t package_id;  /* Stable microSD package identity. */
    uint8_t flags;
    uint8_t parameter_count;
    uint8_t shape;        /* UI hint; DSP never trusts it for processing. */
    uint8_t reserved;
    float parameters[SEEDFX_MAX_PARAMS];
} SeedFxNodeDefinition;

typedef struct __attribute__((packed, aligned(4))) {
    uint16_t source_id;      /* zero = physical stereo input. */
    uint16_t destination_id; /* zero = physical stereo output. */
    float gain;              /* Linear route gain. */
    uint8_t source_port;      /* zero-based output jack, including physical IN. */
    uint8_t destination_port; /* zero-based input jack, including physical OUT. */
    uint16_t reserved;
} SeedFxEdgeDefinition;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;
    uint16_t version;
    uint16_t bytes;
    uint32_t revision;
    uint8_t flags;
    uint8_t node_count;
    uint8_t edge_count;
    uint8_t reserved;
    char name[24];
    SeedFxNodeDefinition nodes[SEEDFX_MAX_NODES];
    SeedFxEdgeDefinition edges[SEEDFX_MAX_EDGES];
    uint32_t crc32;
} SeedFxGraphDefinition;

typedef enum {
    SEEDFX_COMMAND_GRAPH = 1,
    SEEDFX_COMMAND_CACHE_BEGIN = 2,
    SEEDFX_COMMAND_CACHE_CHUNK = 3,
    SEEDFX_COMMAND_CACHE_END = 4,
    SEEDFX_COMMAND_CACHE_CLEAR = 5,
} SeedFxControlCommand;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;
    uint8_t version;
    uint8_t command;
    uint16_t payload_bytes;
    uint32_t sequence;
    uint32_t payload_crc32;
} SeedFxControlHeader;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t package_id;
    uint32_t total_bytes;
    uint32_t content_crc32;
} SeedFxCacheBegin;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t package_id;
    uint32_t offset;
    uint16_t data_bytes;
    uint16_t reserved;
    uint8_t data[SEEDFX_CACHE_CHUNK_BYTES];
} SeedFxCacheChunk;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t package_id;
} SeedFxCacheEnd;

enum {
    SEEDFX_CONTROL_MAX_PAYLOAD = sizeof(SeedFxGraphDefinition),
};

typedef struct __attribute__((packed, aligned(4))) {
    SeedFxControlHeader header;
    uint8_t payload[SEEDFX_CONTROL_MAX_PAYLOAD];
} SeedFxControlPacket;

static inline uint32_t seedfx_crc32_update(uint32_t crc,
                                            const uint8_t *data,
                                            size_t length)
{
    while (length-- != 0U) {
        crc ^= *data++;
        for (uint32_t bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = 0U - (crc & 1U);
            crc = (crc >> 1U) ^ (0xedb88320U & mask);
        }
    }
    return crc;
}

static inline uint32_t seedfx_crc32(const void *data, size_t bytes)
{
    return ~seedfx_crc32_update(
        0xffffffffU, (const uint8_t *)data, bytes);
}

static inline uint32_t seedfx_graph_crc32(const SeedFxGraphDefinition *graph)
{
    const size_t crc_offset = offsetof(SeedFxGraphDefinition, crc32);
    return seedfx_crc32(graph, crc_offset);
}

static inline bool seedfx_graph_header_is_valid(
    const SeedFxGraphDefinition *graph)
{
    return graph != NULL && graph->magic == SEEDFX_GRAPH_MAGIC
           && graph->version == SEEDFX_GRAPH_VERSION
           && graph->bytes == sizeof(SeedFxGraphDefinition)
           && graph->node_count <= SEEDFX_MAX_NODES
           && graph->edge_count <= SEEDFX_MAX_EDGES;
}

#ifdef __cplusplus
static_assert(sizeof(SeedFxNodeDefinition) == 28U,
              "SeedFX node wire format changed");
static_assert(sizeof(SeedFxEdgeDefinition) == 12U,
              "SeedFX edge wire format changed");
static_assert(sizeof(SeedFxGraphDefinition) == 956U,
              "SeedFX graph wire format changed");
static_assert(sizeof(SeedFxControlHeader) == 16U,
              "SeedFX control header wire format changed");
#else
_Static_assert(sizeof(SeedFxNodeDefinition) == 28U,
               "SeedFX node wire format changed");
_Static_assert(sizeof(SeedFxEdgeDefinition) == 12U,
               "SeedFX edge wire format changed");
_Static_assert(sizeof(SeedFxGraphDefinition) == 956U,
               "SeedFX graph wire format changed");
_Static_assert(sizeof(SeedFxControlHeader) == 16U,
               "SeedFX control header wire format changed");
#endif

#ifdef __cplusplus
}
#endif
