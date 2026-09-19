# Direct patching verification — 19 September 2026

## Build and native tests

ESP-IDF 5.5.5 / ESP32-P4 rev1.x and the Seed3 toolchain builds complete.
Seed3 code/data image: 101,856 bytes before its DFU suffix; main SRAM:
30,240 bytes; RAM_D2: 20,832 bytes. The reserved SDRAM arena is 62 MiB.

`python tools/host/build_tests.py` passes:

- **1536** dense stereo/mixed mono-stereo/wide-cabinet layouts, including reversed node
  order: no overlapping parallel line segments and all vertices within the
  adaptive canvas. Current editor contract: one wire per socket.
- Actual LVGL handlers: connect from either end, occupied-socket cancellation,
  repeated-tap cancellation, select-and-blank unplug, stable internal colors,
  fixed physical channel colors, and full command queue rejection.
- **240** add/delete cycles, the 12-node limit, all 16 physical channel masks,
  **100** editor open/close cycles, clickable top LED, outside dismissal and
  separate physical meters with hidden-page gating.
- DSP graph checks on 44.1/48/88.2/96 kHz; independent channels and Splitter/
  Mixer cancellation. **64** twelve-node graphs match a scalar reference.
- 42 DSP profiles produce finite samples; 14 additional controls affect
  output; parameter changes preserve a delay tail; v1 presets migrate.
- Meter protocol CRC, malformed/truncated lines, masks, clipping and a
  maximum of 16 nonblocking UART writes per foreground poll.
- Category/Other fallback, replacement-target retention, double-width cabinet;
  real LVGL pointer press/release simulation checks short-tap editing, no menu
  at 1.8 seconds despite motion inside the card, menu after 2 seconds inside,
  and drag after 1.1 seconds followed by leaving the card bounds.
  Visual slot changes leave the entire graph definition byte-identical.
- Wi-Fi connected view/IP using a mock service; opening a connected page
  does not scan again, and closing its form clears the password text.
  Password keyboard is wholly within 1024×600; tapping its letter button
  inserts that letter into the password field. Explicit TOP_LEFT alignment
  fixes LVGL's default bottom alignment moving it offscreen.

`python tools/test_seedfx_pack.py`: **5 tests pass**, including graph-v2
size/CRC and invalid topology rejection.

The PNG illustrations are snapshots of the real native LVGL renderer with
test graphs and simulated levels. They are not photographs or analog readings.

## Firmware identity

Both boards use the existing wiring, SPI mode 0 / 20 MHz and unchanged
USB descriptors. Seed3 was updated through `seed boot` → ROM DFU and
started automatically. P4 flashing uses the ROM loader without `--force`.

| Image | SHA-256 |
| :--- | :--- |
| Seed3 BIN with suffix | `42b17db6e9f508f9d54086c3174687d823e99548a00597aff6d6a23b2fd396fe` |
| P4 merged | `72d8bccf50aa3c270616c9445300febd90894c94e229c94468e8473fe748371b` |
| P4 application | `342130ba83521d62a1519bb2c6238217efd883caf2ad8fd4798e68a0f2a92083` |

## Why rendering settings changed

An intermediate build with two software draw workers and a 15 ms refresh
period passed the UI's ten 0 → 12 → 0 cycles without a restart, and its SPI
CRC/sequence counters remained zero. However, while continuously rebuilding
the board, the device playback-underrun counter rose even though WASAPI
reported no discontinuities. That is **not a clean audio qualification**.

The same intermediate build held its underrun count constant during a
separate 12-second 96 kHz duplex run without board rebuilding. The final
configuration uses one software draw worker, a 33 ms refresh period and the
LVGL owner pinned to core 1 at priority 3, below SPI priority 15. The USB
worker remains on core 0 at priority 12. Transport framing and device
prefill settings were not changed.

## Hardware checks before the final keyboard/gesture adjustment

