// =============================================================
// OpenPrintTag Weigh Station — parametric enclosure
// =============================================================
// Rev — see printed-parts-issues.md:
//   #1 load-cell fixed-end boss reinforced (fore-aft footprint widened;
//      +X buttress capped below the bar so the beam still flexes).
//   Porch UI reworked: OLED/button/LED removed; single TFT opening + 4
//   adapter screw holes added (matches porch-tft-adapter.scad).
//   plat_gap 2->5: raises the platform so it clears the adapter's top
//   edge (with plate_y=63) — boss grows ~3mm (see fastener note).
//   VERIFY the reinforcement in a render and tune to the physical fix.
// =============================================================
// Parts: set `part` below (or via -D part="...") and export STL.
//   "platform"  — weighing disc + antenna pocket (prints top-face-down)
//   "spigot"    — separate centering cone, bolts to platform center
//                 (swap spigots for different hub bores)
//   "base"      — shell + 45-deg UI porch, load cell boss, tray bosses
//                 (prints fully open-topped — zero bridging)
//   "deck"      — drop-in top plate closing the corners the platform
//                 doesn't cover; rests on the base's rebate ledge
//   "tray"      — electronics tray (modules + crimped jumper harnesses)
//   "assembly"  — preview of everything in place (don't print)
//
// MEASURE BEFORE PRINTING (the four numbers that must match reality):
//   hub_bore_d        — measured: 53mm
//   tag_ring_r        — set from the MK1 drawing (35.8mm); only revisit
//                       if a future tag revision changes coil geometry
//   (load cell: measured — TAL220B 55mm, one M5 per end @ 7.5mm)
//   pcb_hole_*        — tray/boss mounting pattern (shared by base + tray)
//
// Print notes: base in PETG (4 walls, 40% infill near the
// load cell boss), platform + spigot in PLA. All screws: metric
// socket head (SHCS) + washers on plastic. Heat-set inserts:
//   M3 (Ruthex RX-M3x5.7) — tray bosses, deck columns, overload stop
//   M4 (Ruthex RX-M4x6.0) — platform center (spigot bolt)
//   M5 (Ruthex RX-M5x7.0) — load cell fixed-end boss
// =============================================================

part = "assembly"; // [assembly, platform, spigot, base, deck, tray]

$fn = 96;
eps = 0.01;

// ---------------- heat-set insert pockets (Ruthex press-fit style) --------
// Pocket diameter = insert OD; pocket depth = insert length + 0.5mm lead-in.
// All inserts are installed from the TOP of the boss/post (iron in from above).
// Screw clearance bores are Ø = nominal + 0.4mm throughout.
m3_ins_d  = 4.6;   m3_ins_l  = 6.2;   // M3 insert (Ruthex RX-M3x5.7): OD 4.6, L 5.7+0.5
m4_ins_d  = 6.0;   m4_ins_l  = 6.5;   // M4 insert (Ruthex RX-M4x6.0): OD 6.0, L 6.0+0.5
m5_ins_d  = 7.4;   m5_ins_l  = 7.5;   // M5 insert (Ruthex RX-M5x7.0): OD 7.4, L 7.0+0.5
// Socket-head cap screw (SHCS ISO 4762) counterbore dims:
// M3: head Ø5.5 x 3.0 → cbore Ø5.8 x 3.5
// M4: head Ø7.0 x 4.0 → cbore Ø7.4 x 4.5
// M5: head Ø8.5 x 5.0 → cbore Ø9.0 x 5.5
m3_cbore_d = 5.8;  m3_cbore_h = 3.5;
m4_cbore_d = 7.4;  m4_cbore_h = 4.5;
m5_cbore_d = 9.0;  m5_cbore_h = 5.5;

// ---------------- spool / tag ----------------
hub_bore_d   = 53;     // measured spool center bore (Jun 2026)
// Tag geometry per the official MK1 drawing
// (specs.openprinttag.org/media/tag_mk1.pdf): tag OD Ø90; antenna coil
// traces occupy the zone between Ø64 and Ø79.1 → coil mean radius ≈36mm.
// The reader couples to the COIL, so the pocket centers on this radius.
tag_ring_r   = 35.8;   // MK1 coil mean radius (was a 60mm placeholder)

