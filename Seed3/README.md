# Daisy Seed3 SAI2 test firmware

This firmware keeps the Seed3 codec path alive while making Seed3 the audio
clock master for the ESP32-P4 link.

- SAI1: built-in TAC5242 codec, stereo 48 kHz / 24 bit;
- SAI2 B: master receive on D25 (reserved for the later playback milestone);
- SAI2 A: synchronous slave transmit on D26;
- SAI2 clocks: FS on D27 and BCLK on D28;
- transmitted diagnostic signal: 997 Hz left and 1501 Hz right at -12 dBFS;
- local analog input is copied to local analog output and does not depend on
  the P4.

The LED blinks at 1 Hz while callbacks are running. It stays on if the board is
not detected as Seed3 or an SAI/audio initialization call fails.

Build with the installed Daisy Toolchain:

```powershell
.\build.cmd -Clean
```

The build script uses `Seed3/libDaisy` when present. On this workstation it can
also reuse the pinned libDaisy checkout in
`F:/Repos/seed3 audio usb/Seed3MonoUsbInput/libDaisy`.

The build also copies the release image and its SHA-256 to `firmware/`.
Flash it with the included script after putting Seed3 into DFU mode:

```powershell
.\flash.cmd
```

See `../FLASHING_RU.md` for the button sequence and complete wiring/verification
procedure.
