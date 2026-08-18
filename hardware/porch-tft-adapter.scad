// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Norm Sohl

// =============================================================
// porch-tft-adapter.scad — ILI9488 3.5" TFT bezel / cradle plate
// =============================================================
// Rev — issue #2 applied (printed-parts-issues.md):
//   - Window + surrounding pocket enlarged 0.5 mm PER SIDE.
//   - Whole object +10 mm tall (plate_t 5->15); the pocket is deepened to
//     match so the board's pin header seats inside the taller walls — no
//     slot cut, header stays soldered on the board.
//   - Wire notch removed; added a microSD access slot in a side wall.
//   - plate_y trimmed 67->63 so the top edge clears the overhanging
//     weighing platform (paired with plat_gap 2->5 in weighstation.scad;
//     ~3.1 mm clearance). Screw pattern decoupled from the outline.
//   - Added 4x M2.5 board-mounting bores through the front panel (92x50
//     board pattern): cap proud on front, shaft through panel + board
//     into a nut behind that cinches the board to the panel back.
// =============================================================
// Full-width faceplate for the 45° porch face (140 mm wide).
// Mounts with 4× M3 screws — all four are NEW holes; use this
// plate as a drilling template:
//
//   1. Hold plate flush against porch face, centred, tape/clamp.
//   2. Mark all four bore centres with an awl.
//   3. Remove plate and drill Ø3.4 mm through the porch wall.
//   4. Fasten with M3 screws + hex/nyloc nuts inside the cavity.
//      Screws now pass through a 15 mm-thick plate — use M3×20+.
//
// Screw pattern matches the porch holes in weighstation.scad:
//   X = ±60 mm (face width),  Y = ±23.5 mm (up-slope) — FIXED, so the
//   plate outline (plate_y) can be trimmed without moving the screws.
// Plain through-bores — NOT countersunk (pan/button-head screws).
//
// The TFT PCB drops into a deep rear pocket; the front bezel window
// exposes the active area behind a thin 1.8 mm bezel lip.  The pocket is
// deep enough (13.2 mm) to house the board's soldered pin header inside
// the walls — no slot cut.  Wires exit the open back into the porch
// cavity.  A microSD access slot in one side wall lets the card be
// inserted/removed while mounted (the open back faces the porch wall).
//
// ── Coordinate frame (plate lies flat on print bed) ────────────
//   X  = face width  (horizontal when mounted on porch)
//   Y  = up-slope    (when mounted on porch)
//   Z  = outward normal (Z=0 inner/back face; Z=plate_t bezel face)
//
// Print: PETG, 4 walls, 20 % gyroid infill, FRONT FACE DOWN on bed.
//
// Cover note: the plate fully covers the old OLED window (26×14 mm),
// button hole (Ø16.2 mm), and LED hole (Ø5 mm) — no patching needed.
// =============================================================

$fn = 64;
eps = 0.01;

// ── Plate dimensions ───────────────────────────────────────────
plate_x  = 140;   // face width (spans full porch face)
plate_y  = 63;    // slope extent — trimmed from 67 so the top edge clears
                  // the overhanging weighing platform (see interference note)
plate_t  = 15.0;  // total thickness — deep tray; walls house the header
corner_r = 4;

// ── Display opening (front bezel window) ────────────────────────
// Enlarged 0.5 mm per side (was 85×55).
win_x = 86;   // opening width
win_y = 56;   // opening height

// ── Mounting holes ─────────────────────────────────────────────
// FIXED screw pattern — must match the porch holes in weighstation.scad,
// so it is NOT derived from the plate outline (plate_y can be trimmed
// freely). At plate_y=63 the Y holes sit 8 mm from the top/bottom edge.
hole_x_half = 60;     // ±60 mm along face width (V)
hole_y_half = 23.5;   // ±23.5 mm along slope    (U)
m3_cl_d     = 3.4;    // clearance bore Ø (plain through-hole, no countersink)

// ── Display-board mounting (M2.5 front through-bolts) ──────────
// Cap head proud on the front bezel; shaft passes through the panel and
// the board's corner hole into an M2.5 nut behind, cinching the board
// against the back of the front panel. Board hole pattern (measured):
// 92 mm (V) × 50 mm (U) centre-to-centre → ±46 × ±25. These land on the
// bezel shelf (X > window ±43), so there's solid panel for the cap to
// bear on and for the board to clamp against.
brd_screw_v = 46;     // ±46 mm (V, face width) — 92 mm c-c
brd_screw_u = 25;     // ±25 mm (U, slope)      — 50 mm c-c
brd_screw_d = 2.7;    // M2.5 clearance bore (cap rests proud on the face)

