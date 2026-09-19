# Free-grid pedalboard verification

## Scope

P4-only UI update: five-column snap placement with a **hidden idle grid**,
104×198 flat colored cards and 264×198 two-cell cabinets. Guides appear only
during dragging, through the last occupied row plus one extra row (eight-row
capacity). Includes 500/1000 ms movement/menu gestures, valid/invalid drop
targets, edge auto-scroll and fixed physical I/O rails with live meters.
The 40 view cells are not 40 processors: the DSP still supports **12 active nodes**.
Placement remains session-only. USB, SPI, Seed3 DSP, SD packages and the
Wi-Fi service are unchanged. The prior pending gesture-speed update is
included in this snapshot.

![Native LVGL rendering of free placement, with simulated levels](assets/pedalboard-grid.png)

![Guides during dragging, native LVGL render](assets/pedalboard-grid-drag.png)

![The same board scrolled; physical I/O remains fixed](assets/pedalboard-grid-scrolled.png)

These are renders of the actual firmware UI against native LVGL, not photos
or drawings. Graphs and levels in these illustrations are test fixtures.

## Build identity

ESP-IDF **5.5.5**, ESP32-P4 rev1.x-compatible. Build completed without
application compiler warnings. Application size: `0x15c1d0` bytes.

| Artifact | SHA256 |
| :--- | :--- |
| Application | `085e0913d16e50348e7a5b063699666f4683f21365d6bde97febc2d9c6dd31b0` |
| Merged image | `56889f9e726aa8fd8132248a64cf84595ca015ccb5070c131073b515419598a4` |

Only the application at **0x10000** was flashed, using the ROM without a
stub and without `--force`. Flash checksum verification succeeded. Wi-Fi NVS
was not erased. Seed3 was not reflashed.

## Native regression checks

Run `python tools/host/build_tests.py`, then
`tools/host/export_previews.ps1` to update the illustrated documentation.

- **1536** randomized layouts at **three scroll offsets each**, including
  reversed IDs, two-cell cabinets and mono/stereo profiles: no overlapping parallel segments, no wire segments
  through pedal bodies, orthogonal paths and canvas bounds. Physical cable
  endpoints compensate scrolling and remain aligned with fixed sockets.
- Empty-cell placement, single/wide swaps, two-cell occupancy, retained holes,
  deletion cleanup, invalid destinations, row-boundary and third-card
  collision rejection, and every grid cell's hit-test mapping.
- Real LVGL pointer events: 500/1000 ms holds, motion inside the card,
  free-cell drops below/right, edge auto-scroll to an initially hidden row,
  header-drop cancellation and adding to the held empty cell. Grid hints are
  absent at rest; drag hints have exactly the used rows plus one extra, and
  are deleted on release. Canvas height follows actual occupancy.
- Fixed socket screen coordinates, continued meter updates while scrolled,
  unchanged cable object identity and unchanged audio graph after movement.
- **240** additions/deletions, 16 channel masks, 100 editor cycles,
  four-knob parameter commands, full-queue rejection and modal dismissal.
- Existing direct-patching, color identity, Wi-Fi keyboard, graph/DSP and
  telemetry regression tests all pass.

No new LVGL object is created/deleted by the scroll callback; existing cable
point buffers are updated. Internal wires retain their geometry; only physical
endpoint wires reroute. The router uses local row stems and outside buses to
avoid two-cell bodies. Router/grid storage is bounded and never calls the audio transport.

## Hardware scope and limitations

P4 booted without a panic. The startup clock probe read **5000 ms monotonic /
5000 ms LVGL**. `wifi status` reported **connected=1, busy=0, reconnect=1**
without re-entering credentials.

The on-board `ui stress` test completed **10 cycles of 0 → 12 → 0 cards**
(240 steps), then restored the original board/page/grid. This is a display-only
test: it does not load those test effects into the Seed3 DSP graph.

- No panic, watchdog reset or reboot was found in the captured run.
- Empty-board free heap stabilized at **29,839,259 bytes**, largest block
  **29,360,128 bytes**, internal free heap **104,759 bytes** and LVGL task
  stack high-water reading **10,484**. No progressive heap loss was observed.
- SPI header/CRC/sequence/frame errors remained **0/0/0/0**.
- Physical meter packets increased from **745 to 2179** during the observation.
  The UART telemetry `bad=2` count was already present at startup and did not
  increase during stress; this is separate from the zero SPI error counters.
- The artificial full-view rebuild test logged **241 LVGL lock timeouts** in
  concurrent status updates. Those updates are skipped by the existing bounded
  lock path; the test finished and status updates continued. This is not a
  claim of a warning-free log or worst-case redraw latency qualification.
- Audio USB was **not mounted** (`mounted=0`, `hs=0`, capture/playback inactive).
  Hardware checks were in local 48 kHz mode, not an active PC duplex test.

Build/flash/UART logs are kept locally under `.tmp/pedal-grid-*`; generated
logs and recordings are intentionally excluded from Git.

Native pointer tests do not replace subjective touchscreen assessment.
No new end-to-end audio latency or full-duplex endurance qualification is
claimed for this visual update. Earlier concurrent Wi-Fi/audio limitations
in [the broader report](PEDALBOARD_DIRECT_PATCH_TEST.md) still apply. Legacy
imported fan-out can share source stems; use explicit Splitter nodes for
fully separated editor-created connections.
