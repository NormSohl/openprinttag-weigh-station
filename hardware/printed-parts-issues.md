# 3D-Printed Parts — Known Issues / Errata

Field-found problems with the printed parts, and the changes we intend to
make. **These are deferred** — recorded here so they aren't lost, not
scheduled. Update status when a fix lands in the SCAD.

Related files: [`weighstation.scad`](./weighstation.scad),
[`porch-tft-adapter.scad`](./porch-tft-adapter.scad).

| # | Part | Severity | Status |
|---|------|----------|--------|
| 1 | Base — load-cell fixed-end boss | High (affects weighing accuracy) | Open, deferred; workaround applied to built unit |
| 2 | Porch TFT adapter — window + pocket depth | Medium (fit/assembly) | Open, deferred |

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

**Change A — window ~0.5 mm larger.** The display opening is a touch
tight; enlarge it by ~0.5 mm for clearance. (Current `win_x` = 85,
`win_y` = 55.) *To confirm when implementing: 0.5 mm total or per side.*

**Change B — pocket ~10 mm deeper to clear the header pins.** The board's
pin header currently forces cutting a slot for the pins. Increasing the
pocket depth by ~10 mm lets the header seat fully inside the adapter,
**eliminating the slot cut**.

> Design note: the current file assumes the header is **desoldered** and
> wires hand-soldered (hence the `wire_notch`). Change B shifts toward
> **keeping the header** and giving it depth instead. Revisit the
> desolder-vs-keep-header assumption and the wire-exit approach together
> when implementing — deepening the pocket may make the wire notch
> unnecessary.

**Status:** Open, deferred.
