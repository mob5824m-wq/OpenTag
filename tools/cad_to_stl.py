#!/usr/bin/env python3
"""Export the OpenTag shell CAD to binary STL without OpenSCAD.

All top-level parameters are parsed from hardware/cad/opentag_shell.cad so
the numbers stay in sync; the module *structure* is mirrored below. If you
change geometry structure in the .cad, mirror the change here — or, with
OpenSCAD installed, regenerate canonically:

    openscad -o front.stl  -D 'part="front"'  hardware/cad/opentag_shell.cad
    openscad -o back.stl   -D 'part="back"'   hardware/cad/opentag_shell.cad

Outputs (millimetres, binary STL):
    hardware/cad/stl/opentag_front_shell.stl
    hardware/cad/stl/opentag_back_plate.stl
    hardware/cad/stl/opentag_button_cap.stl

Deps:  pip install trimesh manifold3d scipy rtree
"""
import math
import re
import sys
from pathlib import Path

import numpy as np
import trimesh

ROOT = Path(__file__).resolve().parent.parent
CAD = ROOT / "hardware" / "cad" / "opentag_shell.cad"
OUT_DIR = ROOT / "hardware" / "cad" / "stl"

SEG = 48                 # $fn in the .cad
CORNER_SEG = SEG // 4    # arc segments per rbox corner (hull of $fn-gons)

# ---------------------------------------------------------------------------
# OpenSCAD-flavoured primitives (base z=0, centred x/y unless noted)
# ---------------------------------------------------------------------------

def T(mesh, v):
    m = mesh.copy()
    m.apply_translation(list(v))
    return m

def cube(w, l, h):
    """OpenSCAD cube([w,l,h]): min corner at the translate origin."""
    return T(trimesh.creation.box([w, l, h]), [w / 2, l / 2, h / 2])

def cyl(d, h, seg=SEG):
    return T(trimesh.creation.cylinder(radius=d / 2, height=h, sections=seg),
             [0, 0, h / 2])

def _ring_mesh(r0, r1, h, seg):
    """Cylinder/frustum, base z=0 (r0 at z=0, r1 at z=h)."""
    ang = [2 * math.pi * i / seg for i in range(seg)]
    vs = ([[r0 * math.cos(a), r0 * math.sin(a), 0.0] for a in ang] +
          [[r1 * math.cos(a), r1 * math.sin(a), h] for a in ang])
    faces = []
    for i in range(1, seg - 1):
        faces.append([0, i + 1, i])            # bottom cap, -z
        faces.append([seg, seg + i, seg + i + 1])  # top cap, +z
    for i in range(seg):
        j = (i + 1) % seg
        faces.append([i, j, seg + j])           # side quad
        faces.append([i, seg + j, seg + i])
    return trimesh.Trimesh(vertices=vs, faces=faces, process=True)

def frustum(d1, d2, h, seg=SEG):
    return _ring_mesh(d1 / 2, d2 / 2, h, seg)

def rbox(w, l, h, r, cseg=CORNER_SEG):
    """Rounded box, centred x/y, base z=0 — the .cad's hull-of-cylinders."""
    return _rbox_ring(w, l, r, h, w, l, r, cseg)


def _rbox_ring(w0, l0, r0, h, w1, l1, r1, cseg=CORNER_SEG):
    """Rounded 'prism' whose footprint grows from (w0,l0,r0) at z=0 to
    (w1,l1,r1) at z=h — the .cad's hull() of two rboxes (equal chamfer on
    all sides keeps the corner centres coincident, so sides are planar and
    corners conical)."""
    def ring(w, l, r):
        r = min(r, w / 2, l / 2)
        pts = []
        for cx, cy, a0 in ((w / 2 - r, -l / 2 + r, -90.0),
                           (w / 2 - r,  l / 2 - r,   0.0),
                           (-w / 2 + r, l / 2 - r,  90.0),
                           (-w / 2 + r, -l / 2 + r, 180.0)):
            for k in range(cseg):
                a = math.radians(a0 + 90.0 * k / cseg)
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
        return pts
    p0, p1 = ring(w0, l0, r0), ring(w1, l1, r1)
    n = len(p0)
    vs = [[x, y, 0.0] for x, y in p0] + [[x, y, h] for x, y in p1]
    faces = []
    for i in range(1, n - 1):
        faces.append([0, i + 1, i])
        faces.append([n, n + i, n + i + 1])
    for i in range(n):
        j = (i + 1) % n
        faces.append([i, j, n + j])
        faces.append([i, n + j, n + i])
    return trimesh.Trimesh(vertices=vs, faces=faces, process=True)


