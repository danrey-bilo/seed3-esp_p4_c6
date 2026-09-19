#!/usr/bin/env python3
"""Generate the versioned SeedFX microSD manifest library.

The table is the single source for the built-in DSP catalogue. Generated JSON
is intentionally human-readable and may be edited before running seedfx_pack.
"""

from __future__ import annotations

import json
from pathlib import Path


def p(identifier: str, name: str, unit: str, low: float, high: float,
      step: float, default: float) -> dict:
    return {"id": identifier, "name": name, "unit": unit, "min": low,
            "max": high, "step": step, "default": default}


# id, display name, category, color, shape, CPU at 48 kHz, parameters
EFFECTS = [
    ("bypass", "Bypass", "utility", "#5D7892", "rectangle", .02, []),
    ("gain", "Gain", "utility", "#42E8BD", "rounded", .08,
     [p("gain_db", "Gain", "dB", -60, 18, .5, 0)]),
    ("mute", "Mute", "utility", "#718096", "rectangle", .02, []),
    ("polarity", "Polarity", "utility", "#B596FF", "rounded", .05, []),
    ("soft_clip", "Soft Clip", "drive", "#FF9D5C", "pill", .45,
     [p("drive_db", "Drive", "dB", 0, 30, .5, 6),
      p("mix", "Mix", "%", 0, 1, .01, 1)]),
    ("hard_clip", "Hard Clip", "drive", "#FF795C", "rectangle", .25,
     [p("drive_db", "Drive", "dB", 0, 30, .5, 6),
      p("level", "Level", "%", 0, 1, .01, .8)]),
    ("overdrive", "Overdrive", "drive", "#F58B3D", "rounded", .60,
     [p("drive", "Drive", "%", 0, 1, .01, .45),
      p("tone", "Tone", "%", 0, 1, .01, .55),
      p("level", "Level", "%", 0, 1.5, .01, .8)]),
    ("fuzz", "Fuzz", "drive", "#EF5A50", "rectangle", .55,
     [p("gain", "Fuzz", "%", 0, 1, .01, .65),
      p("bias", "Bias", "%", -.5, .5, .01, 0),
      p("level", "Level", "%", 0, 1.5, .01, .65)]),
    ("wavefolder", "Wavefolder", "drive", "#FFB15F", "pill", .65,
     [p("fold", "Fold", "x", 1, 8, .1, 2.5),
      p("mix", "Mix", "%", 0, 1, .01, .7)]),
    ("bitcrusher", "Bitcrusher", "lofi", "#D47AFF", "rectangle", .35,
     [p("bits", "Bits", "bit", 4, 24, 1, 12),
      p("mix", "Mix", "%", 0, 1, .01, 1)]),
    ("sample_rate_reducer", "Rate Reducer", "lofi", "#BD6DFF", "rounded", .28,
     [p("divide", "Divide", "x", 1, 32, 1, 4),
      p("mix", "Mix", "%", 0, 1, .01, 1)]),
    ("lowpass", "Low Pass", "filter", "#5DCBFF", "rounded", .55,
     [p("cutoff", "Cutoff", "Hz", 40, 18000, 10, 6000),
      p("resonance", "Resonance", "%", 0, .95, .01, .15)]),
    ("highpass", "High Pass", "filter", "#58B8FF", "rounded", .55,
     [p("cutoff", "Cutoff", "Hz", 20, 12000, 10, 120),
      p("resonance", "Resonance", "%", 0, .95, .01, .1)]),
    ("bandpass", "Band Pass", "filter", "#4CA7E8", "pill", .58,
     [p("center", "Center", "Hz", 40, 16000, 10, 1200),
      p("resonance", "Resonance", "%", 0, .95, .01, .35)]),
    ("notch", "Notch", "filter", "#3F91D4", "rectangle", .58,
     [p("center", "Center", "Hz", 40, 16000, 10, 1000),
      p("resonance", "Width", "%", 0, .95, .01, .3)]),
    ("bass_boost", "Bass Boost", "tone", "#4FE0C1", "rounded", .42,
     [p("frequency", "Frequency", "Hz", 60, 500, 5, 180),
      p("gain", "Boost", "dB", 0, 18, .5, 6)]),
    ("treble_boost", "Treble Boost", "tone", "#6BE6A6", "rounded", .42,
     [p("frequency", "Frequency", "Hz", 1000, 12000, 10, 3500),
      p("gain", "Boost", "dB", 0, 18, .5, 5)]),
    ("compressor", "Compressor", "dynamics", "#5BE37D", "rounded", .72,
     [p("threshold", "Threshold", "dB", -60, 0, .5, -18),
      p("ratio", "Ratio", "x", 1, 20, .5, 4),
      p("attack", "Attack", "ms", 1, 100, 1, 10),
      p("release", "Release", "ms", 10, 1000, 5, 120)]),
    ("limiter", "Limiter", "dynamics", "#52D66C", "pill", .50,
     [p("ceiling", "Ceiling", "dB", -18, 0, .5, -1),
      p("release", "Release", "ms", 10, 500, 5, 80)]),
    ("noise_gate", "Noise Gate", "dynamics", "#63C57A", "rectangle", .38,
     [p("threshold", "Threshold", "dB", -80, -10, 1, -55),
      p("release", "Release", "ms", 5, 500, 5, 80)]),
    ("tremolo", "Tremolo", "modulation", "#A888FF", "rounded", .38,
     [p("rate", "Rate", "Hz", .1, 20, .1, 4),
      p("depth", "Depth", "%", 0, 1, .01, .6)]),
    ("auto_pan", "Auto Pan", "modulation", "#9877F2", "pill", .42,
     [p("rate", "Rate", "Hz", .1, 12, .1, 2),
      p("depth", "Depth", "%", 0, 1, .01, .8)]),
    ("ring_mod", "Ring Mod", "modulation", "#C178F0", "rectangle", .38,
     [p("frequency", "Frequency", "Hz", 1, 2000, 1, 120),
      p("mix", "Mix", "%", 0, 1, .01, .5)]),
    ("delay", "Delay", "delay", "#FFCA5C", "rounded", .72,
     [p("time", "Time", "ms", 1, 1000, 1, 350),
      p("feedback", "Feedback", "%", 0, .95, .01, .35),
      p("mix", "Mix", "%", 0, 1, .01, .3)]),
    ("ping_pong_delay", "Ping Pong Delay", "delay", "#FFC247", "pill", .82,
     [p("time", "Time", "ms", 1, 1000, 1, 320),
      p("feedback", "Feedback", "%", 0, .95, .01, .4),
      p("mix", "Mix", "%", 0, 1, .01, .35)]),
    ("slapback", "Slapback", "delay", "#E9B74C", "rectangle", .62,
     [p("time", "Time", "ms", 20, 180, 1, 85),
      p("feedback", "Feedback", "%", 0, .6, .01, .12),
      p("mix", "Mix", "%", 0, 1, .01, .3)]),
    ("chorus", "Chorus", "modulation", "#58D9D0", "rounded", 1.05,
     [p("rate", "Rate", "Hz", .05, 8, .05, .8),
      p("depth", "Depth", "ms", 0, 15, .1, 7),
      p("mix", "Mix", "%", 0, 1, .01, .35)]),
    ("flanger", "Flanger", "modulation", "#4CC9D8", "pill", .95,
     [p("rate", "Rate", "Hz", .05, 8, .05, .35),
      p("depth", "Depth", "ms", 0, 8, .1, 3),
      p("feedback", "Feedback", "%", -.9, .9, .01, .35),
      p("mix", "Mix", "%", 0, 1, .01, .5)]),
    ("vibrato", "Vibrato", "modulation", "#49BDD0", "rounded", .92,
     [p("rate", "Rate", "Hz", .1, 12, .1, 5),
      p("depth", "Depth", "ms", 0, 12, .1, 4)]),
    ("phaser", "Phaser", "modulation", "#8878F5", "pill", 1.20,
     [p("rate", "Rate", "Hz", .05, 8, .05, .4),
      p("depth", "Depth", "%", 0, 1, .01, .75),
      p("feedback", "Feedback", "%", -.9, .9, .01, .2),
      p("mix", "Mix", "%", 0, 1, .01, .5)]),
    ("auto_wah", "Auto Wah", "filter", "#E6A94F", "rounded", .92,
     [p("sensitivity", "Sensitivity", "%", 0, 1, .01, .65),
      p("base", "Base", "Hz", 100, 2000, 10, 350),
      p("range", "Range", "Hz", 100, 7000, 10, 2800),
      p("resonance", "Resonance", "%", 0, .95, .01, .5)]),
    ("stereo_widener", "Stereo Widener", "stereo", "#6FA8FF", "rounded", .22,
     [p("width", "Width", "%", 0, 2, .01, 1.3)]),
    ("mono", "Mono", "stereo", "#7892AA", "rectangle", .08, []),
    ("channel_swap", "Channel Swap", "stereo", "#849BB2", "rectangle", .05, []),
    ("dc_blocker", "DC Blocker", "utility", "#65A8A3", "rounded", .20,
     [p("cutoff", "Cutoff", "Hz", 2, 40, 1, 12)]),
    ("reverb", "Reverb", "space", "#7C9DFF", "pill", 1.65,
     [p("size", "Size", "%", 0, 1, .01, .65),
      p("damping", "Damping", "%", 0, 1, .01, .45),
      p("mix", "Mix", "%", 0, 1, .01, .25)]),
    ("cabinet_sim", "Cabinet Sim", "tone", "#C58A52", "rectangle", .70,
     [p("body", "Body", "%", 0, 1, .01, .55),
      p("presence", "Presence", "%", 0, 1, .01, .45)]),
    ("exciter", "Exciter", "tone", "#F4C55B", "rounded", .72,
     [p("drive", "Drive", "%", 0, 1, .01, .35),
      p("tone", "Tone", "Hz", 1000, 10000, 10, 4500),
      p("mix", "Mix", "%", 0, 1, .01, .2)]),
    ("doubler", "Doubler", "space", "#72CFE8", "rounded", .92,
     [p("time", "Time", "ms", 8, 45, .5, 22),
      p("detune", "Detune", "%", 0, 1, .01, .35),
      p("mix", "Mix", "%", 0, 1, .01, .4)]),
    ("rotary", "Rotary", "modulation", "#EE86B7", "pill", 1.10,
     [p("rate", "Rate", "Hz", .2, 10, .1, 1.2),
      p("depth", "Depth", "%", 0, 1, .01, .7),
      p("mix", "Mix", "%", 0, 1, .01, .7)]),
    ("splitter", "Splitter", "routing", "#63C9FF", "rectangle", .03, []),
    ("mixer", "Mixer", "routing", "#FFCE68", "rounded", .06,
     [p("level_a", "Branch A", "%", 0, 1, .01, .5),
      p("level_b", "Branch B", "%", 0, 1, .01, .5),
      p("master", "Master", "%", 0, 1, .01, 1)]),
]


