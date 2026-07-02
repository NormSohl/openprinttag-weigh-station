// =============================================================
// porch-tft-adapter.scad — ILI9488 3.5" TFT bezel / cradle plate
// =============================================================
// Full-width faceplate for the 45° porch face (140 mm wide).
// Mounts with 4× M3 screws — all four are NEW holes; use this
// plate as a drilling template:
//
//   1. Hold plate flush against porch face, centred, tape/clamp.
//   2. Mark all four bore centres with an awl.
//   3. Remove plate and drill Ø3.4 mm through the porch wall.
//   4. Fasten with M3 screws + hex/nyloc nuts inside the cavity.
//
// Holes are symmetric, 10 mm in from every edge:
//   X = ±60 mm  (140/2 − 10),  Y = ±23.5 mm  (67/2 − 10).
// Plain through-bores — NOT countersunk (pan/button-head screws).
//
// The TFT PCB sits in a shallow rear pocket; the front bezel window
// exposes the active area.  No header pocket — header desoldered;
// wires hand-soldered to PCB pads and exit through a notch at the
// low-Y (down-slope) edge.
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
plate_t  = 5.0;   // total thickness
corner_r = 4;

// ── Display opening (front bezel window) ────────────────────────
win_x = 85;   // opening width
win_y = 55;   // opening height

// ── Mounting holes ─────────────────────────────────────────────
// 10 mm in from each edge, symmetric about plate centre.
hole_inset  = 10;
hole_x_half = plate_x / 2 - hole_inset;   // = 60.0 mm
hole_y_half = plate_y / 2 - hole_inset;   // = 23.5 mm
m3_cl_d     = 3.4;   // clearance bore Ø (plain through-hole, no countersink)

// ── ILI9488 TFT — MSP3520-type PCB, landscape ──────────────────
tft_pcb_x = 98.00;   // PCB width  (X, face-width direction)
tft_pcb_y = 56.34;   // PCB height (Y, slope direction)

// PCB cradle pocket: PCB + 0.4 mm assembly clearance; 3.2 mm deep
pocket_clr = 0.4;
pocket_x   = tft_pcb_x + pocket_clr;     // 98.4 mm  (±49.2 from centre)
pocket_y   = tft_pcb_y + pocket_clr;     // 56.74 mm
pocket_d   = 3.2;   // depth from back face (1.8 mm solid lip remains)

// ── Wire exit notch (low-Y edge) ───────────────────────────────
wire_notch_x = 24;   // width — clears ~8 leads at 3 mm pitch
wire_notch_y = 10;   // inward reach from plate edge

// =============================================================
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

        // ── Wire exit notch at low-Y (down-slope) edge ─────────
        translate([0,
                   -(plate_y / 2 - wire_notch_y / 2 + eps),
                   -eps])
            cube([wire_notch_x, wire_notch_y + 2 * eps, pocket_d + 2 * eps],
                 center = true);

        // ── 4× M3 clearance holes — symmetric, 10 mm from edges ─
        // Plain through-bores; use plate as a drill template.
        for (sx = [-1, 1], sy = [-1, 1])
            translate([sx * hole_x_half, sy * hole_y_half, -eps])
                cylinder(d = m3_cl_d, h = plate_t + 2 * eps);
    }
}

adapter();
