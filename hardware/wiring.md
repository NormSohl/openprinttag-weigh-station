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

## SSD1306 OLED 128×64 — I²C (Qwiic)

| Signal | ESP32-S3 pin |
|---|---|
| SDA | GPIO 8 |
| SCL | GPIO 9 |

Shares the Qwiic I²C bus with the NAU7802. I²C address: `0x3C`.

---

## WS2812 NeoPixels

Two NeoPixels show the same status colour: the onboard pixel (inside the
enclosure) and an external WS2812 breakout mounted in the 5 mm light-pipe
hole on the sloped front face.

### Onboard (no external wiring needed)

| Signal | GPIO |
|---|---|
| Data | GPIO 48 (onboard) |

### External WS2812 breakout

| Breakout pin | ESP32-S3 pin |
|---|---|
| Data in | GPIO 13 |
| + (power) | 3.3V |
| − | GND |

The breakout data input is a 3.3 V logic signal — no level shifter needed.

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
| NAU7802 scale ADC | I²C / Qwiic | 8 (SDA), 9 (SCL) |
| SSD1306 OLED | I²C / Qwiic | 8 (SDA), 9 (SCL) |
| WS2812 NeoPixel (onboard) | — | 48 (onboard) |
| WS2812 NeoPixel (external) | Data | 13 |
| Passive buzzer | PWM | 14 |
| BOOT / WiFi reset | — | 0 (onboard) |
