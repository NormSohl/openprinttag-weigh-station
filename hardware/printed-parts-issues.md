# 3D-Printed Parts — Known Issues / Errata

Field-found problems with the printed parts, and the fixes made. Issues 1-3
have been printed and verified as of 2026-08-16. Issues 4 and 5 are design
fixes that exist only in SCAD — render-verified, never printed — and are
**closed without a reprint as of 2026-08-17**: the current prototype was
built from an earlier revision and is working fine as-is, and another
costly print run isn't planned unless a second unit gets built. If that
happens, pick these back up and validate on hardware before trusting them.
**Issue 6 is OPEN** — a field workaround exists, but the SCAD itself still
needs to be fixed. Kept here as the errata history — add a new row and
update status the same way if a future print turns up a new issue.

Related files: [`weighstation.scad`](./weighstation.scad),
[`porch-tft-faceplate.scad`](./porch-tft-faceplate.scad).
`porch-tft-adapter.scad` (issues 2 and 3 below) is no longer in the repo
— superseded by the faceplate (issue 5) and removed rather than kept for
reference, so those two entries describe a file that no longer exists.

| # | Part | Severity | Status |
|---|------|----------|--------|
| 1 | Base — load-cell fixed-end boss | High (affects weighing accuracy) | **Fixed & verified** — reinforced boss printed, no visible deflection under load |
| 2 | Porch TFT adapter — window + pocket depth | Medium (fit/assembly) | **Fixed & verified** — printed, fits as designed. Superseded in the source by issue 5 for any future unit; the current prototype keeps this printed adapter and is working fine |
| 3 | TFT adapter top edge fouls the overhanging platform | High (blocks weighing) | **Fixed & verified** — plate_y 67→63 + plat_gap 2→5 (~3.1mm), printed and clears |
| 4 | Base — hairline crack at the bottom of the front-corner crevice (rounded body meets porch wedge) | Low (unprintable cusp, not a structural issue) | **Closed, not printed** — final fillet design (repositioned + enlarged, sliced clean of the porch face and display recess) is render-verified only. Current prototype predates this fix and is working fine as printed; no reprint planned for this unit |
| 5 | Porch TFT adapter → flush faceplate (mount redesign, not a defect) | — design change, not a fault | **Closed, not printed** — `porch-tft-faceplate.scad` replaces the adapter with a thin flush plate over an enlarged opening, held by 4 corner screws into heat-set bosses in the porch wall. Render-verified, mounting holes confirmed coaxial with the bosses. Current prototype keeps the printed adapter (issue 2) and is working fine; banked for the next unit built |
| 6 | Base — load-cell fixed-end mount, fastener access | Medium (field workaround exists; not yet modeled) | **OPEN** — the built unit needed a hand-drilled, countersunk hole through the case underside to run a long screw up into the fixed-end mount. Works well as a physical fix, but hasn't been carried back into `weighstation.scad` |

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

**Verified (2026-08-16):** printed with `lc_boss_ext` / `lc_boss_fwd` /
`lc_boss_wide` as committed in the SCAD — single solid volume, +X buttress
clears the overload-stop post. Re-checked under a full-scale test weight:
no visible boss deflection; the load cell's strain-gauge beam is the only
compliant element in the load path. Cross-ref the load-cell install guide
([`docs/datasheets/load-cell-install-guide.md`](../docs/datasheets/load-cell-install-guide.md))
— "force direction perpendicular, rigid mount" — satisfied.

**Render-verified (2026-08-16, OpenSCAD 2021.01, headless):** `openscad -D
part="base" --render` reports `Simple: yes`, `Volumes: 2` — a single
connected manifold solid, not the disconnected-sheet failure this note
originally warned about. (The "expect Volumes: 1" comment above was itself
wrong: CGAL's Nef-polyhedron count always includes the unbounded exterior as
an extra volume, confirmed by rendering a plain merged two-cube test case
alongside a genuinely disconnected one — a single real part always reports
2, not 1.) Buttress-to-post clearance computed directly from the SCAD
variables (`lc_x0=-27.5`, buttress far edge at X=1.5, overload-stop post
near edge at X=18): **16.5 mm**, matching the "~16 mm" estimate.

---

## 2. Porch TFT adapter — window size and pocket depth

**File:** `porch-tft-adapter.scad` — **removed from the repo**, superseded
by `porch-tft-faceplate.scad` (issue 5). Entry kept as historical record
of a real fit issue and its fix.

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

**Status:** **Fixed & verified** — applied to `porch-tft-adapter.scad`:
`win_x` 85→86, `win_y` 55→56; `pocket_clr` 0.4→1.4 (pocket grown 0.5 mm/
side); `plate_t` 5→15 with `pocket_d` 3.2→13.2 (keeps the 1.8 mm bezel
lip, adds 10 mm of wall to house the soldered header). The design now
**keeps the header** rather than desoldering it. Printed and verified
(2026-08-16):

