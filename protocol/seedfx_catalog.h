#pragma once

#include "seedfx_graph_protocol.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    SEEDFX_MAX_EFFECTS = 64U,
    SEEDFX_EFFECT_NAME_BYTES = 24U,
    SEEDFX_CATEGORY_BYTES = 16U,
    SEEDFX_CPU_LABEL_BYTES = 16U,
    SEEDFX_PARAMETER_NAME_BYTES = 16U,
    SEEDFX_PARAMETER_UNIT_BYTES = 8U,
    SEEDFX_PACKAGE_MAGIC = 0x31584653U, /* "SFX1" */
    SEEDFX_PACKAGE_VERSION = 1U,
};

typedef struct {
    char name[SEEDFX_PARAMETER_NAME_BYTES];
    char unit[SEEDFX_PARAMETER_UNIT_BYTES];
    float minimum;
    float maximum;
    float step;
    float default_value;
} SeedFxParameterDescriptor;

typedef struct {
    uint32_t package_id;
    uint16_t effect_type;
    uint8_t shape;
    uint8_t parameter_count;
    uint32_t color_rgb;
    char name[SEEDFX_EFFECT_NAME_BYTES];
    char category[SEEDFX_CATEGORY_BYTES];
    char cpu_label[SEEDFX_CPU_LABEL_BYTES];
    SeedFxParameterDescriptor parameters[SEEDFX_MAX_PARAMS];
} SeedFxCatalogEntry;

typedef struct __attribute__((packed, aligned(4))) {
    uint32_t magic;
    uint16_t version;
    uint16_t header_bytes;
    uint32_t package_id;
    uint32_t manifest_bytes;
    uint32_t payload_bytes;
    uint32_t payload_crc32;
    uint32_t package_crc32;
} SeedFxPackageHeader;

#ifdef __cplusplus
static_assert(sizeof(SeedFxPackageHeader) == 28U,
              "SeedFX package header wire format changed");
#else
_Static_assert(sizeof(SeedFxPackageHeader) == 28U,
               "SeedFX package header wire format changed");
#endif

#ifdef __cplusplus
}
#endif
