# Wiring Netlist

Source → destination for the controller and every peripheral. Matches the
firmware pin map (`src/config.h`), `hardware/wiring.md`, and the display
datasheet-of-record (`docs/datasheets/display-hosyond-ili9488.md`).

**MCU:** SparkFun Thing Plus ESP32-S3 (DEV-21230).

> "LED" on the display is the **panel's built-in backlight** (main header
> pin 8, silk-labeled `LED`), not a separate indicator. Tie to 3.3 V for
> always-on. The panel shows nothing without it.

---

## Assembly schedule

Two kinds of connection: **direct** single wires, and **shared nets** that reach
more than one device — each of those is joined once at a **soldered splice**
(pigtail star), with a single clean leg to every endpoint.

### Direct wires — one per row (MCU pin → device)

| # | From | To |
|---|---|---|
| 1 | GPIO 5  | PN5180 : NSS (CS) |
| 2 | GPIO 6  | PN5180 : BUSY *(active-high)* |
| 3 | GPIO 7  | PN5180 : RST *(active-low)* |
| 4 | GPIO 15 | TFT : CS |
| 5 | GPIO 16 | TFT : DC (RS) |
| 6 | GPIO 17 | TFT : RESET |
| 7 | GPIO 14 | Buzzer : + *(PWM; passive piezo, no resistor)* |

### Shared nets — one soldered splice each

Each splice joins the legs below; a single wire then lands at every endpoint.
Bridge **VCC→LED** on the display module so the backlight needs no separate leg.

| Splice | MCU leg | Device legs |
|---|---|---|
| MOSI | GPIO 35 | PN5180 MOSI · TFT SDI |
| SCK  | GPIO 36 | PN5180 SCK · TFT SCK |
| MISO | GPIO 37 | PN5180 MISO · TFT SDO |
| 3V3  | 3V3     | PN5180 3.3V · TFT VCC |
| GND  | GND     | PN5180 GND · TFT GND · Buzzer − |

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

**Totals:** 7 direct wires + 5 soldered splices (13 legs) + 1 Qwiic cable +
4–5 load-cell leads.

---

## Shared-net junctions — soldered pigtails

Five nets reach more than one device — **MOSI, SCK, MISO, 3V3, GND**. Instead of
stacking wires on a pin, join each with **one soldered splice** and heat-shrink
it; a single clean leg then lands at each device (see the splice table above).

**Technique**

- Strip ~4 mm on each leg; gather and solder into one joint (a lineman's /
  Western-Union twist adds mechanical strength before you flow solder).
- **Thread the heat-shrink onto one leg first** and slide it well back from the
  end — once the wires are joined there's no open end to feed a tube over the
  splice. Solder, let it cool, slide the tube over the joint, then heat to shrink.
- **Stagger** the five splices along the loom so the shrink bumps don't stack,
  and anchor the bundle so nothing flexes right at a joint.
- Keep the splices **near the MCU** so the shared-bus stubs stay short.

**Rails with many legs.** The 3V3 (3 legs) and especially GND (4 legs) splices
carry the most wires; if a single solder joint gets bulky, put those two on a
small **perfboard bus** instead — a row of commoned pads, one wire per pad. Same
result, neater for many wires. The three SPI splices are just three wires each.

If you use a perfboard bus, mount it on a flat interior wall near the MCU:
foam tape (VHB) or adhesive-backed nylon PCB standoffs need no reprint; M2.5
standoff bosses can be added to the enclosure SCAD for a screw-down future
revision.

## Device-end connectors & retention

Independent of the splices, the leg that lands at each module can be:

- **Soldered directly** to the module's header pin — most reliable, permanent.
- **DuPont (0.1 in) plug** — keep it if you may want to swap a module. Each
  female terminal holds one wire and each header pin takes one terminal, which is
  exactly why the shared nets are spliced upstream rather than doubled onto a pin.
  Dab hot glue on DuPont housings so they can't back out under handling.

The NAU7802 stays on its latching Qwiic cable and the load cell in its screw
terminal — both already reliable, nothing to change.

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
