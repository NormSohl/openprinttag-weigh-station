// =============================================================
// porch-tft-adapter.scad — ILI9488 3.5" TFT bezel / cradle plate
// =============================================================
// Mounts on the existing 4× M3 through-holes in the 45° porch face
// (the holes originally drilled for the SSD1306 OLED module).
// The TFT PCB sits in a shallow rear pocket; the front bezel window
// exposes the active area.  No header pocket — header is desoldered;
// wires are hand-soldered directly to the PCB pads and exit through
// a notch in the low edge of the pocket.
//
// ── Coordinate frame (plate lies flat on print bed) ────────────
//   X  = face width  (horizontal when mounted on porch; = world Y)
//   Y  = up-slope    (when mounted; = world X/Z at 45°)
//   Z  = outward normal (Z=0 inner/back face; Z=plate_t bezel face)
//
// Print: PETG, 4 walls, 20 % gyroid infill, FRONT FACE DOWN on bed.
//
// Fasten: 4× M3×14 SHCS from outside through the adapter plate and
// through the existing 3.4 mm porch wall holes; secure with M3 hex
// nuts (or nyloc nuts) inside the porch cavity.  Reach nuts via the
// wiring passage that connects the porch cavity to the main body.
//
// Cover note: the adapter plate fully covers the old OLED window
// (26×14 mm), button hole (Ø16.2 mm), and LED hole (Ø5 mm) — they
// become blind pockets inside the porch cavity; no patching needed.
//
// Wire routing: hand-soldered leads exit the low-Y edge of the PCB
// cradle through the wire_notch slot, then route through the old
// OLED window or button hole in the porch wall into the cavity.
// =============================================================

$fn = 64;
eps = 0.01;

// ── Existing porch M3 mounting holes (face-local coords) ───────
// Face-local: X = face width (= world Y), Y = slope direction
// Hole group centred at face-local (X = ui_oled_y = −22, Y = 0)
hole_group_x =  -22;       // group centre, face-width axis
hole_group_y =    0;       // group centre, slope axis (face centre)
hole_dx      = 30.48;      // hole spacing along face width (X)
hole_dy      = 25.40;      // hole spacing along slope      (Y)

// ── ILI9488 TFT — MSP3520-type PCB, landscape ──────────────────
tft_pcb_x   = 98.00;   // PCB width  (face-width direction, X)
tft_pcb_y   = 56.34;   // PCB height (slope direction, Y)
tft_act_x   = 73.44;   // active-area width
tft_act_y   = 48.96;   // active-area height

// Bezel window: 1.5 mm inset from active edge → glass rests on lip
win_inset   = 1.5;
win_x       = tft_act_x - 2 * win_inset;   // 70.44 mm
win_y       = tft_act_y - 2 * win_inset;   // 45.96 mm

// PCB cradle pocket: PCB + 0.4 mm assembly clearance
// depth: PCB 1.6 mm + glass overhang ~1 mm + clearance = 3.2 mm
pocket_clr  = 0.4;
pocket_x    = tft_pcb_x + pocket_clr;      // 98.4 mm
pocket_y    = tft_pcb_y + pocket_clr;      // 56.74 mm
pocket_d    = 3.2;    // pocket depth from back face

// ── Plate ───────────────────────────────────────────────────────
// Centre the plate (and TFT) at X = midpoint of porch items:
//   items at face-local X: OLED=−22, Button=+28, LED=+48 → mid=+13
tft_cx  = 13;    // TFT & plate centre in face-local X
tft_cy  =  0;    // TFT & plate centre in face-local Y (slope centre)

// Width: extends far enough left to keep the M3 holes (at plate-local
// X = −50.24) at least 5 mm from the edge (→ M3 counterbore fits).
plate_x = 112;               // face-width extent
plate_y = tft_pcb_y + 10;   // slope extent: 66.34 mm (5 mm each side of PCB)
plate_t = 5.0;               // total thickness (1.8 mm lip after 3.2 mm pocket)
corner_r = 4;

// ── Mounting holes in PLATE-LOCAL coords (origin = plate centre) ─
// Translate from face-local: subtract (tft_cx, tft_cy) = (13, 0)
hlx = hole_group_x - tft_cx;   // = −35
hly = hole_group_y - tft_cy;   //  =  0

// ── M3 hardware ────────────────────────────────────────────────
m3_cl_d = 3.4;   // clearance bore Ø
m3_hd_d = 5.8;   // SHCS head counterbore Ø  (ISO 4762 M3: head Ø5.5 + margin)
m3_hd_h = 3.0;   // SHCS head counterbore depth from front face

// ── Wire exit notch (low-Y / down-slope edge of cradle pocket) ──
// Centred in X; cuts through the bottom wall of the pocket and
// breaks through the plate low-Y edge, giving wire leads a clear
// exit path.  Sized for 8 leads at ~3 mm pitch.
wire_notch_x =  24;   // notch width
wire_notch_y =  10;   // notch reach inward from plate low-Y edge

// =============================================================
module adapter() {
    difference() {

        // ── Plate body ─────────────────────────────────────────
        linear_extrude(plate_t)
            offset(corner_r) offset(-corner_r)
                square([plate_x, plate_y], center = true);

        // ── TFT PCB cradle pocket (opens at back face Z = 0) ───
        // The PCB slides in from behind; depth = pocket_d.
        translate([0, 0, -eps])
            linear_extrude(pocket_d + eps)
                offset(0.5) offset(-0.5)   // 0.5 mm pocket corner radius
                    square([pocket_x, pocket_y], center = true);

        // ── Bezel window (opens at front face Z = plate_t) ─────
        // Cuts the 1.8 mm solid lip from the front face down to the
        // pocket floor, leaving a shelf that the glass rests on.
        translate([0, 0, plate_t - pocket_d])
            linear_extrude(pocket_d + 2 * eps)
                offset(0.5) offset(-0.5)
                    square([win_x, win_y], center = true);

        // ── Wire exit notch at low-Y edge ──────────────────────
        // Centred at Y just inside the plate bottom edge; cuts from
        // the back face (Z = 0) to the pocket floor (Z = pocket_d),
        // passing through the pocket's bottom wall and the plate edge.
        translate([0,
                   -(plate_y / 2 - wire_notch_y / 2 + eps),
                   -eps])
            cube([wire_notch_x, wire_notch_y + 2 * eps, pocket_d + 2 * eps],
                 center = true);

        // ── 4× M3 clearance bores + SHCS head counterbores ─────
        // Heads sit on the FRONT (bezel) face; nuts inside porch.
        for (dx = [-1, 1], dy = [-1, 1])
            translate([hlx + dx * hole_dx / 2,
                       hly + dy * hole_dy / 2,
                       -eps]) {
                // Through bore
                cylinder(d = m3_cl_d, h = plate_t + 2 * eps);
                // Head counterbore from front face
                translate([0, 0, plate_t - m3_hd_h])
                    cylinder(d = m3_hd_d, h = m3_hd_h + 2 * eps);
            }
    }
}

adapter();