def rrect_frustum(w0, l0, r0, w1, l1, r1, h):
    return _rbox_ring(w0, l0, r0, h, w1, l1, r1)

def union(meshes):
    return trimesh.boolean.union(list(meshes), engine="manifold")

def difference(meshes):
    return trimesh.boolean.difference(list(meshes), engine="manifold")

# ---------------------------------------------------------------------------
# Parameter parsing from the .cad (top-level `name = expr;` lines only)
# ---------------------------------------------------------------------------

def parse_params(path):
    ns = {}
    text = re.sub(r"//[^\n]*", "", path.read_text())  # drop line comments
    # matches every `name = expr;` (several can share a line); the expr
    # filter keeps module-internal ternaries/lists/strings out
    safe = re.compile(r"^[\w\s.+\-*/()]+$")
    for m in re.finditer(r"([A-Za-z_]\w*)\s*=\s*([^;]+);", text):
        name, expr = m.group(1), m.group(2).strip()
        if not safe.match(expr):
            continue
        try:
            val = eval(expr, {"__builtins__": {}}, dict(ns))  # our own file
        except Exception:
            continue
        if isinstance(val, (int, float)) and not isinstance(val, bool):
            ns[name] = float(val)
    return ns

def parse_brand(path):
    """Brand text + A-Z pixel font straight from the .cad (single source of
    truth): brand_text = "OPENTAG"; and the brand_font = [["A",[..]],..]
    table (classic 5x7 columns, bit 0 = top row)."""
    import ast
    src = path.read_text()
    m = re.search(r'brand_text\s*=\s*"([^"]*)"', src)
    text = m.group(1) if m else ""
    m = re.search(r"brand_font\s*=\s*(\[.*?\]);", src, re.S)
    table = ast.literal_eval(m.group(1)) if m else []
    font = {entry[0]: entry[1] for entry in table}
    return text, font


def parse_bolt(path):
    """Lightning-bolt column bytes straight from the .cad (single source of
    truth): bolt_cols = [8, 136, ...]; bit 0 = top row, 6 columns x 8 rows."""
    import ast
    src = path.read_text()
    m = re.search(r"bolt_cols\s*=\s*(\[.*?\]);", src, re.S)
    return ast.literal_eval(m.group(1)) if m else []


def brand_boxes(p):
    """Engraved wordmark pixel boxes, mirroring the .cad's brand_mark():
    laid out in upright text space then rotated 180 deg about the wordmark
    centre (so it reads with the OLED at the top). Each box spans
    z = outer_d - brand_depth .. outer_d + 0.01 (flush floor, overshoot
    outside the face)."""
    text, font = p["_brand_text"], p["_brand_font"]
    px, dep = p["brand_px"], p["brand_depth"]
    n = len(text)
    W = (n * 6 - 1) * px
    eps = 0.02   # diagonal-pixel overlap (matches the .cad): keeps the
                 # engraved surface manifold where glyph pixels touch
                 # corner-to-corner
    boxes = []
    for i, ch in enumerate(text):
        cols = font.get(ch)
        if ch == " " or cols is None:
            continue
        for c in range(5):
            for r in range(8):
                if (cols[c] >> r) & 1:
                    ux, uy = (i * 6 + c) * px, (6 - r) * px
                    fx = p["brand_cx"] + W / 2 - ux - px - eps
                    fy = p["brand_cy"] + 4 * px - uy - px - eps
                    boxes.append(T(cube(px + eps, px + eps, dep + 0.01),
                                   [fx, fy, p["outer_d"] - dep]))
    return boxes

# ---------------------------------------------------------------------------
# Geometry mirror of opentag_shell.cad
# ---------------------------------------------------------------------------

def bolt_boxes(p):
    """Engraved lightning-bolt pixel boxes, mirroring the .cad's
    bolt_mark(): west face above the charge slot, engraved bolt_depth into
    the outer wall. Viewed from the west face (up = +z, right = -y), so
    column c sits at y = bolt_cy + (2.5 - c) * px and row r (0 = top) at
    z = bolt_z0 + (7 - r) * px. Same 0.02 mm diagonal overlap as the
    wordmark."""
    px, dep = p["bolt_px"], p["bolt_depth"]
    cols = p["_bolt_cols"]
    eps = 0.02
    boxes = []
    for c in range(6):
        for r in range(8):
            if (cols[c] >> r) & 1:
                boxes.append(T(cube(dep + 0.01, px + eps, px + eps),
                               [-p["outer_w"] / 2 - 0.01,
                                p["bolt_cy"] + (2.5 - c) * px - eps / 2,
                                p["bolt_z0"] + (7 - r) * px - eps / 2]))
    return boxes


