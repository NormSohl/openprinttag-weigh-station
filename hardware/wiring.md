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

---

## 3.5" ILI9488 TFT Display — SPI

Hosyond / MSP3520-type 480×320 module. Shares the SPI bus with the PN5180
(same MOSI/MISO/SCK pins); a firmware mutex prevents bus contention.
The display mounts in a printed adapter plate on the existing porch face.

| TFT pin | ESP32-S3 pin | Notes |
|---|---|---|
| SDI (MOSI) | GPIO 35 | shared with PN5180 |
| SCK | GPIO 36 | shared with PN5180 |
| SDO (MISO) | GPIO 37 | shared with PN5180; wired for future touch reads |
| CS | GPIO 15 | TFT chip select |
| DC (RS) | GPIO 16 | data/command select |
| RST | GPIO 17 | reset |
| VCC | 3.3V | |
| GND | GND | |

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
