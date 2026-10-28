#!/usr/bin/env python3
"""OpenTag carrier PCB generator.

Generates hardware/pcb/opentag-carrier.kicad_pcb: a 90 x 60 mm 2-layer
all-through-hole carrier that hosts

  * an ESP32-S3-DevKitC-1 in two 1x22 female headers (exact geometry from
    Espressif's board: 22.86 mm row spacing, 2.54 mm pitch, 69.7 x 25.4 board),
  * a 1.3" SSD1351 OLED module in a 1x7 female header,
  * a PN532Killer on a 4-way cable (1x4 pin header),
  * three tactile buttons,
  * LiPo power: JST-PH battery input, TP4056 charger and MT3608 boost modules
    on wire headers, a power switch and a series Schottky diode.

The script is self-checking: it builds a geometric netlist (pads, tracks,
vias), verifies with union-find that every net is fully connected and that
unrelated copper keeps >= MIN_GAP clearance, and only then writes the KiCad
file. Run:  python3 generate_carrier_pcb.py
"""

import math
import random
import sys

# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

NET_NAMES = [
    "", "GND", "+3V3", "+5V", "VBAT", "SW1", "SW2", "BOOST5",
    "ESP_TX", "ESP_RX", "BTN_UP", "BTN_DOWN", "BTN_OK",
    "OLED_SCK", "OLED_MOSI", "OLED_RST", "OLED_DC", "OLED_CS",
]
NET = {name: i for i, name in enumerate(NET_NAMES)}

MIN_GAP = 0.25          # required copper clearance to foreign nets (mm)
TRACK_SIGNAL = 0.4
TRACK_POWER = 0.6
VIA_SIZE, VIA_DRILL = 0.8, 0.4

pads = []    # dicts: fp, pin, x, y, size, shape(circle|rect), net, npth
segs = []    # dicts: x1, y1, x2, y2, w, layer, net
vias = []    # dicts: x, y, net
silks = []   # dicts: kind(text|line|rect), ...
fps = []     # footprint records for output


def pad(fp, pin, x, y, net, size=1.7, drill=1.0, shape="circle", npth=False):
    pads.append(dict(fp=fp, pin=pin, x=x, y=y, size=size, drill=drill,
                     shape=shape, net=net, npth=npth))


def route(net, layer, pts, w=TRACK_SIGNAL):
    for a, b in zip(pts, pts[1:]):
        segs.append(dict(x1=a[0], y1=a[1], x2=b[0], y2=b[1], w=w,
                         layer=layer, net=net))


def via(net, x, y):
    vias.append(dict(x=x, y=y, net=net))


def silk_text(x, y, text, size=1.0, layer="F.SilkS"):
    silks.append(dict(kind="text", x=x, y=y, text=text, size=size, layer=layer))


def silk_line(x1, y1, x2, y2, layer="F.SilkS", w=0.15):
    silks.append(dict(kind="line", x1=x1, y1=y1, x2=x2, y2=y2, layer=layer, w=w))


def silk_rect(x1, y1, x2, y2, layer="F.SilkS", w=0.15):
    silk_line(x1, y1, x2, y1, layer, w)
    silk_line(x2, y1, x2, y2, layer, w)
    silk_line(x2, y2, x1, y2, layer, w)
    silk_line(x1, y2, x1, y1, layer, w)


# ---------------------------------------------------------------------------
# Board constants (mm; origin at board centre, +y = south)
# ---------------------------------------------------------------------------
BOARD_W, BOARD_H = 90.0, 60.0

# ESP32-S3-DevKitC-1 socket, centred at origin, USB end west.
# J1 = north row (y = -11.43), J3 = south row (y = +11.43);
# pin 1 at x = -29.97, pitch 2.54.
DK = dict(cx=0.0, cy=0.0, row=11.43, x1=-29.97, pitch=2.54)


def dk_pin(row, n):
    return (DK["x1"] + (n - 1) * DK["pitch"],
            DK["cy"] + (-DK["row"] if row == "J1" else DK["row"]))


# OLED header (1x7 female): GND 3V3 SCL SDA RES DC CS at y = -26
OLED_Y = -26.0
OLED_X0 = -9.0

# PN532Killer cable header (1x4): 3V3 GND TX RX at y = -26
KIL_Y = -26.0
KIL_X0 = 30.0

