#!/usr/bin/env python3
"""Verify the fixed Seed3/P4 stereo PCM test recording without third-party modules."""

from __future__ import annotations

import argparse
import math
import struct
import sys
from pathlib import Path


EXPECTED = (997.0, 1501.0)


def decode_pcm(
    raw: bytes, width: int, channels: int, encoding: str
) -> list[list[float]]:
    result = [[] for _ in range(channels)]
    frame_bytes = width * channels
    if len(raw) % frame_bytes:
        raise ValueError("WAV data is not an integer number of frames")
    scale = float(1 << (width * 8 - 1))
    for offset in range(0, len(raw), frame_bytes):
        for channel in range(channels):
            pos = offset + channel * width
            if encoding == "float" and width == 4:
                value = struct.unpack_from("<f", raw, pos)[0]
                result[channel].append(value)
                continue
            if width == 2:
                value = struct.unpack_from("<h", raw, pos)[0]
            elif width == 3:
                value = int.from_bytes(raw[pos : pos + 3], "little", signed=False)
                if value & 0x800000:
                    value -= 1 << 24
            elif width == 4:
                value = struct.unpack_from("<i", raw, pos)[0]
            else:
                raise ValueError(f"unsupported PCM width: {width * 8} bits")
            result[channel].append(value / scale)
    return result


def read_wave(path: Path) -> tuple[int, int, int, str, bytes]:
    """Read PCM or IEEE-float WAV, including WAVE_FORMAT_EXTENSIBLE."""
    blob = path.read_bytes()
    if len(blob) < 12 or blob[:4] != b"RIFF" or blob[8:12] != b"WAVE":
        raise ValueError("not a RIFF/WAVE file")

    fmt = None
    audio = None
    offset = 12
    while offset + 8 <= len(blob):
        chunk_id = blob[offset : offset + 4]
        size = struct.unpack_from("<I", blob, offset + 4)[0]
        payload = offset + 8
        if payload + size > len(blob):
            raise ValueError("truncated WAV chunk")
        if chunk_id == b"fmt ":
            fmt = blob[payload : payload + size]
        elif chunk_id == b"data":
            audio = blob[payload : payload + size]
        offset = payload + size + (size & 1)

    if fmt is None or len(fmt) < 16 or audio is None:
        raise ValueError("WAV has no usable fmt/data chunks")
    tag, channels, rate, _, block_align, bits = struct.unpack_from("<HHIIHH", fmt)
    if tag == 0xFFFE:
        if len(fmt) < 40:
            raise ValueError("truncated WAVE_FORMAT_EXTENSIBLE")
        tag = struct.unpack_from("<I", fmt, 24)[0]
    if tag == 1:
        encoding = "pcm"
    elif tag == 3:
        encoding = "float"
    else:
        raise ValueError(f"unsupported WAV format tag: 0x{tag:04x}")
    width = bits // 8
    if block_align != width * channels:
        raise ValueError("unsupported WAV block alignment")
    return channels, rate, width, encoding, audio


def tone_power(samples: list[float], rate: int, frequency: float) -> float:
    """Return single-bin power using a Hann-windowed complex correlation."""
    count = len(samples)
    if count < 2:
        return 0.0
    real = 0.0
    imag = 0.0
    for index, sample in enumerate(samples):
        window = 0.5 - 0.5 * math.cos(2.0 * math.pi * index / (count - 1))
        phase = 2.0 * math.pi * frequency * index / rate
        real += sample * window * math.cos(phase)
        imag -= sample * window * math.sin(phase)
    return real * real + imag * imag


def rms(samples: list[float]) -> float:
    return math.sqrt(sum(value * value for value in samples) / max(1, len(samples)))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("wav", type=Path)
    parser.add_argument("--seconds", type=float, default=5.0,
                        help="seconds to analyse after skipping the first 0.25 s")
    args = parser.parse_args()

    channels, rate, width, encoding, all_audio = read_wave(args.wav)
    frame_bytes = channels * width
    total_frames = len(all_audio) // frame_bytes
    first_frame = min(total_frames, rate // 4)
    frame_count = min(total_frames - first_frame, int(rate * args.seconds))
    first_byte = first_frame * frame_bytes
    raw = all_audio[first_byte : first_byte + frame_count * frame_bytes]

    if channels != 2 or rate != 48000 or width not in (2, 3, 4):
        raise ValueError(
            f"expected stereo 48 kHz PCM, got channels={channels}, rate={rate}, "
            f"width={width * 8}")

    decoded = decode_pcm(raw, width, channels, encoding)
    passed = True
    for channel, expected_hz in enumerate(EXPECTED):
        own = tone_power(decoded[channel], rate, expected_hz)
        other = tone_power(decoded[channel], rate, EXPECTED[1 - channel])
        ratio_db = 10.0 * math.log10(max(own, 1e-30) / max(other, 1e-30))
        level = rms(decoded[channel])
        print(f"channel {channel + 1}: expected={expected_hz:.0f} Hz "
              f"rms={level:.5f} separation={ratio_db:.1f} dB")
        if level < 0.05 or ratio_db < 20.0:
            passed = False

    print("PASS" if passed else "FAIL")
    return 0 if passed else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(2)
