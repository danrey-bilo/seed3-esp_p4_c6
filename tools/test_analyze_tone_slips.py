import math
import unittest
from analyze_tone_slips import infer


class ToneSlipTests(unittest.TestCase):
    def check_skip(self, rate, skip):
        def value(channel, index):
            return .25118864 * math.sin(.3 + channel + 2 * math.pi * (997, 1501)[channel] * index / rate)
        previous = [[value(c, -1), value(c, -2)] for c in range(2)]
        result = infer(previous, [value(c, skip) for c in range(2)],
                       [value(c, skip + 1) for c in range(2)], rate)
        self.assertEqual(result["offset_frames"], skip)
        self.assertLess(result["fit_rms"], 1e-10)

    def test_missing_millisecond(self):
        self.check_skip(48000, 48)

    def test_repeated_frames(self):
        self.check_skip(96000, -12)


if __name__ == "__main__":
    unittest.main()