# Buttons (6x6 tactile): signal side north (y = 14.75), ground side south
BTN_Y = 17.0
BTN_PINS = ((-3.25, -2.25), (3.25, -2.25), (3.25, 2.25), (-3.25, 2.25))
# pins 1,2 = north row (signal), 3,4 = south row (ground)
BTN_CENTERS = ((-18.0, BTN_Y), (0.0, BTN_Y), (18.0, BTN_Y))
BTN_NETS = (NET["BTN_UP"], NET["BTN_DOWN"], NET["BTN_OK"])  # UP | DOWN | OK

# Power headers (1x4) in the east column
CHG_Y, BST_Y = 6.0, -2.0
PWR_X0 = 30.0                      # 30.0, 32.54, 35.08, 37.62
SW_X, SW_YS = 40.0, (12.0, 14.54, 17.08)   # power switch pads
JST = ((34.54, 22.0), (32.54, 22.0))       # BAT+, GND
D1_A, D1_K = (41.16, -12.0), (31.0, -12.0)  # anode east, cathode west

# ---------------------------------------------------------------------------
# Footprints
# ---------------------------------------------------------------------------

# --- DevKitC socket ---------------------------------------------------------
devkit_nets = {
    ("J1", 1): NET["+3V3"], ("J1", 2): NET["+3V3"], ("J1", 22): NET["GND"],
    ("J1", 10): NET["ESP_TX"], ("J1", 11): NET["ESP_RX"],
    ("J1", 12): NET["OLED_RST"], ("J1", 15): NET["OLED_DC"],
    ("J1", 16): NET["OLED_CS"], ("J1", 17): NET["OLED_MOSI"],
    ("J1", 18): NET["OLED_SCK"], ("J1", 21): NET["+5V"],
    ("J3", 1): NET["GND"], ("J3", 21): NET["GND"], ("J3", 22): NET["GND"],
    ("J3", 4): NET["BTN_OK"], ("J3", 5): NET["BTN_DOWN"], ("J3", 6): NET["BTN_UP"],
}
for row in ("J1", "J3"):
    for n in range(1, 23):
        x, y = dk_pin(row, n)
        shape = "rect" if n == 1 else "circle"
        pad("J_DEVKIT", f"{row}-{n}", x, y, devkit_nets.get((row, n), 0),
            shape=shape)
fps.append(dict(name="ot:DevKitC1_socket", ref="J_DEVKIT",
                value="ESP32-S3-DevKitC-1", side="F.Cu"))
# socket outline (DevKit board 69.7 x 25.4) + USB marker
silk_rect(-34.85, -12.7, 34.85, 12.7)
silk_text(0, -14.0, "DEVKITC-1 SOCKET", 1.0, "B.SilkS")
silk_text(-31.0, -13.9, "USB END", 0.9)
silk_text(-31.0, 13.4, "USB END", 0.9, "B.SilkS")

# --- OLED header -------------------------------------------------------------
oled_nets = [NET["GND"], NET["+3V3"], NET["OLED_SCK"], NET["OLED_MOSI"],
             NET["OLED_RST"], NET["OLED_DC"], NET["OLED_CS"]]
for i, net in enumerate(oled_nets):
    pad("J_OLED", str(i + 1), OLED_X0 + i * 2.54, OLED_Y, net,
        shape="rect" if i == 0 else "circle")
fps.append(dict(name="ot:PinHeader_1x07", ref="J_OLED",
                value="SSD1351 OLED", side="F.Cu"))
silk_text(OLED_X0 + 7.6, OLED_Y - 2.2, "OLED  GND 3V3 SCL SDA RES DC CS", 0.8)

# --- Killer cable header -------------------------------------------------------
kil_nets = [NET["+3V3"], NET["GND"], NET["ESP_TX"], NET["ESP_RX"]]
for i, net in enumerate(kil_nets):
    pad("J_KILLER", str(i + 1), KIL_X0 + i * 2.54, KIL_Y, net,
        shape="rect" if i == 0 else "circle")
fps.append(dict(name="ot:PinHeader_1x04", ref="J_KILLER",
                value="PN532Killer J4", side="F.Cu"))
silk_text(KIL_X0 + 3.8, KIL_Y - 2.2, "KILLER  3V3 GND TX RX", 0.8)

