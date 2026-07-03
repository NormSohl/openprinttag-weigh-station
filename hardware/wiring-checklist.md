# Wiring Checklist

Print and tick each connection as it is made.
See `wiring.md` for full notes; `bill-of-materials.md` for part sources.

---

## 1. Power

| # | From | To | Wire | Done |
|---|------|----|------|------|
| 1 | USB-C breakout VBUS | ESP32-S3 5V | red | ☐ |
| 2 | USB-C breakout GND | ESP32-S3 GND | black | ☐ |

---

## 2. PN5180 NFC Module — SPI

| # | From | To | Wire | Done |
|---|------|----|------|------|
| 3 | PN5180 3.3V | ESP32-S3 3V3 | red | ☐ |
| 4 | PN5180 GND | ESP32-S3 GND | black | ☐ |
| 5 | PN5180 NSS | ESP32-S3 GPIO 5 | any | ☐ |
| 6 | PN5180 MOSI | ESP32-S3 GPIO 35 | any | ☐ |
| 7 | PN5180 MISO | ESP32-S3 GPIO 37 | any | ☐ |
| 8 | PN5180 SCK | ESP32-S3 GPIO 36 | any | ☐ |
| 9 | PN5180 BUSY | ESP32-S3 GPIO 6 | any | ☐ |
| 10 | PN5180 RST | ESP32-S3 GPIO 7 | any | ☐ |

> PN5180 is 3.3 V — do not connect to 5 V rail.
>
> Route the PN5180 harness through the platform cable channel and into the
> base via the side-wall pass-through. Leave a slack loop between the
> platform and base so the load cell can deflect freely.

---

## 3. NAU7802 Load Cell ADC — Qwiic

| # | Connection | Done |
|---|-----------|------|
| 11 | Qwiic cable: ESP32-S3 Qwiic → NAU7802 Qwiic | ☐ |

---

## 4. ILI9488 3.5" TFT Display — SPI

Mounts in the printed adapter plate on the porch face.
SPI pins are shared with the PN5180 — firmware mutex prevents conflicts.

| # | From | To | Wire | Done |
|---|------|----|------|------|
| 12 | TFT VCC | ESP32-S3 3.3V | red | ☐ |
| 13 | TFT GND | ESP32-S3 GND | black | ☐ |
| 14 | TFT SDI (MOSI) | ESP32-S3 GPIO 35 | any | ☐ |
| 15 | TFT SCK | ESP32-S3 GPIO 36 | any | ☐ |
| 16 | TFT SDO (MISO) | ESP32-S3 GPIO 37 | any | ☐ |
| 17 | TFT CS | ESP32-S3 GPIO 15 | any | ☐ |
| 18 | TFT DC (RS) | ESP32-S3 GPIO 16 | any | ☐ |
| 19 | TFT RST | ESP32-S3 GPIO 17 | any | ☐ |

---

## 5. TAL220B Load Cell — 4-wire analog to NAU7802

| # | Load cell wire | NAU7802 terminal | Done |
|---|---------------|-----------------|------|
| 20 | Red (E+) | E+ | ☐ |
| 21 | Black (E−) | E− | ☐ |
| 22 | White (A+) | A+ | ☐ |
| 23 | Green (A−) | A− | ☐ |

> Verify wire colors against your specific TAL220B — some batches
> substitute blue for green on A−.

---

## 6. Passive Piezo Buzzer

| # | From | To | Done |
|---|------|----|------|
| 24 | Buzzer + | ESP32-S3 GPIO 14 | ☐ |
| 25 | Buzzer − | ESP32-S3 GND | ☐ |

---

## 7. Onboard NeoPixel

| # | Note | Done |
|---|------|------|
| 26 | No wiring — WS2812 is onboard at GPIO 48 | ☐ |

---

## Post-wiring checks before power-on

| # | Check | Done |
|---|-------|------|
| 27 | No bare wire ends touching adjacent pins or the enclosure | ☐ |
| 28 | PN5180 harness has a slack loop — platform moves freely | ☐ |
| 29 | Load cell wires not kinked or pinched by enclosure parts | ☐ |
| 30 | USB-C breakout seated in rear-wall bosses, connector faces cutout | ☐ |
| 31 | NAU7802 Qwiic connector fully clicked in (audible snap) | ☐ |
| 32 | TFT adapter plate seated flush; no SPI wires pinched | ☐ |

---

## First power-on

| # | Expected | Observed | Done |
|---|----------|----------|------|
| 33 | TFT shows "Weigh Station / Starting..." | | ☐ |
| 34 | TFT turns blue, shows "WiFi Setup / WeighStation-Setup" | | ☐ |
| 35 | Onboard NeoPixel lights blue | | ☐ |
| 36 | `WeighStation-Setup` AP visible on phone/laptop | | ☐ |
| 37 | Serial monitor (115 200 baud) shows `[scale] Ready` | | ☐ |
