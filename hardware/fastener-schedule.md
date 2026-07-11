# Fastener Schedule — opt-weigh-station

Derived directly from `weigh-station.scad`. All screws are metric ISO 4762
socket head cap screws (SHCS), stainless A2. All heat-set inserts are
Ruthex press-fit, installed from above. Washers (DIN 125-A, stainless)
go under every screw head bearing on plastic.

## Heat-Set Inserts

| Size | Location | Qty |
|------|----------|-----|
| M3 (Ruthex RX-M3x5.7) | Tray mount bosses | 4 |
| M3 (Ruthex RX-M3x5.7) | Deck corner columns | 4 |
| M3 (Ruthex RX-M3x5.7) | Overload stop post | 1 |
| M3 (Ruthex RX-M3x5.7) | USB-C breakout floor bosses | 2 |
| M3 (Ruthex RX-M3x5.7) | NAU7802 standoffs | 2 |
| M4 (Ruthex RX-M4x6.0) | Platform center (underside) | 1 |
| M5 (Ruthex RX-M5x7.0) | Load cell fixed-end boss | 1 |

**Total: 15 inserts** — 13× M3, 1× M4, 1× M5

## Screws

| Size | Location | Qty |
|------|----------|-----|
| M3×10 SHCS | Tray board → tray bosses | 4 |
| M3×10 SHCS | USB-C board → floor bosses | 2 |
| M3×10 SHCS | NAU7802 board → tray standoffs | 2 |
| M3×12 SHCS | Deck corners → column inserts | 4 |
| M3×25 SHCS | Display adapter plate → porch face (+ nut) | 4 |
| M3×50 SHCS | Overload stop adjustment screw | 1 |
| M4×25 SHCS | Spigot → platform center | 1 |
| M5×30 SHCS | Load cell fixed end → base boss | 1 |

**Total: 19 screws** — 8× M3×10, 4× M3×12, 4× M3×25, 1× M3×50, 1× M4×25, 1× M5×30

## Nuts

| Size | Location | Qty |
|------|----------|-----|
| M3 nyloc (DIN 985) | Display adapter screws — inside the porch cavity | 4 |

**Total: 4 nuts** — reached via the porch↔body wiring passage.

## Washers

| Size | Qty | Notes |
|------|-----|-------|
| M3 flat (DIN 125-A) | 17 | One per M3 screw (incl. 4 adapter) |
| M4 flat (DIN 125-A) | 1 | Spigot bolt |
| M5 flat (DIN 125-A) | 1 | Load cell fixed-end bolt |

## Notes

- **Overload stop screw (M3×50):** travels from below the base underside
  (~42mm) before reaching the insert mouth — a shorter screw will not
  engage the insert at all. Confirmed against current `lc_boss_h` (46.3mm)
  and post geometry.
- **Load cell screw (M5×30):** stack is platform (8mm) + load cell bar
  (12.7mm) = 20.7mm of clearance before the insert. M5×30 gives ~4–9mm of
  thread engagement depending on insert seating depth.
- **NAU7802 and USB-C breakout boards:** hole sizes have not been measured
  against the actual boards. If either board uses M2 holes instead of M3,
  swap the corresponding screw line for M2 self-tap and drop the matching
  insert (the standoff/boss is sized generously enough to work either way).
- All M3 boss/standoff fastenings (tray, deck, USB, NAU) use the same
  Ø8 boss with a 1.7mm wall around the M3 insert — adequate in PETG.
- **Display adapter screws (M3×25 + nyloc nut):** pass through the 15mm
  adapter plate + ~3mm porch slope wall (~18mm), leaving thread for the
  nyloc nut inside the porch cavity. Plain through-bores (no countersink);
  pan/button head + washer bears on the front bezel face. Nuts are reached
  through the porch↔body wiring passage. The porch wall is too thin (3mm)
  for a heat-set insert, hence nuts. Confirm length once the plate is
  printed — a longer M3×30 is fine if the nut sits deep.
