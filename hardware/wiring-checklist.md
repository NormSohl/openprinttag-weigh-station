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

## 4. SSD1306 OLED — Qwiic

| # | Connection | Done |
|---|-----------|------|
| 12 | Qwiic cable: NAU7802 Qwiic out → SSD1306 Qwiic | ☐ |

> Daisy-chains on the same I2C bus (SDA GPIO 8 / SCL GPIO 9).
> If your NAU7802 has only one Qwiic port, use a Qwiic splitter or
> run a second cable directly from the ESP32-S3 Qwiic connector.

---

## 5. TAL220B Load Cell — 4-wire analog to NAU7802

| # | Load cell wire | NAU7802 terminal | Done |
|---|---------------|-----------------|------|
| 13 | Red (E+) | E+ | ☐ |
| 14 | Black (E−) | E− | ☐ |
| 15 | White (A+) | A+ | ☐ |
| 16 | Green (A−) | A− | ☐ |

> Verify wire colors against your specific TAL220B — some batches
> substitute blue for green on A−.

---

## 6. External WS2812 NeoPixel

Mounts in the 5 mm light-pipe hole on the sloped front face of the enclosure.

| # | From | To | Wire | Done |
|---|------|----|------|------|
| 17 | NeoPixel Data in | ESP32-S3 GPIO 13 | any | ☐ |
| 18 | NeoPixel + | ESP32-S3 3.3V | red | ☐ |
| 19 | NeoPixel − | ESP32-S3 GND | black | ☐ |

---

## 7. Passive Piezo Buzzer

| # | From | To | Done |
|---|------|----|------|
| 20 | Buzzer + | ESP32-S3 GPIO 14 | ☐ |
| 21 | Buzzer − | ESP32-S3 GND | ☐ |

---

## 8. Onboard NeoPixel

| # | Note | Done |
|---|------|------|
| 22 | No wiring — WS2812 is onboard at GPIO 48 | ☐ |

---

## Post-wiring checks before power-on

| # | Check | Done |
|---|-------|------|
| 23 | No bare wire ends touching adjacent pins or the enclosure | ☐ |
| 24 | PN5180 harness has a slack loop — platform moves freely | ☐ |
| 25 | Load cell wires not kinked or pinched by enclosure parts | ☐ |
| 26 | USB-C breakout seated in rear-wall bosses, connector faces cutout | ☐ |
| 27 | All Qwiic connectors fully clicked in (audible snap) | ☐ |

---

## First power-on

| # | Expected | Observed | Done |
|---|----------|----------|------|
| 28 | OLED shows "Weigh Station / Starting..." | | ☐ |
| 29 | Both NeoPixels light blue (WiFi Setup state) | | ☐ |
| 30 | `WeighStation-Setup` AP visible on phone/laptop | | ☐ |
| 31 | Serial monitor (115 200 baud) shows `[scale] Ready` | | ☐ |
