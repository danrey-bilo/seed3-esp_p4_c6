"""Estimate phase-step sizes for the known 997/1501 Hz diagnostic pair.

Supplemental evidence, not a replacement for CRC, timestamps or USB counters.
A sine pair cannot distinguish arbitrary whole-period loss; search is +/-50ms.
"""
import argparse
import json
import math
from pathlib import Path
import struct
from pcm_samples import iter_stereo_pcm


def infer(previous, current, following, rate):
    # previous[c] = x[n-1], x[n-2]. Recover amplitude * cos(phase).
    omega = [2 * math.pi * f / rate for f in (997, 1501)]
    quadrature = [(p[0] * math.cos(w) - p[1]) / math.sin(w)
                  for p, w in zip(previous, omega)]
    best = (float("inf"), 0)
    for skip in range(-rate // 20, rate // 20 + 1):
        error = 0.
        for c, w in enumerate(omega):
            for j, observed in enumerate((current[c], following[c]), 1):
                phase = (skip + j) * w
                predicted = previous[c][0] * math.cos(phase) + quadrature[c] * math.sin(phase)
                error += (predicted - observed) ** 2
        if error < best[0]:
            best = error, skip
    return {"offset_frames": best[1], "fit_rms": math.sqrt(best[0] / 4),
            "offset_ms": best[1] * 1000 / rate}


def analyze(path):
    with open(path, "rb") as stream:
        header = stream.read(12)
        if header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise ValueError("not RIFF/WAVE")
        fmt = None
        while chunk := stream.read(8):
            kind, size = struct.unpack("<4sI", chunk)
            if kind == b"fmt ":
                fmt = stream.read(size)
            elif kind == b"data":
                break
            else:
                stream.seek(size, 1)
            if size & 1:
                stream.seek(1, 1)
        tag, channels, rate, _, block, bits = struct.unpack_from("<HHIIHH", fmt)
        if tag == 0xfffe:
            tag = struct.unpack_from("<I", fmt, 24)[0]
        if tag != 1 or channels != 2 or bits not in (16, 24, 32) or block != bits // 4:
            raise ValueError("stereo integer PCM16/PCM24/PCM32 required")
        frames, position = size // block, 0
        coefficient = [2 * math.cos(2 * math.pi * f / rate) for f in (997, 1501)]
        threshold = 1.5e-4 if bits == 16 else 2e-5
        scale = float(2 ** (bits - 1))
        previous = [[0., 0.], [0., 0.]]
        pending, last_event, events = None, -100, []
        while position < frames:
            count = min(rate, frames - position)
            raw = stream.read(count * block)
            if len(raw) != count * block:
                raise ValueError("truncated PCM")
            for pair in iter_stereo_pcm(raw, bits):
                values = [pair[0] / scale, pair[1] / scale]
                if pending:
                    start, old, current = pending
                    events.append({"frame": start, **infer(old, current, values, rate)})
                    pending = None
                    if len(events) > 1000:
                        raise ValueError("Too many phase events: not the expected tone pair")
                if position > rate // 20 and position - last_event > 2 and any(
                    abs(values[c] - coefficient[c] * previous[c][0] + previous[c][1]) > threshold
                    for c in range(2)
                ):
                    pending = position, [p[:] for p in previous], values
                    last_event = position
                for c in range(2):
                    previous[c][1], previous[c][0] = previous[c][0], values[c]
                position += 1
    matched = all(e["fit_rms"] < .0002 for e in events)
    return {"rate": rate, "frames": frames, "events": events,
            "all_events_fit_known_tones": matched,
            "max_abs_inferred_offset_ms": max((abs(e["offset_ms"]) for e in events), default=0),
            "note": "Inference over +/-50ms, excluding first 50ms; not an independent continuity guarantee."}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav")
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = analyze(args.wav)
    if args.json:
        args.json.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "events"}, indent=2))