// ---------------- platform ----------------
plat_d       = 205;    // fits the MK4S bed (210mm Y, 2.5mm/side margin);
// holds Ø200 spools with the rim still 2.5mm proud — contact is near
// the hub regardless. Bump to 220 only if printing on an XL.
plat_t       = 8;   // disc top is dead flat (prints face-down).
// All fasteners are metric SOCKET HEAD cap screws (user stock), washer
// under every head on plastic. Both platform fastenings live inside
// the spool's Ø53 bore shadow: the free-end M5 counterbore floor
// stacks on the integral spacer pad (2mm disc + 3mm pad = 5mm), and
// the spigot uses an M4 through-bolt from above into a captive-nut
// trap recessed in the underside — no island, no protrusions.
spigot_h     = 45;
spigot_top_d = 30;     // cone: top narrower than hub bore for easy drop-on
spigot_clr   = 1.0;    // diametral clearance at hub seat

// ---------------- load cell (5 kg bar, 80x12.7x12.7 typ.) ----------------
// TAL220B, per datasheet (measured Jun 2026): 55 x 12.7 x 12.7mm,
// ONE M5 per end, holes 40mm apart (7.5mm from each end). Single-bolt
// ends can rotate about the bolt — both mounts get anti-rotation
// features (cradle walls at the fixed end, flanking ribs at the free
// end). Rated output 1.0 mV/V; ultimate overload 150%FS (7.5kg on a
// 5kg cell) — the stop bolt is not optional.
lc_len  = 55;
lc_w    = 12.7;
lc_h    = 12.7;
lc_hole_from_end = 7.5;  // both ends, M5
spacer_h        = 3;    // flex clearance both ends
plat_gap        = 5;    // platform floats this far above the base rim
                        // (raised 2->5 so it clears the TFT adapter's top
                        //  edge; pairs with plate_y=63 -> ~3.1mm clearance)
// Boss height is DERIVED so the platform always clears the shell:
// raise the bar high enough that (boss + bar + spacer) tops the rim.
// (was a fixed 14mm — which buried the platform inside the box)

// ---------------- base ----------------
base_l   = 200;
base_w   = 140;
base_h   = 60;
wall     = 3;
corner_r = 8;

// ------------- electronics tray mounting -------------
pcb_l = 80;  pcb_w = 50;   // tray fits the -Y half, clear of the central load-cell band
pcb_hole_inset = 5;     // hole centers from each edge
pcb_boss_h = 8;
pcb_pos = [20, -38];    // tray in -Y half: clears central LC/stop band, columns, walls

// ---------------- antenna (PN5180-NFC module) ----------------
// Board measured: 40 x 70mm, +5mm radial relief so wires reach the
// header without binding. Header (JP1) sits at the INWARD end, antenna
// coil at the OUTWARD end — the radial cable channel exits inward-to-
// outward, but the header needs to reach DOWN through the platform to
// the base, so the channel still routes from the pocket's outer edge
// to the rim (matches existing harness routing in base()).
ant_l = 40;              // tangential extent (board width, across header)
ant_w = 70 + 5;          // radial extent (board length + 5mm wire relief)
ant_t = 3.5;             // pocket depth
ant_angle = 90;          // angular position on tag ring (deg, 0 = +X)
ant_web_t = 2;           // plastic left between pocket and platform top
// Pocket center is offset OUTWARD from the tag-ring antenna reference
// point (ant_pos, used elsewhere for the OPT coil alignment and the
// base harness pass-through) so the pocket's inward edge clears the
// center M4 boss (Ø10 counterbore, 5mm radius) with a 3mm wall margin.
ant_clear_r   = 8;                          // min radial clearance from center
ant_pocket_dy = ant_clear_r + ant_w/2;      // pocket center offset along radius

// ---------------- ports / UI ----------------
usb_w = 12;  usb_h = 7;            // rear cutout for USB-C breakout module
// The SSD1306 OLED, panel button, and status LED are REMOVED — replaced
// by a single 3.5in ILI9488 TFT on a printed adapter (porch-tft-adapter.scad).
// The adapter is 140 (face-width V) x 63 (up-slope U), centred on the
// porch face and fastened with 4x M3.
// 45-degree UI porch: a wedge extending forward past the platform edge,
// carrying the display on its sloped face, angled at the user.
porch_len = 45;                    // how far the wedge extends (also = height drop @45)

// Display opening through the slope wall: inset 15mm/side from the
// adapter outline -> 110 (V) x 37 (U), centred on the face.
disp_open_w = 110;                 // face-width (V) extent
disp_open_u = 37;                  // up-slope   (U) extent
disp_open_r = 3;                   // corner radius
// Adapter screw pattern (matches porch-tft-adapter.scad hole_x/y_half):
disp_screw_v = 60;                 // +-60 along face width (V)
disp_screw_u = 23.5;               // +-23.5 along slope    (U)

