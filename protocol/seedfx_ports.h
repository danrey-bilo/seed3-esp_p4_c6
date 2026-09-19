#pragma once
#include "seedfx_graph_protocol.h"

/* Concrete SeedFX profiles, not a claim that all pedals of a family have
 * these jacks. New models with different I/O must get their own processor ID.
 * Physical stereo endpoints remain 2x2. A mono output may fan out to two
 * destinations. Graph v2 selects every source/destination jack explicitly.
 * Node.reserved bit 0 is read only while migrating v1 graphs. */
static inline unsigned seedfx_input_channels(uint16_t type)
{
    switch(type) {
        case SEEDFX_EFFECT_BYPASS: case SEEDFX_EFFECT_GAIN:
        case SEEDFX_EFFECT_MUTE: case SEEDFX_EFFECT_POLARITY:
        case SEEDFX_EFFECT_STEREO_WIDENER: case SEEDFX_EFFECT_MONO:
        case SEEDFX_EFFECT_CHANNEL_SWAP: case SEEDFX_EFFECT_DC_BLOCKER:
        case SEEDFX_EFFECT_MIXER:
            return 2;
        default: return 1;
    }
}

static inline unsigned seedfx_output_channels(uint16_t type)
{
    if(type == SEEDFX_EFFECT_MONO || type == SEEDFX_EFFECT_MIXER) return 1;
    if(seedfx_input_channels(type) == 2) return 2;
    switch(type) {
        case SEEDFX_EFFECT_AUTO_PAN: case SEEDFX_EFFECT_PING_PONG_DELAY:
        case SEEDFX_EFFECT_CHORUS: case SEEDFX_EFFECT_REVERB:
        case SEEDFX_EFFECT_DOUBLER: case SEEDFX_EFFECT_ROTARY:
        case SEEDFX_EFFECT_SPLITTER:
            return 2;
        default: return 1;
    }
}

/* Legacy stereo-edge expansion; never used by the v2 audio renderer. */
static inline unsigned seedfx_route_channel(unsigned source_channels,
    unsigned destination_channels, unsigned selected, unsigned destination)
{
    if(source_channels == 1) return 0;
    return destination_channels == 1 ? selected & 1U : destination;
}
