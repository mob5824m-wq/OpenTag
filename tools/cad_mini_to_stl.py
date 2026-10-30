#!/usr/bin/env python3
"""Export the OpenTag Mini shell CAD to binary STL without OpenSCAD.

Same approach as tools/cad_to_stl.py (which it imports its primitives from):
all top-level parameters are parsed from hardware/cad/opentag_mini_shell.cad
so the numbers stay in sync; the module *structure* is mirrored below. If
you change geometry structure in the .cad, mirror the change here — or,
with OpenSCAD installed, regenerate canonically:

    openscad -o front.stl -D 'part="front"' hardware/cad/opentag_mini_shell.cad
    openscad -o back.stl  -D 'part="back"'  hardware/cad/opentag_mini_shell.cad
    openscad -o cap.stl   -D 'part="cap"'   hardware/cad/opentag_mini_shell.cad

Outputs (millimetres, binary STL):
    hardware/cad/stl/opentag_mini_front_shell.stl
    hardware/cad/stl/opentag_mini_back_plate.stl
    hardware/cad/stl/opentag_mini_button_cap.stl

Deps:  pip install trimesh manifold3d scipy rtree
"""
import re
import sys
from pathlib import Path

import trimesh

sys.path.insert(0, str(Path(__file__).resolve().parent))
from cad_to_stl import (T, cube, cyl, cyl_x, rbox, frustum, union, difference,
                        parse_params, parse_brand, body_count)

ROOT = Path(__file__).resolve().parent.parent
CAD = ROOT / "hardware" / "cad" / "opentag_mini_shell.cad"
OUT_DIR = ROOT / "hardware" / "cad" / "stl"


def parse_text(path, name):
    """A top-level `name = "TEXT";` string from the .cad (brand/mini marks)."""
    m = re.search(rf'{name}\s*=\s*"([^"]*)"', path.read_text())
    return m.group(1) if m else ""


def text_boxes(p, text, px, depth, cx, cy):
    """Engraved pixel boxes, mirroring the .cad's text_mark(): laid out in
    upright text space then rotated 180 deg about the mark's centre (it
    reads with the USB/north end up). Each box spans z = outer_d - depth ..
    outer_d + 0.01 (flush floor, overshoot outside the face). Same 0.02 mm
    diagonal-pixel overlap as the handheld's wordmark."""
    font = p["_brand_font"]
    n = len(text)
    W = (n * 6 - 1) * px
    eps = 0.02
    boxes = []
    for i, ch in enumerate(text):
        cols = font.get(ch)
        if ch == " " or cols is None:
            continue
        for c in range(5):
            for r in range(8):
                if (cols[c] >> r) & 1:
                    ux, uy = (i * 6 + c) * px, (6 - r) * px
                    fx = cx + W / 2 - ux - px - eps
                    fy = cy + 4 * px - uy - px - eps
                    boxes.append(T(cube(px + eps, px + eps, depth + 0.01),
                                   [fx, fy, p["outer_d"] - depth]))
    return boxes


def wall_slot(p, y, l, z0, z1, west):
    """Wall slot cut through BOTH walls (a cut ending flush on the cavity
    face leaves a zero-thickness cap in manifold3d), then the opposite wall
    is plugged back — same pattern as the handheld's front shell."""
    thru = T(cube(p["outer_w"] + 0.03, l, z1 - z0),
             [-p["outer_w"] / 2 - 0.01, y - l / 2, z0])
    plug = T(cube(p["wall"], l, z1 - z0),
             [(p["inner_w"] / 2) if west else (-p["outer_w"] / 2),
              y - l / 2, z0])
    return thru, plug