P4 ROM flashing completed with flash hash verification. C6 answered over
4-bit SDIO / 40 MHz on the documented pins, and a real scan returned **two**
networks (`connected=0 busy=0 reconnect=1 networks=2`). C6 firmware was not
changed. SSIDs/passwords are excluded from this report.

With C6 initialized after scanning:

- Real P4 `ui stress`: **10 cycles 0 → 12 → 0**, original board restored,
  DSP untouched. No reboot/panic. Minimum reported internal free memory
  **108,927 bytes**; lowest reported UI stack watermark **10,468 bytes**.
- **96 kHz / PCM24 packed / duplex / 12 seconds**, concurrent with that
  display stress: **1,152,000 / 1,152,000 captured frames**, WASAPI
  discontinuities/timestamp/position errors/silent packets **0**.
- **44.1 kHz / PCM24 packed / duplex / 8 seconds**, after the display stress:
  **352,800 / 352,800 frames**, same host error counters **0**.
- SPI header/CRC/sequence/frame-error counters **0/0/0/0** throughout.
  Physical-meter valid frames continued arriving; malformed-line count stayed
  at its one pre-existing startup/interrupted-line event.
- In the sampled steady 96 kHz window (162.833–173.503 seconds uptime),
  playback underruns stayed **0**, capture overflow **239**, capture
  incomplete **12**, recovery **1**: none increased. Capture silence,
  malformed playback and USB reset counters stayed **0**. One playback
  underrun appeared around stream closure, not in the sampled steady window.
- The logged 44.1 kHz steady window (199.054–202.243 seconds uptime) kept
  playback underrun **1**, capture overflow **239**, incomplete **28**,
  recovery **2** constant. Startup/rate-change recovery counts are nonzero;
  this is **not** a claim of zero lifetime USB faults.

The 12 s / 8 s checks above used application hash
`79af0715fcec8d8e8f2db612c53dd81adb7c663fc52e6ed3432770967061f07d`.
The final adjustment fixes the keyboard position, changes the drag gesture
to 1 second + exit-card bounds, and rejects stale DHCP events when changing
networks. USB/SPI/DSP code is unchanged between those images.

The artificial full-board rebuild still produces `Failed to acquire LVGL
lock` from bounded status-update attempts while rendering owns the lock.
These are skipped UI updates, not a panic; normal operation resumes when
stress stops. Network scan is verified. The user reported that the IP address
was displayed after connecting, but saved-credential restart and radio-loss
reconnect have not been qualified. A subsequent post-flash diagnostic showed
Wi-Fi off/no active connection, so persistence is not reported as passed.
No password was requested in chat. No high-throughput network load was tested.

## Shipped image recheck

The image in the hash table was flashed **application-only at 0x10000**,
preserving NVS, and verified by the ROM flash checksum. ESP-IDF build completed
without application warnings. The native gesture/keyboard tests above were
rerun against these sources.

A further **12-second 96 kHz PCM24 duplex run while initializing/scanning C6**
captured exactly **1,152,000 frames**, with host discontinuities, timestamp
errors, position errors and silent packets all **0**. Scan again returned
two networks and the device did not restart; SPI errors remained **0/0/0/0**.
Device capture silence/overflow, playback overflow/malformed data, USB reset
and recovery counters stayed zero. However, **two device playback underruns**
were counted during the active logged stream,
and capture incomplete reached four by stream closure. This concurrent
scan test is therefore **not a zero-underrun audio pass**, despite clean
WASAPI flags. Avoid scanning during critical low-buffer playback until
network scheduling/buffering has separate audio qualification.

## Scope

Native tests do not prove physical touchscreen behavior, DSP deadlines on a
fully loaded effects graph, SNR/THD, analog channel separation or end-to-end
latency. Device telemetry and host-side discontinuity flags are separate
measurements and must not be substituted for one another.

Old imported fan-out presets may share a stem at a multiply connected
output. Replace such fan-outs with explicit Splitter nodes for the new
editor's completely separated tracks.

Historical 100-cycle/301-second UAC2 qualification belongs to its earlier
firmware snapshot. It was not rerun as part of this UI change.