# --- Buttons ------------------------------------------------------------------
for (bx, by), net in zip(BTN_CENTERS, BTN_NETS):
    for i, (dx, dy) in enumerate(BTN_PINS, start=1):
        n = net if i <= 2 else NET["GND"]
        pad("SW_BTN", f"{bx:.0f}_{i}", bx + dx, by + dy, n,
            size=1.6, drill=0.9)
for (bx, _), label in zip(BTN_CENTERS, ("UP", "DOWN", "OK")):
    silk_text(bx, BTN_Y + 5.2, label, 1.0)

# --- TP4056 / MT3608 wire headers ----------------------------------------------
chg_nets = [NET["VBAT"], NET["GND"], NET["SW1"], NET["GND"]]
bst_nets = [NET["SW2"], NET["GND"], NET["GND"], NET["BOOST5"]]
for i, net in enumerate(chg_nets):
    pad("J_CHG", str(i + 1), PWR_X0 + i * 2.54, CHG_Y, net,
        shape="rect" if i == 0 else "circle")
for i, net in enumerate(bst_nets):
    pad("J_BOOST", str(i + 1), PWR_X0 + i * 2.54, BST_Y, net,
        shape="rect" if i == 0 else "circle")
fps.append(dict(name="ot:PinHeader_1x04", ref="J_CHG", value="TP4056 B+ B- OUT+ OUT-", side="F.Cu"))
fps.append(dict(name="ot:PinHeader_1x04", ref="J_BOOST", value="MT3608 IN+ IN- OUT- OUT+", side="F.Cu"))
silk_text(PWR_X0 + 3.8, CHG_Y + 3.6, "TP4056  B+ B- OUT+ OUT-", 0.8)
silk_text(PWR_X0 + 3.8, BST_Y - 3.8, "BOOST  IN+ IN- OUT- OUT+", 0.8)

# --- Power switch ----------------------------------------------------------------
pad("SW_PWR", "1", SW_X, SW_YS[0], NET["SW1"])
pad("SW_PWR", "2", SW_X, SW_YS[1], NET["SW2"])
pad("SW_PWR", "3", SW_X, SW_YS[2], 0)
fps.append(dict(name="ot:Switch_SPDT_row", ref="SW_PWR", value="POWER", side="F.Cu"))
silk_text(SW_X - 6.0, SW_YS[1], "PWR", 1.0)

# --- Battery JST-PH ----------------------------------------------------------------
pad("J_BAT", "1", JST[0][0], JST[0][1], NET["VBAT"], size=1.3, drill=0.8)
pad("J_BAT", "2", JST[1][0], JST[1][1], NET["GND"], size=1.3, drill=0.8)
fps.append(dict(name="ot:JST_PH_2", ref="J_BAT", value="LiPo 3.7V", side="F.Cu"))
silk_text(JST[0][0] + 2.0, JST[0][1] + 3.0, "BAT+  GND", 0.8)

# --- Schottky diode ------------------------------------------------------------------
pad("D1", "A", D1_A[0], D1_A[1], NET["BOOST5"], size=1.8, drill=1.0)
pad("D1", "K", D1_K[0], D1_K[1], NET["+5V"], size=1.8, drill=1.0)
fps.append(dict(name="ot:D_DO-41_10mm", ref="D1", value="1N5819", side="F.Cu"))
silk_text(D1_K[0] - 1.2, D1_K[1] - 2.6, "K (band)", 0.8)
silk_text(D1_K[0] - 1.2, D1_K[1] + 2.9, "1N5819", 0.8)

# --- Mounting holes --------------------------------------------------------------------
for mx, my in ((-41.5, -25.5), (41.5, -25.5), (-41.5, 25.5), (41.5, 25.5)):
    pad("MH", f"{mx:.0f}_{my:.0f}", mx, my, 0, size=3.2, drill=3.2,
        npth=True)

# ---------------------------------------------------------------------------
# Routing
# ---------------------------------------------------------------------------
F, B = "F.Cu", "B.Cu"