def front_shell(p):
    outer = rbox(p["outer_w"], p["outer_l"], p["outer_d"], p["corner"])
    inner = T(rbox(p["inner_w"], p["inner_l"], p["inner_d"] + 0.02,
                   max(p["corner"] - p["wall"], 1)), [0, 0, -0.01])
    ceiling = p["inner_d"] + 0.01          # cavity cut top plane
    # the single button hole (top face, overshoots 0.02 above the face)
    btn = T(cyl(p["btn_hole"], p["wall"] + 0.02),
            [p["btn_x"], p["btn_y"], ceiling])
    # wall slots: USB west (DevKitC), power switch + charge port east
    usb_thru, usb_plug = wall_slot(p, p["usb_slot_y"], p["usb_slot_l"],
                                    p["usb_slot_z0"], p["usb_slot_z1"], west=True)
    sw_thru, sw_plug = wall_slot(p, p["sw_slot_y"], p["sw_slot_l"],
                                  p["sw_slot_z0"], p["sw_slot_z1"], west=False)
    chg_thru, chg_plug = wall_slot(p, p["chg_slot_y"], p["chg_slot_l"],
                                    p["chg_slot_z0"], p["chg_slot_z1"], west=False)
    # charge-status light pipe: bore along x through the east wall, just
    # north of the charge slot (through BOTH walls + west plug, see above)
    lp_thru = T(cyl_x(p["lp_d"], p["outer_w"] + 0.03),
                [-p["outer_w"] / 2 - 0.01, p["lp_y"], p["lp_z"]])
    lp_plug = T(cyl_x(p["lp_d"], p["wall"]),
                [-p["outer_w"] / 2, p["lp_y"], p["lp_z"]])
    # lanyard tab: bored through, fuses with the outer box
    lug = difference([T(rbox(p["lug_w"], p["lug_l"], p["outer_d"], p["lug_r"]),
                        [p["lug_cx"], p["lug_cy"], 0]),
                      T(cyl(p["lug_hole"], p["outer_d"] + 0.02),
                        [p["lug_cx"], p["lug_cy"], -0.01])])
    marks = (text_boxes(p, p["_brand_text"], p["brand_px"], p["brand_depth"],
                        p["brand_cx"], p["brand_cy"]) +
             text_boxes(p, p["_mini_text"], p["mini_px"], p["mini_depth"],
                        p["mini_cx"], p["mini_cy"]))
    shell = union([difference([outer, inner, btn,
                               usb_thru, sw_thru, chg_thru, lp_thru] + marks),
                   usb_plug, sw_plug, chg_plug, lp_plug, lug])
    # battery clamp rails + end stops (fused +0.02 into the ceiling; the
    # +0.02 is required or the rails print as loose pieces — see the .cad)
    rails = [T(cube(p["rail_w"], p["rail_y1"] - p["rail_y0"],
                    p["inner_d"] - p["rail_z0"] + 0.02),
               [p["bat_cx"] + sx * (p["bat_w"] / 2 + p["rail_w"] / 2) - p["rail_w"] / 2,
                p["rail_y0"], p["rail_z0"]]) for sx in (-1, 1)]
    stops = [T(cube(p["bat_w"] + 2 * p["rail_w"], 1.2,
                    p["inner_d"] - p["rail_z0"] + 0.02),
               [p["bat_cx"] - p["bat_w"] / 2 - p["rail_w"],
                (p["rail_y0"] - 1.2) if sy < 0 else p["rail_y1"],
                p["rail_z0"]]) for sy in (-1, 1)]
    # screw bosses (hole flush with the boss top)
    bosses = []
    for x in (-1, 1):
        for y in (-1, 1):
            at = [x * p["screw_dx"], y * p["screw_dy"], 0]
            bosses.append(difference([T(cyl(7, 10), at), T(cyl(2.8, 10), at)]))
    return union([shell] + rails + stops + bosses)


