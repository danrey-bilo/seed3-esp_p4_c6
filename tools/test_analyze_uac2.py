"""Regression tests for the measurement tool; no audio devices are accessed."""
import math
from pathlib import Path
import struct
import tempfile
import unittest

from analyze_uac2 import analyze


class Measurements(unittest.TestCase):
    def setUp(self):
        self.folder = tempfile.TemporaryDirectory()
        self.path = Path(self.folder.name) / "fixture.wav"

    def tearDown(self):
        self.folder.cleanup()

    def write(self, silence=False, padding=False, phase_skip=False, rate=48000, bits=24):
        frames = 2 * rate
        pcm = bytearray()
        for n in range(frames):
            phase = n + (144 if phase_skip and n >= 36000 else 0)
            values = [int(.251188643 * math.sin(2 * math.pi * f * phase / rate) * 2147483648) & ~255
                      for f in (997, 1501)]
            if silence and 30000 <= n < 30144:
                values = [0, 0]
            if padding:
                values[0] |= 1
            pcm.extend(struct.pack("<hh", *(v >> 16 for v in values)) if bits == 16 else struct.pack("<ii", *values))
        # WAVEFORMATEXTENSIBLE, PCM GUID, valid bits 24, FL/FR mask 3.
        block, container = (4, 16) if bits == 16 else (8, 32)
        fmt = struct.pack("<HHIIHHHHI", 0xfffe, 2, rate, rate * block, block, container, 22, bits, 3)
        fmt += bytes.fromhex("0100000000001000800000aa00389b71")
        self.path.write_bytes(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(pcm)) + b"WAVE" +
                              b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" +
                              struct.pack("<I", len(pcm)) + pcm)

    def test_clean_tones(self):
        self.write()
        result = analyze(self.path, 96000)
        self.assertTrue(result["pass"])
        self.assertEqual(result["phase_suspect_samples_after_50ms"], [0, 0])
        self.assertEqual(result["valid_bits"], 24)

    def test_wrong_frame_count(self):
        self.write()
        self.assertFalse(analyze(self.path, 96001)["pass"])

    def test_all_multirate_formats(self):
        for rate in (44100, 48000, 88200, 96000):
            for bits in (16, 24):
                with self.subTest(rate=rate, bits=bits):
                    self.write(rate=rate, bits=bits)
                    result = analyze(self.path, rate * 2)
                    self.assertTrue(result["pass"])
                    self.assertEqual(result["phase_suspect_samples_after_50ms"], [0, 0])

    def test_padding_corruption(self):
        self.write(padding=True)
        result = analyze(self.path, 96000)
        self.assertFalse(result["pass"])
        self.assertEqual(result["padding_errors"], 96000)

    def test_three_ms_silence(self):
        self.write(silence=True)
        result = analyze(self.path, 96000)
        self.assertFalse(result["pass"])
        self.assertGreaterEqual(min(result["max_silence_frames_after_50ms"]), 144)

    def test_nonzero_phase_skip_is_detected(self):
        self.write(phase_skip=True)
        result = analyze(self.path, 96000)
        self.assertFalse(result["pass"])
        self.assertGreater(min(result["phase_suspect_samples_after_50ms"]), 0)
        self.assertLess(max(result["max_silence_frames_after_50ms"]), 2)

    def test_truncated_file(self):
        self.write()
        with self.path.open("r+b") as fixture:
            fixture.truncate(1024)
        with self.assertRaisesRegex(ValueError, "truncated"):
            analyze(self.path, 96000)


if __name__ == "__main__":
    unittest.main()
