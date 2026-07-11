# 3D-Printed Parts — Known Issues / Errata

Field-found problems with the printed parts, and the changes we intend to
make. **These are deferred** — recorded here so they aren't lost, not
scheduled. Update status when a fix lands in the SCAD.

Related files: [`weighstation.scad`](./weighstation.scad),
[`porch-tft-adapter.scad`](./porch-tft-adapter.scad).

| # | Part | Severity | Status |
|---|------|----------|--------|
| 1 | Base — load-cell fixed-end boss | High (affects weighing accuracy) | Open, deferred; workaround applied to built unit |
| 2 | Porch TFT adapter — window + pocket depth | Medium (fit/assembly) | **Fixed** — SCAD updated |

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
built unit to stiffen it. Physical part only — **the SCAD is not yet
updated.**

**Planned change (deferred):**
- Increase the boss cross-section (the `18` X-depth and/or `lc_w + 8`
  width) and/or add gusset ribs so the fixed end is effectively rigid at
  max rated load.
- Re-verify: no visible deflection of the boss under a full-scale test
  weight; only the load-cell beam should move.
- Cross-ref the load-cell install guide
  ([`docs/datasheets/load-cell-install-guide.md`](../docs/datasheets/load-cell-install-guide.md))
  — "force direction perpendicular, rigid mount."

**Why deferred:** the built unit works with the enlarged pillar; fold the
fix into the SCAD on the next revision so we don't re-cut it by hand.

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

- The header stays soldered, so the `wire_notch` / hand-solder assumption
  is superseded; the notch now just serves as a back wire exit. Revisit
  whether to keep it or add a proper back cover / cable relief.
- Screws pass through 15 mm of plate now — use M3×20+ (noted in the SCAD
  header).
- Not rendered here (OpenSCAD unavailable in this env) — **verify in
  OpenSCAD / a slicer before printing.**