def back_plate(p):
    plate = T(rbox(p["outer_w"], p["outer_l"], p["floor_t"], p["corner"]),
              [0, 0, -p["floor_t"]])
    # mating lip: four segments (windows clear the DevKitC tray, the Killer
    # posts, the power trays and the screw bosses — see the .cad notes)
    lips = []
    for sy in (-1, 1):
        y0 = sy * (p["inner_l"] / 2 - 0.2) - (0 if sy < 0 else 1.2)
        lips.append(T(cube(p["inner_w"] - 18, 1.2, 1.8),
                      [-(p["inner_w"] / 2 - 9), y0, -0.01]))
    lips.append(T(cube(1.2, 26, 1.8), [p["inner_w"] / 2 - 1.4, -26, -0.01]))
    lips.append(T(cube(1.2, 26, 1.8), [-p["inner_w"] / 2 + 0.2, -26, -0.01]))
    # DevKitC tray: north wall + two x walls (stopping short of the north
    # screw bosses) + a centre south stop between the battery rails
    dw, dl, dg, dt = (p["dev_w"], p["dev_l"], p["dev_gap"], p["dev_wall"])
    tray = [T(cube(dw + 2 * (dg + dt), dt, p["dev_rim"]),
              [p["dev_cx"] - dw / 2 - dg - dt,
               p["dev_cy"] - dl / 2 - dg - dt, 0])]
    for sx in (-1, 1):
        tray.append(T(cube(dt, dl + 2 * dg, p["dev_rim"]),
                      [p["dev_cx"] + sx * (dw / 2 + dg) - (dt if sx < 0 else 0),
                       p["dev_cy"] - dl / 2 - dg, 0]))
    tray.append(T(cube(p["dev_stop_w"], dt, p["dev_rim"]),
                  [p["dev_cx"] - p["dev_stop_w"] / 2,
                   p["dev_cy"] + dl / 2 + dg, 0]))
    # PN532Killer snap bay posts (42 along x, 32 along y — same board as the
    # handheld) + anti-slide nubs at the edge midpoints
    kil_posts = []
    for qx in (-1, 1):
        for qy in (-1, 1):
            px = p["kil_cx"] + qx * p["kil_l"] / 2
            py = p["kil_cy"] + qy * p["kil_w"] / 2
            ex = p["kil_lip"] if qx < 0 else 0
            ey = p["kil_lip"] if qy < 0 else 0
            kil_posts.append(T(cube(p["kil_post"] + ex, p["kil_post"] + ey,
                                    p["kil_post_h"]),
                               [px - p["kil_post"] / 2 - ex,
                                py - p["kil_post"] / 2 - ey, 0]))
    kn, knh = p["kil_nub"], p["kil_nub_h"]
    nubs = [T(cube(kn, kn, knh),
              [p["kil_cx"] + nx * (p["kil_l"] / 2 + 0.8) - kn / 2,
               p["kil_cy"] - kn / 2, 0]) for nx in (-1, 1)]
    nubs += [T(cube(kn, kn, knh),
               [p["kil_cx"] - kn / 2,
                p["kil_cy"] + ny * (p["kil_w"] / 2 + 0.8) - kn / 2, 0])
             for ny in (-1, 1)]

    def module_tray(cx, cy, w, d, notch_east=False):
        """Walls-only tray (the module rests on the plate) + two zip-tie
        bars. When notch_east, the east wall is notched behind the charge
        slot so the TP4056's USB-C and charge LEDs reach the shell slot."""
        tw, th = p["tray_wall"], p["tray_h"]
        tx, ty = cx - w / 2 - tw, cy - d / 2 - tw
        parts = [T(cube(tw, d + 2 * tw, th), [tx, ty, 0]),                      # west
                 T(cube(w + 2 * tw, tw, th), [tx, ty, 0]),                      # north
                 T(cube(w + 2 * tw, tw, th), [tx, ty + tw + d, 0])]             # south
        east = T(cube(tw, d + 2 * tw, th), [tx + tw + w, ty, 0])
        if notch_east:
            east = difference([east,
                               T(cube(tw + 0.02, p["chg_slot_l"], th + 0.02),
                                 [tx + tw + w - 0.01,
                                  p["chg_slot_y"] - (cy - d / 2) - p["chg_slot_l"] / 2,
                                  -0.01])])
        parts.append(east)
        parts += [T(cube(w + 2 * tw - 14, 2.6, 2.02), [tx + 7, ty + yy, -0.01])
                  for yy in (3.5, d - 6)]
        return parts

    # the button pedestal (the tactile switch is foam-taped on top)
    pedestal = T(cube(p["btn_post"], p["btn_post"], p["btn_post_h"]),
                 [p["btn_x"] - p["btn_post"] / 2, p["btn_y"] - p["btn_post"] / 2, 0])
    # lanyard tab: same footprint as the front shell's, flush at z = 0
    lug = T(rbox(p["lug_w"], p["lug_l"], p["floor_t"], p["lug_r"]),
            [p["lug_cx"], p["lug_cy"], -p["floor_t"]])
    body = union([plate, lug] + lips + tray + kil_posts + nubs
                 + module_tray(p["tp_cx"], p["tp_cy"], p["tp_w"], p["tp_d"],
                               notch_east=True)
                 + module_tray(p["mt_cx"], p["mt_cy"], p["mt_w"], p["mt_d"])
                 + [pedestal])
    cuts = []
    # lanyard bore (overshoots up into the front tab's bore region only)
    cuts.append(T(cyl(p["lug_hole"], p["floor_t"] + 0.02),
                  [p["lug_cx"], p["lug_cy"], -p["floor_t"] - 0.01]))
    # light-pipe rod seat through the TP tray's east wall (overshoot into
    # the gap between tray wall and shell wall — a wall-local cut would be
    # refilled by the wall's own union)
    cuts.append(T(cyl_x(p["lp_tray_d"], p["tray_wall"] + 0.02),
                  [p["tp_cx"] + p["tp_w"] / 2 - 0.01, p["lp_y"], p["lp_z"]]))
    # NFC tap window: fully OPEN over the antenna, through-cut from below
    cuts.append(T(cyl(p["tap_win"], p["floor_t"] + 0.02),
                  [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]))
    # controller cradle: shallow outside recess centred on the window
    cuts.append(T(cyl(p["cradle_d"], p["cradle_t"] + 0.01),
                  [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]))
    # service window over the DevKitC's BOOT/RST end
    cuts.append(T(rbox(p["svc_w"], p["svc_l"], p["floor_t"] + 0.02, p["svc_r"]),
                  [p["svc_cx"], p["svc_cy"], -p["floor_t"] - 0.01]))
    # charger/boost vents through the plate, between each tray's tie bars
    for cx, cy, l in ((p["tp_cx"], p["tp_cy"], p["tp_vent_l"]),
                      (p["mt_cx"], p["mt_cy"], p["mt_vent_l"])):
        cuts.append(T(cube(l, p["vent_w"], p["floor_t"] + 0.02),
                      [cx - l / 2, cy - p["vent_w"] / 2, -p["floor_t"] - 0.01]))
    # engraved halo ring around the cradle (annular groove, outer face)
    cuts.append(difference([
        T(cyl(p["halo_d"], p["halo_t"] + 0.01),
          [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]),
        T(cyl(p["halo_d"] - 2 * p["halo_w"], p["halo_t"] + 0.03),
          [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.02])]))
    # M3 screw holes, countersunk from the outside
    for x in (-1, 1):
        for y in (-1, 1):
            at = [x * p["screw_dx"], y * p["screw_dy"]]
            z0 = -p["floor_t"] - 0.01
            cuts.append(T(cyl(3.2, p["floor_t"] + 0.01), [at[0], at[1], z0]))
            cuts.append(T(frustum(6.4, 3.2, 1.6), [at[0], at[1], z0]))
    return difference([body] + cuts)