// ---------------- overload stop ----------------
stop_bolt_d = 5;                   // M5 set bolt under free end
// gap is set by threading the bolt, not by the print

// ---------------- derived ----------------
lc_boss_h  = base_h + plat_gap - lc_h - spacer_h;  // see note above (~46mm)
lc_top_z   = lc_boss_h + lc_h;                 // top of bar
plat_z     = lc_top_z + spacer_h;              // platform underside
lc_x0      = -lc_len/2;                        // bar centered in X
ant_pos    = [tag_ring_r*cos(ant_angle), tag_ring_r*sin(ant_angle)];
// PN5180 board pocket center: ant_pocket_dy IS the pocket's center
// radius (already includes the clearance margin — see antenna params
// above), at the same angle as ant_pos. This keeps the pocket's inward
// edge at radius ant_clear_r, clear of the center M4 boss.
ant_pocket_pos = [ant_pocket_dy*cos(ant_angle), ant_pocket_dy*sin(ant_angle)];

module rrect(l, w, h, r) {
  linear_extrude(h)
    offset(r) offset(-r) square([l, w], center=true);
}

// =============================================================
// PLATFORM — disc + antenna pocket + free-end mount
// Prints TOP-FACE-DOWN: smooth weighing surface off the bed; the
// pocket, channel, and spacer pads all build upward, no supports.
// The spigot is a SEPARATE part (see spigot() below), bolted on with
// an M4 flat-head from below into a heat-set insert in the cone base.
// =============================================================

module platform() {
  difference() {
    // Top face is COMPLETELY flat (one Ø4.4 hole only) — prints
    // face-down with a full first layer, no recesses to bridge.
    // The spigot self-centers on its through-bolt (Ø4.4 hole vs M4
    // = ~0.2mm max offset, far inside the coil's 7.5mm tolerance),
    // so no locating recess is needed; its flange sits proud inside
    // the Ø53 bore shadow where no spool ever touches.
    cylinder(d=plat_d, h=plat_t);
    // center bolt: M4 SHCS from below, FULLY counterbored.
    // CRITICAL: head + washer must end up sub-flush — the disc center
    // sits directly over the load cell bar's mid-span, where nothing
    // may protrude (or touch the bar under deflection). Ø10 x 5.5
    // pocket swallows head (Ø7 x 4) + Ø9 washer with margin.
    // center: M4 THROUGH-BOLT from above (down the spigot's axial
    // bore) into a captive nut. Hex nut trap in the underside: nut
    // (7mm AF x 3.2) sits fully recessed — nothing protrudes toward
    // the bar's mid-span, and the top face stays flat for printing.
    translate([0,0,-eps]) {
      cylinder(d=4.4, h=plat_t + 2*eps);              // through-hole
      cylinder(d=m4_ins_d, h=m4_ins_l);               // M4 heat-set insert pocket (underside)
    }
    // PN5180-NFC board pocket from below, leaving a thin web on top.
    // Centered at ant_pocket_pos (NOT ant_pos directly) — offset radially
    // outward so the pocket's inward edge clears the center M4 boss.
    translate([ant_pocket_pos[0], ant_pocket_pos[1], -eps])
      linear_extrude(ant_t)
        square([ant_l, ant_w], center=true);
    // PN5180 cable channel: runs RADIALLY OUTWARD from the pocket's
    // outer edge to the nearest rim (the -90 makes the slot's long axis
    // follow the pocket's radius vector). Exits the platform edge
    // directly above the base's side-wall harness pass-through.
    translate([ant_pocket_pos[0], ant_pocket_pos[1], -eps])
      rotate([0,0,ant_angle - 90])
        linear_extrude(ant_t)
          translate([-6, ant_w/2])
            square([12, plat_d/2], center=false);
    // free-end mounting: single M5 SHCS from above, counterbored sub-flush.
    // SHCS head Ø8.5 x 5.0 → counterbore Ø9.0 x 5.5, leaving a 2.5mm floor.
    translate([lc_x0 + lc_len - lc_hole_from_end, 0, -eps]) {
      cylinder(d=5.4, h=plat_t + 2*eps);              // M5 clearance bore through
      translate([0,0,plat_t - m5_cbore_h]) cylinder(d=m5_cbore_d, h=m5_cbore_h + eps); // SHCS counterbore, top-flush
    }
  }
  // integral spacer pad at the free-end hole (replaces a loose spacer)
  translate([lc_x0 + lc_len - lc_hole_from_end, 0, -spacer_h])
    difference() {
      cylinder(d=14, h=spacer_h+eps);
      translate([0,0,-eps]) cylinder(d=5.4, h=spacer_h+3*eps);
    }
  // anti-rotation ribs: with only ONE bolt, the platform could yaw
  // about it. Two ribs flank the bar's free end with 0.3mm/side
  // clearance — they never touch in normal operation (no force shunt,
  // no friction in the measurement path) but stop any rotation at
  // first contact. They reach 4mm down the bar's sides.
  for (sy = [-1, 1])
    translate([lc_x0 + lc_len - 21,                  // 14mm rib, near the free end
               sy*(lc_w/2 + 0.3) + (sy < 0 ? -3 : 0), // 0.3mm gap to bar side
               -(spacer_h + 4)])                      // hangs below the disc
      cube([14, 3, spacer_h + 4]);
}

