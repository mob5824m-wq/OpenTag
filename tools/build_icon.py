#!/usr/bin/env python3
"""OpenTag's icon geometry, kept in one place (same approach as SoundTrail's
tools/build_icon.py: one source of truth emits every artifact).

The mark: a bold mint ring — the "O" — with a notch opened at 45 deg
upper-right, and a violet dot (the tag) seated in that notch. Dark
full-bleed background, so whatever shape a launcher masks (circle,
squircle, rounded square) is filled edge to edge — the icon is "full",
never a square tile floating on white.

Artifacts written from this geometry:
  android-app/app/src/main/res/drawable/ic_launcher_foreground.xml    adaptive foreground (108dp)
  android-app/app/src/main/res/drawable/ic_launcher_monochrome.xml    Android 13+ themed icon
  android-app/app/src/main/res/mipmap-anydpi-v26/ic_launcher.xml      the adaptive icon itself
  android-app/app/src/main/res/mipmap-{mdpi..xxxhdpi}/ic_launcher.png legacy rasters (API < 26)

Run from the repository root (needs Pillow for the PNGs):
    pip install pillow && python3 tools/build_icon.py
"""
import math
import pathlib
import re

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "android-app" / "app" / "src" / "main" / "res"

# ----- palette (mirrors res/values/colors.xml and the web sync page) --------------------
BACKGROUND = "#11151c"   # full-bleed launcher background
PANEL = "#1b212b"
MINT = "#b6f36a"         # the ring — the O of OpenTag
VIOLET = "#bba8ff"       # the tag dot seated in the notch
GLOW = (40, 51, 75)      # #28334b, the page's top-right glow

# ----- canvas: 108dp, the adaptive-icon standard ----------------------------------------
SIZE = 108.0
CX, CY = 54.0, 54.0
SAFE_R = 33.0            # only the central 66dp circle is guaranteed visible

# Ring geometry (centre deliberately low-left so the notch faces the canvas centre)
RCX, RCY = 49.0, 60.0
RING_OUTER = 23.5
RING_INNER = 10.6
NOTCH_DEG = -45.0        # screen angle, y-down: up and to the right
NOTCH_HALF = 18.0        # degrees of ring removed either side of the notch
DOT_R = 6.2              # sits on the ring's centreline, in the notch
MONO_EXTRA = 0.8         # themed icons get slightly heavier strokes

PNG_SIZES = {"mdpi": 48, "hdpi": 72, "xhdpi": 96, "xxhdpi": 144, "xxxhdpi": 192}


# ----- geometry ------------------------------------------------------------------------

def polar(cx, cy, r, deg):
    a = math.radians(deg)
    return (cx + r * math.cos(a), cy + r * math.sin(a))


def ring_path(cx=RCX, cy=RCY, r_out=RING_OUTER, r_in=RING_INNER,
              notch=NOTCH_DEG, half=NOTCH_HALF):
    """Annular band with a wedge removed at `notch` degrees (y-down screen
    angles): outer arc the long way round, radial cut, inner arc back."""
    a1, a2 = notch + half, notch - half          # band spans a1 -> a2 the long way
    ox1, oy1 = polar(cx, cy, r_out, a1)
    ox2, oy2 = polar(cx, cy, r_out, a2)
    ix2, iy2 = polar(cx, cy, r_in, a2)
    ix1, iy1 = polar(cx, cy, r_in, a1)
    return (f"M{ox1:.2f},{oy1:.2f}"
            f"A{r_out:.2f},{r_out:.2f} 0 1 1 {ox2:.2f},{oy2:.2f}"
            f"L{ix2:.2f},{iy2:.2f}"
            f"A{r_in:.2f},{r_in:.2f} 0 1 0 {ix1:.2f},{iy1:.2f}Z")


def circle_path(cx, cy, r):
    return (f"M{cx - r:.2f},{cy:.2f}"
            f"a{r:.2f},{r:.2f} 0 1 0 {2 * r:.2f},0"
            f"a{r:.2f},{r:.2f} 0 1 0 {-2 * r:.2f},0Z")


