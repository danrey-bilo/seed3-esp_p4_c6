"""Check packed-24 WAV layout/count, not analog quality or sample continuity.

No known signal is assumed for the ADC input. Continuity still needs the WASAPI
position/flag checks and device counters; RMS/phase tests require test tones.
"""
import argparse
import json
import struct
from pathlib import Path


def check(path, rate, frames):
    with Path(path).open("rb") as stream:
        header = stream.read(12)
        if len(header) != 12 or header[:4] != b"RIFF" or header[8:] != b"WAVE":
            raise ValueError("not RIFF/WAVE")
        fmt = None
        while chunk := stream.read(8):
            kind, size = struct.unpack("<4sI", chunk)
            if kind == b"fmt ":
                if size > 4096:
                    raise ValueError("oversized WAV format")
                fmt = stream.read(size)
            elif kind == b"data":
                break
            else:
                stream.seek(size, 1)
            if size & 1:
                stream.seek(1, 1)
        else:
            raise ValueError("missing WAV data")
        if fmt is None or len(fmt) < 16:
            raise ValueError("missing WAV format")
        tag, channels, actual_rate, byte_rate, block, bits = struct.unpack_from("<HHIIHH", fmt)
        valid = bits
        if tag == 0xfffe:
            if len(fmt) < 40 or fmt[24:40] != bytes.fromhex("0100000000001000800000aa00389b71"):
                raise ValueError("not extensible integer PCM")
            valid = struct.unpack_from("<H", fmt, 18)[0]
            tag = 1
        offset = stream.tell()
        stream.seek(0, 2)
        present = stream.tell() - offset
        passed = (tag == 1 and channels == 2 and actual_rate == rate
                  and bits == valid == 24 and block == 6 and byte_rate == rate * 6
                  and size == frames * 6 and present >= size)
        return {"layout_pass": passed, "rate": actual_rate, "channels": channels,
                "container_bits": bits, "valid_bits": valid, "block_bytes": block,
                "frames": size // max(1, block), "expected_frames": frames,
                "payload_bytes": size, "file_payload_bytes": present,
                "analog_quality_measured": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("wav")
    parser.add_argument("--rate", type=int, required=True)
    parser.add_argument("--frames", type=int, required=True)
    parser.add_argument("--json", type=Path)
    args = parser.parse_args()
    result = check(args.wav, args.rate, args.frames)
    text = json.dumps(result, indent=2)
    if args.json:
        args.json.write_text(text + "\n", encoding="utf-8")
    print(text)
    raise SystemExit(0 if result["layout_pass"] else 1)