// =============================================================
// SPIGOT — separate centering cone. Prints base-down, no supports.
// Different hub bores = print another spigot, not another platform.
// =============================================================
module spigot() {
  // Fastening: one M4 x 25 SHCS dropped down the axial bore from the
  // cone tip; head + washer seat on the internal shelf ~14mm up, the
  // thread passes through the platform into the captive nut beneath.
  // A 3mm hex key reaches the head through the tip opening. Prints
  // base-down; the shelf is a widening taper, so no supports.
  // Pure cone — its own Ø52 base seats flat on the disc (the old
  // narrow flange was a tenon for the deleted locating recess).
  difference() {
    cylinder(h=spigot_h, d1=hub_bore_d - spigot_clr, d2=spigot_top_d);
    translate([0,0,-eps]) cylinder(d=4.5, h=14);             // shank passage
    translate([0,0,14 - eps]) cylinder(d1=4.5, d2=9, h=2.5); // shelf taper
    translate([0,0,16.5 - 2*eps]) cylinder(d=9, h=spigot_h); // access bore
  }
}

// =============================================================
// BASE — shell, load cell boss, overload stop boss, tray bosses
// Architecture: union ALL solid geometry first, then subtract ALL
// voids in a SINGLE difference(). This guarantees every cut reaches
// every cavity — separate difference() blocks don't interact, which
// was causing the sealed porch cavity in previous versions.
// =============================================================
module base() {
  porch_x0 = -base_l/2;   // wedge rear face, flush with main shell front