# --- GND ---------------------------------------------------------------------
# DevKit east side: J1-22 -- J3-22 -- J3-21
route(NET["GND"], F, [dk_pin("J1", 22), dk_pin("J3", 22), dk_pin("J3", 21)], TRACK_POWER)
# south backbone + west tree + north edge
GND_BB_Y = 26.5
route(NET["GND"], F, [(-33.5, -28.5), (-33.5, GND_BB_Y)], TRACK_POWER)          # west spine
route(NET["GND"], F, [(-33.5, -28.5), (KIL_X0 + 2.54, -28.5)], TRACK_POWER)     # north edge
route(NET["GND"], F, [(OLED_X0, OLED_Y), (OLED_X0, -28.5)], TRACK_POWER)        # OLED GND drop
route(NET["GND"], F, [(KIL_X0 + 2.54, KIL_Y), (KIL_X0 + 2.54, -28.5)], TRACK_POWER)  # Killer GND drop
route(NET["GND"], F, [(-33.5, GND_BB_Y), (KIL_X0 + 2.54, GND_BB_Y)], TRACK_POWER)    # backbone
route(NET["GND"], F, [dk_pin("J3", 1), (-29.97, GND_BB_Y)], TRACK_POWER)        # J3-1 to backbone
route(NET["GND"], F, [dk_pin("J3", 22), (23.37, GND_BB_Y)], TRACK_POWER)        # J3-22 link
for (bx, _), (dxn, dxs) in zip(BTN_CENTERS, ((-3.25, None), (-3.25, None), (-3.25, None))):
    route(NET["GND"], F, [(bx - 3.25, BTN_Y + 2.25), (bx - 3.25, GND_BB_Y)], TRACK_POWER)
    route(NET["GND"], F, [(bx - 3.25, BTN_Y + 2.25), (bx + 3.25, BTN_Y + 2.25)], TRACK_POWER)
# east column: J_BOOST-2 down through J_CHG-2 and J_BAT-2 to the backbone
route(NET["GND"], F, [dk_pad_xy := (32.54, BST_Y), (32.54, GND_BB_Y)], TRACK_POWER)
route(NET["GND"], F, [(37.62, CHG_Y), (37.62, 4.0), (32.54, 4.0)], TRACK_POWER)     # J_CHG-4
route(NET["GND"], F, [(35.08, BST_Y), (35.08, 2.0), (32.54, 2.0)], TRACK_POWER)    # J_BOOST-3 (OUT-)

# --- +3V3 ---------------------------------------------------------------------
route(NET["+3V3"], F, [dk_pin("J1", 1), dk_pin("J1", 2)], TRACK_POWER)
route(NET["+3V3"], F, [dk_pin("J1", 2), (-27.43, -24.0)], TRACK_POWER)
via(NET["+3V3"], -27.43, -24.0)
route(NET["+3V3"], B, [(-27.43, -24.0), (30.0, -24.0)], TRACK_POWER)
via(NET["+3V3"], 30.0, -24.0)
route(NET["+3V3"], F, [(30.0, -24.0), (30.0, KIL_Y)], TRACK_POWER)            # Killer 3V3
via(NET["+3V3"], -6.46, -24.0)
route(NET["+3V3"], F, [(-6.46, -24.0), (-6.46, OLED_Y)], TRACK_POWER)         # OLED VCC

# --- +5V (boost output, through D1) ----------------------------------------------
route(NET["BOOST5"], F, [(37.62, BST_Y), (37.62, -8.0), (D1_A[0], -8.0), D1_A], TRACK_POWER)
route(NET["+5V"], F, [D1_K, (31.0, -14.0)], TRACK_POWER)
via(NET["+5V"], 31.0, -14.0)
route(NET["+5V"], B, [(31.0, -14.0), (20.83, -14.0)], TRACK_POWER)
via(NET["+5V"], 20.83, -14.0)
route(NET["+5V"], F, [(20.83, -14.0), dk_pin("J1", 21)], TRACK_POWER)

# --- VBAT ----------------------------------------------------------------------------
route(NET["VBAT"], F, [JST[0], (34.54, 20.0)], TRACK_POWER)
via(NET["VBAT"], 34.54, 20.0)
route(NET["VBAT"], B, [(34.54, 20.0), (30.0, 20.0)], TRACK_POWER)
via(NET["VBAT"], 30.0, 20.0)
route(NET["VBAT"], F, [(30.0, 20.0), (30.0, CHG_Y)], TRACK_POWER)

