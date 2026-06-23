# Bill of Materials — opt-weigh-station

Everything needed to build one weigh station, beyond the fasteners
(see `fastener-schedule.md` for screws, inserts, washers).

## Electronics

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| ESP32-S3 dev board | SparkFun Thing Plus ESP32-S3 | 1 | Native USB CDC, Qwiic on GPIO 8/9 |
| NFC reader module | PN5180-NFC (ISO 15693) | 1 | Needs both 3.3V logic + 5V RF rail |
| Load cell amplifier | NAU7802 (SparkFun Qwiic Scale) | 1 | I²C, on Qwiic bus |
| Load cell | TAL220B, 5kg, 55×12.7×12.7mm | 1 | Single-bolt mount, ~40mm hole spacing |
| TFT display | 3.5" ILI9488 SPI TFT, 480×320 (Hosyond / MSP3520-type) | 1 | SPI, shared bus with PN5180; CS GPIO 15, DC GPIO 16, RST GPIO 17 |
| Piezo buzzer | Passive, GPIO 14 | 1 | Feedback tones |
| USB-C breakout | Power input only | 1 | 40×70mm, mounted on dedicated floor bosses |
| USB-C panel-mount cable | Extension, panel-mount | 1 | Replaces original breakout-board plan |

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

## Mechanical — Hardware (Wiring)

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| Hookup wire | Thin stranded, various colors | — | Slack loop required between RFID platform and controller — a stiff harness corrupts load cell readings |
| Dupont jumpers / crimp housings | 2.54mm pitch | — | Per user's preferred prototyping style |
| Header pins | 2.54mm | — | For breakout board connections |

## Fasteners

See `fastener-schedule.md` for the complete screw/insert/washer list
(15 heat-set inserts, 15 screws, 15 washers).

## OPT NFC Tags

| Item | Spec | Qty | Notes |
|------|------|-----|-------|
| Blank ISO15693 NFC tags | NTAG / ICODE-compatible | as needed | Require NDEF formatting via NFC Tools before OPT data can be written |

## Open Items — Not Yet Specified

These appear in the design but don't have confirmed part numbers or
quantities yet:

- **NAU7802 mounting screws/board hole size** — not measured against
  the actual board; assumed M3, may be M2 (see fastener schedule notes)
- **USB-C breakout board hole size** — not measured against the actual
  board (PN5180-style 40×70mm board pocket sized, but mounting holes
  for the USB breakout itself haven't been confirmed)
- **Rubber feet** — base has recesses for them (4 corners) but no
  spec/source chosen yet
- **Spoolman host hardware** — ruled out on Synology NAS (no Docker
  support on j-series); separate dev server still to be sourced

## Tools Required (not consumed, but needed for assembly)

- Soldering iron (heat-set insert installation)
- 2.5mm and 3mm hex keys (M3/M4 SHCS)
- 4mm hex key (M5 SHCS)
- Digital calipers
- NFC Tools app (NDEF tag formatting) + NFC.cool Tools (tag testing)