def front_shell(p):
    outer = rbox(p["outer_w"], p["outer_l"], p["outer_d"], p["corner"])
    inner = T(rbox(p["inner_w"], p["inner_l"], p["inner_d"] + 0.02,
                   max(p["corner"] - p["wall"], 1)), [0, 0, -0.01])
    # NOTE on epsilons: the .cad lets cuts overshoot "into the room" by
    # 0.01-0.02 mm (harmless under OpenSCAD/CGAL). manifold3d can leave a
    # zero-thickness membrane where an overshoot lands in empty space, so
    # here every cut either stops flush on the far face or overshoots
    # outside the part entirely (where it is clipped away cleanly).
    ceiling = p["inner_d"] + 0.01          # cavity cut top plane
    # Rectangular OLED window, chamfered outward (starts flush at the
    # ceiling, overshoots 0.02 above the outer face — clean in manifold3d).
    oled = T(rrect_frustum(p["oled_win_w"], p["oled_win_l"], p["oled_win_r"],
                           p["oled_win_w"] + 2 * p["oled_bezel"],
                           p["oled_win_l"] + 2 * p["oled_bezel"],
                           p["oled_win_r"] + p["oled_bezel"],
                           p["wall"] + 0.03),
             [p["oled_cx"], p["oled_cy"], ceiling - 0.01])
    btns = [T(cyl(p["btn_hole"], p["wall"] + 0.02),
              [x, p["btn_dy"], ceiling])
            for x in (-1.5 * p["btn_dx"], -0.5 * p["btn_dx"],
                      0.5 * p["btn_dx"], 1.5 * p["btn_dx"])]
    # DevKit USB slot, left wall — cut through BOTH walls, then plug the
    # east wall back. A cut that stops flush on the cavity face leaves a
    # zero-thickness cap over the slot in manifold3d (OpenSCAD/CGAL does
    # not); cutting fully through and re-adding pure material is robust.
    def wall_slot(y, l, z0, z1, west):
        thru = T(cube(p["outer_w"] + 0.03, l, z1 - z0),
                 [-p["outer_w"] / 2 - 0.01, y - l / 2, z0])
        # plug exactly refills the opposite wall (coplanar union)
        plug = T(cube(p["wall"], l, z1 - z0),
                 [(p["inner_w"] / 2) if west else (-p["outer_w"] / 2),
                  y - l / 2, z0])
        return thru, plug
    usb_thru, usb_plug = wall_slot(p["usb_slot_y"], p["usb_slot_l"],
                                    p["usb_slot_z0"], p["usb_slot_z1"], west=True)
    sw_thru, sw_plug = wall_slot(p["sw_slot_y"], p["sw_slot_l"],
                                  p["sw_slot_z0"], p["sw_slot_z1"], west=False)
    chg_thru, chg_plug = wall_slot(p["chg_slot_y"], p["chg_slot_l"],
                                    p["chg_slot_z0"], p["chg_slot_z1"], west=True)
    shell = union([difference([outer, inner, oled] + btns
                              + [usb_thru, sw_thru, chg_thru]
                              + brand_boxes(p) + bolt_boxes(p)),
                   usb_plug, sw_plug, chg_plug])
    # OLED side rails (fused +0.02 into the face, see .cad)
    rails = [T(cube(1.6, 34.5, p["inner_d"] - p["oled_top"] + 0.02),
               [p["oled_cx"] + sx * (37.5 / 2 + 0.8) - 0.8,
                p["oled_cy"] - 34.5 / 2, p["oled_top"]])
             for sx in (-1, 1)]
    # screw bosses (hole flush with the boss top)
    bosses = []
    for x in (-1, 1):
        for y in (-1, 1):
            at = [x * p["screw_dx"], y * p["screw_dy"], 0]
            bosses.append(difference([T(cyl(7, 10), at), T(cyl(2.8, 10), at)]))
    return union([shell] + rails + bosses)