- Header stays soldered: the wire notch is **removed**; wires exit the
  **open back** into the porch cavity. No back cover / cable relief — the
  open back is intentional. Confirmed on the printed part.
- A **microSD access slot** was added in the top wall (card exits the top;
  socket on the PCB back). First print had it on the wrong side and a bit
  tight — corrected: offset flipped to **+18 mm**, width **22 mm**
  (+3/side), and the display-side edge extended 1.5 mm (Z 4.7→11.2).
  Reprinted with the corrected offset/width and now clears.
- Screws pass through 15 mm of plate now — use M3×20+ (noted in the SCAD
  header). Confirmed on the printed part.
- Printed and test-fit against the board and bezel — fits as designed.

**Render-verified (2026-08-16, OpenSCAD 2021.01, headless):** `openscad
--render` reports `Simple: yes`, `Volumes: 2` (single connected solid, same
CGAL convention as the base part above). `win_x`/`win_y`/`pocket_clr`/
`plate_t`/`pocket_d`/`plate_y`/`sd_slot_offset`/`sd_slot_w` all confirmed
present in the SCAD exactly as described above.

---

## 4. Hairline crack at the bottom of the front-corner crevice

**File:** `weighstation.scad` — `base()`, SOLIDS union, ~line 288
(`fillet_r`).

**Symptom:** at both front corners (where the main shell's rounded corner
transitions into the 45-degree porch wedge), a hairline crack runs down the
outer wall from the top rim, right where the curved wall and the flat porch
wall meet. Spotted in an exterior render, not yet on a physical print.

**Root cause — genuinely different from a modeling gap.** The crescent-
shaped notch/crevice at this corner (bounded by `rrect()`'s corner arc on
one side and the porch wedge's exposed front face on the other) is the
*intended* shape of this transition — it's what a rounded corner meeting a
sharp-edged wedge looks like, and it's the look this corner is meant to
have. The problem is narrower than that: the arc is centred at
(`porch_x0+corner_r`, `base_w/2-corner_r`), so at X=`porch_x0` its tangent
direction is exactly vertical — the same direction as the porch's flat
face there. A tangent meeting closes the gap to **exactly zero width at one
point** and opens up on either side of it. No nozzle can resolve a
zero-width feature, so that one point prints as a hairline gap even though
the model is a valid, fully manifold solid there (confirmed: `Volumes: 2`,
`Simple: yes`, unchanged before and after the fix below — this was never a
hole in the geometry, just a cusp too fine to print).

**First attempt (reverted):** a full-height gusset cube filling the entire
crevice. This closed the crack, but also squared off the whole notch,
changing the corner's silhouette — not what was wanted. Reverted
(`git revert`) in favour of the fix below.

**Fix, first pass (superseded, see below):** a small vertical cylinder
(`fillet_r` = 1.5mm radius, full base height) at each front corner — not
filling the crevice, just padding the point where it pinches to zero. The
broader crescent shape stays exactly as it was.

> **First version of this fix was itself broken** — centred exactly on the
> tangent point (X=`porch_x0`), which straddles the porch wedge's own
> boundary there. The porch's top edge is a 45-degree slope: its solid
> only reaches the full `base_h` height exactly AT X=`porch_x0`, receding
> forward in Z for every mm forward in X past it. A cylinder centred ON
> that boundary therefore pokes a visible stub through the receding slope
> near the rim on the porch side, even though it's fine on the shell side
> — caught visually (a render sent for review showed it clearly) before
> ever reaching a printer. Fixed by moving the cylinder's centre to
> X=`porch_x0 + fillet_r`, keeping the entire cylinder at X >= `porch_x0`
> (tangent to the boundary, never crossing it) — entirely on the
> shell/void side, where nothing recedes with height, so it can safely run
> the full `base_h`. Confirmed by isolating just the porch wedge and the
> fillet cylinder in a standalone test file and rendering an orthographic
> side profile: the cylinder's silhouette sits entirely inside the wedge's
> own outline at every height.

**Render-verified (2026-08-16, OpenSCAD 2021.01, headless):** `openscad -D
part="base" --render` reports `Simple: yes`, `Volumes: 2` — unchanged from
both the original and the (reverted) gusset version, still one connected
solid. The hairline seam is gone in the same exterior viewpoint used to
find it, and the crescent notch's overall shape is visibly preserved (not
squared off) compared side-by-side with the gusset version.

