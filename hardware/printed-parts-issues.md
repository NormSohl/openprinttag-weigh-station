# 3D-Printed Parts — Known Issues / Errata

Field-found problems with the printed parts, and the changes we intend to
make. **These are deferred** — recorded here so they aren't lost, not
scheduled. Update status when a fix lands in the SCAD.

Related files: [`weighstation.scad`](./weighstation.scad),
[`porch-tft-adapter.scad`](./porch-tft-adapter.scad).

| # | Part | Severity | Status |
|---|------|----------|--------|
| 1 | Base — load-cell fixed-end boss | High (affects weighing accuracy) | **Fixed** — SCAD reinforced (verify + tune) |
| 2 | Porch TFT adapter — window + pocket depth | Medium (fit/assembly) | **Fixed** — SCAD updated |
| 3 | TFT adapter top edge fouls the overhanging platform | High (blocks weighing) | **Fixed** — plate_y 67→63 + plat_gap 2→5 (~3.1mm) |

---

## 1. Load-cell fixed-end boss (pillar) deflects under load

**File:** `weighstation.scad` — load-cell fixed-end boss, ~line 408
(`cube([18, lc_w + 8, lc_boss_h])`, i.e. ~18 × 20.7 × 46 mm tall).

**Symptom:** under scale load the printed boss/pillar that anchors the
load cell's fixed end **flexes**, so part of the deflection is taken up by
the mount instead of by the strain-gauge beam. The load cell must be the
*only* compliant element in the load path; a springy mount corrupts the
reading (non-linear, hysteresis, drift).

**Interim workaround:** the pillar cross-section was increased on the
built unit to stiffen it.

**Fix applied to SCAD:** the boss's fore-aft (X) footprint — the direction
it bends — is widened on both sides: a full-height extension rearward
(`lc_boss_ext`, −X) and wider ±Y (`lc_boss_wide`), plus a forward
(`lc_boss_fwd`, +X) buttress toward the free end that stops `lc_beam_gap`
(5 mm) below the bar so the beam still deflects freely. Fore-aft base
grows from 18 mm to ~43 mm. Only the strain-gauge beam should move.

> The first attempt used a `hull()` of thin boxes as a rear buttress —
> that rendered as a degenerate zero-thickness sheet ("polygon with no
> sides", Volumes: 2). Replaced with the solid +X buttress above.

**Follow-ups:**
- Tune `lc_boss_ext` / `lc_boss_fwd` / `lc_boss_wide` to match the
  physical fix; **verify in a render** — expect **Volumes: 1** and the +X
  buttress clearing the overload-stop post (~16 mm gap).
- Re-check under a full-scale test weight: no visible boss deflection.
- Cross-ref the load-cell install guide
  ([`docs/datasheets/load-cell-install-guide.md`](../docs/datasheets/load-cell-install-guide.md))
  — "force direction perpendicular, rigid mount."

---

## 2. Porch TFT adapter — window size and pocket depth

**File:** `porch-tft-adapter.scad`.

**Change A — enlarge 0.5 mm PER SIDE, opening + surrounding shelf
equally.** The 0.5 mm is per side (i.e. +1.0 mm on each overall
dimension), applied equally to the bezel window opening **and** the shelf
/ pocket recess around it — so the lip width is unchanged, the whole
recessed area just grows. In the SCAD: `win_x` 85→86, `win_y` 55→56, and
grow the `pocket_x`/`pocket_y` recess by the same 0.5 mm/side.

**Change B — +10 mm to the WHOLE object (taller walls), to seat the
header.** The 10 mm applies to the entire part, not just the pocket floor:
plate thickness / perimeter walls grow by 10 mm (`plate_t` 5→15) so the
adapter becomes a deeper tray whose raised walls house the board's pin
header. This **eliminates the slot cut** for the header pins.

**Status:** **Fixed** — applied to `porch-tft-adapter.scad`:
`win_x` 85→86, `win_y` 55→56; `pocket_clr` 0.4→1.4 (pocket grown 0.5 mm/
side); `plate_t` 5→15 with `pocket_d` 3.2→13.2 (keeps the 1.8 mm bezel
lip, adds 10 mm of wall to house the soldered header). The design now
**keeps the header** rather than desoldering it. Follow-ups when
convenient:

- Header stays soldered: the wire notch is **removed**; wires exit the
  **open back** into the porch cavity. No back cover / cable relief — the
  open back is intentional.
- A **microSD access slot** was added in the top wall (card exits the top;
  socket on the PCB back), offset −18 mm (socket mouth 67 mm ±1 from the
  board's right edge). Verify the sign prints on the correct side.
- Screws pass through 15 mm of plate now — use M3×20+ (noted in the SCAD
  header).
- Not rendered here (OpenSCAD unavailable in this env) — **verify in
  OpenSCAD / a slicer before printing.**
