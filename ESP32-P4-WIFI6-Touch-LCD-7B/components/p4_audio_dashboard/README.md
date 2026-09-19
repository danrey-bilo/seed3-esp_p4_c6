# P4 audio dashboard

LVGL 9.5 UI for the 1024×600 P4 touch display. The application publishes
snapshots and consumes nonblocking commands; it does not manipulate LVGL
objects from USB/SPI interrupts.

The LVGL owner is pinned to core 1 at priority 3, below the SPI worker
(priority 15). USB runs on core 0 at priority 12. The configuration selects
one software drawing worker and a 33 ms refresh period to reduce simultaneous
PSRAM rendering. Existing build directories must also update these two
`sdkconfig` values; `sdkconfig.defaults` initializes new configurations only:
`CONFIG_LV_DRAW_SW_DRAW_UNIT_CNT=1`, `CONFIG_LV_DEF_REFR_PERIOD=33`.

- `audio_dashboard.c`: page lifecycle, views, overlays and command binding.
- `pedal_patch.*`: allocation-free two-tap state machine, one-wire-per-port
  editing, safe disconnect and color allocation. Physical channel colors are
  fixed independently of palette state.
- `pedal_layout.*`: allocation-free orthogonal cable routing. Horizontal
  interval coloring prevents overlapping parallel runs; separate socket
  local escape tracks, 12 lanes per row gutter and 30 outside bus tracks keep
  wires out of single/wide card bodies. Only physical endpoint wires reroute
  on scroll; internal routes stay unchanged. Perpendicular crossings are not junctions.
- `pedal_grid.*`: bounded 5×8 placement keyed by node ID, single/two-cell spans,
  retained holes, collision-safe move/swap, hit testing and dynamic canvas height.
  No DSP reordering or heap allocation.
- `pedalboard_widgets.*`: rotary controls, rate-limited parameter callbacks,
  colored cable paths and arrowheads; scrolling updates existing line objects
  and their persistent point buffers without object allocation/deletion.
- `pedal_categories.h`: presentation categories with an Other fallback.
- `pedal_gesture.h`: pure 500 ms + exit original card bounds to drag /
  1000 ms inside bounds to open the menu. Motion inside is still a hold.
  A white outline at 500 ms indicates drag readiness without changing the
  card's layout or jack positions. It clears on release, cancellation or menu.
  Hardware hold durations use monotonic `esp_timer_get_time()` milliseconds;
  LVGL polls the gesture every 25 ms, so action occurs on the next UI poll.
  Visual slot swaps never reorder the authoritative DSP node array.
- `wifi_view.*`: settings overlay; only reads network snapshots while visible.
  Scanning/connecting/NVS writes live in `p4_wifi_settings`, not LVGL.

Cards use a 42 px LED touch target rather than an ON/BYPASS button. All bodies
use the same corner radius and effect-tinted flat fill: 104×198 for one cell,
264×198 for the two-cell Cabinet Sim. Its editor exposes three meaningful knobs.
Grid hints exist **only during dragging**, through the last occupied row plus
one extra row, capped at eight. Idle canvas height follows occupied rows (minimum
viewport height). Dragging shows a green valid/red invalid target, supports
edge auto-scroll and cancels outside the field. Wide drops/swaps cannot cross
row boundaries or overlap other cards. Placement is session-only.

Direct editing creates one wire per port; legacy imported fan-out graphs
remain accepted by the DSP. Shared stems at legacy multi-connected outputs
can coincide: convert those branches to explicit Splitter nodes for fully
separated tracks. The geometric no-overlap test uses the current editor's
one-wire-per-port contract.

The authoritative graph lives in `p4_audio_control`; all edits are queued
before the visible graph changes. A full queue rejects the edit. The UI
never reassigns node IDs or automatically reconnects neighbors after delete.
Keep the pure patch/layout modules independent of the SPI transport.

## Physical meters

`audio_dashboard_submit_local_peaks(in1,in2,out1,out2)` accepts four unsigned
Q31 magnitudes. These are ADC/final-DAC peaks from Seed3's CRC-protected UART
telemetry, not the USB monitor's PCM streams. Atomic max publication retains
peaks between screen updates. Hidden pedalboard pages do not consume new
peaks or redraw bars. No LVGL calls occur in this submission function.
Physical socket/meter objects belong to the fixed page, not the scrolling
node layer. Adding/deleting/dragging cards never recreates those rail objects.

`audio_dashboard_needs_live_audio()` requests USB PCM meter work only while
the USB monitor is visible; it is not enabled merely by opening the pedalboard.

## Verification

From the repository root, `python tools/host/build_tests.py` compiles the
actual UI against native LVGL. It checks 1536 arbitrary single/wide layouts at
three scroll offsets each (stereo and mixed with reversed IDs), cable/body intersections, simulated pointer timing,
real event callbacks, page gating and
full-queue rejection. This does not replace a physical touchscreen check.

[User guide](../../../docs/PEDALBOARD_GUIDE.md)

[Gesture timing and verification](../../../docs/PEDAL_GESTURE_TIMING.md)

[Free-grid build and verification](../../../docs/PEDALBOARD_GRID_TEST.md)
