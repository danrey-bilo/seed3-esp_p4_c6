# ESP32-P4 I2S slave -> UAC2 capture test

This is a fixed 48 kHz milestone firmware for
`ESP32-P4-WIFI6-Touch-LCD-7B`. It receives the two Seed3 diagnostic channels
on GPIO2/3/4 and exposes one stereo UAC2 capture stream through the J1 USB 2.0
High-Speed controller.

The screen, SD, Wi-Fi, BLE, GPIO5 playback path and GPIO30/31 control UART are
not enabled. USB playback is intentionally deferred until capture has passed a
30-minute stability test.

## Tool versions

- ESP-IDF: 5.5.5 (the component manifest pins this exact release)
- `espressif/usb_device_uac`: 1.3.1
- TinyUSB: selected transitively by that component

## Build

On the prepared workstation run:

```powershell
.\build.cmd -Clean
```

The script activates the installed ESP-IDF 5.5.5 environment, builds the
project, and creates both separate images and a ready-to-flash merged image in
`firmware/`. The component manager downloads the pinned UAC component on the
first build. The project selects its High-Speed root port and a capture-only
UAC2 descriptor: 2 channels, 48 kHz, packed PCM24.

To flash through the board's programming USB port:

```powershell
.\flash.cmd COM7
```

Replace `COM7` with the actual port. See `../FLASHING_RU.md` for the exact
connector and BOOT/RESET sequence.

## Expected monitor output

At boot, the PCM24 vector test prints `PASS`, followed by one statistics line
per second. `under` can increase before Windows opens capture or while the Seed
is not clocking. During the acceptance recording, both `under` and `over` must
remain unchanged and `align` must remain zero.

The ring holds 512 stereo frames (10.67 ms) and the UAC component requests four
milliseconds at a time. On overflow the oldest frames are dropped to bound
latency. On underflow the host receives zeroes; stale samples are never repeated.

## Hardware test

1. Power both boards independently and connect the common ground first.
2. Flash Seed3 and confirm FS=48 kHz and BCLK=3.072 MHz with a logic analyser.
3. Connect D26 to GPIO4 and the two clocks through 33 ohm series resistors.
4. Connect J1 using the specified data-only adapter with VBUS physically open.
5. Confirm `High-Speed`, 2 channels, 48 kHz and 24 bits in USBTreeView.
6. Record at least five seconds and run:

```powershell
python ..\tools\verify_test_tones.py recording.wav
```

The stock component descriptor is a development descriptor. Before treating
the hardware as a product, assign a legitimate VID/PID and re-check its power
attributes against the final J1/VBUS circuit.
