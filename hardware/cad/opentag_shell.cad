// ---------------------------------------------------------------------------
// OpenTag handheld shell — parametric, two printed parts + button caps.
//
//   part = "both" | "front" | "back"
//
// FRONT SHELL : OLED window, three button holes, USB + power-switch slots,
//               screw bosses.
// BACK PLATE  : carrier-PCB standoffs, PN532Killer snap bay with thinned
//               NFC tap window, LiPo bay with retainers, power-module tray.
//
// Stack-up (z, from the back plate's inner face):
//   0 .. 15   under-PCB volume: female headers (8.5) + DevKitC board +
//             components (USB ~3.2 mm deep)
//   15 .. 16.6  carrier PCB
//   16.6 .. 27.1 OLED module on its 1x7 female header, buttons (~8.5 tall)
//   30        front inner wall
//
// The DevKitC (69.7 x 25.4) hangs under the middle of the carrier; the
// PN532Killer, battery and power modules live in the south bay where nothing
// sits above them. Tap zone = lower-left of the back.
//
// All dimensions in millimetres. Measure your modules and adjust before
// printing; every position derives from the parameters below.
// ---------------------------------------------------------------------------

part = "both";
$fn = 48;

// ---- shell ------------------------------------------------------------------
wall        = 2.4;    // general wall
floor_t     = 2.0;    // back plate
inner_w     = 94;     // interior width (carrier PCB is 90 wide)
inner_l     = 132;    // interior length (PCB zone + south bay)
inner_d     = 30;     // interior depth
corner      = 8;      // exterior corner radius

// ---- carrier PCB (90 x 60, holes at ±41.5 / ±25.5) ---------------------------
pcb_w = 90; pcb_l = 60; pcb_t = 1.6;
north_gap = 6;
pcb_y = -inner_l/2 + north_gap + pcb_l/2;   // PCB centre line
standoff_h = 15;                            // plate -> PCB underside
pcb_hole_dx = 41.5; pcb_hole_dy = 25.5;

// ---- OLED (1.3" SSD1351 module ~37.5 x 34.5) ---------------------------------
oled_win   = 30;                 // display area is ~26.9 mm
oled_cx    = -1.38;              // module centre, from the carrier layout
oled_cy    = pcb_y - 6;
oled_mod_h = 10.5;               // header (8.5) + module PCB

// ---- buttons (carrier y = +17) --------------------------------------------------
btn_dy  = pcb_y + 17;
btn_dx  = 18;                    // -18 / 0 / +18
btn_hole = 6.4;                  // 6 mm tactile + clearance
cap_d = 9; cap_h = 2.2; cap_collar = 7.5;   // printed cap

// ---- DevKitC edge details --------------------------------------------------------
usb_slot_y = pcb_y;              // both USB ports, west end of the DevKit
usb_slot_l = 24; usb_slot_z0 = 1; usb_slot_z1 = 5.5;
sw_slot_y  = pcb_y + 14.5;       // power toggle, right wall
sw_slot_l  = 10;  sw_slot_z0 = 16; sw_slot_z1 = 25;

// ---- south bay (nothing above it) -------------------------------------------------
bay_y0 = pcb_y + pcb_l/2 + 2;    // = 7
// PN532Killer bay — MEASURE YOUR BOARD (default ~42 x 32)
kil_l = 42; kil_w = 32; kil_t = 2.6;
kil_cx = -24; kil_cy = bay_y0 + 19;         // bay centre
tap_win = 30; tap_t = 1.0;                  // thinned window for NFC
kil_post = 5; kil_lip = 1.6; kil_post_h = kil_t + 2.2;
// battery bay (default 805040: 50 long x 40 wide x 8 thick, lying flat)
bat_l = 52; bat_w = 42; bat_t = 9;
bat_cx = 22; bat_cy = bay_y0 + 34;   // keeps heel inside the interior
// power module tray (TP4056 ~26x17, MT3608 ~21x14, side by side)
tray_cx = -23; tray_cy = bay_y0 + 51; tray_w = 47; tray_d = 18;

// ---- derived ----------------------------------------------------------------------
outer_w = inner_w + 2*wall;
outer_l = inner_l + 2*wall;
outer_d = inner_d + wall;
oled_top = standoff_h + pcb_t + oled_mod_h;

module rbox(w, l, h, r) {           // rounded box, centred x/y, base z=0
  hull() for (x = [-1, 1], y = [-1, 1])
    translate([x*(w/2 - r), y*(l/2 - r), 0]) cylinder(r = r, h = h);
}

