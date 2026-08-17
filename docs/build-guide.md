# Build Guide

Everything needed to build your own Weigh Station, start to finish: what it
is, what to buy, what to print, how to wire it, how to flash it, and how to
confirm it actually works before you trust it with your filament.

This guide is the front door — it sequences and links out to the detailed
reference docs rather than repeating them, so those stay the single source
of truth as the design evolves. Read it top to bottom on your first build;
come back to individual sections on a repeat build.

## Contents

1. [What you're building](#1-what-youre-building)
2. [What you'll need](#2-what-youll-need)
3. [Parts & sourcing](#3-parts--sourcing)
4. [Printing the parts](#4-printing-the-parts)
5. [Wiring](#5-wiring)
6. [Flashing the firmware](#6-flashing-the-firmware)
7. [Testing your build](#7-testing-your-build)
8. [Operating it](#8-operating-it)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. What you're building

A **self-contained** NFC + load-cell filament scale: place a spool, it reads
the spool's [OpenPrintTag](https://github.com/prusa3d/OpenPrintTag) NFC tag,
weighs it, and records the remaining filament — all on the device itself,
with no server, no cloud account, and no app to install. A built-in web
page (open from any phone or laptop on the same network) handles inventory,
onboarding new spools, low-stock reordering, backups, and calibration.

Why this exists, in case you're deciding whether to build one rather than
buy a commercial equivalent:

- **It speaks the open OpenPrintTag standard**, not a proprietary tag
  format — a spool tagged here reads correctly on a Prusa printer or any
  other OPT-aware reader, and a genuine vendor-tagged spool (Prusament,
  eSun, etc.) is recognized and adopted automatically the first time it's
  weighed, no manual entry required.
- **Nothing leaves the device.** Inventory, usage history, and popularity
  analytics all live in on-board flash. There's no account to create, no
  internet dependency for day-to-day use, and no third-party service that
  can shut down or change its pricing under you.
- **It's built for a shared shop, not a single desk.** Multiple people use
  it over months without administration: a shelf audit workflow reconciles
  "what's on the shelf" against the log without anyone hand-editing
  records, and a reorder page tells whoever's ordering supplies what's
  actually running low, weighted by real consumption, not by "we think
  we're out."

It is **not** a general-purpose postal scale, and it's tuned for spools in
roughly the 0–5 kg range on a fixed platform — see the Bill of Materials
for the exact load cell rating.

This is a from-scratch electromechanical build: expect a few hours of
printing (several parts, some multi-hour), an evening of soldering and
wiring, and an hour or so of flashing and bring-up. None of the steps are
hard on their own; there are just several of them.

## 2. What you'll need

**Skills:** comfortable soldering small-gauge wire, running a slicer and a
3D printer, and following a PlatformIO/Arduino-style flashing workflow. No
firmware-writing skill is required to *build* one — the firmware is
already written; you're flashing it as-is.

**Tools** (from [`hardware/bill-of-materials.md`](../hardware/bill-of-materials.md)):

- Soldering iron — for heat-set insert installation *and* the wiring splices
- 2.5mm and 3mm hex keys (M3/M4 SHCS), 4mm hex key (M5 SHCS)
- Digital calipers
- A 3D printer with a bed at least 210×210mm (the platform disc is Ø205mm)
- A phone or laptop on the same WiFi as the finished station, for setup
- NFC Tools app (NDEF tag formatting) + NFC.cool Tools (tag testing) — for
  formatting/testing blank NFC tags, separate from the station's own
  onboarding flow
- A known-mass reference weight (anything from a calibrated gym plate to a
  bag of sugar you've weighed elsewhere) — needed once, to calibrate the
  scale in step 7

## 3. Parts & sourcing

Full list with specs and notes:
[`hardware/bill-of-materials.md`](../hardware/bill-of-materials.md). Fastener
sizes and quantities (heat-set inserts, screws, washers, nuts):
[`hardware/fastener-schedule.md`](../hardware/fastener-schedule.md).

The short version — five things worth reading the notes on *before* you
order, because getting them wrong is either expensive or hard to diagnose
later:

- **PN5180 NFC reader:** needs both a 3.3V logic supply *and* a separate
  5V feed to its RF/antenna-driver pin. Cheap breakout listings sometimes
  only call out the 3.3V logic rail — check yours has a 5V pin broken out,
  or the reader will look alive over SPI but never actually read a tag.
- **TFT display:** a 3.5" ILI9488 SPI panel, 480×320 (Hosyond/MSP3520-type
  in the BOM). Get the **SPI** version, not an 8-bit parallel one — the
  firmware's display driver is configured for SPI.
- **Load cell:** TAL220B, 5kg rating, ~40mm bolt spacing. A different
  capacity works fine (swap the calibration factor in step 7); a different
  bolt pattern means adjusting the mounting boss in the SCAD.
- **USB-C:** the ESP32-S3 board's *native* USB-C is the only port this
  design uses — for power, serial console, and flashing. No separate power
  input or USB bridge chip to source.
- **NFC tags:** blank ISO15693 (ICODE SLIX2-compatible) tags. They need
  NDEF formatting before the station can write OPT data to them — the
  station itself formats blank tags automatically the first time you place
  one (see step 7), so you don't need to pre-format anything, but the tags
  themselves must be ISO15693, not NTAG/MIFARE.

## 4. Printing the parts

Printed parts, by module in [`hardware/weighstation.scad`](../hardware/weighstation.scad)
plus the standalone [`hardware/porch-tft-faceplate.scad`](../hardware/porch-tft-faceplate.scad):

| Part | Material | Orientation | Notes |
|---|---|---|---|
| Base | PETG | as modeled | 4 walls, 40% infill near the load-cell boss — this is the structural part carrying weighing load |
| Deck | PETG or PLA | face-down | drop-in top plate, no supports |
| Platform | PLA | face-down | Ø205mm disc — confirm it fits your bed with margin |
| Spigot | PLA | as modeled | centering cone |
| Tray | PETG or PLA | as modeled | carries the ESP32 + NAU7802 |
| Porch display faceplate | PETG | face-down | thin retainer plate, no supports |

Rendering each part: `weighstation.scad` picks which module to export off a
top-level `part` variable (`assembly` by default — the full preview).
Override it from the command line, **keeping the inner double-quotes
intact** — dropping them passes an unquoted bareword that OpenSCAD's `-D`
parser doesn't accept as a string, which hangs rather than erroring
cleanly:

```
# macOS/Linux/Git-Bash — wrap the whole thing in single quotes
openscad -D 'part="base"' --render -o base.stl hardware/weighstation.scad

# Windows cmd.exe — escape the inner quotes instead
openscad -D "part=\"base\"" --render -o base.stl hardware\weighstation.scad
```

Valid values: `base`, `deck`, `platform`, `spigot`, `tray`. The standalone
`porch-tft-faceplate.scad` has no `part` switch — it only ever exports the
one plate, so render it directly with no `-D` needed.

**Before you print:** check
[`hardware/printed-parts-issues.md`](../hardware/printed-parts-issues.md)
for known errata — some entries there are closed *without* being
print-validated on hardware (the current prototype predates them and
wasn't reprinted to confirm), so if you hit a fit problem matching one of
those descriptions, that file has the context and the exact SCAD variables
involved.

**Heat-set inserts:** installed from above with a soldering iron, after
printing — see the fastener schedule for sizes (mostly M3, one M4, one
M5) and locations. Let the part cool fully before handling; a warm insert
can pull back out.

**Print envelope:** the platform (Ø205mm) is the tightest fit — it needs a
bed with at least 210mm of clearance in one direction. The base footprint
is 200×140×60mm.

## 5. Wiring

Full pin-by-pin netlist, wiring diagram, and every gotcha that's bitten a
prior build: [`hardware/netlist.md`](../hardware/netlist.md).

This guide assumes **point-to-point wiring** — solder (or crimp) each
connection directly, no custom PCB. If you want to get fancier (a
perfboard bus for the shared SPI/power/ground splices, or a breakout
board), the netlist's "Rails with many legs" note covers that option; it's
not required.

Read the netlist's **Cautions** section before you start soldering — three
mistakes there are easy to make and each one produces a confusing symptom
rather than an obvious failure:

- **PN5180 needs both 3.3V (logic) and 5V (RF driver) wired.** Skipping
  the 5V leg makes the reader answer SPI commands but never actually see a
  tag — it looks alive, it just can't read anything.
- **Never wire the display's SDO pin to the shared MISO line.** The
  ILI9488 panel's SDO doesn't tri-state, so it will permanently drive
  MISO low and the NFC reader will appear completely dead while the
  display works fine. Leave that pin unconnected.
- **PN5180 and TFT share MOSI and SCK, never MISO** — this is handled in
  firmware (`src/spi_bus.cpp`), not at the bench, but it's why this looks
  like an ordinary shared SPI bus and isn't quite one.

The **shared nets** (MOSI, SCK, MISO, 3V3, GND) each get one soldered
splice near the controller, not multiple wires stacked on one pin — see
the netlist's splice table for exactly which legs join at each one, and
its soldering technique notes (thread the heat-shrink on *before* you
solder, stagger the splices, keep them near the MCU).

Leave a **slack loop** in the wiring between the load-cell platform and the
controller — a taut harness transmits its own tension into the load cell
and corrupts every reading.

## 6. Flashing the firmware

Full setup/build/flash walkthrough, including first-flash download-mode
instructions and what a healthy boot log looks like:
[`DEVELOPMENT.md`](../DEVELOPMENT.md).

Short version once the tools are installed:

1. `git clone` this repo, open the folder in VS Code with the PlatformIO
   extension.
2. Plug the board in over USB-C.
3. **First flash only:** hold **BOOT**, tap **RESET**, release **BOOT**,
   *then* upload — the S3's native USB won't present a bootloader
   otherwise. Every flash after the first one auto-resets on its own.
4. Click **Upload** (or `pio run -t upload`).

There's no separate filesystem image to upload — config tables seed
themselves on first boot and the web UI is built into the firmware image.

## 7. Testing your build

Run through this once, in order, before putting the station into service.
Each step below depends on the previous one succeeding.

1. **Power on, watch the serial monitor** (`pio device monitor -b 115200`).
   You should see the store, config, and WiFi-manager come up as
   independent lines — a missing line points at exactly which subsystem
   didn't start (see the "healthy boot" example in `DEVELOPMENT.md`). The
   TFT should light up and show a WiFi-setup screen.
2. **Join it to WiFi.** On first boot it opens a `WeighStation-Setup`
   access point — connect to it from your phone and enter your lab's
   WiFi. It then reboots and joins as `weighstation.local`. If it can't
   reach your network within the setup window, it falls back to its own
   `WeighStation` access point instead — the idle screen tells you which
   state it's in.
3. **Open the web app** at the address shown on the idle screen (or scan
   the QR code it displays).
4. **Calibrate the scale**, Calibrate page: *Zero* with nothing on the
   platform, then *Set calibration* with your known-mass reference weight,
   entering its actual grams. The "not calibrated" banner on the idle
   screen should clear.
5. **Format and onboard a blank NFC tag.** Place a blank tag on the
   reader: the display should show a countdown ("New tag — remove to
   cancel"); leave it in place and it formats the tag and creates a stub
   spool record automatically. Fill in the material details on the
   **Onboard** page it points you to.
6. **Weigh it.** Remove and replace the spool — the display should show
   its spool number, material, and remaining weight, and the reading
   should show up on the web app's Inventory page.
7. **Confirm the round trip.** Edit that spool's detail on the web app
   (change the tare, say), then place the spool back on the scale — the
   physical tag should get rewritten with the new data on the next
   placement. This proves tag reads, tag writes, and the store are all
   actually talking to each other, not just individually alive.

If every step above passes, the station is ready for day-to-day use.

## 8. Operating it

Once it's built, calibrated, and confirmed working, day-to-day use — for
both regular members and whoever administers it — is covered in
[`docs/user-manual.md`](./user-manual.md). That's the doc to hand to
someone who didn't build the thing and just needs to use it.

## 9. Troubleshooting

Symptoms you might hit during bring-up, and where the root cause usually
is:

| Symptom | Likely cause |
|---|---|
| NFC reader never detects any tag, but SPI/boot looks fine | PN5180's 5V (RF driver) line isn't connected — 3.3V alone runs the logic but not the antenna field |
| Display works, but the NFC reader seems dead | Display's SDO pin is wired into the shared MISO line — disconnect it, the panel doesn't need it |
| Serial monitor is blank after a successful upload | ESP32-S3 native-USB CDC quirk — add `-DARDUINO_USB_CDC_ON_BOOT=1` to `build_flags` in `platformio.ini` |
| Upload fails / board not found | Needs a manual entry into download mode: hold **BOOT**, tap **RESET**, release **BOOT**, then upload |
| Scale reading is noisy or drifts under a placed spool | Check for a taut wiring harness between the platform and the controller — it needs slack |
| Web app unreachable after WiFi setup | Check the idle screen — it shows whether the station joined your network or fell back to its own `WeighStation` access point |
| A printed part doesn't fit as expected | Check `hardware/printed-parts-issues.md` first — several fit issues are already documented with the exact SCAD variables involved |

For anything not covered here, the deep-dive design notes and hardware
gotchas are in [`CLAUDE.md`](../CLAUDE.md) at the repo root, and issues can
be filed against this repository.
