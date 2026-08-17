# Fastener Schedule — opt-weigh-station

Derived directly from `weighstation.scad` and `porch-tft-faceplate.scad`.
All screws are metric ISO 4762
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
| M3 (Ruthex RX-M3x5.7) | Porch display faceplate corner bosses | 4 |
| M4 (Ruthex RX-M4x6.0) | Platform center (underside) | 1 |
| M5 (Ruthex RX-M5x7.0) | Load cell fixed-end boss | 1 |

**Total: 19 inserts** — 17× M3, 1× M4, 1× M5

## Screws

| Size | Location | Qty |
|------|----------|-----|
| M3×10 SHCS | Tray board → tray bosses | 4 |
| M3×10 SHCS | USB-C board → floor bosses | 2 |
| M3×10 SHCS | NAU7802 board → tray standoffs | 2 |
| M2.5×8 SHCS | Display board → faceplate front (+ nut) | 4 |
| M3×12 SHCS | Deck corners → column inserts | 4 |
| M3×10 SHCS | Faceplate corners → porch heat-set bosses | 4 |
| M3×50 SHCS | Overload stop adjustment screw | 1 |
| M4×25 SHCS | Spigot → platform center | 1 |
| M5×30 SHCS | Load cell fixed end → base boss | 1 |

**Total: 23 screws** — 4× M2.5×8, 12× M3×10, 4× M3×12, 1× M3×50, 1× M4×25, 1× M5×30

## Nuts

| Size | Location | Qty |
|------|----------|-----|
| M2.5 nyloc (or hex) | Display board through-bolts — behind the board, faceplate's open back | 4 |

**Total: 4 nuts** — the faceplate's own corner screws thread straight into
heat-set bosses in the porch wall and need no nut; only the board-to-plate
screws do, reached through the plate's open back before final mounting.

## Washers

| Size | Qty | Notes |
|------|-----|-------|
| M2.5 flat (DIN 125-A) | 4 | Under the display-board nuts (spread load on the PCB) |
| M3 flat (DIN 125-A) | 17 | One per M3 screw (incl. 4 faceplate) |
| M4 flat (DIN 125-A) | 1 | Spigot bolt |
| M5 flat (DIN 125-A) | 1 | Load cell fixed-end bolt |

## Notes

- **Overload stop screw (M3×50):** travels from below the base underside
  (~45mm) before reaching the insert mouth — a shorter screw will not
  engage the insert at all. `plat_gap` was raised 2→5 (platform/adapter
  clearance), so `lc_boss_h` grew 46.3→49.3mm and the post is ~3mm taller;
  M3×50 still engages (~5mm) but confirm on the printed part — step up to
  M3×55 if engagement is marginal.
- **Load cell screw (M5×30):** stack is platform (8mm) + load cell bar
  (12.7mm) = 20.7mm of clearance before the insert. M5×30 gives ~4–9mm of
  thread engagement depending on insert seating depth.
- **NAU7802 and USB-C breakout boards:** hole sizes have not been measured
  against the actual boards. If either board uses M2 holes instead of M3,
  swap the corresponding screw line for M2 self-tap and drop the matching
  insert (the standoff/boss is sized generously enough to work either way).
- All M3 boss/standoff fastenings (tray, deck, USB, NAU, faceplate) use the
  same Ø8 boss with a 1.7mm wall around the M3 insert — adequate in PETG.
- **Display board through-bolts (M2.5×8 + nut):** cap head sits proud on
  the faceplate's front bezel; the shaft passes through the thin front
  panel and the board's corner hole (92×50mm pattern) into an M2.5 nut
  behind, cinching the board against the panel back. Nuts + washers go on
  from the faceplate's open back before it's mounted to the porch. M2.5×8
  gives ~2–3mm past the nut; drop to M2.5×6 if the nut sits proud. No
  counterbore — the cap is intentionally proud (front service).
- **Faceplate corner screws (M3×10, no nut):** the faceplate (3mm) sits
  directly over an opening cut into the porch's own front face and picks
  up 4 heat-set-insert bosses that project sideways from the porch's real
  side wall — no wiring-passage reach-through, no nut. Head sits proud
  (no counterbore on a 3mm plate). This replaced the older adapter's
  M3×25 + nyloc-through-the-porch-cavity pattern; see
  `printed-parts-issues.md` issue 5 for why.
