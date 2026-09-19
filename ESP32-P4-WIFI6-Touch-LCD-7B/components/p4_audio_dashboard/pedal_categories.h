#pragma once
#include "seedfx_catalog.h"

/* Presentation groups, independent of the on-card folder names. Unknown
 * processors always remain reachable in Other. */
static const char *const pedal_categories[] = {
    "DISTORTION", "OVERDRIVE", "FUZZ", "DELAY", "REVERB", "CHORUS",
    "MODULATION", "FILTER / EQ", "DYNAMICS", "CABINET / AMP",
    "LO-FI", "STEREO", "ROUTING", "OTHER"
};
enum { PEDAL_CATEGORY_COUNT=sizeof(pedal_categories)/sizeof(pedal_categories[0]) };
static unsigned pedal_category(uint16_t type)
{
    switch(type) {
    case SEEDFX_EFFECT_SOFT_CLIP: case SEEDFX_EFFECT_HARD_CLIP:
    case SEEDFX_EFFECT_WAVEFOLDER: return 0;
    case SEEDFX_EFFECT_OVERDRIVE: return 1;
    case SEEDFX_EFFECT_FUZZ: return 2;
    case SEEDFX_EFFECT_DELAY: case SEEDFX_EFFECT_PING_PONG_DELAY:
    case SEEDFX_EFFECT_SLAPBACK: return 3;
    case SEEDFX_EFFECT_REVERB: return 4;
    case SEEDFX_EFFECT_CHORUS: return 5;
    case SEEDFX_EFFECT_TREMOLO: case SEEDFX_EFFECT_AUTO_PAN:
    case SEEDFX_EFFECT_RING_MOD: case SEEDFX_EFFECT_FLANGER:
    case SEEDFX_EFFECT_VIBRATO: case SEEDFX_EFFECT_PHASER:
    case SEEDFX_EFFECT_ROTARY: case SEEDFX_EFFECT_DOUBLER: return 6;
    case SEEDFX_EFFECT_LOWPASS: case SEEDFX_EFFECT_HIGHPASS:
    case SEEDFX_EFFECT_BANDPASS: case SEEDFX_EFFECT_NOTCH:
    case SEEDFX_EFFECT_BASS_BOOST: case SEEDFX_EFFECT_TREBLE_BOOST:
    case SEEDFX_EFFECT_AUTO_WAH: case SEEDFX_EFFECT_EXCITER: return 7;
    case SEEDFX_EFFECT_COMPRESSOR: case SEEDFX_EFFECT_LIMITER:
    case SEEDFX_EFFECT_NOISE_GATE: return 8;
    case SEEDFX_EFFECT_CABINET_SIM: return 9;
    case SEEDFX_EFFECT_BITCRUSHER: case SEEDFX_EFFECT_SAMPLE_RATE_REDUCER: return 10;
    case SEEDFX_EFFECT_STEREO_WIDENER: case SEEDFX_EFFECT_MONO:
    case SEEDFX_EFFECT_CHANNEL_SWAP: return 11;
    case SEEDFX_EFFECT_SPLITTER: case SEEDFX_EFFECT_MIXER: return 12;
    default: return 13;
    }
}