  difference() {

    // ==================== SOLIDS ====================
    union() {
      // main shell
      rrect(base_l, base_w, base_h, corner_r);

      // 45-degree UI porch wedge (solid — hollowed below)
      translate([0, base_w/2, 0]) rotate([90, 0, 0])
        linear_extrude(base_w)
          polygon([[porch_x0, 0], [porch_x0, base_h],
                   [porch_x0 - porch_len, base_h - porch_len],
                   [porch_x0 - porch_len, 0]]);

      // (ALL interior bosses — load cell, overload stop, tray, deck
      // columns — are added AFTER the difference below. Inside this union
      // the full-height main cavity void would erase them, since they sit
      // within the cavity footprint. Only the shell + porch wedge, which
      // the cavity is meant to hollow, belong in here.)
    }

    // ==================== VOIDS ====================

    // --- main cavity (open-topped; deck is a separate drop-in part) ---
    translate([0,0,wall])
      rrect(base_l - 2*wall - 5, base_w - 2*wall - 5, base_h, corner_r-1);
    // deck rebate ledge
    translate([0,0,base_h-wall])
      rrect(base_l - 2*wall, base_w - 2*wall, wall+eps, corner_r-1);

    // --- porch cavity (inner hollow, OPEN toward main cavity) ---
    // The inner ceiling follows the true 45-deg slope up to JUST UNDER
    // the deck rebate ledge (z = base_h - wall - 0.5), runs horizontal
    // beneath where the ledge sits, then rises to full height only once
    // it is inside the open main cavity (past the cavity wall). This
    // opens the porch fully WITHOUT cutting through the rebate ledge the
    // deck rests on, and avoids coincident faces (0.5mm under the ledge,
    // rear inside the cavity, top overshooting the rim).
    porch_drop = wall * sqrt(2);
    front_x    = porch_x0 - porch_len + wall;          // -142
    front_top  = front_x + (160 - porch_drop);         // 13.76 (inner slope)
    cap_z      = base_h - wall - 0.5;                   // 56.5: just under ledge
    slope_cap_x = cap_z - (160 - porch_drop);          // where slope meets cap_z
    rear_x     = -(base_l - 2*wall - 5)/2 + 6.5;        // -88: inside main cavity
    over_z     = base_h + 5;                            // overshoot rim
    // Porch hollow width MATCHES the main cavity (base_w - 2*wall - 5),
    // so the porch and body side walls are flush (both at ±64.5) with no
    // step at the junction. An earlier width of base_w-2*wall made the
    // porch walls 3mm but the body walls 5.5mm, opening a thin slot where
    // they met. Centered via the (wall + 2.5) translate offset.
    translate([0, base_w/2 - wall - 2.5, 0]) rotate([90, 0, 0])
      linear_extrude(base_w - 2*wall - 5)
        polygon([
          [rear_x,      wall],       // rear-bottom (into main cavity)
          [front_x,     wall],       // front-bottom (floor)
          [front_x,     front_top],  // front-top (base of inner slope)
          [slope_cap_x, cap_z],      // slope rises to just under the ledge
          [rear_x,      cap_z],      // horizontal under the ledge region
          [rear_x,      over_z]      // rise to full height INSIDE the cavity
        ]);

    // --- wiring passage: bridges porch cavity to main cavity ---
    // Verified geometry (base_l=200, wall=3, porch_len=45):
    //   main cavity -X edge  = -(base_l-2*wall-5)/2 = -92
    //   porch cavity -X edge =  porch_x0 + eps       = -100
    // So 8mm of wall separates them. This cutter spans X = -104..-88
    // (16mm) at the cavity floor level, opening a clear wiring route.
    translate([-96, 0, wall + 8])
      cube([16, base_w - 4*wall, 16], center=true);

    // --- ports: each cutter is OVER-LENGTH and centered on the wall so
    // it spans well past both faces (a snug cutter that just reaches a
    // face leaves a blind pocket — the bug seen in earlier renders). ---
    // rear USB-C port: cuts through the rear wall at X=+base_l/2.
    // Centred at usbbo_world[1] in Y (Y=-30, clear of deck columns).
    // Cutter spans 4*wall in X (over-long — guarantees it exits both faces).
    // Z height tracks the board standoff height + PCB + connector centre.
    translate([base_l/2 - 2*wall, usbbo_world[1],
               wall + pcb_boss_h + 1.6 + usb_h/2])
      rotate([0,90,0]) rrect(usb_h, usb_w, 4*wall, 2);
    // load cell harness pass-through: side wall at y = +base_w/2; cut
    // along Y, centered, length 4*wall
    translate([ant_pos[0],
               (ant_pos[1] > 0 ? base_w/2 : -base_w/2) - wall/2,
               base_h - 15])
      rotate([90,0,0]) cylinder(d=10, h=4*wall, center=true);

    // --- UI cutouts on the 45-degree face ---
    // Single TFT display opening + 4 adapter screw holes, centred on the
    // face. Cut through the slope wall from the face center point.
    // V = face width (world Y); U = up-slope (local X after the -45 tilt).
    translate([porch_x0 - porch_len/2, 0, base_h - porch_len/2]) {
      // Display opening (inset 15mm/side from the adapter outline)
      rotate([0, -45, 0])
        translate([0, 0, -2*wall])
          rrect(disp_open_u, disp_open_w, 4*wall, disp_open_r);
      // 4x M3 adapter screw clearance holes (Ø3.4; nuts inside the porch
      // cavity). Match porch-tft-adapter.scad's ±60 (V) × ±23.5 (U).
      for (sv = [-1, 1], su = [-1, 1])
        translate([0, sv*disp_screw_v, 0]) rotate([0, -45, 0])
          translate([su*disp_screw_u, 0, 0])
            cylinder(d=3.4, h=4*wall, center=true);
    }

    // --- floor cuts (these stay in the difference: they cut the shell) ---
    // overload stop adjustment bore through the floor: the post's own
    // difference() cuts the post solid, but the shell floor (z=0..wall)
    // is a separate solid and must be cut here. Ø3.4 from z=-2 (exits
    // the base underside cleanly) up to z=wall+eps (meets the post bore).
    translate([lc_x0 + lc_len - 2.5, 0, -2])
      cylinder(d=3.4, h=wall + 2 + eps);
    // (The free-end bolt is reached via the overload-stop post's
    // through-bore, see the interior bosses section — no separate
    // calibration hole needed.)
    // rubber-foot recesses in the base floor underside (4 corners, away
    // from the load-cell column so foot squish never tilts the axis)
    for (sx=[-1,1], sy=[-1,1])
      translate([sx*(base_l/2 - 22), sy*(base_w/2 - 22), -eps])
        cylinder(d=10.4, h=1.2);
  }

