"""Synthetic parser tests, not hardware evidence."""
import tempfile
from pathlib import Path
import unittest
from analyze_transport_log import analyze, fields


class TransportLogTests(unittest.TestCase):
    def test_numeric_fields(self):
        self.assertEqual(fields("err=0 seed_status=0x0003 rate=044100"),
                         {"err": 0, "seed_status": 3, "rate": 44100})

    def test_separate_rings_and_steady_delta(self):
        lines = []
        for n in range(7):
            ms = 1000 * n
            lines.extend([
                f"I ({ms}) x: spi={n*3000} err=0 frame_err[h/c/s/f]=0/0/0/0 seed_status=0x0002 sessions=1 echoed=0",
                f"I ({ms}) x: format rate=96000 seed=96000 bits=24/24 prefill=96 budget=1ms changes=1 epochs=2",
                f"I ({ms}) x: uac mounted=1 hs=1 active=1/1 src=100 cap_done=96 pkts=2 fill=50+96 sil=0 over=23 inc=11 play_rx=300 play_read=200 fill=100 under=2 over=0 bad=0 recover=1 reset=0 gap=4/4 fb=3145728 fb_done=2 fb_inc=3 play_pkt=2 play_len=0/0 ctrl=5/0",
                f"I ({ms}) x: cpu permille busy_core0=180 busy_core1=275 spi_task=270 usb_task=140",
            ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "synthetic.log"
            path.write_text("\r\r\n".join(lines), encoding="utf-8")
            report = analyze(path)
        self.assertTrue(report["spi_errors_all_zero"])
        self.assertTrue(report["steady_duplex_counters_unchanged"])
        self.assertEqual(report["usb_last"]["capture_over"], 23)
        self.assertEqual(report["usb_last"]["playback_over"], 0)
        self.assertEqual(report["usb_last"]["capture_fill"], 50)
        self.assertEqual(report["usb_last"]["playback_fill"], 100)
        self.assertEqual(report["steady_duplex"][0]["duration_ms"], 3000)
        self.assertEqual(report["cpu_scheduler_estimate"]["96000"]["busy_core1"]["median_percent"], 27.5)


if __name__ == "__main__":
    unittest.main()
