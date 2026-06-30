// =============================================================
// porch-tft-adapter.scad — ILI9488 3.5" TFT bezel / cradle plate
// =============================================================
// Mounts on the 45° porch face with 4× M3 screws:
//
//   LEFT (2×): existing porch holes originally drilled for the SSD1306
//              OLED — outer pair only, at face-local X = −37.24 mm,
//              Y = ±12.70 mm.  (Inner OLED holes at X = −6.76 fall
//              inside the TFT active area and cannot be used.)
//
//   RIGHT (2×): NEW holes — use this plate as a drilling template.
//              Hold the plate flush against the porch face, aligned
//              to the two left screws (finger-tight), then mark the
//              right bores with an awl.  Remove plate and drill Ø3.4 mm
//              through the porch wall at the marks.
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
// Fasten: 4× M3×14 SHCS from outside (bezel face) through the plate
// and porch wall; M3 hex or nyloc nuts inside the porch cavity (reach
// via the wiring passage between porch cavity and main body).
//
// Cover note: the plate fully covers the old OLED window (26×14 mm),
// button hole (Ø16.2 mm), and LED hole (Ø5 mm) — no patching needed.
// =============================================================

$fn = 64;
eps = 0.01;

// ── Face-local coordinate reference ────────────────────────────
// Face-local origin = centre of porch face (slope centre, width centre).
// ui_oled_y = −22, ui_btn_y = +28, ui_led_y = +48  (face-local X).
// TFT is centred at the midpoint of those three items:
tft_cx = 13;   // face-local X of TFT / plate centre  (mm)
tft_cy =  0;   // face-local Y of TFT / plate centre  (mm)

// Plate-local = face-local − (tft_cx, tft_cy).

// ── Mounting hole positions (plate-local) ───────────────────────
// Left pair — existing OLED outer holes (face-local X = −37.24 mm):
left_hole_x  = -37.24 - tft_cx;   // = −50.24 mm
hole_y_half  =  12.70;             // = oled_hole_dy / 2 = 25.40 / 2

// Right pair — NEW holes drilled using this plate as template:
//   Face-local X = +65 mm  (plate right edge is at +59; 6 mm clearance).
//   Clears TFT PCB right edge (face-local +62 mm) by 3 mm.
//   Clears right-most old porch feature (LED at face-local +48 mm).
//   Porch face half-width = base_w/2 = 70 mm → 5 mm from face edge.
right_hole_x = 65.00 - tft_cx;    // = +52.00 mm (plate-local)

// ── ILI9488 TFT — MSP3520-type PCB, landscape ──────────────────
tft_pcb_x   = 98.00;   // PCB width  (X, face-width direction)
tft_pcb_y   = 56.34;   // PCB height (Y, slope direction)
tft_act_x   = 73.44;   // active-area width
tft_act_y   = 48.96;   // active-area height

// Bezel window: 1.5 mm inset from active edge — glass rests on lip
win_inset = 1.5;
win_x     = tft_act_x - 2 * win_inset;   // 70.44 mm
win_y     = tft_act_y - 2 * win_inset;   // 45.96 mm

// PCB cradle pocket: PCB + 0.4 mm assembly clearance; 3.2 mm deep
pocket_clr = 0.4;
pocket_x   = tft_pcb_x + pocket_clr;     // 98.4 mm  (±49.2 from centre)
pocket_y   = tft_pcb_y + pocket_clr;     // 56.74 mm
pocket_d   = 3.2;   // depth from back face (1.8 mm solid lip remains)

// ── Plate dimensions ───────────────────────────────────────────
// Width: right_hole_x (52) + m3_hd_d/2 (2.9) + 4 mm margin → 59 mm
//        plate_x = 2 × 59 = 118 mm (symmetric about plate centre).
// Clearance check left: 59 − 50.24 = 8.76 mm  ✓
// Clearance check right: 59 − 52.00 = 7.00 mm  ✓ (need 2.9+3 = 5.9)
plate_x  = 118;
plate_y  = tft_pcb_y + 10;   // 66.34 mm (5 mm margin above/below PCB)
plate_t  = 5.0;               // total thickness
corner_r = 4;

// ── M3 hardware ────────────────────────────────────────────────
m3_cl_d = 3.4;   // clearance bore Ø
m3_hd_d = 5.8;   // SHCS head counterbore Ø  (M3 head Ø5.5 + 0.3 margin)
m3_hd_h = 3.0;   // counterbore depth from front face

// ── Wire exit notch (low-Y edge) ───────────────────────────────
wire_notch_x = 24;   // width — clears ~8 leads at 3 mm pitch
wire_notch_y = 10;   // inward reach from plate edge

// =============================================================
module m3_bore() {
    // Clearance through-bore + SHCS head counterbore from front face.
    cylinder(d = m3_cl_d, h = plate_t + 2 * eps);
    translate([0, 0, plate_t - m3_hd_h])
        cylinder(d = m3_hd_d, h = m3_hd_h + 2 * eps);
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

        // ── Wire exit notch at low-Y (down-slope) edge ─────────
        translate([0,
                   -(plate_y / 2 - wire_notch_y / 2 + eps),
                   -eps])
            cube([wire_notch_x, wire_notch_y + 2 * eps, pocket_d + 2 * eps],
                 center = true);

        // ── Left M3 holes (existing OLED outer porch holes) ────
        for (sy = [-1, 1])
            translate([left_hole_x, sy * hole_y_half, -eps])
                m3_bore();

        // ── Right M3 holes (NEW — plate is drill template) ─────
        // Mark through these bores with an awl, then drill Ø3.4 mm.
        for (sy = [-1, 1])
            translate([right_hole_x, sy * hole_y_half, -eps])
                m3_bore();
    }
}

adapter();