def button_cap(p):
    head = cyl(p["cap_d"], p["cap_h"])
    skirt = T(cyl(p["cap_skirt_o"], p["cap_skirt"]), [0, 0, -p["cap_skirt"]])
    fz = -(p["cap_skirt"] + p["cap_flange_t"])
    flange = T(cyl(p["cap_flange_d"], p["cap_flange_t"]), [0, 0, fz])
    # pusher rod reaches 0.4 mm past the flange into the skirt so it fuses
    # volumetrically with both (a flush end would only face-touch)
    pusher = T(cyl(p["cap_push_d"],
                   p["cap_push"] + p["cap_flange_t"] + 0.4),
               [0, 0, fz - p["cap_push"]])
    return union([head, skirt, flange, pusher])


# ---------------------------------------------------------------------------
# Verification: watertight, single body, expected envelope, feature probes,
# closed-position interference — the same gates the handheld shell passes.
# ---------------------------------------------------------------------------

def check(mesh, name, vol_lo, vol_hi):
    if not mesh.is_watertight:
        sys.exit(f"{name}: not watertight")
    if mesh.is_empty:
        sys.exit(f"{name}: empty mesh")
    n = body_count(mesh)
    if n != 1:
        sys.exit(f"{name}: {n} disconnected bodies (expected 1)")
    vol = mesh.volume / 1000.0
    if not (vol_lo <= vol <= vol_hi):
        sys.exit(f"{name}: volume {vol:.1f} cm^3 outside [{vol_lo}, {vol_hi}]")
    print(f"  {name}: watertight, 1 body, {vol:.1f} cm^3")
    return True


