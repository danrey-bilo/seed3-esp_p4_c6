#pragma once
#include "seedfx_routing.h"

/* Pure two-tap interaction model. No LVGL, allocation or callbacks. */
typedef struct { uint16_t id; uint8_t port; bool output; } pedal_jack_t;
typedef struct { bool active; pedal_jack_t jack; } pedal_patch_t;
typedef enum { PEDAL_PATCH_CANCEL, PEDAL_PATCH_SELECTED,
               PEDAL_PATCH_CONNECT, PEDAL_PATCH_DISCONNECT } pedal_patch_result_t;
pedal_patch_result_t pedal_patch_tap(pedal_patch_t *state,
    const SeedFxGraphDefinition *graph, pedal_jack_t jack, SeedFxEdgeDefinition *edit);
pedal_patch_result_t pedal_patch_blank(pedal_patch_t *state,
    const SeedFxGraphDefinition *graph, SeedFxEdgeDefinition *edit);
bool pedal_patch_is_selected(const pedal_patch_t *state, pedal_jack_t jack);

typedef struct { SeedFxEdgeDefinition edge; bool used; } pedal_color_slot_t;
typedef struct { pedal_color_slot_t slots[SEEDFX_MAX_EDGES]; } pedal_colors_t;
void pedal_colors_sync(pedal_colors_t *colors, const SeedFxGraphDefinition *graph);
uint32_t pedal_color_for(const pedal_colors_t *colors, const SeedFxEdgeDefinition *edge);
/* Physical channel colors never depend on graph order or palette slots. */
static inline uint32_t pedal_physical_color(unsigned channel)
{ return channel==0?0x48e0a8:0x58bdff; }