def dot_centre(notch=NOTCH_DEG, r=RING_OUTER):
    return polar(RCX, RCY, r, notch)


def mark():
    """(path data, paint, colour). `paint` is "fill" or ("stroke", width)."""
    dx, dy = dot_centre()
    return [
        (ring_path(), "fill", MINT),
        (circle_path(dx, dy, DOT_R), "fill", VIOLET),
    ]


def monochrome_mark():
    """Android 13+ themed icons: one flat silhouette. The system tints the
    whole layer a single colour, so the mark is drawn slightly heavier and
    both subpaths live in one path element (separate elements would work,
    but one element keeps the silhouette portable)."""
    heavy = ring_path(r_out=RING_OUTER + MONO_EXTRA, r_in=RING_INNER - MONO_EXTRA,
                      half=NOTCH_HALF - 1.5)
    dx, dy = dot_centre()
    dot = circle_path(dx, dy, DOT_R + MONO_EXTRA)
    return [(heavy + " " + dot, "fill", "#FFFFFFFF")]


def check_safe_zone():
    """No painted geometry may leave the 66dp safe circle (radius 33 at 54,54).
    The mark is circles, so the reach is exact: ring = |centre-offset| +
    outer radius (monochrome's heavier stroke included), dot likewise."""
    ring_reach = math.hypot(RCX - CX, RCY - CY) + RING_OUTER + MONO_EXTRA
    dx, dy = dot_centre()
    dot_reach = math.hypot(dx - CX, dy - CY) + DOT_R + MONO_EXTRA
    worst = max(ring_reach, dot_reach)
    if worst > SAFE_R:
        raise SystemExit(f"geometry reaches {worst:.1f}dp from centre, "
                         f"safe circle is {SAFE_R}dp")
    return worst


# ----- path-data validation (a path Android cannot parse becomes a placeholder icon) -----

ARITY = {"M": 2, "L": 2, "H": 1, "V": 1, "C": 6, "S": 4, "Q": 4, "T": 2, "A": 7, "Z": 0}
NUMBER = re.compile(r"[-+]?(?:\d*\.\d+|\d+\.?)(?:[eE][-+]?\d+)?")


def validate(path_data, label):
    for cmd, args in re.findall(r"([MmLlHhVvCcSsQqTtAaZz])([^MmLlHhVvCcSsQqTtAaZz]*)", path_data):
        need = ARITY[cmd.upper()]
        if need == 0:
            continue
        found = len(NUMBER.findall(args))
        if found == 0 or found % need:
            raise SystemExit(f"{label}: '{cmd}' carries {found} numbers; needs groups of {need}")
    return path_data


def vector_document(paint_layers, width_dp=108.0, comment="Generated by tools/build_icon.py — edit the geometry there, not here."):
    lines = []
    for data, paint, colour in paint_layers:
        attrs = [f'android:fillColor="{colour}"']
        if isinstance(paint, tuple) and paint[0] == "stroke":
            attrs = [f'android:strokeColor="{colour}"',
                     f'android:strokeWidth="{paint[1]}"',
                     'android:strokeLineCap="round"',
                     'android:strokeLineJoin="round"']
        lines.append('    <path %s\n                android:pathData="%s" />'
                     % (" ".join(attrs), validate(data, "pathData")))
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            f'<!-- {comment} -->\n'
            '<vector xmlns:android="http://schemas.android.com/apk/res/android"\n'
            f'    android:width="{width_dp:g}dp"\n'
            f'    android:height="{width_dp:g}dp"\n'
            f'    android:viewportWidth="{SIZE:g}"\n'
            f'    android:viewportHeight="{SIZE:g}">\n'
            + "\n".join(lines) + "\n</vector>\n")


def write(path, text):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text)
    print(f"wrote {path.relative_to(ROOT)}")


# ----- legacy rasters (API < 26) --------------------------------------------------------

