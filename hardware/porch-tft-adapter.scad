// =============================================================
// porch-tft-adapter.scad — ILI9488 3.5" TFT bezel / cradle plate
// =============================================================
// Rev — issue #2 applied (printed-parts-issues.md):
//   - Window + surrounding pocket enlarged 0.5 mm PER SIDE.
//   - Whole object +10 mm tall (plate_t 5->15); the pocket is deepened to
//     match so the board's pin header seats inside the taller walls — no
//     slot cut, header stays soldered on the board.
//   - Wire notch removed; added a microSD access slot in a side wall.
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
// Holes are symmetric, 10 mm in from every edge:
//   X = ±60 mm  (140/2 − 10),  Y = ±23.5 mm  (67/2 − 10).
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
plate_y  = 67;    // slope extent
plate_t  = 15.0;  // total thickness — deep tray; walls house the header
corner_r = 4;

// ── Display opening (front bezel window) ────────────────────────
// Enlarged 0.5 mm per side (was 85×55).
win_x = 86;   // opening width
win_y = 56;   // opening height

// ── Mounting holes ─────────────────────────────────────────────
// 10 mm in from each edge, symmetric about plate centre.
hole_inset  = 10;
hole_x_half = plate_x / 2 - hole_inset;   // = 60.0 mm
hole_y_half = plate_y / 2 - hole_inset;   // = 23.5 mm
m3_cl_d     = 3.4;   // clearance bore Ø (plain through-hole, no countersink)

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
// CONFIRMED: the card exits the TOP edge of the display, and the socket
//   is on the BACK of the PCB — so its mouth sits near the open back of
//   this tray (see sd_slot_z).
// OFFSET: socket mouth measured 67 mm (±1) from the board's right edge on
//   the 98 mm-wide board → 31 mm from the left edge → 18 mm off-centre.
//   Sign assumes the board's right edge = +X here; if the slot prints on
//   the wrong side, flip to +18.
sd_slot_edge   = "top";   // "top"(+Y) | "bottom"(-Y) | "left"(-X) | "right"(+X)
sd_slot_offset = -18;     // 18 mm toward -X (=67 mm from the +X/right edge)
sd_slot_w      = 13;      // slot width  (microSD ~11 mm + clearance)
sd_slot_h      = 3;       // slot height (card + socket-lip clearance)
// Card-mouth height ≈ pocket floor (13.2) − PCB (1.6) − socket standoff
// (~1.1) ≈ 10.5 mm above the back face.  Verify exact standoff on board.
sd_slot_z      = 10.5;

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
    }
}

adapter();
