// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// =============================================================
// porch-tft-faceplate.scad — thin retainer plate for the porch's TFT
// window, replacing an earlier through-bolted adapter design (removed
// from the repo -- note this design itself is still render-verified
// only, not yet printed or fitted to real hardware; see
// printed-parts-issues.md issue 5).
// =============================================================
// The display sits FLAT against the porch's own front face, directly in
// the large opening cut in weighstation.scad's base() -- it is no longer
// housed in a deep pocket machined into this plate. This plate exists
// only to make mounting easy: a thin frame with a bezel window, held
// down over the board by 4 corner screws into the porch's own
// heat-set-insert bosses (see weighstation.scad, "porch display
// faceplate" comment in base()'s interior-bosses section). The board
// itself is still held by its own 4 screws through the plate into a nut
// behind it, the same mechanism the old adapter used.
//
// Dimensions here MUST match weighstation.scad's disp_* variables and
// the ILI9488 board geometry -- kept in sync by hand, not shared code,
// same as the old adapter's screw pattern always was.
//
// Print: PETG, 3-4 walls, 15-20% infill, FRONT FACE DOWN on bed (flat,
// no supports needed -- same convention the old adapter used).
// Corner screws: 4x M3x10 SHCS + M3 washer, straight into the porch's
// heat-set-insert bosses -- no nut, nothing reached through the porch
// wiring passage (that was the old adapter's M3x25+nyloc pattern; this
// plate doesn't use it). Board screws: 4x M2.5x8 SHCS + M2.5 washer +
// M2.5 nyloc nut behind the board, same as the old adapter used.
// =============================================================

$fn = 64;
eps = 0.01;

// ── Plate dimensions ────────────────────────────────────────────────────
// Matches weighstation.scad's disp_open_w/disp_open_u (134.6 x 58.2396)
// minus clearance to seat in the opening, same 0.6mm-total convention the
// deck uses against its own rebate ledge. disp_rim_w (there) is set to
// porch_edge_w-0.3 so this plate's own final installed edge measures
// exactly porch_edge_w (3mm, matching the deck's edge) once the 0.3mm
// fit clearance is added back on -- see weighstation.scad's disp_rim_w
// comment for the derivation. (Earlier history: disp_rim_w went 1mm ->
// 2mm after a hairline crack/hole showed up at the front-corner fillet;
// this 2.7mm is a further tuning of that same number, not a reversion.)
plate_v = 134.6  - 0.6;   // 134     -- face-width (V)
plate_u = 58.2396 - 0.6;  // 57.6396 -- up-slope   (U)
plate_t = 3;              // matches wall -- "as thick as the top panel"
corner_r = 7;   // matches weighstation.scad's corner_r-1 (the deck's own corner radius), for visual consistency

// ── Corner mounting screws (plate -> porch bosses) ──────────────────────
// Must match weighstation.scad's disp_boss_u/disp_boss_v exactly.
// Plain through-hole, no counterbore: a proper SHCS counterbore is 5.8dia
// x 3.5 deep (m3_cbore_d/m3_cbore_h elsewhere in this project) -- more
// depth than this whole 3mm plate has. The deck solves the same problem
// by hanging a reinforcing pad off its back at each screw, extending the
// local material enough to hold a real counterbore with floor to spare.
// Doing that here means re-coordinating the porch's insert-boss depth to
// match, for a screw head that's only ever seen from an angle anyway --
// not worth it. Head sits proud instead, same call already made for the
// board-mounting screws below.
boss_v = 61.3;      // disp_open_w/2 - 6
boss_u = 23.1198;   // disp_open_u/2 - 6
m3_cl_d = 3.4;

// ── Display bezel window ─────────────────────────────────────────────────
// Unchanged from the old adapter design -- the viewable active area.
win_v = 86;
win_u = 56;

// ── ILI9488 TFT — MSP3520-type PCB, landscape (for reference; the board
//    itself sits directly against the porch face now, not in a pocket
//    here) ──────────────────────────────────────────────────────────────
tft_pcb_v = 98.00;
tft_pcb_u = 56.34;

// ── Board mounting screws (plate -> board -> nut behind), same mechanism
//    and same pattern the old adapter used: cap proud on the front,
//    shaft through the board's corner hole into a nut trapped behind it. ──
brd_screw_v = 46;     // +-46mm (V), 92mm c-c
brd_screw_u = 25;     // +-25mm (U), 50mm c-c
brd_screw_d = 2.7;    // M2.5 clearance bore

module rrect(l, w, h, r) {
    linear_extrude(h)
        offset(r) offset(-r) square([l, w], center = true);
}

module faceplate() {
    difference() {
        rrect(plate_v, plate_u, plate_t, corner_r);

        // bezel window, full thickness
        translate([0, 0, -eps])
            linear_extrude(plate_t + 2*eps)
                offset(0.5) offset(-0.5)
                    square([win_v, win_u], center = true);

        // 4x corner mounting screws: plain clearance bore, head proud
        for (sv = [-1, 1], su = [-1, 1])
            translate([sv*boss_v, su*boss_u, -eps])
                cylinder(d = m3_cl_d, h = plate_t + 2*eps);

        // 4x M2.5 board-mounting bores, plain through-holes, cap proud
        for (sv = [-1, 1], su = [-1, 1])
            translate([sv*brd_screw_v, su*brd_screw_u, -eps])
                cylinder(d = brd_screw_d, h = plate_t + 2*eps);
    }
}

faceplate();
