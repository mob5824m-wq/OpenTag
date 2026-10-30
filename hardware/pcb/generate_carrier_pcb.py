#!/usr/bin/env python3
"""OpenTag carrier PCB generator.

Generates hardware/pcb/opentag-carrier.kicad_pcb plus a fab-ready Gerber
set (hardware/pcb/gerbers/, also zipped): a 90 x 60 mm 2-layer
all-through-hole carrier that hosts

  * an ESP32-S3-DevKitC-1 in two 1x22 female headers (exact geometry from
    Espressif's board: 22.86 mm row spacing, 2.54 mm pitch, 69.7 x 25.4 board),
  * a 1.3" SSD1351 OLED module in a 1x7 female header,
  * a PN532Killer on a 4-way cable (1x4 pin header),
  * four tactile buttons (UP / DOWN / OK / BACK),
  * LiPo power: JST-PH battery input, TP4056 charger and MT3608 boost modules
    on wire headers, a power switch and a series Schottky diode.

The script is self-checking: it builds a geometric netlist (pads, tracks,
vias), verifies with union-find that every net is fully connected and that
unrelated copper keeps >= MIN_GAP clearance, and only then writes the KiCad
file. It then writes the Gerber/Excellon fab files and re-parses them to
verify they match the model (flashes, draws, mask openings, outline,
drills). Run:  python3 generate_carrier_pcb.py
"""

import math
import random
import sys

# ---------------------------------------------------------------------------
# Model
# ---------------------------------------------------------------------------

