# Pedal hold timing

This records the gesture-speed change. The subsequent
[free-grid build](PEDALBOARD_GRID_TEST.md) retains these 500/1000 ms thresholds
and adds empty-cell placement, auto-scroll and fixed I/O rails.

The current interaction thresholds are **500 ms to arm movement** and
**1000 ms to open the context menu**. These replace the previous 1000/2000 ms
thresholds at the user's request. Tap-to-edit remains shorter than 500 ms.

At 500 ms the card gains a thin white outline. Moving outside its original
bounds starts the drag; movement within the body remains a hold. The outline
does not change card size, jack coordinates or cable layout. Release,
cancellation and context-menu opening remove it. Dragging still changes only
visual grid slots, not DSP node IDs, audio order or connections.

`pedal_gesture.h` owns the three duration constants. On P4, elapsed hold time
comes from monotonic `esp_timer_get_time() / 1000`; a 25 ms LVGL timer handles
the gesture on the UI thread. This is a UI deadline, not an ISR deadline:
rendering/scheduling may delay the next poll. No audio priorities change.
The ESP32 microsecond-to-millisecond clock conversion follows the
[LVGL timing guidance](https://lvgl.io/docs/open/9.5/integration/overview).

## Diagnosis before the speed change

The diagnostic image retained the old 1000/2000 ms thresholds. Physical
touches were recorded over COM6:

- Clock probe: **5001 ms monotonic elapsed / 5000 ms LVGL elapsed**.
- Drag readiness: **1002-1006 ms** after body press.
- Menu callback: **2008 and 2011 ms** after body press.
- Actual drag started at **1031, 1257 and 1459 ms**, depending on when the
  finger left the original card bounds.

Thus the measured old thresholds were approximately 1/2 seconds, not 2/4.
The follow-up clarification explicitly requested twice the response speed;
the new constants halve those measured thresholds. No clock multiplier was
introduced. The board logs readiness/drag/menu elapsed milliseconds, plus
one clock comparison after startup, for subsequent touch checks.

## Build and native verification

- ESP-IDF 5.5.5, ESP32-P4 rev1.x-compatible application.
- Application SHA256:
  `2f1aef6ef8d98d2e015833b52fcca71ee1b758b8bd9728ebadd9eb3bc16f65d9`.
- Merged image SHA256:
  `7df2364ac8757a6508d806cf396760c0a708dede63695fb61cd03cad18b165e0`.
- `python tools/host/build_tests.py`: passed. Includes exact 499/500 and
  999/1000 ms boundaries, unsigned timer wrap, real LVGL pointer events,
  readiness-outline appearance/removal, inside-body movement, menu opening,
  visual-only reorder, layout/routing/DSP/meter regressions.

Native pointer tests do not measure the physical touchscreen or LCD scanout.
The SPI/USB/DSP implementations and the Seed3 image are unchanged. Previous
Wi-Fi/audio qualification limits in
[the broader test report](PEDALBOARD_DIRECT_PATCH_TEST.md) still apply.

## Device deployment

The application above was flashed to the physical P4 at **0x10000**, with
`--no-stub`, no `--force`, and a successful ROM checksum verification.
Boot completed without a panic; USB reported mounted / High Speed and the
idle SPI header/CRC/sequence/frame counters remained **0/0/0/0**. The startup
clock probe again measured 5001 ms monotonic / 5000 ms LVGL. A subsequent
`wifi status` reported connected=1, busy=0, reconnect=1 without entering
credentials again. This verifies retained credentials across this app update,
not recovery after a router outage or heavy simultaneous Wi-Fi/audio traffic.
The final observation contained no physical pedal holds; the shortened
500/1000 ms interactions are native-test verified, pending tactile confirmation.

For updates to an already configured board, flash **only the P4 application
at 0x10000** to preserve Wi-Fi NVS. Do not use the merged image for such an
update: its address span also covers the settings partition.