// ── ILI9488 TFT — MSP3520-type PCB, landscape ──────────────────
tft_pcb_x = 98.00;   // PCB width  (X, face-width direction)
tft_pcb_y = 56.34;   // PCB height (Y, slope direction)

// PCB cradle pocket: PCB + assembly clearance, grown 0.5 mm/side with the
// window (pocket_clr 0.4 → 1.4). Deep enough to house the pin header.
pocket_clr = 1.4;
pocket_x   = tft_pcb_x + pocket_clr;     // 99.4 mm  (±49.7 from centre)
pocket_y   = tft_pcb_y + pocket_clr;     // 57.74 mm
pocket_d   = 13.2;  // depth from back face — houses PCB + header;
                    // 1.8 mm bezel lip remains (plate_t − pocket_d)

// ── microSD access slot ────────────────────────────────────────
// Aligns with the board's microSD socket mouth so the card can be
// inserted/removed while mounted (the open back faces the porch wall and
// is inaccessible). Cuts through the TOP wall at the card's height.
// Card exits the TOP edge; socket is on the BACK of the PCB.
// CORRECTED from the first print: the slot was on the wrong side and a
// bit tight — flipped the offset sign (+18), widened 3 mm/side, and
// extended the display-side (front) edge 1.5 mm to reach the socket mouth.
sd_slot_edge   = "top";   // "top"(+Y) | "bottom"(-Y) | "left"(-X) | "right"(+X)
sd_slot_offset = 18;      // +18 mm — socket sits on the +X side (was -18)
sd_slot_w      = 22;      // slot width (was 16; +3 mm each side)
// Z-span (front = display side, Z=pocket_d). Back edge unchanged at 4.7;
// front/display-side edge raised 1.5 mm (9.7 -> 11.2) to reach the socket.
sd_slot_back_z  = 4.7;               // back (open-face) edge
sd_slot_front_z = pocket_d - 2.0;    // 11.2 — display-side edge
sd_slot_h       = sd_slot_front_z - sd_slot_back_z;       // 6.5 mm
sd_slot_z       = (sd_slot_back_z + sd_slot_front_z) / 2; // 7.95 mm centre

// =============================================================
// microSD access slot: a box cut through one perimeter wall, spanning
// from just inside the pocket cavity to just outside the plate edge, at
// the card-mouth height (sd_slot_z).
module sd_slot() {
    vert = (sd_slot_edge == "top" || sd_slot_edge == "bottom");
    sgn  = (sd_slot_edge == "top" || sd_slot_edge == "right") ? 1 : -1;

    if (vert) {
        inner = pocket_y / 2 - 1;      // start inside the pocket
        outer = plate_y / 2 + 1;       // end outside the plate edge
        translate([sd_slot_offset, sgn * (inner + outer) / 2, sd_slot_z])
            cube([sd_slot_w, outer - inner, sd_slot_h], center = true);
    } else {
        inner = pocket_x / 2 - 1;
        outer = plate_x / 2 + 1;
        translate([sgn * (inner + outer) / 2, sd_slot_offset, sd_slot_z])
            cube([outer - inner, sd_slot_w, sd_slot_h], center = true);
    }
}

module adapter() {
    difference() {

        // ── Plate body ─────────────────────────────────────────
        linear_extrude(plate_t)
            offset(corner_r) offset(-corner_r)
                square([plate_x, plate_y], center = true);

        // ── TFT PCB cradle pocket (opens at back face, Z = 0) ──
        translate([0, 0, -eps])
            linear_extrude(pocket_d + eps)
                offset(0.5) offset(-0.5)
                    square([pocket_x, pocket_y], center = true);

        // ── Bezel window (opens at front face, Z = plate_t) ────
        translate([0, 0, plate_t - pocket_d])
            linear_extrude(pocket_d + 2 * eps)
                offset(0.5) offset(-0.5)
                    square([win_x, win_y], center = true);

        // ── microSD access slot through one perimeter wall ─────
        // Spans from just inside the pocket to just outside the plate
        // edge, at the card-mouth height, so the card passes through.
        sd_slot();

        // ── 4× M3 clearance holes — symmetric, 10 mm from edges ─
        // Plain through-bores; use plate as a drill template.
        for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * hole_x_half, sy * hole_y_half, -eps])
                cylinder(d = m3_cl_d, h = plate_t + 2 * eps);

        // ── 4× M2.5 display-board mounting bores (front panel) ──
        // Through the front bezel into the pocket; the screw continues
        // through the board's corner hole to a nut behind. Cap proud.
        for (sv = [-1, 1], su = [-1, 1])
            translate([sv * brd_screw_v, su * brd_screw_u, -eps])
                cylinder(d = brd_screw_d, h = plate_t + 2 * eps);
    }
}

adapter();
