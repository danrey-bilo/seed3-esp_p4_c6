"""Check the shared byte-table CRC against independent CRC-32 implementations."""
import pathlib
import random
import re
import unittest
import zlib


class SpiCrcTests(unittest.TestCase):
    def test_table_matches_original_wire_crc(self):
        header = (pathlib.Path(__file__).resolve().parents[1] /
                  "protocol/spi_audio_protocol.h").read_text()
        block = re.search(r"table\[256\] = \{(.*?)\};", header, re.S).group(1)
        table = [int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)U", block)]
        self.assertEqual(len(table), 256)
        rng = random.Random(0x303A40A4)
        for size in [0, 1, 4, 28, 32, 256, 288, 1024] * 20:
            data = bytes(rng.randrange(256) for _ in range(size))
            fast = slow = 0xFFFFFFFF
            for byte in data:
                fast = (fast >> 8) ^ table[(fast ^ byte) & 0xFF]
                slow ^= byte
                for _ in range(8):
                    slow = (slow >> 1) ^ (0xEDB88320 if slow & 1 else 0)
            self.assertEqual(fast, slow)
            self.assertEqual(fast ^ 0xFFFFFFFF, zlib.crc32(data))


if __name__ == "__main__":
    unittest.main()