# Append-only controls: old cards/presets keep the same parameter indices.
# The generated header also upgrades a verified older card in P4 RAM, so the
# user does not have to remove the card merely to expose these new knobs.
EXTENSIONS = {
    "gain": [p("balance", "Balance", "%", -1, 1, .01, 0)],
    "soft_clip": [p("level", "Level", "%", 0, 1.5, .01, 1)],
    "hard_clip": [p("mix", "Mix", "%", 0, 1, .01, 1)],
    "wavefolder": [p("level", "Level", "%", 0, 1.5, .01, 1)],
    "limiter": [p("input", "Input", "dB", -18, 18, .5, 0)],
    "noise_gate": [p("attack", "Attack", "ms", 1, 100, 1, 1)],
    "delay": [p("tone", "Tone", "%", 0, 1, .01, 1)],
    "ping_pong_delay": [p("tone", "Tone", "%", 0, 1, .01, 1)],
    "slapback": [p("tone", "Tone", "%", 0, 1, .01, 1)],
    "chorus": [p("stereo", "Stereo", "%", 0, 1, .01, 1)],
    "vibrato": [p("mix", "Mix", "%", 0, 1, .01, 1)],
    "stereo_widener": [p("balance", "Balance", "%", -1, 1, .01, 0)],
    "reverb": [p("decay", "Decay", "%", 0, .92, .01, .675)],
    "cabinet_sim": [p("mix", "Mix", "%", 0, 1, .01, 1)],
}