  // ==================== INTERIOR BOSSES (added after the difference) ====
  // These all sit inside the main-cavity footprint, so they must be
  // unioned with the hollowed shell rather than placed in the difference
  // (where the cavity void would erase them). Each carries its own bore.

  // load cell fixed-end boss + cradle walls + M5 heat-set insert in top.
  // The M5 SHCS comes down through the load cell hole and the spacer pad
  // on the platform, threading into the insert here. Cradle walls prevent
  // rotation about the single bolt.
  // pillar reinforcement (issue #1): this boss flexed under scale load.
  // Bulk up the fore-aft (X) footprint — the direction it bends — on both
  // sides:
  //   - full-height extension rearward (-X) and wider ±Y,
  //   - forward (+X) buttress toward the free end, capped lc_beam_gap
  //     below the bar (z = lc_boss_h - lc_beam_gap) so the beam still
  //     deflects freely.
  // Only the load-cell beam should move. VERIFY in a render (expect
  // Volumes: 1) and tune the params.
  lc_boss_ext  = 10;   // rearward (-X) extension (full height)
  lc_boss_fwd  = 15;   // forward (+X) extension toward the free end
  lc_boss_wide = 3;    // extra width per ±Y side
  lc_beam_gap  = 5;    // clearance left below the bar for it to flex
  difference() {
    union() {
      // main boss: full height, extended -X, widened ±Y
      // (+X edge held at lc_x0+14 at full height for the M5 clamp)
      translate([lc_x0 - 4 - lc_boss_ext, -lc_w/2 - 4 - lc_boss_wide, 0])
        cube([18 + lc_boss_ext, lc_w + 8 + 2*lc_boss_wide, lc_boss_h]);
      // forward (+X) buttress: reaches toward the free end, stopping
      // lc_beam_gap below the bar so the beam stays free to flex
      translate([lc_x0 + 14, -lc_w/2 - 4 - lc_boss_wide, 0])
        cube([lc_boss_fwd, lc_w + 8 + 2*lc_boss_wide, lc_boss_h - lc_beam_gap]);
      // anti-rotation cradle walls (unchanged, atop the boss)
      for (sy = [-1, 1])
        translate([lc_x0 - 4, sy*(lc_w/2 + 0.05) + (sy < 0 ? -3 : 0), lc_boss_h])
          cube([18, 3, 5]);
    }
    // M5 heat-set insert pocket in the top of the boss (from above)
    translate([lc_x0 + lc_hole_from_end, 0, lc_boss_h - m5_ins_l])
      cylinder(d=m5_ins_d, h=m5_ins_l + eps);
  }

  // overload stop post — the stop SCREW sets the ~0.5mm gap under the
  // load cell's free end. An M3 heat-set insert goes in the TOP; the
  // screw threads down into it. The bore continues ALL THE WAY THROUGH
  // the post and the floor so the screw can be adjusted from below with
  // a hex key. NOTE: the floor segment of this bore is also cut in the
  // main shell difference() above (search "overload stop floor bore") —
  // the post difference() only cuts the post solid itself.
  difference() {
    translate([lc_x0 + lc_len - 2.5, 0, 0])
      cylinder(d=14, h=lc_boss_h - 6);
    // M3 heat-set insert pocket in the top (insert pressed in from above)
    translate([lc_x0 + lc_len - 2.5, 0, lc_boss_h - 6 - m3_ins_l])
      cylinder(d=m3_ins_d, h=m3_ins_l + eps);
    // adjustment through-bore: runs from z=-2 (below post base, so it punches
    // through the post solid at z=0..wall that was plugging the floor bore)
    // up to the insert pocket bottom. The main diff's floor bore covers z=-2..wall
    // in the SHELL; this bore covers z=-2..pocket_bottom in the POST SOLID.
    // Together they give a continuous channel from the base underside to the insert.
    translate([lc_x0 + lc_len - 2.5, 0, -2])
      cylinder(d=3.4, h=lc_boss_h - 6 - m3_ins_l + 2 + eps);
  }

  // tray mounting bosses + M3 heat-set inserts (insert pressed in from above)
  for (sx=[-1,1], sy=[-1,1])
    translate([pcb_pos[0] + sx*(pcb_l/2 - pcb_hole_inset),
               pcb_pos[1] + sy*(pcb_w/2 - pcb_hole_inset), wall-eps])
      difference() {
        cylinder(d=8, h=pcb_boss_h);
        translate([0,0,pcb_boss_h - m3_ins_l]) cylinder(d=m3_ins_d, h=m3_ins_l + eps);
      }

