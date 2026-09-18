# Hardware test evidence

Final v0.2 results use `analysis_version: 2` and the corrected WASAPI recorder.
The authoritative explanation and binary hashes are in
[TEST_REPORT_RU.md](../../libraries/p4_uac2_stream_v2/docs/TEST_REPORT_RU.md).

Qualified on this PC with **WASAPI exclusive 10 ms**, device budget 1 ms:

- `qualification-v2-48k-10ms`: 60 s capture and 100 open/close cycles.
- `qualification-v2-96k-10ms`: 301 s full-duplex, serial counters and separate
  post-test recovery/Seed load observations.
- `qualification-v2-10ms-formats`: all 20 format/duplex switch tests.

Limits and negative evidence are intentionally retained:

- `qualification-v2-min`: 20/20 short tests passed at the driver minimum.
- `qualification-v2-48k`: the longer minimum-buffer test FAILED (6 ms loss).
- `qualification-v2-formats` and `release-v2-formats`: earlier strict failures.
- `acceptance-v2-*`: historical diagnostics. Their old PASS fields are **not
  final acceptance**. The earlier analyzer could miss nonzero PCM block loss;
  old `phase-steps.json` files document the discovered 10/20 ms jumps.

Other directories predate v0.2 and do not qualify its firmware. Raw WAV, build
output and serial logs remain local and ignored. JSON does not substitute for
the hardware run; preserve local raw recordings if independent reanalysis is needed.