STEREO_INPUT = {"bypass", "gain", "mute", "polarity", "stereo_widener",
                "mono", "channel_swap", "dc_blocker", "mixer"}
STEREO_OUTPUT = (STEREO_INPUT - {"mono", "mixer"}) | {"auto_pan", "ping_pong_delay",
    "chorus", "reverb", "doubler", "rotary", "splitter"}


def write_extensions_header() -> None:
    rows = []
    for effect_type, row in enumerate(EFFECTS, 1):
        for offset, param in enumerate(EXTENSIONS.get(row[0], []), len(row[-1])):
            def cf(value):
                return f"{float(value)}f"
            rows.append("    {%d, %d, {%s, %s, %s, %s, %s, %s}}," % (
                effect_type, offset, json.dumps(param["name"]),
                json.dumps(param["unit"]), cf(param["min"]), cf(param["max"]),
                cf(param["step"]), cf(param["default"])))
    header = '''// Generated by tools/generate_seedfx_catalog.py; append-only parameter ABI.
#pragma once
#include "seedfx_catalog.h"
typedef struct {
    uint16_t type;
    uint8_t index;
    SeedFxParameterDescriptor parameter;
} SeedFxExtension;
static const SeedFxExtension seedfx_extensions[] = {
''' + "\n".join(rows) + '''
};
static inline void seedfx_extend_catalog(SeedFxCatalogEntry *entry)
{
    for (unsigned i = 0; i < sizeof(seedfx_extensions) / sizeof(seedfx_extensions[0]); ++i) {
        const SeedFxExtension *ext = &seedfx_extensions[i];
        if (ext->type == entry->effect_type && entry->parameter_count == ext->index) {
            entry->parameters[ext->index] = ext->parameter;
            ++entry->parameter_count;
        }
    }
}
static inline void seedfx_extension_defaults(uint16_t type, float *values, unsigned count)
{
    for (unsigned i = 0; i < sizeof(seedfx_extensions) / sizeof(seedfx_extensions[0]); ++i) {
        const SeedFxExtension *ext = &seedfx_extensions[i];
        if (ext->type == type && count <= ext->index)
            values[ext->index] = ext->parameter.default_value;
    }
}
'''
    Path("protocol/seedfx_extensions.h").write_text(header, encoding="utf-8")


