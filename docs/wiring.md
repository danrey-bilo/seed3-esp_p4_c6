# Current SPI wiring (2026-09-18)

Confirmed against both current source files. P4 is the SPI master, while
Seed3's audio callback and READY pacing determine the audio sample rate.

| Signal | Seed3 | ESP32-P4 GPIO | Direction |
|---|---|---|---|
| SPI SCLK | D8 | 2 | P4 → Seed3 |
| SPI MOSI / playback | D10 | 3 | P4 → Seed3 |
| SPI MISO / capture | D9 | 4 | Seed3 → P4 |
| SPI CS | D7 | 5 | P4 → Seed3 |
| READY | D0 | 28 | Seed3 → P4 |
| UART TX | D13 | 30 / RX | Seed3 → P4 |
| UART RX | D14 | 31 / TX | P4 → Seed3 |
| GND | DGND | GND | common |

SPI mode 0, 20 MHz, 32 stereo frames per transaction: 1378.125 / 1500 /
2756.25 / 3000 transactions/s at 44.1 / 48 / 88.2 / 96 kHz. Use short wiring;
the new UAC2 component does not require reconnecting the boards.

Do not connect the boards' 3.3 V rails together. USB-A J1 uses the existing
data-only D+/D−/GND adapter with VBUS isolated between independently powered
PC and board. Do not connect an ordinary USB-A ↔ USB-A cable.

## Historical I2S wiring — not used by the current SPI firmware

| Signal | Daisy Seed3 | ESP32-P4 P3 | Direction |
|---|---|---|---|
| BCLK | D28 / physical pin 35 | GPIO2 / P3-10 | Seed3 -> P4 |
| FS / LRCLK | D27 / physical pin 34 | GPIO3 / P3-9 | Seed3 -> P4 |
| Record data | D26 / physical pin 33 | GPIO4 / P3-8 | Seed3 -> P4 |
| Playback data | D25 / physical pin 32 | GPIO5 / P3-7 | P4 -> Seed3 (reserved; unused in this profile) |
| UART TX | D13 / physical pin 14 | GPIO30 / P3-4 | reserved |
| UART RX | D14 / physical pin 15 | GPIO31 / P3-3 | reserved |
| Ground | DGND / physical pin 40 | GND / P3-11 | common |

Use 33 ohm series resistors in BCLK, FS and both data lines. Do not connect the
3.3 V rails of the two boards. Do not use GPIO46--GPIO48 on P1.

The PC connection is J1 USB-A High Speed. Do not use an ordinary USB-A to
USB-A cable. The bench adapter must carry D+, D- and ground only; VBUS between
the independently powered PC and board must be physically open.

## Expected signals

- FS: 48,000 Hz
- BCLK: 3.072 MHz (64 BCLK per stereo frame)
- slot format: stereo, MSB-justified, 24 valid bits in each 32-bit slot
- left tone: 997 Hz, approximately -12 dBFS peak
- right tone: 1501 Hz, approximately -12 dBFS peak