# --- switch chain ----------------------------------------------------------------------
route(NET["SW1"], F, [(35.08, CHG_Y), (35.08, 9.0), (SW_X, 9.0), (SW_X, SW_YS[0])], TRACK_SIGNAL)
route(NET["SW2"], F, [(SW_X, SW_YS[1]), (43.0, SW_YS[1]), (43.0, -10.0),
                      (28.0, -10.0), (28.0, BST_Y), (30.0, BST_Y)], TRACK_SIGNAL)

# --- UART to Killer -------------------------------------------------------------------------
route(NET["ESP_TX"], F, [dk_pin("J1", 10), (-7.11, -8.5), (35.08, -8.5),
                         (35.08, KIL_Y)], TRACK_SIGNAL)
route(NET["ESP_RX"], F, [dk_pin("J1", 11), (-4.57, -9.3), (37.62, -9.3),
                         (37.62, KIL_Y)], TRACK_SIGNAL)

# --- OLED bus (F channel for RST/DC/CS, B channel for SCK/MOSI) -------------------------------
route(NET["OLED_RST"], F, [dk_pin("J1", 12), (-2.03, -14.5), (1.16, -14.5),
                           (1.16, OLED_Y)], TRACK_SIGNAL)
route(NET["OLED_DC"], F, [dk_pin("J1", 15), (5.59, -15.7), (3.70, -15.7),
                          (3.70, OLED_Y)], TRACK_SIGNAL)
route(NET["OLED_CS"], F, [dk_pin("J1", 16), (8.13, -16.9), (6.24, -16.9),
                          (6.24, OLED_Y)], TRACK_SIGNAL)
route(NET["OLED_SCK"], F, [dk_pin("J1", 18), (13.21, -19.5)], TRACK_SIGNAL)
via(NET["OLED_SCK"], 13.21, -19.5)
route(NET["OLED_SCK"], B, [(13.21, -19.5), (-3.92, -19.5)], TRACK_SIGNAL)
via(NET["OLED_SCK"], -3.92, -19.5)
route(NET["OLED_SCK"], F, [(-3.92, -19.5), (-3.92, OLED_Y)], TRACK_SIGNAL)
route(NET["OLED_MOSI"], F, [dk_pin("J1", 17), (10.67, -21.5)], TRACK_SIGNAL)
via(NET["OLED_MOSI"], 10.67, -21.5)
route(NET["OLED_MOSI"], B, [(10.67, -21.5), (-1.38, -21.5)], TRACK_SIGNAL)
via(NET["OLED_MOSI"], -1.38, -21.5)
route(NET["OLED_MOSI"], F, [(-1.38, -21.5), (-1.38, OLED_Y)], TRACK_SIGNAL)

# --- Buttons (J3-4/5/6 nest outward to the three buttons) --------------------------------------
route(NET["BTN_UP"], F, [dk_pin("J3", 6), (-17.27, 13.4), (-14.75, 13.4),
                         (-14.75, BTN_Y - 2.25)], TRACK_SIGNAL)
route(NET["BTN_UP"], F, [(-21.25, BTN_Y - 2.25), (-14.75, BTN_Y - 2.25)], TRACK_SIGNAL)
route(NET["BTN_DOWN"], F, [dk_pin("J3", 5), (-19.81, 16.4), (3.25, 16.4),
                           (3.25, BTN_Y - 2.25)], TRACK_SIGNAL)
route(NET["BTN_DOWN"], F, [(-3.25, BTN_Y - 2.25), (3.25, BTN_Y - 2.25)], TRACK_SIGNAL)
route(NET["BTN_OK"], F, [dk_pin("J3", 4), (-22.35, 13.2), (-26.5, 13.2),
                         (-26.5, 17.6), (21.25, 17.6),
                         (21.25, BTN_Y - 2.25)], TRACK_SIGNAL)
route(NET["BTN_OK"], F, [(14.75, BTN_Y - 2.25), (21.25, BTN_Y - 2.25)], TRACK_SIGNAL)

# ---------------------------------------------------------------------------
# Geometry helpers / self-check
# ---------------------------------------------------------------------------

def seg_point_dist(px, py, x1, y1, x2, y2):
    dx, dy = x2 - x1, y2 - y1
    L2 = dx * dx + dy * dy
    if L2 == 0:
        return math.hypot(px - x1, py - y1)
    t = max(0.0, min(1.0, ((px - x1) * dx + (py - y1) * dy) / L2))
    return math.hypot(px - (x1 + t * dx), py - (y1 + t * dy))


