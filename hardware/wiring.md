# Wiring Reference

**MCU:** SparkFun Thing Plus ESP32-S3 (DEV-21230)

---

## PN5180 NFC Module — SPI

| PN5180 pin | ESP32-S3 pin | Notes |
|---|---|---|
| NSS (CS) | GPIO 5 | SPI chip select |
| MOSI | GPIO 35 | |
| MISO | GPIO 37 | |
| SCK | GPIO 36 | |
| BUSY | GPIO 6 | Active-high busy signal |
| RST | GPIO 7 | Active-low reset |
| 3.3V | 3V3 | |
| GND | GND | |

> The PN5180 is a 3.3 V device. Do not connect to 5 V.

---

## NAU7802 Load Cell ADC — I²C (Qwiic)

| Signal | ESP32-S3 pin |
|---|---|
| SDA | GPIO 8 |
| SCL | GPIO 9 |
| 3.3V | 3V3 (via Qwiic) |
| GND | GND (via Qwiic) |

Connect with a Qwiic cable directly to the SparkFun Thing Plus Qwiic connector.
The NAU7802 breakout's load cell terminal block accepts a standard 4-wire load cell
(E+, E−, A+, A−).

### Load cell wiring (full-bridge)

| Load cell wire | Function | NAU7802 terminal |
|---|---|---|
| Red (红) | Excitation + | E+ |
| Black (黑) | Excitation − | E− |
| Green (绿) | Signal + | A+ |
| White (白) | Signal − | A− |
| Yellow (黄) | Shield / drain | GND (omit if cell has no yellow lead) |

Mount the cell so its force axis is vertical and the sealed (potted) face points
**down** — that orientation gives a positive output. Calibrate with a known weight
at 50–80 % of the cell's rated range (the `ZERO` / `CAL <grams>` serial workflow).

> Full details and the original manufacturer sheet:
> [`docs/datasheets/load-cell-install-guide.md`](../docs/datasheets/load-cell-install-guide.md)

---

## 3.5" ILI9488 TFT Display — SPI

Hosyond 3.5" ILI9488 480×320 module (board marking `3.5'' TFT SPI 480X320
V1.0`). Shares the SPI bus with the PN5180 (same MOSI/MISO/SCK pins); a
firmware mutex prevents bus contention. The display mounts in a printed
adapter plate on the existing porch face. Full silkscreen pinout and the
board photo: [`docs/datasheets/display-hosyond-ili9488.md`](../docs/datasheets/display-hosyond-ili9488.md).

| TFT pin | ESP32-S3 pin | Notes |
|---|---|---|
| SDI (MOSI) | GPIO 35 | shared with PN5180 |
| SCK | GPIO 36 | shared with PN5180 |
| SDO (MISO) | GPIO 37 | shared with PN5180 (display read-back; rarely used) |
| CS | GPIO 15 | TFT chip select |
| DC (RS) | GPIO 16 | data/command select |
| RST | GPIO 17 | reset |
| LED | 3.3V | backlight enable (tie high for always-on) |
| VCC | 3.3V | |
| GND | GND | |

**Resistive touch (T_CLK/T_CS/T_DIN/T_DO/T_IRQ):** separate SPI slave on
the same header — not wired in this project.

**On-board microSD (`SD_CS`/`SD_MOSI`/`SD_MISO`/`SD_SCK`):** broken out on
a **separate 4-pin header** and **not** bonded on-PCB to the display SPI
lines. Currently unwired. For the SD-local redesign the plan is to put it
on the ESP32-S3's second SPI host (its own bus) rather than share the
NFC/TFT bus — see the datasheet note and
[`docs/design/sd-local-ecosystem.md`](../docs/design/sd-local-ecosystem.md).

---

## WS2812 NeoPixel (onboard only)

The porch-face NeoPixel is replaced by the TFT display, which shows status
colours directly. Only the onboard pixel (GPIO 48) remains active; no external
wiring needed.

---

## Passive Piezo Buzzer

| Buzzer pin | ESP32-S3 pin |
|---|---|
| + | GPIO 14 |
| − | GND |

No resistor needed for a passive piezo driven by PWM.

---

## WiFi Reset (BOOT button)

The BOOT button already on the SparkFun Thing Plus ESP32-S3 board (GPIO 0) doubles
as a WiFi credential reset trigger — no external wiring needed. Hold for 3 seconds
at power-on to reopen the captive portal.

---

## Summary

| Peripheral | Interface | ESP32-S3 pins |
|---|---|---|
| PN5180 NFC | SPI | 5 (CS), 35 (MOSI), 37 (MISO), 36 (SCK), 6 (BUSY), 7 (RST) |
| ILI9488 TFT display | SPI (shared) | 15 (CS), 16 (DC), 17 (RST), 35 (MOSI), 37 (MISO), 36 (SCK) |
| NAU7802 scale ADC | I²C / Qwiic | 8 (SDA), 9 (SCL) |
| WS2812 NeoPixel (onboard) | — | 48 (onboard) |
| Passive buzzer | PWM | 14 |
| BOOT / WiFi reset | — | 0 (onboard) |