  // deck hold-down corner columns — added AFTER the difference so the
  // main cavity void can't erase them. Ø18 so each column MERGES into
  // both cavity walls at its corner, fusing with the shell for strength.
  // Top stops at z = base_h - 2*wall (3mm below the ledge underside) to
  // leave room for the deck's reinforcing pads, which hang down into the
  // rebate. M3 heat-set insert pocket in the top (insert pressed in from above).
  for (sx=[-1,1], sy=[-1,1])
    translate([sx*(base_l/2 - corner_r - 4), sy*(base_w/2 - corner_r - 4), wall])
      difference() {
        cylinder(d=18, h=base_h - 3*wall);             // top at z = base_h-2*wall = 54
        translate([0, 0, base_h - 3*wall - m3_ins_l])
          cylinder(d=m3_ins_d, h=m3_ins_l + eps);
      }

  // USB-C breakout floor bosses — two Ø8 posts rising from the floor,
  // X=75 (PCB -X edge ~72mm, connector +X edge reaches the inner rear wall at X=97),
  // Y = usbbo_world[1] ± usbbo_hole_dx/2 = -37 and -23.
  // Board lies flat on top of the bosses; connector faces the rear wall cutout.
  // Bosses clear the tray (+X edge at X=60) and the deck columns (nearest is
  // 24mm away). M3 heat-set inserts — same as tray bosses and standoffs.
  for (sy = [-1, 1])
    translate([75,
               usbbo_world[1] + sy * usbbo_hole_dx/2,
               wall - eps])
      difference() {
        cylinder(d=8, h=pcb_boss_h);
        // M3 clearance bore from base up to insert pocket bottom
        translate([0, 0, -eps]) cylinder(d=3.4, h=pcb_boss_h - m3_ins_l + eps);
        // M3 heat-set insert pocket in the top
        translate([0, 0, pcb_boss_h - m3_ins_l]) cylinder(d=m3_ins_d, h=m3_ins_l + eps);
      }
}

// =============================================================
// ELECTRONICS TRAY — modules + crimped jumper harnesses
// =============================================================
// Mounts on the base's four M3 bosses.
// Devkit is held by side rails (most S3 devkits have no mounting
// holes); NAU7802, OLED, and USB-C power breakout sit on standoff
// pairs with parametric hole spacing — MEASURE your modules.
devkit_l = 71;   devkit_w = 26;   devkit_pcb_t = 1.6;   // MEASURE
nau_hole_dx = 20.3; nau_hole_dy = 7.6;                  // MEASURE
usbbo_hole_dx = 14; usbbo_hole_dy = 0;                  // MEASURE (0 = single pair)
// USB-C breakout world position (rear wall, X = base_l/2).
// DECOUPLED from the tray: the breakout board mounts on two integral wall
// bosses on the rear interior — the tray only reaches X=60, 40mm short of
// the rear wall, so it can't carry this board.
// Y=-30 clears both rear-corner deck columns (which span Y=-67..-49 and Y=49..67).
usbbo_world = [base_l/2 - wall - 4, -30];  // X≈93 (boss face at inner wall), Y=-30
tray_t = 3;

module standoff_pair(dx, dy, h=7) {
  // Ø8 standoffs: gives 1.7mm wall around M3 insert (Ø4.6) — adequate in PETG.
  // h=7: insert pocket is 6.2mm deep, leaving a 0.8mm floor.
  // Insert pocket in the top; M3 clearance bore (Ø3.4) from z=0 to pocket bottom.
  // NOTE: if dy > 0 and dy < 8, adjacent standoffs will nearly touch at Ø8 —
  //   verify your board's hole spacing before using a 4-post layout.
  module one_standoff() {
    difference() {
      cylinder(d=8, h=h);
      // M3 clearance bore from base up to insert pocket bottom
      translate([0, 0, -eps]) cylinder(d=3.4, h=h - m3_ins_l + eps);
      // M3 heat-set insert pocket in the top
      translate([0, 0, h - m3_ins_l]) cylinder(d=m3_ins_d, h=m3_ins_l + eps);
    }
  }
  for (sx = [-1, 1])
    translate([sx*dx/2, dy/2, 0]) one_standoff();
  if (dy > 0)
    for (sx = [-1, 1])
      translate([sx*dx/2, -dy/2, 0]) one_standoff();
}

