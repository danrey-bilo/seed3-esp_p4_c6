# Bring-up log

## 2026-09-17: software baseline

Implemented the fixed M1--M3 diagnostic profile.

Confirmed from the pinned local libDaisy source (`265aae3`):

- Seed3 is detected by `DaisySeed::CheckBoardVersion()`;
- its built-in TAC5242 path uses SAI1 at 48 kHz and 24-bit data;
- libDaisy's `SAI_24BIT` uses MSB-justified protocol;
- SAI2 exposes the required D25--D28 alternate functions.

Software verification completed:

- `Seed3/build.cmd -Clean`: PASS;
- main Seed3 BIN: 73,472 bytes;
- main Seed3 SHA-256:
  `c99ff75fa79143ad4064f21e8c29e6b3ea181c92982b82ab436b707c7b49aff9`;
- standalone `F:/Repos/seed3 audio usb/Seed3P4SaiTest`: PASS and produces
  the same binary hash;
- ESP32-P4 project built with ESP-IDF 5.5.5: PASS;
- P4 application image: 261,776 bytes;
- P4 application SHA-256:
  `30182bd82bcb961ca992bf7f8e3b236b4e4e77c1b9a92b95fb092693040c8a11`;
- P4 merged image (load at `0x2000`): 319,120 bytes, SHA-256
  `ba50c6077e120a4b33e554c9c887940a4ec3e62eaac3fffee3e3660f147f53ec`;
- the generated configuration confirms ESP32-P4, USB High-Speed, two input
  channels, zero output channels, 48 kHz, packed 24-bit samples and the
  board's 32 MB NOR Flash;
- connected P4 was identified as chip rev v1.3; the image was rebuilt for the
  mutually exclusive v1.x hardware branch (`min v1.0`, `max v1.99`), flashed
  through the ROM loader, and its flash hash was verified;
- UART boot log confirms ESP-IDF 5.5.5, 32 MB DIO/80 MHz, PCM24 self-test PASS
  and stable once-per-second statistics without a reset loop; APLL is selected
  as the rev-v1.x I2S slave sampling clock;
- with Seed3 connected, the P4 RX frame count increases continuously and
  `align=0`; the ring reaches 512 frames and reports expected overruns while
  `usb=0` because the Windows capture stream is not open yet;
- PCM24 boundary-vector self-test is present in the P4 firmware;
- host WAV verifier passed its synthetic stereo-tone self-test;
- P4 I2S initializers were checked against the ESP-IDF 5.5.5 public headers.

Implemented but not yet confirmed with instruments:

- actual BCLK and FS frequency/duty cycle on the assembled wiring;
- left/right ordering and bit alignment on D26;
- ESP32-P4 enumeration through J1 as High Speed;
- a Windows WAV recording containing the two tones;
- a 30-minute zero-overrun/zero-underrun run.

Ready-to-flash images and SHA-256 files are generated in each project's
`firmware/` directory. Hardware flashing and electrical/USB acceptance tests
still require the physical boards.