def back_plate(p):
    plate = T(rbox(p["outer_w"], p["outer_l"], p["floor_t"], p["corner"]),
              [0, 0, -p["floor_t"]])
    # mating lip: four segments (see .cad notes)
    lips = []
    for sy in (-1, 1):
        y0 = sy * (p["inner_l"] / 2 - 0.2) - (0 if sy < 0 else 1.2)
        lips.append(T(cube(p["inner_w"] - 18, 1.2, 1.8),
                      [-(p["inner_w"] / 2 - 9), y0, -0.01]))
    lips.append(T(cube(1.2, 61, 1.8), [p["inner_w"] / 2 - 1.4, -57, -0.01]))
    lips.append(T(cube(1.2, 23, 1.8), [-p["inner_w"] / 2 + 0.2, 10, -0.01]))
    # PCB standoffs (pocket flush with the standoff top)
    standoffs = []
    for x in (-1, 1):
        for y in (-1, 1):
            at = [x * p["pcb_hole_dx"], p["pcb_y"] + y * p["pcb_hole_dy"], 0]
            standoffs.append(difference(
                [T(cyl(7.5, p["standoff_h"]), at),
                 T(cyl(2.8, 5.5), [at[0], at[1], p["standoff_h"] - 5.5])]))
    # button-load posts: carry button-press load straight into the plate
    btn_posts = [T(cube(8, 4, p["standoff_h"]), [px - 4, p["btn_dy"] - 2, 0])
                 for px in (-14, 14)]
    # PN532Killer snap bay posts
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
    # anti-slide nubs at the Killer board's edge midpoints
    kn, knh = p["kil_nub"], p["kil_nub_h"]
    nubs = [T(cube(kn, kn, knh),
              [p["kil_cx"] + nx * (p["kil_l"] / 2 + 0.8) - kn / 2,
               p["kil_cy"] - kn / 2, 0]) for nx in (-1, 1)]
    nubs += [T(cube(kn, kn, knh),
               [p["kil_cx"] - kn / 2,
                p["kil_cy"] + ny * (p["kil_w"] / 2 + 0.8) - kn / 2, 0])
             for ny in (-1, 1)]
    # battery retainers: two side lips + toe + heel
    bat_sides = [T(cube(3, p["bat_l"], 7),
                   [p["bat_cx"] + sx * p["bat_w"] / 2 - (3 if sx < 0 else 0),
                    p["bat_cy"] - p["bat_l"] / 2, 0])
                 for sx in (-1, 1)]
    toe = T(cube(p["bat_w"] + 6, 3, 5),
            [p["bat_cx"] - p["bat_w"] / 2 - 3, p["bat_cy"] - p["bat_l"] / 2 - 3, 0])
    heel = T(cube(p["bat_w"] - 1, 3, 5),
             [p["bat_cx"] - p["bat_w"] / 2 - 3, p["bat_cy"] + p["bat_l"] / 2, 0])
    # power module tray (walls + two tie-down bars)
    tx, ty = p["tray_cx"] - p["tray_w"] / 2, p["tray_cy"] - p["tray_d"] / 2
    tray = [T(cube(2, p["tray_d"], 5), [tx + (p["tray_w"] - 2), ty, 0])]  # east
    # west wall notched behind the charge port (TP4056 USB-C + LEDs)
    tray += [difference([T(cube(2, p["tray_d"], 5), [tx, ty, 0]),
                         T(cube(2.02, p["chg_slot_l"], 5.02),
                           [tx - 0.01, p["chg_slot_y"] - p["chg_slot_l"] / 2,
                            -0.01])])]
    tray += [T(cube(p["tray_w"], 2, 5), [tx, ty + my * (p["tray_d"] - 2), 0])
             for my in (0, 1)]
    tray += [T(cube(p["tray_w"] - 14, 2.6, 2.02), [tx + 7, ty + yy, -0.01])
             for yy in (3.5, p["tray_d"] - 6)]
    body = union([plate] + lips + standoffs + btn_posts + kil_posts + nubs +
                 bat_sides + [toe, heel] + tray)
    cuts = []
    # NFC tap window: fully OPEN over the antenna — best read range.
    # Through-cut from below, overshooting past the outer face.
    cuts.append(T(cyl(p["tap_win"], p["floor_t"] + 0.02),
                  [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]))
    # controller cradle: shallow outside recess centred on the window
    # (outer-face pocket terminating mid-material — countersink pattern).
    cuts.append(T(cyl(p["cradle_d"], p["cradle_t"] + 0.01),
                  [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]))
    # service window over the DevKitC (BOOT/RST access + lighter plate)
    cuts.append(T(rbox(p["svc_w"], p["svc_l"], p["floor_t"] + 0.02, p["svc_r"]),
                  [0, p["svc_cy"], -p["floor_t"] - 0.01]))
    # charger/boost vent through the plate, between the tray's tie-down bars
    cuts.append(T(cube(p["vent_l"], p["vent_w"], p["floor_t"] + 0.02),
                  [p["tray_cx"] - p["vent_l"] / 2,
                   p["tray_cy"] - p["vent_w"] / 2, -p["floor_t"] - 0.01]))
    # engraved halo ring around the cradle (tap-zone locator): annular tool,
    # shallow groove terminating mid-material from the outer face
    halo = difference([T(cyl(p["halo_d"], p["halo_t"] + 0.01),
                         [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.01]),
                       T(cyl(p["halo_d"] - 2 * p["halo_w"], p["halo_t"] + 0.03),
                         [p["kil_cx"], p["kil_cy"], -p["floor_t"] - 0.02])])
    cuts.append(halo)
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
# Verification
# ---------------------------------------------------------------------------