module tray() {
  difference() {
    rrect(pcb_l, pcb_w, tray_t, 4);
    // mount holes matching the base's M3 bosses
    for (sx=[-1,1], sy=[-1,1])
      translate([sx*(pcb_l/2 - pcb_hole_inset), sy*(pcb_w/2 - pcb_hole_inset), -eps])
        cylinder(d=3.4, h=tray_t + 2*eps);
    // zip-tie anchor slots for harness strain relief
    for (x = [-25, 0, 25])
      translate([x, 0, -eps]) rrect(3, 8, tray_t + 2*eps, 1);
  }
  // devkit side rails + end stops (friction fit, no holes needed)
  translate([0, pcb_w/2 - devkit_w/2 - 6, tray_t - eps]) {
    for (sy = [-1, 1])
      translate([0, sy*(devkit_w/2 + 1), 0])
        difference() {
          cube([devkit_l + 4, 3, 6], center=true);
          translate([0, -sy*1.0, 2.2]) cube([devkit_l + 6, 2, 2.2], center=true); // PCB slot
        }
    for (sx = [-1, 1])
      translate([sx*(devkit_l/2 + 1), 0, 0]) cube([3, 10, 4], center=true);
  }
  // NAU7802 standoffs (front-left of tray).
  // dy=0 → 2-post mount: at Ø8 standoffs the 7.6mm Y-spacing leaves only -0.4mm
  // gap, so the 4-post layout is skipped. If your NAU7802 board has a 2-hole
  // footprint (single row) confirm nau_hole_dx and set dy=0 here.
  // For a 4-hole board: reduce standoff OD in standoff_pair to Ø7 and restore dy=nau_hole_dy.
  translate([-pcb_l/2 + 16, -pcb_w/2 + 12, tray_t - eps])
    standoff_pair(nau_hole_dx, 0);
  // NOTE: USB-C breakout mounts on dedicated wall bosses in base(), not on
  // the tray — the tray +X edge (X=60) is 40mm short of the rear wall (X=97).
}

// =============================================================
// DECK — drop-in top plate. Prints flat, face-down, no supports.
// Closes the box corners the platform doesn't cover; its only
// opening is the load-cell slot, hidden in the platform's shadow.
// Rests on the base's rebate ledge; gravity + the platform above
// keep it seated (add screws to the ledge if you ever want them).
// =============================================================
module deck() {
  difference() {
    union() {
      rrect(base_l - 2*wall - 0.6, base_w - 2*wall - 0.6, wall, corner_r-1);
      // reinforcing pads UNDER the deck at each corner screw (hang into the
      // rebate gap, not up toward the platform) for solid countersink body
      for (sx=[-1,1], sy=[-1,1])
        translate([sx*(base_l/2 - corner_r - 4),
                   sy*(base_w/2 - corner_r - 4), -wall])
          cylinder(d=10, h=wall + eps);
    }
    // load-cell stack opening: assembly access for the cell-to-boss
    // bolts + heat-set inserts, and running clearance for the platform's
    // spacer pads. Stays within the platform's shadow.
    rrect(lc_len + 30, lc_w + 30, 3*wall, 4);
    // finger notch at the rear edge to lift the deck out
    translate([base_l/2 - wall - 8, 0, -wall - eps])
      cylinder(d=22, h=wall + 2*eps);
    // corner hold-down screws: M3 SHCS from above into the column inserts.
    // Clearance bore Ø3.4 through the full pad+deck stack (6mm + overshoot).
    // Counterbore Ø5.8 x 3.5 from the deck top for the socket head — starts
    // 0.5mm above the top surface and overshoots to avoid a near-coincident
    // face that would render as a film.
    for (sx=[-1,1], sy=[-1,1])
      translate([sx*(base_l/2 - corner_r - 4),
                 sy*(base_w/2 - corner_r - 4), 0]) {
        translate([0, 0, -wall - eps])
          cylinder(d=3.4, h=2*wall + 1);               // clearance, exits both faces
        translate([0, 0, wall - m3_cbore_h + 0.5])
          cylinder(d=m3_cbore_d, h=m3_cbore_h + 0.5); // SHCS counterbore, overshoots top
      }
  }
}

// =============================================================
// ASSEMBLY PREVIEW
// =============================================================
module assembly() {
  color("BurlyWood")  base();
  color("Tan")        translate([0,0,base_h-wall]) deck();
  color("SteelBlue")  translate([lc_x0, -lc_w/2, lc_boss_h]) cube([lc_len, lc_w, lc_h]);
  color("DarkSeaGreen") translate([0,0,plat_z]) platform();
  color("SeaGreen")    translate([0,0,plat_z + plat_t]) spigot();
}

if (part == "platform")  platform();
else if (part == "base") base();
else if (part == "deck") deck();
else if (part == "spigot") spigot();
else if (part == "tray") tray();
else assembly();