// ---------------------------------------------------------------------------
// FRONT SHELL (prints face-down)
// ---------------------------------------------------------------------------
module front_shell() {
  difference() {
    rbox(outer_w, outer_l, outer_d, corner);
    translate([0, 0, -0.01]) rbox(inner_w, inner_l, inner_d + 0.02, max(corner - wall, 1));
    // OLED window with chamfered bezel on the outside (top face)
    translate([oled_cx, oled_cy, inner_d - 0.01])
      cylinder(d1 = oled_win, d2 = oled_win + 2*1.2, h = wall + 0.02);
    // button holes
    for (x = [-btn_dx, 0, btn_dx])
      translate([x, btn_dy, inner_d - 0.01]) cylinder(d = btn_hole, h = wall + 0.02);
    // DevKit USB slot, left wall
    translate([-outer_w/2 - 0.01, usb_slot_y - usb_slot_l/2, usb_slot_z0])
      cube([wall + 0.02, usb_slot_l, usb_slot_z1 - usb_slot_z0]);
    // power switch slot, right wall
    translate([inner_w/2 - 0.01, sw_slot_y - sw_slot_l/2, sw_slot_z0])
      cube([wall + 0.02, sw_slot_l, sw_slot_z1 - sw_slot_z0]);
  }
  // OLED side rails: clamp the module against the carrier socket
  for (sx = [-1, 1])
    translate([oled_cx + sx*(37.5/2 + 0.8) - 0.8, oled_cy - 34.5/2, oled_top])
      cube([1.6, 34.5, inner_d - oled_top]);
  // screw bosses (M3 from the back plate)
  for (x = [-1, 1], y = [-1, 1])
    translate([x*(outer_w/2 - 6), y*(outer_l/2 - 6), 0])
      difference() {
        cylinder(d = 8, h = 10);
        cylinder(d = 2.8, h = 12);
      }
}

// ---------------------------------------------------------------------------
// BACK PLATE (prints flat-side-down)
// ---------------------------------------------------------------------------
module back_plate() {
  difference() {
    union() {
      translate([0, 0, -floor_t]) rbox(outer_w, outer_l, floor_t, corner);
      // mating lip
      difference() {
        translate([0, 0, -0.01]) rbox(inner_w + 2*1.2, inner_l + 2*1.2, 1.8, max(corner - 3, 1));
        translate([0, 0, -0.1]) rbox(inner_w - 0.4, inner_l - 0.4, 2.2, max(corner - 4, 1));
      }
      // PCB standoffs (M3, screws from the top through the PCB)
      for (x = [-1, 1], y = [-1, 1])
        translate([x*pcb_hole_dx, pcb_y + y*pcb_hole_dy, 0])
          difference() {
            cylinder(d = 7.5, h = standoff_h);
            translate([0, 0, standoff_h - 5.5]) cylinder(d = 2.8, h = 6);
          }
      // PN532Killer snap bay
      for (qx = [-1, 1], qy = [-1, 1]) {
        px = kil_cx + qx*kil_l/2; py = kil_cy + qy*kil_w/2;
        translate([px - kil_post/2 - (qx < 0 ? kil_lip : 0),
                   py - kil_post/2 - (qy < 0 ? kil_lip : 0), 0])
          cube([kil_post + (qx < 0 ? kil_lip : 0),
                kil_post + (qy < 0 ? kil_lip : 0), kil_post_h]);
      }
      // battery retainers: two side lips + toe + heel
      for (sx = [-1, 1])
        translate([bat_cx + sx*bat_w/2 - (sx < 0 ? 3 : 0),
                   bat_cy - bat_l/2, 0]) cube([3, bat_l, 7]);
      translate([bat_cx - bat_w/2 - 3, bat_cy - bat_l/2 - 3, 0])
        cube([bat_w + 6, 3, 5]);
      translate([bat_cx - bat_w/2 - 3, bat_cy + bat_l/2, 0])
        cube([bat_w + 6, 3, 5]);
      // power module tray with zip-tie slots
      translate([tray_cx - tray_w/2, tray_cy - tray_d/2, 0]) {
        for (mx = [0, 1]) translate([mx*(tray_w - 2), 0, 0]) cube([2, tray_d, 5]);
        for (my = [0, 1]) translate([0, my*(tray_d - 2), 0]) cube([tray_w, 2, 5]);
        for (yy = [3.5, tray_d - 6])
          translate([7, yy, -0.01]) cube([tray_w - 14, 2.6, 2.02]);
      }
    }
    // NFC tap window: thin the plate from the inside
    translate([kil_cx - tap_win/2, kil_cy - tap_win/2, tap_t - floor_t])
      cube([tap_win, tap_win, floor_t - tap_t + 0.01]);
    // M3 screw holes, countersunk from the outside
    for (x = [-1, 1], y = [-1, 1])
      translate([x*(outer_w/2 - 6), y*(outer_l/2 - 6), 0]) {
        translate([0, 0, -floor_t - 0.01]) cylinder(d = 3.2, h = floor_t + 0.02);
        translate([0, 0, -floor_t - 0.01]) cylinder(d1 = 6.4, d2 = 3.2, h = 1.6);
      }
  }
}

// ---------------------------------------------------------------------------
// Button caps (print 3)
// ---------------------------------------------------------------------------
module button_cap() {
  union() {
    cylinder(d = cap_d, h = cap_h);
    translate([0, 0, -cap_collar]) cylinder(d = btn_hole - 0.6, h = cap_collar);
  }
}

module assembly_front() { front_shell(); }

if (part == "front") assembly_front();
if (part == "back") back_plate();
if (part == "both") {
  translate([-outer_w/2 - 14, 0, outer_d]) rotate([180, 0, 0]) assembly_front();
  translate([outer_w/2 + 14, 0, 0]) back_plate();
  for (i = [-1, 0, 1])
    translate([i*15, outer_l/2 + 12, 0]) button_cap();
}

// Print notes:
//  * front shell: face down; the OLED rails and bosses need no supports.
//  * back plate : flat side down; snap-lip overhangs are 1.6 mm (printable).
//  * Use PLA/PETG; keep carbon- or metal-filled filaments away from the NFC
//    tap zone. Verify the killer bay size against your PN532Killer PCB and
//    the USB slot position against your DevKitC revision before printing.