**Fix, final (2026-08-17) — the tangent-point fillet above undersold the
real gap.** Tracing the actual cross-section showed the porch cavity's own
cut leaves only a thin, Z-sliding sliver of the wedge's rear wall standing,
so the true gap between the shell's rounded corner and the porch's back
wall is far wider than a single tangent point through most of the case's
height (derived exactly: `gap(Z) = 55.757 - Z` for Z in [13.757, 56.5]).
The fillet is now anchored where two fixed planes meet instead — the
porch's own back face (`X = porch_x0`) and the case's inside wall
(`Y = 64.5`, the porch cavity's own Y-extent boundary) — confirmed solid by
cross-section at multiple heights, radius bumped 1.5mm → 2.0mm, then
sliced against the porch's own 45-degree roof plane and the real display
cutout geometry so it can't poke through the sloped face or intrude on the
faceplate's recess (this fix also caught and closed a related poke-through
into the display pocket — see issue 5). Render-verified: `Simple: yes`,
`Volumes: 2`, unchanged.

**Closed 2026-08-17, not printed.** The current prototype was built before
any of this section's fillet work and is working fine as printed — the
crack was only ever seen in a render, never confirmed as a real defect on
this unit. No reprint is planned to validate the fillet fix; if a second
unit gets built, print it with the current SCAD and confirm the crevice is
actually gone and the fillet doesn't foul the faceplate before trusting
this closed.

---

## 5. Porch TFT adapter replaced by a flush faceplate

**Files:** `porch-tft-faceplate.scad` (new); `weighstation.scad` — `base()`,
"porch display faceplate" boss section.

**This is a design change, not a fault fix** — logged here because it
retires the part covered by issue 2 and shares the same corner-fillet
history as issue 4 above. `porch-tft-adapter.scad` mounted the display
proud, through-bolted, as a full-face panel; it has been **removed from
the repo** (2026-08-17) now that this faceplate is the intended part —
see issue 2 and the top-of-doc note for what that means for those
historical entries.

**New design:** the display sits flat against the porch's own front face,
in a large opening cut directly into `base()`. `porch-tft-faceplate.scad`
is a thin retainer plate over that opening — a bezel window, held down by
4 corner screws into heat-set-insert bosses that reach sideways to the
porch's own real side walls (not a wall behind the opening, so display
wiring keeps a clear path through). The board itself is still held by its
own 4 screws through the plate into a nut behind it, the same mechanism
the adapter used.

**Mounting-hole coaxiality confirmed** (2026-08-17): the faceplate's
corner screw holes (`boss_u`/`boss_v` in `porch-tft-faceplate.scad`) and
the porch's heat-set bosses (`disp_boss_u`/`disp_boss_v` in
`weighstation.scad`) are hand-copied between the two files, not shared
code — checked by extracting the live values from both real files via
OpenSCAD `echo()` rather than trusting the source comments: both resolve
to identical U/V offsets (23.1198mm / 61.3mm). The two files code the
local frame with X/Y transposed relative to each other, but the hole
pattern is a symmetric `±U, ±V` rectangle, so that's immaterial — only the
magnitudes matter, and they match exactly.

**Closed 2026-08-17, not printed.** Render-verified only (`Simple: yes`,
`Volumes: 2` for the base with the new boss geometry). The current
prototype keeps the printed adapter from issue 2 and is working fine; this
faceplate is banked for the next unit built, not scheduled for a reprint
of this one. If a second unit gets built: print the faceplate, confirm all
4 screws seat straight into their bosses with no binding, and confirm the
board still clears the bezel window before trusting this closed.

---

## 6. Load-cell fixed-end mount needed a field-drilled, countersunk access hole

**File:** `weighstation.scad` — load-cell fixed-end boss (same area as
issue 1's stiffening fix, but a different problem: this one is about the
fastener's access path, not the boss's stiffness).

**Symptom:** as printed, the boss didn't provide a workable way to
actually drive the mounting screw into the strain gauge's fixed end. The
designed path (per issue 1's fix and `fastener-schedule.md`: M5×30 SHCS
down through the platform and load-cell bar into a top-of-boss insert)
wasn't usable in practice on the built unit.

**Field fix (2026-08-17):** drilled through the case from underneath and
countersunk the hole, then ran a long screw up into the mount from below
instead. Works well — the load cell is properly secured — but this was a
hand-drilled modification to the physical part, not a SCAD change, so
none of it is reflected in `weighstation.scad`.

**Status: OPEN.** Not yet investigated in the model. Before designing a
fix: pin down *why* the top-down path wasn't usable (tool/screwdriver
clearance under the platform once assembled? insert depth or thread
engagement issue? something else?), then model the underside access hole
and countersink as a real feature — including whatever floor thickness
and boss geometry changes that implies — so a future print doesn't need
the same manual drill-and-countersink step. Not printed, not
render-verified — this is only a written record of the field fix so far.