def probe(mesh, name, points_inside, points_outside):
    """Point-membership probes: solid where expected, open where expected."""
    inside = mesh.contains(points_inside)
    for pt, got in zip(points_inside, inside):
        if not got:
            sys.exit(f"{name}: expected solid at {pt} — feature missing")
    outside = mesh.contains(points_outside)
    for pt, got in zip(points_outside, outside):
        if got:
            sys.exit(f"{name}: expected open at {pt} — cut missing")
    print(f"  {name}: {len(points_inside)}+{len(points_outside)} probes OK")


def verify(p, front, back, cap):
    ok = True
    print("verify: volumes (bounds) + watertight + single body")
    ok &= check(front, "front shell", 18.0, 34.0)
    ok &= check(back, "back plate", 10.0, 22.0)
    ok &= check(cap, "button cap", 0.05, 0.6)

    ft = p["floor_t"]
    print("verify: envelopes")
    for mesh, name in ((front, "front"), (back, "back")):
        lo, hi = mesh.bounds
        if not (abs(hi[0] - p["outer_w"] / 2) < 0.05 and
                abs(lo[0] - (p["lug_cx"] - p["lug_w"] / 2)) < 0.05 and
                abs(hi[1] - p["outer_l"] / 2) < 0.05 and
                abs(lo[1] + p["outer_l"] / 2) < 0.05):
            sys.exit(f"{name}: unexpected XY envelope {lo} .. {hi}")
    zhi, zlo = front.bounds[1][2], front.bounds[0][2]
    if abs(zhi - p["outer_d"]) > 0.05 or abs(zlo) > 0.05:
        sys.exit(f"front: unexpected Z envelope {zlo} .. {zhi}")
    zhi, zlo = back.bounds[1][2], back.bounds[0][2]
    # the plate floor tops at z = 0; the DevKitC tray walls rise dev_rim
    if abs(zlo + ft) > 0.05 or abs(zhi - p["dev_rim"]) > 0.05:
        sys.exit(f"back: unexpected Z envelope {zlo} .. {zhi}")
    print("  envelopes OK")

    print("verify: features")
    # front shell: button hole open, USB/charge/switch slots open, light-pipe
    # bore open, cavity open, boss bore open; rails + boss + lug solid
    probe(front, "front",
          [[p["bat_cx"] + p["bat_w"] / 2 + p["rail_w"] / 2, p["bat_cy"],
            p["rail_z0"] + 0.4],                                   # east rail
           [p["screw_dx"] + 2.0, p["screw_dy"], 5.0],              # boss solid
           [p["lug_cx"] - p["lug_w"] / 2 + 2.5,
            p["lug_cy"] + p["lug_l"] / 2 - 4.0, 8.0],              # lug corner
           [0, p["outer_l"] / 2 - p["wall"] / 2, 8.0]],            # south wall
          [[p["btn_x"], p["btn_y"], p["inner_d"] + 0.3],           # button hole
           [-p["outer_w"] / 2 + p["wall"] / 2, p["usb_slot_y"], 2.5],  # USB slot
           [p["outer_w"] / 2 - p["wall"] / 2, p["sw_slot_y"], 8.0],    # switch slot
           [p["outer_w"] / 2 - p["wall"] / 2, p["chg_slot_y"], 2.5],   # charge slot
           [p["outer_w"] / 2 - p["wall"] / 2, p["lp_y"], p["lp_z"]],   # lp bore
           [0, 0, 7.0],                                            # cavity
           [p["screw_dx"], p["screw_dy"], 5.0]])                   # boss bore
    # back plate: tap window/cradle/service window/vents/screw holes open;
    # pedestal, Killer post, tray wall, lip, floor solid
    probe(back, "back",
          [[p["btn_x"], p["btn_y"], p["btn_post_h"] / 2],          # pedestal
           [p["kil_cx"] - p["kil_l"] / 2 + 2.0, p["kil_cy"] - p["kil_w"] / 2 + 2.0,
            1.0],                                                  # Killer post
           [p["tp_cx"] - p["tp_w"] / 2 - p["tray_wall"] / 2, p["tp_cy"], 2.0],  # tray wall
           [p["mt_cx"] - p["mt_w"] / 2 - p["tray_wall"] / 2,
            p["mt_cy"], 2.0],                                      # MT tray wall
           [0, -(p["inner_l"] / 2 - 0.6), 0.9],                    # north lip
           [0, 0, -ft / 2]],                                       # floor
          [[p["kil_cx"], p["kil_cy"], -ft / 2],                    # tap window
           [p["kil_cx"], p["kil_cy"], -ft - 0.01 - 0.3],           # cradle recess
           [p["svc_cx"], p["svc_cy"], -ft / 2],                    # service window
           [p["tp_cx"], p["tp_cy"], -ft / 2],                      # TP vent
           [p["mt_cx"], p["mt_cy"], -ft / 2],                      # MT vent
           [p["screw_dx"], p["screw_dy"], -ft / 2],                # screw hole
           [p["kil_cx"], p["kil_cy"], 1.0]])                       # bay interior
    # the cap: head + skirt solid on axis, open beyond the pusher rod tip
    probe(cap, "cap",
          [[0, 0, p["cap_h"] / 2],
           [0, 0, -p["cap_skirt"] + 0.5]],
          [[p["cap_d"] / 2 + 1.0, 0, p["cap_h"] / 2],
           [0, 0, -(p["cap_skirt"] + p["cap_flange_t"] + p["cap_push"]) - 0.2]])

    print("assembly check: front/back closed-position interference")
    import warnings
    with warnings.catch_warnings():
        warnings.simplefilter("ignore", RuntimeWarning)
        inter = trimesh.boolean.intersection([front, back], engine="manifold")
    vol = 0.0 if inter.is_empty else inter.volume
    # The halves clamp face-to-face at z=0 (rim, boss bottoms), which the
    # intersection reports as zero-thickness contact patches, and
    # near-tangent surfaces produce degenerate sub-0.1 mm^3 fins. Anything
    # above 0.5 mm^3 is a real collision.
    if vol > 0.5:
        sys.exit(f"front and back plates interfere by {vol:.2f} mm^3 "
                 "in the closed position — STL not written")
    print(f"  interference volume {vol:.3f} mm^3 (contact patches only)")
    return ok


