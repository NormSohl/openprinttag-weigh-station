# Display — Hosyond 3.5" ILI9488 SPI TFT (pinout)

No manufacturer datasheet could be found for this module. This pinout is
transcribed directly from the board silkscreen — the back-of-board photo
below **is** our datasheet of record.

**Board marking:** `3.5'' TFT SPI 480X320 V1.0` (CE / RoHS)
**Source:** Hosyond 3.5" ILI9488 480×320 SPI touch shield — Amazon
(Hosyond Store, sold by HONGXINBAORUI), ~$17.99. Ships with a stylus.
**Controller:** ILI9488 (480×320), SPI. Resistive touch (XPT2046-style).

**Photo:** [`display-hosyond-ili9488-back.jpeg`](./display-hosyond-ili9488-back.jpeg)

**Mechanical (measured):** PCB 98 × 56.34 mm. Four corner mounting holes,
**Ø≈2.75 mm**, pattern **92 mm × 50 mm** centre-to-centre (~3 mm inset
from each edge). Takes M2.5 (an M3 will not pass). The printed adapter
(`hardware/porch-tft-adapter.scad`) bolts the board via these with M2.5
front through-bolts into nuts behind — see `brd_screw_*`.

---

## Main header (right edge, top → bottom)

| Pin | Signal | Function |
|-----|--------|----------|
| 1  | VCC       | 3.3 V supply |
| 2  | GND       | Ground |
| 3  | CS        | Display chip select |
| 4  | RESET     | Display reset |
| 5  | DC/RS     | Data / command select |
| 6  | SDI(MOSI) | Display SPI data in (MOSI) |
| 7  | SCK       | Display SPI clock |
| 8  | LED       | Backlight enable |
| 9  | SDO(MISO) | Display SPI data out (MISO) |
| 10 | T_CLK     | Touch SPI clock |
| 11 | T_CS      | Touch chip select |
| 12 | T_DIN     | Touch SPI data in (MOSI) |
| 13 | T_DO      | Touch SPI data out (MISO) |
| 14 | T_IRQ     | Touch pen-interrupt |

Pins 10–14 are silk-grouped as **TOUCH**. Touch is a separate SPI slave
(its own T_CS / T_CLK / T_DIN / T_DO) — not used in this project.

## SD card — separate 4-pin header (left side of board)

| Pin | Signal  |
|-----|---------|
| 1 | SD_CS   |
| 2 | SD_MOSI |
| 3 | SD_MISO |
| 4 | SD_SCK  |

## ⚠️ Important: SD is NOT bonded to the display SPI bus

On this board the microSD lines are broken out on their **own header**
and are **not** connected on-PCB to the display's `SDI(MOSI)/SCK/
SDO(MISO)`. Nothing is shared internally. The wiring topology is a design
choice:

- **Share the bus** — externally jumper SD_SCK→SCK, SD_MOSI→SDI(MOSI),
  SD_MISO→SDO(MISO); SD_CS to a free GPIO. SD then joins the PN5180 +
  TFT on the shared bus (and must be arbitrated by `gSpiMutex`).
- **Dedicated bus (chosen)** — wire the four SD pins to the ESP32-S3's second
  SPI host (FSPI/HSPI). SD I/O then runs independently of the NFC/TFT bus: no
  mutex contention, no interference with display refresh or PN5180 polling.
  Costs 4 GPIOs.

**Assigned pins** (`src/config.h`, second SPI host):

| Display SD header | MCU GPIO |
|---|---|
| SD_SCK  | ~~10~~ |
| SD_MOSI | ~~18~~ |
| SD_MISO | ~~21~~ |
| SD_CS   | ~~42~~ |

> **Not wired — the SD slot is unused.** These assignments are kept only as a
> record of what was planned. The ESP32-S3 has two general-purpose SPI
> peripherals and both are taken (PN5180 on SPI2, TFT on SPI3), so the card had
> no host. Backup goes through the web app instead.

All four are free broken-out header pins (GPIO 42 is SparkFun's silk-labeled
"FREEBIE" spare). The Thing Plus's **own** onboard microSD slot is on the SDIO
bus (GPIO 33/34/38/39/40/47, card-detect 48) — those are **not** broken out.
This project uses the *display's* SD for front-panel access, so the onboard slot
is left **empty**. (Note: GPIO 33–37 are consumed by the module and not on the
header at all — the board's SPI is 11/12/13.)

See [`docs/design/sd-local-ecosystem.md`](../design/sd-local-ecosystem.md)
for how this feeds the Spoolman-free redesign, and
[`hardware/netlist.md`](../../hardware/netlist.md) for current pin
assignments.
