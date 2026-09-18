"""Decode stereo PCM for the offline audio checks (no device access)."""
import struct


def iter_stereo_pcm(raw, bits, floating=False):
    if floating:
        if bits != 32:
            raise ValueError("float32 required")
        yield from struct.iter_unpack("<ff", raw)
    elif bits == 24:
        for left, right in struct.iter_unpack("<3s3s", raw):
            yield int.from_bytes(left, "little", signed=True), int.from_bytes(right, "little", signed=True)
    elif bits in (16, 32):
        yield from struct.iter_unpack("<hh" if bits == 16 else "<ii", raw)
    else:
        raise ValueError("PCM16/PCM24/PCM32 required")