def seg_seg_dist(a, b):
    (x1, y1), (x2, y2) = a
    (x3, y3), (x4, y4) = b
    # endpoint-to-segment distances cover all segment-segment cases
    d1 = seg_point_dist(x3, y3, x1, y1, x2, y2)
    d2 = seg_point_dist(x4, y4, x1, y1, x2, y2)
    d3 = seg_point_dist(x1, y1, x3, y3, x4, y4)
    d4 = seg_point_dist(x2, y2, x3, y3, x4, y4)
    return min(d1, d2, d3, d4)


def pad_radius(p):
    if p["shape"] == "rect":
        return p["size"] * 0.7071
    return p["size"] / 2.0


def shapes():
    """Uniform copper-shape dicts: kind pad|seg|via, net, radius, geometry."""
    out = []
    for p in pads:
        if not p["npth"]:
            out.append(dict(kind="pad", net=p["net"], x=p["x"], y=p["y"],
                            r=pad_radius(p), layer="FB",
                            ref=f'pad {p["fp"]}.{p["pin"]}'))
    for s in segs:
        out.append(dict(kind="seg", net=s["net"], r=s["w"] / 2.0,
                        x1=s["x1"], y1=s["y1"], x2=s["x2"], y2=s["y2"],
                        layer=s["layer"], ref=f'track {s["layer"]}'))
    for v in vias:
        out.append(dict(kind="via", net=v["net"], x=v["x"], y=v["y"],
                        r=VIA_SIZE / 2.0, layer="FB", ref="via"))
    return out


def shapes_conflict(a, b):
    """True if the two shapes coexist on at least one copper layer."""
    la, lb = a["layer"], b["layer"]
    return la == "FB" or lb == "FB" or la == lb


def touch_dist(a, b):
    """Surface-to-surface distance between two copper shapes."""
    rs = a["r"] + b["r"]
    if a["kind"] == "seg" and b["kind"] == "seg":
        d = seg_seg_dist(((a["x1"], a["y1"]), (a["x2"], a["y2"])),
                         ((b["x1"], b["y1"]), (b["x2"], b["y2"])))
    elif a["kind"] == "seg":
        d = seg_point_dist(b["x"], b["y"], a["x1"], a["y1"], a["x2"], a["y2"])
    elif b["kind"] == "seg":
        d = seg_point_dist(a["x"], a["y"], b["x1"], b["y1"], b["x2"], b["y2"])
    else:
        d = math.hypot(a["x"] - b["x"], a["y"] - b["y"])
    return d - rs