def body_count(mesh):
    """Connected components via BFS over edges (no scipy/networkx needed)."""
    edges = mesh.edges_sorted
    parent = np.arange(len(mesh.vertices))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for a, b in edges:
        ra, rb = find(a), find(b)
        if ra != rb:
            parent[ra] = rb
    return len({find(i) for i in range(len(mesh.vertices))})

def ray_hits(mesh, origin, direction):
    """Intersections along a slightly tilted ray, nearest first.

    Exactly axis-aligned rays confuse trimesh's pure-python caster
    (spurious/missed hits on parallel faces), so we always add a tiny
    lateral tilt and deduplicate coincident hits.
    """
    origin = np.array(origin, dtype=float)
    d = np.array(direction, dtype=float) + np.array([1e-4, 2e-4, 1e-4])
    d = d / np.linalg.norm(d)
    locs, _, _ = mesh.ray.intersects_location([origin], [d])
    hits = []
    for loc in sorted(locs, key=lambda l: float(np.dot(l - origin, d))):
        z = round(float(np.dot(loc - origin, d)), 3)
        if not hits or abs(hits[-1] - z) > 0.05:
            hits.append(z)
    return hits

def check(mesh, name, bbox, vol_exp, tol, bodies=1):
    ok = True
    def fail(msg):
        nonlocal ok
        ok = False
        print(f"  FAIL {name}: {msg}")
    if not mesh.is_watertight:
        fail("not watertight")
    if not mesh.is_winding_consistent:
        fail("winding inconsistent")
    if mesh.volume <= 0:
        fail(f"volume {mesh.volume}")
    if not np.allclose(mesh.extents, bbox, atol=0.05):
        fail(f"bbox {mesh.extents} != {bbox}")
    if abs(mesh.volume - vol_exp) > tol * vol_exp:
        fail(f"volume {mesh.volume:.0f} vs expected {vol_exp:.0f} ({tol:.0%})")
    if body_count(mesh) != bodies:
        fail(f"{body_count(mesh)} bodies (expected {bodies})")
    print(f"  {name}: {len(mesh.faces)} tris, volume {mesh.volume/1000:.1f} cm3, "
          f"bbox {np.round(mesh.extents, 2)}, bodies {body_count(mesh)}, "
          f"watertight {mesh.is_watertight}")
    return ok