def main():
    p = parse_params(CAD)
    p["_brand_text"], p["_brand_font"] = parse_brand(CAD)
    p["_mini_text"] = parse_text(CAD, "mini_text")
    need = ("outer_w", "outer_l", "outer_d", "inner_w", "inner_l", "inner_d",
            "wall", "floor_t", "corner", "screw_dx", "screw_dy",
            "dev_w", "dev_l", "dev_cx", "dev_cy", "dev_gap", "dev_wall",
            "dev_rim", "dev_stop_w",
            "bat_l", "bat_w", "bat_t", "bat_cx", "bat_cy",
            "rail_w", "rail_t", "rail_z0", "rail_y0", "rail_y1",
            "kil_l", "kil_w", "kil_cx", "kil_cy", "kil_post", "kil_lip",
            "kil_post_h", "kil_nub", "kil_nub_h", "tap_win", "cradle_d",
            "cradle_t", "halo_d", "halo_w", "halo_t",
            "tray_wall", "tray_h", "tp_w", "tp_d", "tp_cx", "tp_cy",
            "mt_w", "mt_d", "mt_cx", "mt_cy", "tp_vent_l", "mt_vent_l",
            "vent_w", "usb_slot_y", "usb_slot_l", "usb_slot_z0", "usb_slot_z1",
            "sw_slot_y", "sw_slot_l", "sw_slot_z0", "sw_slot_z1",
            "chg_slot_y", "chg_slot_l", "chg_slot_z0", "chg_slot_z1",
            "lp_d", "lp_tray_d", "lp_y", "lp_z",
            "btn_x", "btn_y", "btn_hole", "btn_post", "btn_post_h",
            "cap_d", "cap_h", "cap_proud", "cap_skirt_o", "cap_skirt",
            "cap_flange_d", "cap_flange_t", "cap_push", "cap_push_d",
            "svc_w", "svc_l", "svc_r", "svc_cx", "svc_cy",
            "brand_px", "brand_depth", "brand_cx", "brand_cy",
            "mini_px", "mini_depth", "mini_cx", "mini_cy",
            "lug_cx", "lug_cy", "lug_w", "lug_l", "lug_r", "lug_hole")
    missing = [k for k in need if k not in p]
    if missing:
        sys.exit(f"parameters not parsed from {CAD}: {missing}")
    print(f"parsed {len(p)} parameters + brand \"{p['_brand_text']}\" / "
          f"\"{p['_mini_text']}\" ({len(p['_brand_font'])} glyphs) "
          f"from {CAD.name}")
    print(f"envelope: {p['outer_w']:.1f} x {p['outer_l']:.1f} x "
          f"{p['outer_d']:.1f} mm (handheld: 97.6 x 135.6 x 29.3)")

    parts = {
        "opentag_mini_front_shell.stl": front_shell(p),
        "opentag_mini_back_plate.stl": back_plate(p),
        "opentag_mini_button_cap.stl": button_cap(p),
    }
    if not verify(p, parts["opentag_mini_front_shell.stl"],
                  parts["opentag_mini_back_plate.stl"],
                  parts["opentag_mini_button_cap.stl"]):
        sys.exit("verification failed — STL not written")

    OUT_DIR.mkdir(parents=True, exist_ok=True)
    for name, mesh in parts.items():
        # STL stores float32 vertices: weld near-coincident vertices first
        # so the round-trip stays watertight, then re-check.
        mesh.merge_vertices()
        if not mesh.is_watertight:
            sys.exit(f"{name}: mesh not watertight after vertex merge")
        path = OUT_DIR / name
        mesh.export(path)
        again = trimesh.load(path)
        if not again.is_watertight:
            sys.exit(f"{name}: reloaded STL is not watertight")
        print(f"wrote {path.relative_to(ROOT)} ({path.stat().st_size/1024:.0f} KiB, "
              f"round-trip watertight)")


if __name__ == "__main__":
    main()