def rasterise(size_px):
    """The same mark on a rounded dark tile with the brand glow, supersampled
    4x for smooth edges. Geometry is the same as the vector's."""
    from PIL import Image, ImageDraw

    ss = 4
    W = size_px * ss
    scale = W / SIZE
    img = Image.new("RGB", (W, W), BACKGROUND)
    d = ImageDraw.Draw(img)

    # top-right glow, like the app's window background
    glow_r = int(W * 0.9)
    for i in range(28, 0, -1):
        r = int(glow_r * i / 28)
        a = int(255 * 0.16 * (1 - i / 28))
        layer = Image.new("RGB", (W, W), BACKGROUND)
        ld = ImageDraw.Draw(layer)
        ld.ellipse([W - r, -r, W + r, r], fill=GLOW)
        img = Image.blend(img, layer, a / 255.0)
    d = ImageDraw.Draw(img)

    def P(pt):
        return (pt[0] * scale, pt[1] * scale)

    # ring band minus notch: polygon strips between inner and outer radii
    a1 = math.radians(NOTCH_DEG + NOTCH_HALF)
    a2 = math.radians(NOTCH_DEG - NOTCH_HALF)
    steps = 220
    outer, inner = [], []
    # from a1 sweep the long way (increasing angle) to a2 + 2pi
    start = a1
    end = a2 if a2 > a1 else a2 + 2 * math.pi
    for i in range(steps + 1):
        a = start + (end - start) * i / steps
        outer.append(P((RCX + RING_OUTER * math.cos(a), RCY + RING_OUTER * math.sin(a))))
        inner.append(P((RCX + RING_INNER * math.cos(a), RCY + RING_INNER * math.sin(a))))
    d.polygon(outer + inner[::-1], fill=MINT)

    dx, dy = dot_centre()
    cx, cy = dx * scale, dy * scale
    d.ellipse([cx - DOT_R * scale, cy - DOT_R * scale,
               cx + DOT_R * scale, cy + DOT_R * scale], fill=VIOLET)

    # rounded-corner tile (full bleed to the rounded edge, no white margins)
    radius = int(size_px * 0.22) * ss
    mask = Image.new("L", (W, W), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, W - 1, W - 1], radius=radius, fill=255)
    out = Image.new("RGBA", (W, W), (0, 0, 0, 0))
    out.paste(img, (0, 0), mask)
    return out.resize((size_px, size_px), Image.LANCZOS)


# ----- main ----------------------------------------------------------------------------

def main():
    fg = mark()
    mono = monochrome_mark()
    reach = check_safe_zone()
    print(f"safe-zone check ok: mark reaches {reach:.1f}dp of the allowed {SAFE_R:.0f}dp")

    write(RES / "drawable" / "ic_launcher_foreground.xml", vector_document(fg))
    write(RES / "drawable" / "ic_launcher_monochrome.xml", vector_document(mono))

    adaptive = (
        '<?xml version="1.0" encoding="utf-8"?>\n'
        '<!-- Generated by tools/build_icon.py — edit the geometry there, not here. -->\n'
        '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
        '    <background android:drawable="@color/icon_background" />\n'
        '    <foreground android:drawable="@drawable/ic_launcher_foreground" />\n'
        '    <monochrome android:drawable="@drawable/ic_launcher_monochrome" />\n'
        '</adaptive-icon>\n')
    write(RES / "mipmap-anydpi-v26" / "ic_launcher.xml", adaptive)

    for bucket, px in PNG_SIZES.items():
        p = RES / ("mipmap-" + bucket) / "ic_launcher.png"
        p.parent.mkdir(parents=True, exist_ok=True)
        rasterise(px).save(p)
        print(f"wrote {p.relative_to(ROOT)}")

    # the web sync page's inline SVG uses this same geometry — print it for
    # pasting into device/web-sync/index.html (48-unit viewBox)
    print("\nweb SVG (viewBox 0 0 108 108):")
    for data, paint, colour in fg:
        print(f'  <path d="{data}" fill="{colour}"/>')


if __name__ == "__main__":
    main()