def verify(p, front, back, cap):
    ok = True
    print("geometry checks:")
    ok &= check(front, "front shell",
                [p["outer_w"], p["outer_l"], p["outer_d"]], 44325.6, 0.02)
    ok &= check(back, "back plate",
                [p["outer_w"], p["outer_l"], p["standoff_h"] + p["floor_t"]],
                27310.2, 0.02)
    # cap volume: head + solid skirt + flange + pusher rod, minus the rod's
    # overlap with skirt and flange (pi*r^2 * (flange_t + 0.4))
    cap_vol = (math.pi * (p["cap_d"] / 2) ** 2 * p["cap_h"]
               + math.pi * (p["cap_skirt_o"] / 2) ** 2 * p["cap_skirt"]
               + math.pi * (p["cap_flange_d"] / 2) ** 2 * p["cap_flange_t"]
               + math.pi * (p["cap_push_d"] / 2) ** 2
               * (p["cap_push"] + p["cap_flange_t"] + 0.4)
               - math.pi * (p["cap_push_d"] / 2) ** 2
               * (p["cap_flange_t"] + 0.4))
    ok &= check(cap, "button cap",
                [p["cap_d"], p["cap_d"],
                 p["cap_h"] + p["cap_skirt"] + p["cap_flange_t"] + p["cap_push"]],
                cap_vol, 0.03)

    print("openness checks (perturbed rays — 0 hits means open):")
    fails = []
    def expect(cond, msg):
        if not cond:
            fails.append(msg)
            print(f"  FAIL {msg}")
    def expect_ray(actual, exp, msg):
        if actual != exp:
            fails.append(msg)
            print(f"  FAIL {msg}: actual {actual} != {exp}")

    for x in (-1.5 * p["btn_dx"], -0.5 * p["btn_dx"],
              0.5 * p["btn_dx"], 1.5 * p["btn_dx"]):
        expect(ray_hits(front, [x, p["btn_dy"], 40], [0, 0, -1]) == [],
               f"button hole at x={x} not open")
    expect(ray_hits(front, [p["oled_cx"], p["oled_cy"], 40], [0, 0, -1]) == [],
           "OLED window not open")
    for x in (-1, 1):
        for y in (-1, 1):
            expect(ray_hits(front, [x * p["screw_dx"], y * p["screw_dy"], 20],
                            [0, 0, -1]) == [],
                   f"front boss hole at ({x},{y}) not open")
    # wall slots: ray crossing the shell must miss the slotted wall
    expect(len(ray_hits(front, [-60, p["usb_slot_y"], 3], [1, 0, 0])) == 2,
           "USB slot: west wall should be pierced, east solid (2 hits)")
    expect(len(ray_hits(front, [-60, p["usb_slot_y"] + 20, 3], [1, 0, 0])) == 4,
           "away from USB slot both walls solid (4 hits)")
    expect(len(ray_hits(front, [60, p["sw_slot_y"], 20], [-1, 0, 0])) == 2,
           "switch slot: east wall pierced, west solid (2 hits)")
    expect(len(ray_hits(front, [-60, p["chg_slot_y"], 3], [1, 0, 0])) == 2,
           "charge slot: west wall should be pierced, east solid (2 hits)")
    expect(len(ray_hits(front, [60, 0, 20], [-1, 0, 0])) == 4,
           "away from switch slot both walls solid (4 hits)")
    for x in (-1, 1):
        for y in (-1, 1):
            expect(ray_hits(back, [x * p["screw_dx"], y * p["screw_dy"], 10],
                            [0, 0, -1]) == [],
                   f"back screw hole at ({x},{y}) not open")
    # NFC tap window must be fully OPEN (zero plastic over the antenna)
    expect(ray_hits(back, [p["kil_cx"], p["kil_cy"], 5], [0, 0, -1]) == [],
           "NFC tap window not fully open")
    # cradle annulus from below at 45 deg (r17.5, clear of the edge nubs):
    # enters the annulus floor at z=-0.6, exits the plate top at z=0
    r = 17.5 / math.sqrt(2)
    expect_ray(ray_hits(back, [p["kil_cx"] + r, p["kil_cy"] + r, -10],
                         [0, 0, 1]), [9.4, 10.0],
               "tap cradle annulus profile wrong")
    # service window over the DevKitC must be open through the plate
    expect(ray_hits(back, [0, p["svc_cy"], 5], [0, 0, -1]) == [],
           "service window not open")
    # power-tray vent, from below through the vent between the tie bars
    expect(ray_hits(back, [p["tray_cx"], p["tray_cy"], -10], [0, 0, 1]) == [],
           "tray vent not open")
    # standoff pocket: the ray runs down the pocket axis, so it enters at
    # the pocket bottom (z=8.5) and exits the plate underside (z=-1.8)
    expect_ray(ray_hits(back, [p["pcb_hole_dx"], p["pcb_y"] - p["pcb_hole_dy"],
                         20], [0, 0, -1]), [11.5, 21.8],
               "standoff pocket depth wrong")
    # cap on axis (solid skirt): pusher bottom (-5.8) .. head top (2.2)
    expect_ray(ray_hits(cap, [0, 0, -10], [0, 0, 1]),
               [4.2, 12.2], "cap profile wrong")

    print("solid/void checks (signed distance, one batch per part):")
    def probe(mesh, spec):
        pts = np.array([s[0] for s in spec], dtype=float)
        sd = trimesh.proximity.signed_distance(mesh, pts)
        for (pt, want), got in zip(spec, sd):
            expect((got > 0) if want == "solid" else (got < 0),
                   f"point {pt} should be {want}, signed distance {got:.2f}")

    # engraved wordmark: centre of the first lit pixel of the first glyph
    # (brand boxes are rotated 180 deg, hence the mirrored formula)
    bpx = p["brand_px"]
    bw = (len(p["_brand_text"]) * 6 - 1) * bpx
    bi, bc, br = next((i, c, r) for i, ch in enumerate(p["_brand_text"])
                      if ch in p["_brand_font"]
                      for c in range(5) for r in range(8)
                      if (p["_brand_font"][ch][c] >> r) & 1)
    bx = p["brand_cx"] + bw / 2 - (bi * 6 + bc) * bpx - bpx / 2
    by = p["brand_cy"] + 4 * bpx - (6 - br) * bpx - bpx / 2
    halo_r = p["halo_d"] / 2 - p["halo_w"] / 2   # groove centreline radius
    # bolt pixel (col 4, row 1) centre, mirroring bolt_boxes()
    bol_y = p["bolt_cy"] + (2.5 - 4) * p["bolt_px"]
    bol_z = p["bolt_z0"] + (7 - 1) * p["bolt_px"] + p["bolt_px"] / 2

    probe(front, [
        ([-48, p["usb_slot_y"], 3], "void"),          # USB slot channel
        ([-48, -10, 3], "solid"),                     # west wall
        ([48, p["sw_slot_y"], 20], "void"),           # switch slot channel
        ([48, 0, 20], "solid"),                       # east wall
        ([p["oled_cx"] + 19.5, p["oled_cy"], 28.5], "solid"),  # face beside window
        ([p["oled_cx"] + 13, p["oled_cy"], 28.5], "void"),    # window chamfer
        ([p["oled_cx"] + 15, p["oled_cy"] + 15, 28.6], "solid"),  # window corner
        ([p["oled_cx"] + 19.5, p["oled_cy"], 26.8], "solid"),  # OLED rail
        ([p["oled_cx"] + 15, p["oled_cy"], 26.8], "void"),    # beside rail
        ([p["screw_dx"] + 2, p["screw_dy"], 5], "solid"),     # boss
        ([p["screw_dx"], p["screw_dy"], 5], "void"),          # boss hole
        ([bx, by, p["outer_d"] - p["brand_depth"] / 2], "void"),   # wordmark pixel
        ([bx, by, p["outer_d"] - p["brand_depth"] - 0.4], "solid"), # under engraving
        ([bx, by - 10, p["outer_d"] - 0.3], "solid"),         # face below wordmark
        ([-48, p["chg_slot_y"], 3], "void"),        # charge port channel
        ([-48.5, bol_y, bol_z], "void"),            # bolt pixel engraving
        ([-48.5, bol_y, 5.5], "solid"),             # wall below the bolt
    ])
    probe(back, [
        ([p["kil_cx"], p["kil_cy"], -0.5], "void"),   # tap window open
        ([p["kil_cx"], p["kil_cy"], -1.5], "void"),   # ..through the cradle too
        ([p["kil_cx"] + 17, p["kil_cy"], -0.3], "solid"),  # cradle annulus floor
        ([-45.5, p["chg_slot_y"], 2.5], "void"),    # tray wall notch (port)
        ([-45.5, p["chg_slot_y"] + 7.5, 2.5], "solid"),   # wall stub beside
        ([p["kil_cx"] + 17, p["kil_cy"], -1.0], "void"),   # cradle recess
        ([p["kil_cx"], p["kil_cy"] + 16.8, 1.2], "solid"), # north anti-slide nub
        ([p["kil_cx"] + 21.8, p["kil_cy"], 1.2], "solid"), # east anti-slide nub
        ([0, p["svc_cy"], -1], "void"),               # service window open
        ([0, -48.5, -1], "solid"),                    # plate south of it
        ([14, p["btn_dy"], 8], "solid"),              # button-load post
        ([-14, p["btn_dy"], 8], "solid"),             # button-load post
        ([6, p["btn_dy"], 8], "void"),                # between the posts
        ([p["tray_cx"], p["tray_cy"], -1], "void"),   # vent through the plate
        ([p["tray_cx"], 52, -1], "solid"),            # plate beside the vent
        ([p["screw_dx"], p["screw_dy"], 2.5], "void"),   # NE screw shaft clear
        ([p["screw_dx"], p["screw_dy"], -1], "void"),    # in screw hole
        ([p["pcb_hole_dx"], p["pcb_y"] - p["pcb_hole_dy"], 12], "void"),   # standoff pocket
        ([p["pcb_hole_dx"] + 2, p["pcb_y"] - p["pcb_hole_dy"], 12], "solid"),
        ([0, p["inner_l"] / 2 - 0.8, 0.9], "solid"),      # north lip segment
        ([0, p["inner_l"] / 2 - 2.5, 0.9], "void"),       # just inside it
        ([p["inner_w"] / 2 - 0.8, -30, 0.9], "solid"),    # east lip segment
        ([-p["inner_w"] / 2 + 0.8, 20, 0.9], "solid"),    # west lip segment
        ([-p["inner_w"] / 2 + 0.8, 8.7, 0.9], "void"),    # gap: post to lip
        ([-p["inner_w"] / 2 + 0.8, 5, 0.9], "solid"),     # killer-bay post
        ([30, p["bat_cy"] + p["bat_l"] / 2 + 1.5, 2], "solid"),  # battery heel
        ([45, 30, 3], "solid"),                            # east battery retainer
        ([p["tray_cx"], p["tray_cy"], 2.5], "void"),       # tray interior open
        ([p["tray_cx"], p["tray_cy"] - 4.2, 1.0], "solid"),  # tie-down bar
        ([0, 0, -1], "solid"),                             # plate floor
        ([p["kil_cx"] + halo_r, p["kil_cy"], -p["floor_t"] + p["halo_t"] / 2],
         "void"),                                          # halo groove
        ([p["kil_cx"] + halo_r + 1.25, p["kil_cy"],
          -p["floor_t"] + p["halo_t"] / 2], "solid"),      # plate beyond the halo
        ([p["kil_cx"] + halo_r, p["kil_cy"], -p["floor_t"] + p["halo_t"] + 0.1],
         "solid"),                                         # material over the groove
    ])
    probe(cap, [
        ([0, 0, 1.1], "solid"),      # head
        ([4.7, 0, 1.1], "void"),     # outside the head
        ([2.7, 0, -1.5], "solid"),   # solid skirt
        ([3.2, 0, -1.5], "void"),    # outside the skirt, above the flange
        ([3.5, 0, -3.4], "solid"),   # capture flange
        ([3.5, 0, -4.5], "void"),    # below the flange, off the pusher
        ([0, 0, -4.5], "solid"),     # pusher rod
    ])
    ok &= not fails
    print("  all feature checks passed" if not fails else
          f"  {len(fails)} FEATURE CHECKS FAILED")
    return ok