def main() -> None:
    write_extensions_header()
    root = Path("microSD-ready/SEEDFX/effects")
    for effect_type, row in enumerate(EFFECTS, 1):
        identifier, name, category, color, shape, cpu, parameters = row
        parameters = parameters + EXTENSIONS.get(identifier, [])
        folder = root / category
        folder.mkdir(parents=True, exist_ok=True)
        manifest = {
            "format": "seedfx-effect", "version": 1,
            "package_id": f"0x{0x00010000 + effect_type:08X}",
            "id": f"builtin.{identifier}", "name": name,
            "processor": f"builtin:{identifier}",
            "effect_type": effect_type, "category": category,
            "audio_ports": {"inputs": 2 if identifier in STEREO_INPUT else 1,
                            "outputs": 2 if identifier in STEREO_OUTPUT else 1},
            "color": color, "shape": shape,
            "cpu_percent": {"44100": round(cpu * .92, 2),
                            "48000": cpu,
                            "88200": round(cpu * 1.72, 2),
                            "96000": round(cpu * 1.84, 2)},
            "parameters": parameters,
        }
        (folder / f"{identifier}.json").write_text(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n",
            encoding="utf-8")
    print(f"Generated {len(EFFECTS)} effect manifests in {root.resolve()}")


if __name__ == "__main__":
    main()
