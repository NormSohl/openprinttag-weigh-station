# Bill of Materials — opt-weigh-station

Everything needed to build one weigh station, beyond the fasteners
(see `fastener-schedule.md` for screws, inserts, washers).

## Electronics

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| ESP32-S3 dev board | SparkFun Thing Plus ESP32-S3 | 1 | Native USB CDC, Qwiic on GPIO 8/9 |
| NFC reader module | PN5180-NFC (ISO 15693) | 1 | **Needs BOTH rails:** 3.3 V logic *and* 5 V on the module's RF/antenna-driver pin (VUSB). 3.3 V alone can answer SPI but produce no usable RF field |
| Load cell amplifier | NAU7802 (SparkFun Qwiic Scale) | 1 | I²C, on Qwiic bus |
| Load cell | TAL220B, 5kg, 55×12.7×12.7mm | 1 | Single-bolt mount, ~40mm hole spacing |
| TFT display | Hosyond 3.5" ILI9488 SPI TFT, 480×320, resistive touch (MSP3520-type) | 1 | Amazon (Hosyond Store, sold by HONGXINBAORUI), ~$17.99; incl. stylus. SPI, shared bus with PN5180; CS GPIO 15, DC GPIO 16, RST GPIO 17. On-board microSD **not wired** (no SPI peripheral left — see netlist.md); resistive touch (T_xx pins) intentionally unwired |
| Piezo buzzer | Passive, GPIO 14 | 1 | Feedback tones |
| USB-C panel-mount cable | Full-function extension, panel-mount | 1 | Routes the ESP32's **native USB-C** to the panel — the single port for the whole unit: power, serial console, and firmware flashing (no OTA needed). Supersedes the earlier power-only USB-C breakout board. |

## Mechanical — Printed Parts

| Part | Material | Notes |
|------|----------|-------|
| Base | PETG | 4 walls, 40% infill near load cell boss |
| Deck | PETG or PLA | Prints face-down for finish |
| Platform | PLA | Disc, Ø205mm, prints face-down |
| Spigot | PLA | Centering cone |
| Tray | PETG or PLA | Carries ESP32 + NAU7802 |

**Print envelope check:** platform Ø205mm fits the Prusa MK4S bed (210mm Y)
with 2.5mm margin per side. Base footprint 200×140×60mm.

> **Known issues / errata:** see
> [`printed-parts-issues.md`](./printed-parts-issues.md) for deferred
> fixes (load-cell boss stiffening, TFT adapter window/depth) before the
> next print revision.

## Mechanical — Hardware (Wiring)

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| Hookup wire | Thin stranded, various colors | — | Slack loop required between RFID platform and controller — a stiff harness corrupts load cell readings |
| Dupont jumpers / crimp housings | 2.54mm pitch | — | Per user's preferred prototyping style |
| Header pins | 2.54mm | — | For breakout board connections |

## Fasteners

See `fastener-schedule.md` for the complete screw/insert/washer list
(15 heat-set inserts, 23 screws, 23 washers, 8 nuts — incl. 4× M3×25 +
nyloc for the display adapter and 4× M2.5×8 + nut for the display board).

## OPT NFC Tags

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| Blank ISO15693 NFC tags | NTAG / ICODE-compatible | as needed | Require NDEF formatting via NFC Tools before OPT data can be written |

## Open Items — Not Yet Specified

These appear in the design but don't have confirmed part numbers or
quantities yet:

- **NAU7802 mounting screws/board hole size** — not measured against
  the actual board; assumed M3, may be M2 (see fastener schedule notes)
- **Rubber feet** — base has recesses for them (4 corners) but no
  spec/source chosen yet
- **External server hardware** — no longer needed. The station is
  self-contained (local storage + built-in web app), so there is no
  Spoolman/Prometheus host to source. (Was previously an open item.)
  <!-- (Display microSD interface pinout — RESOLVED: the SD lines are on a
       separate header, not bonded to the TFT bus, now wired to a dedicated
       2nd SPI host at GPIO 10/18/21/42. See netlist.md and
       display-hosyond-ili9488.md.) -->

## Tools Required (not consumed, but needed for assembly)

- Soldering iron (heat-set insert installation)
- 2.5mm and 3mm hex keys (M3/M4 SHCS)
- 4mm hex key (M5 SHCS)
- Digital calipers
- NFC Tools app (NDEF tag formatting) + NFC.cool Tools (tag testing)