def run_checks():
    errors = []
    shp = shapes()

    # ---- connectivity (union-find over same-net touching shapes) ----
    parent = list(range(len(shp)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    def union(i, j):
        parent[find(i)] = find(j)

    for i in range(len(shp)):
        for j in range(i + 1, len(shp)):
            a, b = shp[i], shp[j]
            if a["net"] == 0 or b["net"] == 0 or a["net"] != b["net"]:
                continue
            if not shapes_conflict(a, b):
                continue
            if touch_dist(a, b) <= 0.05:
                union(i, j)

    by_net = {}
    for idx, s in enumerate(shp):
        if s["net"]:
            by_net.setdefault(s["net"], []).append(idx)
    for net, idxs in sorted(by_net.items()):
        roots = {find(i) for i in idxs}
        if len(roots) > 1:
            split = [shp[i]["ref"] for i in idxs]
            errors.append(f"net {NET_NAMES[net]}: {len(roots)} disconnected "
                          f"groups involving {split}")

    # ---- clearance between foreign nets ----
    for i in range(len(shp)):
        for j in range(i + 1, len(shp)):
            a, b = shp[i], shp[j]
            if a["net"] == b["net"]:       # same net (or both unconnected)
                continue
            if not shapes_conflict(a, b):
                continue
            d = touch_dist(a, b)
            if d < MIN_GAP:
                errors.append(f"clearance {d:.3f} < {MIN_GAP} between "
                              f"{a['ref']}({NET_NAMES[a['net']]}) and "
                              f"{b['ref']}({NET_NAMES[b['net']]})")

    # ---- NPTH mounting holes vs all copper ----
    for p in pads:
        if not p["npth"]:
            continue
        for s in shp:
            if s["kind"] == "seg":
                d = seg_point_dist(p["x"], p["y"], s["x1"], s["y1"],
                                   s["x2"], s["y2"]) - (s["r"] + 1.6)
            else:
                d = math.hypot(p["x"] - s["x"], p["y"] - s["y"]) - (s["r"] + 1.6)
            if d < 0.3:
                errors.append(f"mounting hole ({p['x']},{p['y']}) too close to "
                              f"{s['ref']} (gap {d:.3f})")

    # ---- sanity: every routed net has at least 2 pads ----
    pad_nets = {p["net"] for p in pads if p["net"]}
    for net in pad_nets:
        n = sum(1 for p in pads if p["net"] == net)
        if n < 2:
            errors.append(f"net {NET_NAMES[net]} has a single pad")

    return errors


# ---------------------------------------------------------------------------
# KiCad output
# ---------------------------------------------------------------------------

def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("", "-0") else s


LAYERS = """  (layers
    (0 "F.Cu" signal)
    (31 "B.Cu" signal)
    (32 "B.Adhes" user "B.Adhesive")
    (33 "F.Adhes" user "F.Adhesive")
    (34 "B.Paste" user)
    (35 "F.Paste" user)
    (36 "B.SilkS" user "B.Silkscreen")
    (37 "F.SilkS" user "F.Silkscreen")
    (38 "B.Mask" user)
    (39 "F.Mask" user)
    (40 "Dwgs.User" user "User.Drawings")
    (41 "Cmts.User" user "User.Comments")
    (42 "Eco1.User" user "User.Eco1")
    (43 "Eco2.User" user "User.Eco2")
    (44 "Edge.Cuts" user)
    (45 "Margin" user)
    (46 "B.CrtYd" user "B.Courtyard")
    (47 "F.CrtYd" user "F.Courtyard")
    (48 "B.Fab" user)
    (49 "F.Fab" user)
  )"""

_rng = random.Random(0x0D7A6)


def ts():
    return format(_rng.getrandbits(32), "08X")


def write_kicad(path):
    out = []
    out.append(f'(kicad_pcb (version 20221018) (generator opentag_carrier_gen)')
    out.append(f'  (general (thickness 1.6)')
    out.append('    (paper "A4")')
    out.append('  )')
    out.append(LAYERS)
    out.append('''  (setup
    (pad_to_mask_clearance 0.05)
    (pcbplotparams
      (layerselection 0x00010fc_ffffffff)
      (plot_on_all_layers_selection 0x0000000_00000000)
      (usegerberextensions false)
      (excludeedgelayer true)
      (linewidth 0.100000)
      (plotframeref false)
      (viasonmask false)
      (mode 1)
      (useauxorigin false)
      (hpglpennumber 1)
      (hpglpenspeed 20)
      (hpglpendiameter 15.000000)
      (dxfpolygonmode true)
      (dxfimperialunits true)
      (dxfusepcbnewfont true)
      (psnegative false)
      (psa4output false)
      (plotreference true)
      (plotvalue true)
      (plotinvisibletext false)
      (sketchpadsonfab false)
      (subtractmaskfromsilk false)
      (outputformat 1)
      (mirror false)
      (drillshape 1)
      (scaleselection 1)
      (outputdirectory ""))
  )''')

    # nets
    for i, name in enumerate(NET_NAMES):
        out.append(f'  (net {i} "{name}")')

    # footprints: group pads by footprint id
    by_fp = {}
    for p in pads:
        by_fp.setdefault(p["fp"], []).append(p)
    fp_meta = {f["ref"]: f for f in fps}
    # pads with no explicit fp record (buttons, holes) still need one
    for fp_id, plist in by_fp.items():
        meta = fp_meta.get(fp_id)
        if meta is None:
            label = plist[0]["fp"]
            meta = dict(name="ot:Misc", ref=fp_id, value=fp_id, side="F.Cu")
        out.append(f'  (footprint "{meta["name"]}" (layer "{meta["side"]}") (tstamp {ts()})')
        out.append(f'    (attr through_hole)')
        out.append(f'    (fp_text reference "{meta["ref"]}" (at 0 -3.5) (layer "F.SilkS") (tstamp {ts()})')
        out.append('      (effects (font (size 1 1) (thickness 0.15))))')
        out.append(f'    (fp_text value "{meta["value"]}" (at 0 3.5) (layer "F.Fab") (tstamp {ts()})')
        out.append('      (effects (font (size 1 1) (thickness 0.15))))')
        for p in plist:
            net = (f' (net {p["net"]} "{NET_NAMES[p["net"]]}")') if p["net"] else ""
            drill = "0" if p["npth"] else fmt(p["drill"])
            typ = "np_thru_hole" if p["npth"] else "thru_hole"
            shape = p["shape"] if not p["npth"] else "circle"
            out.append(
                f'    (pad "{p["pin"]}" {typ} {shape} (at {fmt(p["x"])} {fmt(p["y"])}) '
                f'(size {fmt(p["size"])} {fmt(p["size"])}) (drill {drill}) '
                f'(layers "*.Cu" "*.Mask"){net})')
        out.append('  )')

    # tracks and vias
    for s in segs:
        out.append(
            f'  (segment (start {fmt(s["x1"])} {fmt(s["y1"])}) '
            f'(end {fmt(s["x2"])} {fmt(s["y2"])}) (width {fmt(s["w"])}) '
            f'(layer "{s["layer"]}") (net {s["net"]}) (tstamp {ts()}))')
    for v in vias:
        out.append(
            f'  (via (at {fmt(v["x"])} {fmt(v["y"])}) (size {VIA_SIZE}) '
            f'(drill {VIA_DRILL}) (layers "F.Cu" "B.Cu") (net {v["net"]}) (tstamp {ts()}))')

    # silkscreen
    for s in silks:
        if s["kind"] == "text":
            out.append(
                f'  (fp_text user "{s["text"]}" (at {fmt(s["x"])} {fmt(s["y"])}) '
                f'(layer "{s["layer"]}") (tstamp {ts()})')
            out.append(
                f'      (effects (font (size {fmt(s["size"])} {fmt(s["size"])}) (thickness 0.12))))')
        else:
            out.append(
                f'  (fp_line (start {fmt(s["x1"])} {fmt(s["y1"])}) '
                f'(end {fmt(s["x2"])} {fmt(s["y2"])} ) (stroke (width {fmt(s["w"])} '
                f'(type solid))) (layer "{s["layer"]}") (tstamp {ts()}))')

    # board outline
    hw, hh = BOARD_W / 2.0, BOARD_H / 2.0
    for (x1, y1), (x2, y2) in (((-hw, -hh), (hw, -hh)), ((hw, -hh), (hw, hh)),
                               ((hw, hh), (-hw, hh)), ((-hw, hh), (-hw, -hh))):
        out.append(
            f'  (fp_line (start {fmt(x1)} {fmt(y1)}) (end {fmt(x2)} {fmt(y2)}) '
            f'(stroke (width 0.1) (type solid)) (layer "Edge.Cuts") (tstamp {ts()}))')

    # GND pours (fill in KiCad with "B")
    for layer in ("F.Cu", "B.Cu"):
        out.append(f'''  (zone (net 1) (net_name "GND") (layer "{layer}") (tstamp {ts()})
    (hatch edge 0.5)
    (connect_pads (clearance 0.4))
    (min_thickness 0.25)
    (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))
    (polygon
      (pts
        (xy {fmt(-hw + 1.2)} {fmt(-hh + 1.2)})
        (xy {fmt(hw - 1.2)} {fmt(-hh + 1.2)})
        (xy {fmt(hw - 1.2)} {fmt(hh - 1.2)})
        (xy {fmt(-hw + 1.2)} {fmt(hh - 1.2)})
      )
    )
  )''')

    out.append(')')
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def main():
    errors = run_checks()
    if errors:
        print("SELF-CHECK FAILED:")
        for e in errors:
            print("  -", e)
        return 1
    n_pads = len(pads)
    n_seg = len(segs)
    n_via = len(vias)
    write_kicad(__file__.rsplit("/", 1)[0] + "/opentag-carrier.kicad_pcb")
    print(f"self-check passed: {n_pads} pads, {n_seg} tracks, {n_via} vias, "
          f"{len(NET_NAMES) - 1} nets")
    print("wrote opentag-carrier.kicad_pcb "
          f"({BOARD_W} x {BOARD_H} mm, 2 layers)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
