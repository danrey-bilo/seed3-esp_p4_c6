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
  escape tracks separate vertical runs. Dense graphs expand the centre
  corridor and canvas height. Perpendicular crossings are not junctions.
- `pedalboard_widgets.*`: rotary controls, rate-limited parameter callbacks,
  colored cable paths and arrowheads.
- `pedal_categories.h`: presentation categories with an Other fallback.
- `pedal_gesture.h`: pure 1000 ms + exit original card bounds to drag /
  2000 ms inside bounds to open the menu. Motion inside is still a hold.
  Visual slot swaps never reorder the authoritative DSP node array.
- `wifi_view.*`: settings overlay; only reads network snapshots while visible.
  Scanning/connecting/NVS writes live in `p4_wifi_settings`, not LVGL.

Cards use a 42 px clickable LED rather than an ON/BYPASS button. Cabinet Sim
uses double width; the layout adapts columns/rows and preserves separate
cable corridors. The current cabinet DSP exposes three meaningful knobs.

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

`audio_dashboard_needs_live_audio()` requests USB PCM meter work only while
the USB monitor is visible; it is not enabled merely by opening the pedalboard.

## Verification

From the repository root, `python tools/host/build_tests.py` compiles the
actual UI against native LVGL. It checks 1536 dense layouts (stereo, mixed and
wide multirow layouts with reversed node order), simulated pointer timing,
real event callbacks, page gating and
full-queue rejection. This does not replace a physical touchscreen check.

[User guide](../../../docs/PEDALBOARD_GUIDE.md)