# ---------------------------------------------------------------------------

def main():
    p = parse_params(CAD)
    p["_brand_text"], p["_brand_font"] = parse_brand(CAD)
    p["_bolt_cols"] = parse_bolt(CAD)
    need = ("outer_w", "outer_l", "outer_d", "inner_w", "inner_l", "inner_d",
            "wall", "floor_t", "corner", "oled_cx", "oled_cy", "oled_win_w",
            "oled_win_l", "oled_win_r", "oled_bezel", "oled_top", "btn_dx",
            "btn_dy", "btn_hole", "usb_slot_y", "usb_slot_l", "usb_slot_z0",
            "usb_slot_z1", "sw_slot_y", "sw_slot_l", "sw_slot_z0",
            "sw_slot_z1", "pcb_y", "pcb_hole_dx", "pcb_hole_dy", "standoff_h",
            "screw_dx", "screw_dy", "kil_cx", "kil_cy", "kil_l", "kil_w",
            "kil_post", "kil_lip", "kil_post_h", "kil_nub", "kil_nub_h",
            "tap_win", "cradle_d", "cradle_t", "bat_cx", "bat_cy", "bat_l",
            "bat_w", "tray_cx", "tray_cy", "tray_w", "tray_d", "vent_l",
            "vent_w", "svc_w", "svc_l", "svc_cy", "svc_r", "cap_d", "cap_h",
            "cap_proud", "cap_skirt_o", "cap_skirt",
            "cap_flange_d", "cap_flange_t", "cap_push", "cap_push_d",
            "brand_px", "brand_depth", "brand_cx", "brand_cy",
            "halo_d", "halo_w", "halo_t",
            "chg_slot_y", "chg_slot_l", "chg_slot_z0", "chg_slot_z1",
            "bolt_px", "bolt_depth", "bolt_cy", "bolt_z0")
    missing = [k for k in need if k not in p]
    if missing:
        sys.exit(f"parameters not parsed from {CAD}: {missing}")
    print(f"parsed {len(p)} parameters + brand "
          f"\"{p['_brand_text']}\" ({len(p['_brand_font'])} glyphs) "
          f"from {CAD.name}")

    parts = {
        "opentag_front_shell.stl": front_shell(p),
        "opentag_back_plate.stl": back_plate(p),
        "opentag_button_cap.stl": button_cap(p),
    }
    if not verify(p, parts["opentag_front_shell.stl"],
                  parts["opentag_back_plate.stl"],
                  parts["opentag_button_cap.stl"]):
        sys.exit("verification failed — STL not written")

    print("assembly check: front/back closed-position interference")
    import warnings
    with warnings.catch_warnings():
        # an empty/degenerate intersection makes trimesh's center_mass
        # divide by zero — harmless, but keep the gate output clean
        warnings.simplefilter("ignore", RuntimeWarning)
        inter = trimesh.boolean.intersection(
            [parts["opentag_front_shell.stl"],
             parts["opentag_back_plate.stl"]], engine="manifold")
    vol = 0.0 if inter.is_empty else inter.volume
    # The halves clamp face-to-face at z=0 (rim, boss bottoms), which the
    # intersection reports as zero-thickness contact patches, and
    # near-tangent surfaces produce degenerate sub-0.1 mm^3 fins. Anything
    # above 0.5 mm^3 is a real collision.
    if vol > 0.5:
        sys.exit(f"front and back plates interfere by {vol:.2f} mm^3 "
                 "in the closed position — STL not written")
    print(f"  interference volume {vol:.3f} mm^3 (contact patches only)")

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
