# Wiring Netlist

Source → destination for the controller and every peripheral. Matches the
firmware pin map (`src/config.h`), `hardware/wiring.md`, and the display
datasheet-of-record (`docs/datasheets/display-hosyond-ili9488.md`).

**MCU:** SparkFun Thing Plus ESP32-S3 (DEV-21230).

> "LED" on the display is the **panel's built-in backlight** (main header
> pin 8, silk-labeled `LED`), not a separate indicator. Tie to 3.3 V for
> always-on. The panel shows nothing without it.

---

## Wire-by-wire assembly schedule

One physical wire per row. Build top to bottom and tick them off. `W##` =
discrete MCU jumper; `L#` = load-cell lead into the NAU7802 terminal block.

### Power — +3.3 V

| # | From | To |
|---|---|---|
| W01 | MCU 3V3 | PN5180 : 3.3V |
| W02 | MCU 3V3 | TFT : VCC |
| W03 | MCU 3V3 | TFT : LED (panel backlight) |

> W03 can instead be a short bridge from TFT VCC→LED **on the display module**,
> saving one wire back to the MCU.

### Ground — GND

| # | From | To |
|---|---|---|
| W04 | MCU GND | PN5180 : GND |
| W05 | MCU GND | TFT : GND |
| W06 | MCU GND | Buzzer : − |

### SPI bus (shared: PN5180 + TFT)

| # | From | To |
|---|---|---|
| W07 | MCU GPIO 35 (MOSI) | PN5180 : MOSI |
| W08 | MCU GPIO 35 (MOSI) | TFT : SDI (MOSI) |
| W09 | MCU GPIO 36 (SCK)  | PN5180 : SCK |
| W10 | MCU GPIO 36 (SCK)  | TFT : SCK |
| W11 | MCU GPIO 37 (MISO) | PN5180 : MISO |
| W12 | MCU GPIO 37 (MISO) | TFT : SDO (MISO) |

> Alternatively daisy-chain each signal PN5180→TFT so only one wire leaves the
> MCU pin; electrically identical.

### PN5180 control

| # | From | To |
|---|---|---|
| W13 | MCU GPIO 5 | PN5180 : NSS (CS) |
| W14 | MCU GPIO 6 | PN5180 : BUSY *(active-high)* |
| W15 | MCU GPIO 7 | PN5180 : RST *(active-low)* |

### TFT control

| # | From | To |
|---|---|---|
| W16 | MCU GPIO 15 | TFT : CS |
| W17 | MCU GPIO 16 | TFT : DC (RS) |
| W18 | MCU GPIO 17 | TFT : RESET |

### Buzzer signal

| # | From | To |
|---|---|---|
| W19 | MCU GPIO 14 | Buzzer : + *(PWM; passive piezo, no resistor)* |

### NAU7802 load-cell ADC — one Qwiic cable

Not jumpers — a single 4-conductor Qwiic cable from the Thing Plus Qwiic
connector to the NAU7802 board carries all four:

| Conductor | MCU | NAU7802 |
|---|---|---|
| SDA | GPIO 8 | SDA |
| SCL | GPIO 9 | SCL |
| +3.3 V | Qwiic 3V3 | 3.3V |
| GND | Qwiic GND | GND |

### Load cell → NAU7802 terminal block (4-wire full bridge)

| # | Load-cell wire | Function | NAU7802 terminal |
|---|---|---|---|
| L1 | Red (红)    | Excitation + | E+ |
| L2 | Black (黑)  | Excitation − | E− |
| L3 | Green (绿)  | Signal +     | A+ |
| L4 | White (白)  | Signal −     | A− |
| L5 | Yellow (黄) | Shield/drain | GND *(omit if absent)* |

**Totals:** 19 MCU jumper wires (W01–W19) + 1 Qwiic cable + 4–5 load-cell leads.

---

## Pins that carry more than one wire

Everything else is a single wire per pin. Only these are shared — plan the
fan-out (use the board's several GND / multiple 3V3 pins, or a small distribution
point, so you're not stacking many leads on one hole):

| MCU pin | Wires | Goes to |
|---|---|---|
| 3V3 | 3 | W01 PN5180, W02 TFT VCC, W03 TFT LED *(NAU7802 3V3 is separate, via Qwiic)* |
| GND | 3 | W04 PN5180, W05 TFT, W06 buzzer *(NAU7802 GND is separate, via Qwiic)* |
| GPIO 35 (MOSI) | 2 | W07 PN5180, W08 TFT |
| GPIO 36 (SCK)  | 2 | W09 PN5180, W10 TFT |
| GPIO 37 (MISO) | 2 | W11 PN5180, W12 TFT |

---

## Onboard — no external wiring

| Function | MCU pin |
|---|---|
| WS2812 status pixel | GPIO 48 (onboard) |
| BOOT / WiFi-reset button | GPIO 0 (onboard) |

## Not wired (planned / out of scope)

| Device | Interface | Status |
|---|---|---|
| microSD (on display board) | own SPI host (2nd bus) | **planned, unwired** — separate 4-pin header, not bonded to the TFT bus; GPIOs TBD on the ESP32-S3 second SPI host (backup feature) |
| Resistive touch (T_CLK/T_CS/T_DIN/T_DO/T_IRQ) | SPI slave, same header | **out of scope** — intentionally unwired |
| PN5180 IRQ | — | unused (firmware polls BUSY) |

---

## Cautions

- The **PN5180 is 3.3 V only** — do not connect it to 5 V.
- PN5180 + TFT **share MOSI/SCK/MISO**; only the CS lines differ (GPIO 5 vs 15).
  A firmware mutex (`gSpiMutex`) keeps them off the bus simultaneously.
- Load-cell orientation: force axis vertical, sealed/potted face **down** for
  positive output. Calibrate at 50–80 % of rated range via the web
  **Calibrate** page (or serial `ZERO` / `CAL <grams>`).