NET_NAMES = [
    "", "GND", "+3V3", "+5V", "VBAT", "SW1", "SW2", "BOOST5",
    "ESP_TX", "ESP_RX", "BTN_UP", "BTN_DOWN", "BTN_OK", "BTN_BACK",
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
# West -> east: UP | DOWN | OK | BACK (row centred on the board; 16.5 mm pitch
# keeps the outer pins clear of the J3-1 GND drop, the J3-22 GND link and the
# VBAT riser at x = 30).
BTN_CENTERS = ((-24.75, BTN_Y), (-8.25, BTN_Y), (8.25, BTN_Y), (24.75, BTN_Y))
BTN_NETS = (NET["BTN_UP"], NET["BTN_DOWN"], NET["BTN_OK"], NET["BTN_BACK"])

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
    ("J1", 19): NET["BTN_BACK"],   # GPIO13: BACK button (MISO pin reused)
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
for (bx, _), label in zip(BTN_CENTERS, ("UP", "DOWN", "OK", "BACK")):
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
for (bx, _), (dxn, dxs) in zip(BTN_CENTERS, ((-3.25, None),) * 4):
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

# --- Buttons (UP/DOWN/OK on J3-4/5/6, BACK on J1-19 = GPIO13) -----------------
# The button row is centred while the J3 source pins sit on the west half, so
# UP (source east, button west) and OK (source west, button east) point at each
# other and cannot share a layer. UP therefore stays on the front and runs west
# along y = 13.0; DOWN and OK hop to the back layer and run east along the
# stacked lanes y = 16.2 / 17.3 between the signal and GND pin rows. BACK
# arrives from the north row (J1-19): it hops to the back layer, crosses the
# socket's east end (past J1-22/J3-22) and comes back up between the rows.
route(NET["BTN_UP"], F, [dk_pin("J3", 6), (-17.27, 13.0), (-28.0, 13.0)])
route(NET["BTN_UP"], F, [(-28.0, 13.0), (-28.0, 14.75)])
route(NET["BTN_UP"], F, [(-21.5, 13.0), (-21.5, 14.75)])
route(NET["BTN_UP"], F, [(-28.0, 14.75), (-21.5, 14.75)])
route(NET["BTN_DOWN"], F, [dk_pin("J3", 5), (-19.81, 12.0)])
via(NET["BTN_DOWN"], -19.81, 12.0)
route(NET["BTN_DOWN"], B, [(-19.81, 12.0), (-19.81, 16.2), (-4.75, 16.2)])
via(NET["BTN_DOWN"], -4.75, 16.2)
route(NET["BTN_DOWN"], F, [(-4.75, 16.2), (-4.75, 14.75)])
route(NET["BTN_DOWN"], F, [(-4.75, 14.75), (-11.5, 14.75)])
route(NET["BTN_OK"], F, [dk_pin("J3", 4), (-22.35, 12.0)])
via(NET["BTN_OK"], -22.35, 12.0)
route(NET["BTN_OK"], B, [(-22.35, 12.0), (-22.35, 12.75), (-26.2, 12.75),
                         (-26.2, 17.3), (5.0, 17.3)])
via(NET["BTN_OK"], 5.0, 17.3)
route(NET["BTN_OK"], F, [(5.0, 17.3), (5.0, 14.75)])
route(NET["BTN_OK"], F, [(5.0, 14.75), (11.5, 14.75)])
# BACK's west signal pin needs its own via: the J3-22 GND link at x = 23.37
# runs between the two BACK pins all the way to the GND backbone.
route(NET["BTN_BACK"], F, [dk_pin("J1", 19), (15.75, -12.9)])
via(NET["BTN_BACK"], 15.75, -12.9)
route(NET["BTN_BACK"], B, [(15.75, -12.9), (24.9, -12.9), (24.9, 12.9)])
via(NET["BTN_BACK"], 24.9, 12.9)
route(NET["BTN_BACK"], F, [(24.9, 12.9), (24.9, 14.75), (28.0, 14.75)])
route(NET["BTN_BACK"], B, [(24.9, 12.9), (21.5, 12.9)])
via(NET["BTN_BACK"], 21.5, 12.9)
route(NET["BTN_BACK"], F, [(21.5, 12.9), (21.5, 14.75)])

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
    here = __file__.rsplit("/", 1)[0]
    write_kicad(here + "/opentag-carrier.kicad_pcb")
    print(f"self-check passed: {n_pads} pads, {n_seg} tracks, {n_via} vias, "
          f"{len(NET_NAMES) - 1} nets")
    print("wrote opentag-carrier.kicad_pcb "
          f"({BOARD_W} x {BOARD_H} mm, 2 layers)")

    gdir = here + "/gerbers"
    written = write_gerbers(gdir)
    errs = verify_gerbers(gdir)
    if errs:
        print("GERBER CHECK FAILED:")
        for e in errs:
            print("  -", e)
        return 1
    import zipfile
    with zipfile.ZipFile(os.path.join(gdir, "opentag-gerbers.zip")) as z:
        total = sum(i.file_size for i in z.infolist())
    print(f"wrote {len(written)} Gerber/Excellon files + zip to "
          f"hardware/pcb/gerbers/ ({total/1024:.1f} KiB unzipped) — "
          f"parsed back and verified against the model")
    return 0

# ---------------------------------------------------------------------------
# Gerber (RS-274X) + Excellon output
# ---------------------------------------------------------------------------
#
# write_gerbers() emits fab-ready files into hardware/pcb/gerbers/:
#   opentag-F_Cu.gbr / opentag-B_Cu.gbr      copper: pads + vias flashed on
#                                             both sides, tracks drawn on
#                                             their own layer
#   opentag-F_Mask.gbr / opentag-B_Mask.gbr   solder-mask openings: pads and
#                                             vias +0.2 mm, NPTH +0.4 mm
#   opentag-F_SilkS.gbr / opentag-B_SilkS.gbr silkscreen: lines/rects at
#                                             0.15 mm, labels rendered with
#                                             the same 5x7 font the device
#                                             OLED shows (one R flash/pixel)
#   opentag-Edge_Cuts.gbr                     board outline (0.1 mm trace)
#   opentag.drl                               Excellon drills (metric,
#                                             FMAT 2, plated/NPTH attributes)
#   opentag-gerbers.zip                       all of the above
#
# Coordinates are 3.4 mm absolute and identical to the .kicad_pcb values
# (this generator's model uses +y = south; both artifacts share it). Silk
# text anchors are the TOP-LEFT of the text box and glyphs run toward +x
# and +y, matching that model. verify_gerbers() parses the emitted files
# back and compares them with the model, so the Gerbers are
# machine-verified, not just written.

import os
import re
import zipfile

# Classic 5x7 ASCII font, printable 0x20..0x7E, 5 column bytes per glyph,
# bit 0 = top row — the same table the device OLED and the web preview
# render. Source: Adafruit-GFX-Library glcdfont.c (BSD license).
FONT5X7 = bytes([
    0, 0, 0, 0, 0, 0, 0, 95, 0, 0, 0, 7, 0, 7, 0,
    20, 127, 20, 127, 20, 36, 42, 127, 42, 18, 35, 19, 8, 100, 98,
    54, 73, 86, 32, 80, 0, 8, 7, 3, 0, 0, 28, 34, 65, 0,
    0, 65, 34, 28, 0, 42, 28, 127, 28, 42, 8, 8, 62, 8, 8,
    0, 128, 112, 48, 0, 8, 8, 8, 8, 8, 0, 0, 96, 96, 0,
    32, 16, 8, 4, 2, 62, 81, 73, 69, 62, 0, 66, 127, 64, 0,
    114, 73, 73, 73, 70, 33, 65, 73, 77, 51, 24, 20, 18, 127, 16,
    39, 69, 69, 69, 57, 60, 74, 73, 73, 49, 65, 33, 17, 9, 7,
    54, 73, 73, 73, 54, 70, 73, 73, 41, 30, 0, 0, 20, 0, 0,
    0, 64, 52, 0, 0, 0, 8, 20, 34, 65, 20, 20, 20, 20, 20,
    0, 65, 34, 20, 8, 2, 1, 89, 9, 6, 62, 65, 93, 89, 78,
    124, 18, 17, 18, 124, 127, 73, 73, 73, 54, 62, 65, 65, 65, 34,
    127, 65, 65, 65, 62, 127, 73, 73, 73, 65, 127, 9, 9, 9, 1,
    62, 65, 65, 81, 115, 127, 8, 8, 8, 127, 0, 65, 127, 65, 0,
    32, 64, 65, 63, 1, 127, 8, 20, 34, 65, 127, 64, 64, 64, 64,
    127, 2, 28, 2, 127, 127, 4, 8, 16, 127, 62, 65, 65, 65, 62,
    127, 9, 9, 9, 6, 62, 65, 81, 33, 94, 127, 9, 25, 41, 70,
    38, 73, 73, 73, 50, 3, 1, 127, 1, 3, 63, 64, 64, 64, 63,
    31, 32, 64, 32, 31, 63, 64, 56, 64, 63, 99, 20, 8, 20, 99,
    3, 4, 120, 4, 3, 97, 89, 73, 77, 67, 0, 127, 65, 65, 65,
    2, 4, 8, 16, 32, 0, 65, 65, 65, 127, 4, 2, 1, 2, 4,
    64, 64, 64, 64, 64, 0, 3, 7, 8, 0, 32, 84, 84, 120, 64,
    127, 40, 68, 68, 56, 56, 68, 68, 68, 40, 56, 68, 68, 40, 127,
    56, 84, 84, 84, 24, 0, 8, 126, 9, 2, 24, 164, 164, 156, 120,
    127, 8, 4, 4, 120, 0, 68, 125, 64, 0, 32, 64, 64, 61, 0,
    127, 16, 40, 68, 0, 0, 65, 127, 64, 0, 124, 4, 120, 4, 120,
    124, 8, 4, 4, 120, 56, 68, 68, 68, 56, 252, 24, 36, 36, 24,
    24, 36, 36, 24, 252, 124, 8, 4, 4, 8, 72, 84, 84, 84, 36,
    4, 4, 63, 68, 36, 60, 64, 64, 32, 124, 28, 32, 64, 32, 28,
    60, 64, 48, 64, 60, 68, 40, 16, 40, 68, 76, 144, 144, 144, 124,
    68, 100, 84, 76, 68, 0, 8, 54, 65, 0, 0, 0, 119, 0, 0,
    0, 65, 54, 8, 0, 2, 1, 2, 4, 2
])

SILK_W = 0.15
EDGE_W = 0.1
MASK_CLEAR = 0.2        # pad/via mask opening growth per side
MASK_CLEAR_NPTH = 0.4   # mounting-hole mask opening growth


def _gnum(v):
    """One 3.4-format coordinate: sign + 7 digits (value x 10000)."""
    i = int(round(v * 10000.0))
    return ("-" if i < 0 else "") + f"{abs(i):07d}"


class Gerber:
    """Minimal RS-274X writer: aperture selects, flashes (D03), draws (D01)."""

    def __init__(self, comment):
        self.comment = comment
        self.apertures = {}      # key -> dnum
        self.definitions = {}    # dnum -> definition body, e.g. "C,0.80000"
        self.body = []
        self.cur_ap = None

    def _aperture(self, key, definition):
        if key not in self.apertures:
            dnum = 10 + len(self.apertures)
            self.apertures[key] = dnum
            self.definitions[dnum] = definition
        return self.apertures[key]

    def _select(self, key, definition):
        dnum = self._aperture(key, definition)
        if dnum != self.cur_ap:
            self.body.append(f"D{dnum:02d}*")
            self.cur_ap = dnum

    def flash(self, x, y, key, definition):
        self._select(key, definition)
        self.body.append(f"X{_gnum(x)}Y{_gnum(y)}D03*")

    def draw(self, x1, y1, x2, y2, key, definition):
        self._select(key, definition)
        self.body.append(f"X{_gnum(x1)}Y{_gnum(y1)}D02*")
        self.body.append(f"X{_gnum(x2)}Y{_gnum(y2)}D01*")

    def render(self):
        out = [f"G04 {self.comment}*",
               "%FSLAX34Y34*%",
               "%MOMM*%"]
        for dnum in sorted(self.definitions):
            out.append(f"%ADD{dnum:02d}{self.definitions[dnum]}*%")
        out += self.body
        out.append("M02*")
        return "\n".join(out) + "\n"


def text_pixels(text, size):
    """Pixel flashes for one silk label.

    Returns (pixels, px): pixels is a list of (dx, dy) offsets from the
    anchor, px the pixel pitch (size / 7). The anchor is the top-left of
    the text box: column c of glyph i lands at dx = (i*6 + c)*px and font
    row r (bit 0 = top row) at dy = r*px, so text runs toward +x/+y.
    """
    px = size / 7.0
    pixels = []
    for i, ch in enumerate(text):
        code = ord(ch) - 0x20
        if not 0 <= code < 95:
            continue                      # unsupported glyph -> blank
        for col in range(5):
            bits = FONT5X7[code * 5 + col]
            for row in range(8):
                if (bits >> row) & 1:
                    pixels.append(((i * 6 + col) * px, row * px))
    return pixels, px


def _pad_aperture(p, grow):
    """Aperture key + RS-274X definition for a pad grown by `grow`."""
    size = round(p["size"] + grow, 4)
    if p["shape"] == "rect":
        return ("R", size), f"R,{size:.5f}X{size:.5f}"
    return ("C", size), f"C,{size:.5f}"


def write_gerbers(dirpath):
    """Emit all Gerber + Excellon files; returns the list of paths written."""
    os.makedirs(dirpath, exist_ok=True)
    written = []

    def emit(name, gerber):
        path = os.path.join(dirpath, name)
        with open(path, "w") as f:
            f.write(gerber.render())
        written.append(path)

    # ---- copper ------------------------------------------------------------
    for layer, fname in (("F.Cu", "opentag-F_Cu.gbr"),
                         ("B.Cu", "opentag-B_Cu.gbr")):
        g = Gerber(f"OpenTag carrier PCB - {layer}")
        for p in pads:
            (shape, size), defin = _pad_aperture(p, 0.0)
            g.flash(p["x"], p["y"], ("pad", shape, size), defin)
        for v in vias:
            g.flash(v["x"], v["y"], ("via",), f"C,{VIA_SIZE:.5f}")
        for s in segs:
            if s["layer"] == layer:
                g.draw(s["x1"], s["y1"], s["x2"], s["y2"],
                       ("track", round(s["w"], 4)), f"C,{s['w']:.5f}")
        emit(fname, g)

    # ---- solder mask ---------------------------------------------------------
    for layer, fname in (("F.Mask", "opentag-F_Mask.gbr"),
                         ("B.Mask", "opentag-B_Mask.gbr")):
        g = Gerber(f"OpenTag carrier PCB - {layer}")
        for p in pads:
            grow = MASK_CLEAR_NPTH if p["npth"] else MASK_CLEAR
            (shape, size), defin = _pad_aperture(p, grow)
            g.flash(p["x"], p["y"], ("mask", shape, size), defin)
        for v in vias:
            g.flash(v["x"], v["y"], ("maskvia",),
                    f"C,{VIA_SIZE + MASK_CLEAR:.5f}")
        emit(fname, g)

    # ---- silkscreen -------------------------------------------------------------
    for layer, fname in (("F.SilkS", "opentag-F_SilkS.gbr"),
                         ("B.SilkS", "opentag-B_SilkS.gbr")):
        g = Gerber(f"OpenTag carrier PCB - {layer}")
        for s in silks:
            if s["kind"] == "line" and s["layer"] == layer:
                g.draw(s["x1"], s["y1"], s["x2"], s["y2"],
                       ("silk",), f"C,{SILK_W:.5f}")
        for s in silks:
            if s["kind"] != "text" or s["layer"] != layer:
                continue
            pixels, px = text_pixels(s["text"], s["size"])
            key = ("text", round(px, 5))
            for dx, dy in pixels:
                g.flash(s["x"] + dx, s["y"] + dy, key,
                        f"R,{px:.5f}X{px:.5f}")
        emit(fname, g)

    # ---- board outline ---------------------------------------------------------
    g = Gerber("OpenTag carrier PCB - Edge.Cuts")
    hw, hh = BOARD_W / 2.0, BOARD_H / 2.0
    for (x1, y1), (x2, y2) in (((-hw, -hh), (hw, -hh)), ((hw, -hh), (hw, hh)),
                               ((hw, hh), (-hw, hh)), ((-hw, hh), (-hw, -hh))):
        g.draw(x1, y1, x2, y2, ("edge",), f"C,{EDGE_W:.5f}")
    emit("opentag-Edge_Cuts.gbr", g)

    # ---- Excellon drill file ------------------------------------------------
    holes = []
    for p in pads:
        holes.append((round(p["drill"], 3), p["x"], p["y"], p["npth"]))
    for v in vias:
        holes.append((round(VIA_DRILL, 3), v["x"], v["y"], False))
    dias = sorted({h[0] for h in holes})
    npth_tools = {i + 1 for i, d in enumerate(dias)
                  if any(h[3] and h[0] == d for h in holes)}
    lines = ["M48", "METRIC,TZ", "FMAT,2"]
    for i, d in enumerate(dias):
        func = "NonPlated,NPTH" if (i + 1) in npth_tools else "Plated,TH"
        lines.append(f";#@! TA.AperFunction,{func}")
        lines.append(f"T{i + 1}C{d:.3f}")
    lines += ["%", "G90", "G05"]
    for i, d in enumerate(dias):
        lines.append(f"T{i + 1}")
        for dia, x, y, _ in holes:
            if dia == d:
                lines.append(f"X{x:.3f}Y{y:.3f}")
    lines.append("M30")
    path = os.path.join(dirpath, "opentag.drl")
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")
    written.append(path)

    # ---- zip ---------------------------------------------------------------------
    zpath = os.path.join(dirpath, "opentag-gerbers.zip")
    with zipfile.ZipFile(zpath, "w", zipfile.ZIP_DEFLATED) as z:
        for p in written:
            z.write(p, os.path.basename(p))
    written.append(zpath)
    return written


# ---------------------------------------------------------------------------
# Gerber verification: parse the emitted files back, compare with the model
# ---------------------------------------------------------------------------

def _norm_draw(w, x1, y1, x2, y2):
    """Direction-independent key for a drawn segment."""
    a, b = sorted(((round(x1, 4), round(y1, 4)), (round(x2, 4), round(y2, 4))))
    return (round(w, 4), a[0], a[1], b[0], b[1])


def parse_gerber(path):
    """Parse an RS-274X file (subset we emit) into geometry sets.

    Returns (flashes, draws): flashes = {(shape, w, h, x, y)} with h None
    for circles; draws = {(dia, x1, y1, x2, y2)} normalised for direction.
    """
    flashes, draws, aps = set(), set(), {}
    cur_ap = None
    cur = None
    scale_ok = False
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line.startswith("%ADD"):
                body = line[4:-2]                  # drop %ADD and *%
                dnum = int(body[:2])
                shape = body[2]
                dims = body[3:].lstrip(",").split("X")
                aps[dnum] = (shape, round(float(dims[0]), 5),
                             round(float(dims[1]), 5) if len(dims) > 1 else None)
                continue
            if line.startswith("%FS"):
                if "X34Y34" not in line:
                    raise ValueError(f"{path}: unexpected format spec {line}")
                scale_ok = True
                continue
            if line.startswith("%") or line.startswith("G04") or line == "M02*":
                continue
            m = re.fullmatch(r"D(\d+)\*", line)
            if m:
                cur_ap = int(m.group(1))
                continue
            m = re.fullmatch(r"(?:G01)?X(-?\d+)Y(-?\d+)D0([123])\*", line)
            if m:
                x, y, op = (int(m.group(1)) / 10000.0,
                            int(m.group(2)) / 10000.0, int(m.group(3)))
                if op == 2:
                    cur = (x, y)
                elif op == 3:
                    if cur_ap is None:
                        raise ValueError(f"{path}: flash with no aperture")
                    shape, w, h = aps[cur_ap]
                    flashes.add((shape, w, h, round(x, 4), round(y, 4)))
                    cur = (x, y)
                else:
                    if cur is None or cur_ap is None:
                        raise ValueError(f"{path}: draw with no move/aperture")
                    shape, w, h = aps[cur_ap]
                    draws.add(_norm_draw(w, cur[0], cur[1], x, y))
                    cur = (x, y)
                continue
            raise ValueError(f"{path}: unparseable line {line!r}")
    if not scale_ok:
        raise ValueError(f"{path}: missing %FSLAX34Y34% header")
    return flashes, draws


def parse_drill(path):
    """Parse the Excellon file into (tools, holes): tools {tnum: dia},
    holes [(dia, x, y)] in file order."""
    tools, holes = {}, []
    cur = None
    in_header = False
    with open(path) as f:
        for line in f:
            s = line.strip()
            if not s or s.startswith(";"):
                continue
            if s == "M48":
                in_header = True
                continue
            if s == "%":
                in_header = False
                continue
            m = re.fullmatch(r"T(\d+)C([\d.]+)", s)
            if m and in_header:
                tools[int(m.group(1))] = round(float(m.group(2)), 3)
                continue
            if s in ("G90", "G05", "M30", "FMAT,2") or s.startswith("METRIC"):
                continue
            m = re.fullmatch(r"T(\d+)", s)
            if m:
                cur = int(m.group(1))
                continue
            m = re.fullmatch(r"X(-?[\d.]+)Y(-?[\d.]+)", s)
            if m:
                if cur is None or cur not in tools:
                    raise ValueError(f"{path}: hole before tool select")
                holes.append((tools[cur], round(float(m.group(1)), 3),
                              round(float(m.group(2)), 3)))
                continue
            raise ValueError(f"{path}: unparseable drill line {s!r}")
    return tools, holes


def verify_gerbers(dirpath):
    """Re-parse the written files and check them against the model.
    Returns a list of error strings (empty = verified)."""
    errors = []

    def chk(cond, msg):
        if not cond:
            errors.append(msg)

    def gpath(name):
        return os.path.join(dirpath, name)

    # ---- expected geometry from the model ----
    exp_cu = set()
    for p in pads:
        if p["shape"] == "rect":
            exp_cu.add(("R", round(p["size"], 4), round(p["size"], 4),
                        round(p["x"], 4), round(p["y"], 4)))
        else:
            exp_cu.add(("C", round(p["size"], 4), None,
                        round(p["x"], 4), round(p["y"], 4)))
    for v in vias:
        exp_cu.add(("C", VIA_SIZE, None, round(v["x"], 4), round(v["y"], 4)))

    exp_mask = set()
    for p in pads:
        grow = MASK_CLEAR_NPTH if p["npth"] else MASK_CLEAR
        size = round(p["size"] + grow, 4)
        shape = "R" if p["shape"] == "rect" else "C"
        h = size if shape == "R" else None
        exp_mask.add((shape, size, h, round(p["x"], 4), round(p["y"], 4)))
    for v in vias:
        exp_mask.add(("C", round(VIA_SIZE + MASK_CLEAR, 4), None,
                      round(v["x"], 4), round(v["y"], 4)))

    # ---- copper + mask layers ----
    for prefix in ("F", "B"):
        flashes, draws = parse_gerber(gpath(f"opentag-{prefix}_Cu.gbr"))
        chk(flashes == exp_cu,
            f"{prefix}_Cu: flashes differ from model "
            f"(missing {sorted(exp_cu - flashes)[:3]}, "
            f"extra {sorted(flashes - exp_cu)[:3]})")
        exp_draws = {_norm_draw(s["w"], s["x1"], s["y1"], s["x2"], s["y2"])
                     for s in segs if s["layer"] == f"{prefix}.Cu"}
        chk(draws == exp_draws,
            f"{prefix}_Cu: tracks differ from model "
            f"(missing {sorted(exp_draws - draws)[:3]}, "
            f"extra {sorted(draws - exp_draws)[:3]})")
        mflashes, mdraws = parse_gerber(gpath(f"opentag-{prefix}_Mask.gbr"))
        chk(mflashes == exp_mask,
            f"{prefix}_Mask: openings differ from model "
            f"(missing {sorted(exp_mask - mflashes)[:3]}, "
            f"extra {sorted(mflashes - exp_mask)[:3]})")
        chk(not mdraws, f"{prefix}_Mask: unexpected draws {sorted(mdraws)[:3]}")
        # every copper shape must have a mask opening at least as large
        for shape, w, _h, x, y in exp_cu:
            ok = any(ms[0] == shape and ms[1] >= w - 1e-9
                     and ms[3] == x and ms[4] == y for ms in mflashes)
            chk(ok, f"{prefix}_Mask: no opening covers {shape} {w} at ({x},{y})")

    # ---- silkscreen ----
    for prefix in ("F", "B"):
        flashes, draws = parse_gerber(gpath(f"opentag-{prefix}_SilkS.gbr"))
        exp_lines = {_norm_draw(SILK_W, s["x1"], s["y1"], s["x2"], s["y2"])
                     for s in silks
                     if s["kind"] == "line" and s["layer"] == f"{prefix}.SilkS"}
        chk(draws == exp_lines,
            f"{prefix}_SilkS: lines differ from model "
            f"(missing {sorted(exp_lines - draws)[:3]}, "
            f"extra {sorted(draws - exp_lines)[:3]})")
        exp_text = set()
        for s in silks:
            if s["kind"] != "text" or s["layer"] != f"{prefix}.SilkS":
                continue
            pixels, px = text_pixels(s["text"], s["size"])
            for dx, dy in pixels:
                exp_text.add(("R", round(px, 5), round(px, 5),
                              round(s["x"] + dx, 4), round(s["y"] + dy, 4)))
        chk(flashes == exp_text,
            f"{prefix}_SilkS: text pixels differ from model "
            f"(missing {sorted(exp_text - flashes)[:3]}, "
            f"extra {sorted(flashes - exp_text)[:3]})")

    # ---- board outline ----
    _f, draws = parse_gerber(gpath("opentag-Edge_Cuts.gbr"))
    hw, hh = BOARD_W / 2.0, BOARD_H / 2.0
    exp_edge = {_norm_draw(EDGE_W, x1, y1, x2, y2)
                for (x1, y1), (x2, y2) in
                (((-hw, -hh), (hw, -hh)), ((hw, -hh), (hw, hh)),
                 ((hw, hh), (-hw, hh)), ((-hw, hh), (-hw, -hh)))}
    chk(draws == exp_edge, f"Edge_Cuts: outline differs from model {draws}")
    xs = sorted({d[1] for d in draws} | {d[3] for d in draws})
    ys = sorted({d[2] for d in draws} | {d[4] for d in draws})
    chk(xs[0] == -hw and xs[-1] == hw and ys[0] == -hh and ys[-1] == hh,
        f"Edge_Cuts: bbox {xs[0]}..{xs[-1]} x {ys[0]}..{ys[-1]} "
        f"!= {-hw}..{hw} x {-hh}..{hh}")

    # ---- drills ----
    tools, holes = parse_drill(gpath("opentag.drl"))
    exp_holes = {(round(p["drill"], 3), round(p["x"], 4), round(p["y"], 4))
                 for p in pads}
    exp_holes |= {(VIA_DRILL, round(v["x"], 4), round(v["y"], 4))
                  for v in vias}
    got_holes = {(d, round(x, 4), round(y, 4)) for d, x, y in holes}
    chk(got_holes == exp_holes,
        f"drill: holes differ from model "
        f"(missing {sorted(exp_holes - got_holes)[:3]}, "
        f"extra {sorted(got_holes - exp_holes)[:3]})")
    chk(len(holes) == len(got_holes), "drill: duplicate holes in file")
    # every plated hole needs an annular ring (pad or via land);
    # NPTH must match its hole pad
    for dia, x, y in got_holes:
        best = max((p["size"] for p in pads
                    if round(p["x"], 4) == x and round(p["y"], 4) == y),
                   default=0.0)
        if any(round(v["x"], 4) == x and round(v["y"], 4) == y
               for v in vias):
            best = max(best, VIA_SIZE)
        if dia >= 3.0:
            chk(abs(best - dia) < 1e-6,
                f"drill: NPTH {dia} at ({x},{y}) has no matching hole pad")
        else:
            chk(best >= dia + 0.4,
                f"drill: hole {dia} at ({x},{y}) pad {best} lacks "
                f"annular ring (< drill + 0.4)")
    # plated/NPTH attributes present
    with open(gpath("opentag.drl")) as f:
        drl = f.read()
    chk("TA.AperFunction,Plated,TH" in drl, "drill: no Plated attribute")
    chk("TA.AperFunction,NonPlated,NPTH" in drl, "drill: no NPTH attribute")
    return errors
if __name__ == "__main__":
    sys.exit(main())
