"""Streaming acceptance measurements; standard Python only, no whole-WAV allocation."""
import argparse
import json
import math
import struct


def power(samples, frequency, rate):
    coefficient = 2 * math.cos(2 * math.pi * frequency / rate)
    a = b = 0.0
    for value in samples:
        a, b = value + coefficient * a - b, a
    return max(1e-30, a * a + b * b - coefficient * a * b)


def analyze(path, expected):
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
        if fmt is None or kind != b"data":
            raise ValueError("missing WAV chunks")
        tag, channels, rate, _, block, bits = struct.unpack_from("<HHIIHH", fmt)
        valid_bits = struct.unpack_from("<H", fmt, 18)[0] if tag == 0xfffe else bits
        if tag == 0xfffe:
            tag = struct.unpack_from("<I", fmt, 24)[0]
        if (channels != 2 or rate not in (44100, 48000, 88200, 96000, 176400, 192000)
                or bits not in (16, 32) or block != bits // 8 * 2
                or tag not in (1, 3) or (tag == 3 and bits != 32)):
            raise ValueError("requires stereo 44.1-192kHz PCM16/PCM32 or float32")
        if size % block:
            raise ValueError("partial audio frame")
        frames = size // block
        startup_frames = rate // 20
        max_gap_frames = rate // 500
        phase_threshold = 1.5e-4 if bits == 16 else 2e-5
        position = 0
        total_power = [0., 0.]
        zero_run = [0, 0]
        max_zero = [0, 0]
        suffix_zero = [0, 0]
        nonfinite = 0
        per_second = []
        padding_errors = 0
        # A pure sine also obeys this recurrence. Unlike a zero detector this
        # exposes a missing/repeated block concatenated directly to nonzero PCM.
        coefficients = [2 * math.cos(2 * math.pi * f / rate) for f in (997, 1501)]
        previous = [[0., 0.], [0., 0.]]
        phase_suspect_samples = [0, 0]
        max_recurrence_error = [0., 0.]
        while position < frames:
            count = min(rate, frames - position)
            raw = stream.read(count * block)
            if len(raw) != count * block:
                raise ValueError("truncated audio data")
            samples = [[], []]
            for pair in struct.iter_unpack("<ff" if tag == 3 else ("<hh" if bits == 16 else "<ii"), raw):
                for c, value in enumerate(pair):
                    if tag == 1:
                        if bits == 32 and valid_bits == 24 and value & 255:
                            padding_errors += 1
                        value /= 32768.0 if bits == 16 else 2147483648.0
                    if not math.isfinite(value):
                        nonfinite += 1
                        value = 0.
                    samples[c].append(value)
                    total_power[c] += value * value
                    residual = abs(value - coefficients[c] * previous[c][0] + previous[c][1])
                    if position >= startup_frames:
                        max_recurrence_error[c] = max(max_recurrence_error[c], residual)
                        if residual > phase_threshold:
                            phase_suspect_samples[c] += 1
                    previous[c][1], previous[c][0] = previous[c][0], value
                    if abs(value) < 1e-5:
                        suffix_zero[c] += 1
                        # Report startup separately; the 50ms engine start is not steady state.
                        if position >= startup_frames:
                            zero_run[c] += 1
                            max_zero[c] = max(max_zero[c], zero_run[c])
                    else:
                        zero_run[c] = suffix_zero[c] = 0
                position += 1
            if count == rate:
                # A half-second analysis keeps ppm clock offset from cancelling the correlation.
                window = [channel[count // 2:] for channel in samples]
                levels = [math.sqrt(sum(v * v for v in channel) / count) for channel in samples]
                separation = [10 * math.log10(power(window[c], (997, 1501)[c], rate) /
                                              power(window[c], (1501, 997)[c], rate)) for c in range(2)]
                per_second.append({"second": position // rate, "rms": levels, "separation_db": separation})
        rms = [math.sqrt(p / max(1, frames)) for p in total_power]
        minimum_separation = [min((s["separation_db"][c] for s in per_second), default=-999) for c in range(2)]
        tail_rms = per_second[-1]["rms"] if per_second else [0, 0]
        passed = (frames == expected and nonfinite == 0 and padding_errors == 0 and
                  not any(phase_suspect_samples) and
                  all(abs(r - .1776) < .0036 for r in rms) and
                  all(s > 40 for s in minimum_separation) and
                  max(max_zero) <= max_gap_frames and max(suffix_zero) <= max_gap_frames and min(tail_rms) > .17)
        return {"analysis_version": 2, "pass": passed, "frames": frames, "expected_frames": expected,
                "format": "float32" if tag == 3 else "PCM", "rate": rate, "container_bits": bits, "valid_bits": valid_bits,
                "rms": rms, "min_separation_db": minimum_separation,
                "max_silence_frames_after_50ms": max_zero, "tail_silence_frames": suffix_zero,
                "tail_rms": tail_rms, "nonfinite_samples": nonfinite,
                "padding_errors": padding_errors,
                "phase_suspect_samples_after_50ms": phase_suspect_samples,
                "max_recurrence_error_after_50ms": max_recurrence_error,
                "per_second": per_second}


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("wav")
    parser.add_argument("--frames", type=int, default=2880000)
    parser.add_argument("--json")
    args = parser.parse_args()
    result = analyze(args.wav, args.frames)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as out:
            json.dump(result, out, indent=2)
    print(json.dumps({k: v for k, v in result.items() if k != "per_second"}, indent=2))
    raise SystemExit(0 if result["pass"] else 1)
