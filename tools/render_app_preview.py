#!/usr/bin/env python3
"""Renders docs/app-ui-preview.png — a scroll-capture-style preview of the
Android app UI, from the same spec (colors/texts/structure) as
activity_main.xml + item_amiibo.xml, plus the mark geometry from
tools/build_icon.py. Preview only — not a build artifact.

Run from the repository root:  pip install pillow && python3 tools/render_app_preview.py
"""
import math
from PIL import Image, ImageDraw, ImageFont

W = 760
PAD = 36
CW = W - 2 * PAD
F = "/usr/share/fonts/truetype/dejavu/"

def font(sz, bold=False, mono=False):
    name = ("DejaVuSansMono-Bold.ttf" if bold else "DejaVuSansMono.ttf") if mono \
        else ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf")
    return ImageFont.truetype(F + name, sz)

BG, GLOW = "#11151c", "#28334b"
CARD_A, CARD_B, CARD_LN = "#202735", "#1a2029", "#2b3442"
ITEM, ITEM_LN = "#141a22", "#293341"
TEXT, MUTED = "#f4f6f8", "#9ba7b7"
MINT, MINT_DK, MINT_LN = "#b6f36a", "#10131b", "#5b7135"
VIOLET, VIOLET_LN = "#bba8ff", "#554c78"
BAD, BAD_LN = "#ff8b8b", "#7a4040"
WARN, SEC, LINE = "#ffcc9b", "#2a3341", "#303a49"
INPUT_BG, LOG_BG, LOG_LN, LOG_TX = "#111720", "#0d1117", "#232c39", "#a8b4c6"
PANEL = "#1b212b"

H = 5600
img = Image.new("RGB", (W, H), BG)
d = ImageDraw.Draw(img)
overlay = Image.new("RGBA", (W, H), (0, 0, 0, 0))
od = ImageDraw.Draw(overlay)
for i in range(50, 0, -1):
    a = max(0, int(50 * (1 - i / 50)) * 2)
    od.ellipse([W - 5 - i * 9, -5 - i * 6, W + 60, 160], fill=(40, 51, 75, a))
img = Image.alpha_composite(img.convert("RGBA"), overlay).convert("RGB")
d = ImageDraw.Draw(img)

def text(x, yy, s, f, fill, anchor="la"):
    d.text((x, yy), s, font=f, fill=fill, anchor=anchor)
    return d.textbbox((x, yy), s, font=f, anchor=anchor)[3]

def wrap(s, f, maxw):
    out, line = [], ""
    for w in s.split(" "):
        t = (line + " " + w).strip()
        if d.textlength(t, font=f) <= maxw: line = t
        else: out.append(line); line = w
    if line: out.append(line)
    return out

def para(x, yy, s, f, fill, maxw, lh):
    for ln in wrap(s, f, maxw): yy = text(x, yy, ln, f, fill) + lh
    return yy

def rrect(box, r, fill=None, outline=None, w=1):
    d.rounded_rectangle(box, radius=r, fill=fill, outline=outline, width=w)

def gradient_card(yy, h):
    c1, c2 = (0x20, 0x27, 0x35), (0x1a, 0x20, 0x29)
    gh = Image.new("RGB", (CW, h), CARD_A)
    gd = ImageDraw.Draw(gh)
    for i in range(h):
        t = i / max(1, h - 1)
        gd.line([(0, i), (CW, i)], fill=tuple(int(c1[k] + (c2[k] - c1[k]) * t) for k in range(3)))
    mask = Image.new("L", (CW, h), 0)
    ImageDraw.Draw(mask).rounded_rectangle([0, 0, CW - 1, h - 1], radius=44, fill=255)
    img.paste(gh, (PAD, yy), mask)
    d.rounded_rectangle([PAD, yy, PAD + CW, yy + h], radius=44, outline=CARD_LN, width=1)
    return [PAD, yy, PAD + CW, yy + h]

def button(x, yy, w, label, primary=True, enabled=True):
    f = font(26, bold=True)
    h = 84
    rrect([x, yy, x + w, yy + h], 26, fill=(MINT if primary else SEC))
    if not enabled:
        ov = Image.new("RGBA", (w, h), (17, 21, 28, 115))
        m = Image.new("L", (w, h), 0)
        ImageDraw.Draw(m).rounded_rectangle([0, 0, w - 1, h - 1], radius=26, fill=255)
        img.paste(Image.alpha_composite(img.crop((x, yy, x + w, yy + h)).convert("RGBA"), ov).convert("RGB"), (x, yy), m)
    tc = MINT_DK if primary else TEXT
    d.text((x + w / 2, yy + h / 2), label, font=f, fill=tc, anchor="mm")
    return yy + h

def chip(x, yy, label, kind):
    f = font(20, bold=True)
    w = d.textlength(label, font=f) + 28
    col, ln = {"violet": (VIOLET, VIOLET_LN), "mint": (MINT, MINT_LN), "bad": (BAD, BAD_LN)}[kind]
    rrect([x, yy, x + w, yy + 44], 14, outline=ln)
    d.text((x + 14, yy + 22), label, font=f, fill=col, anchor="lm")
    return w

def row(yy, title, meta, sub=None, chipkind=None, chiplabel=None, art="#3a6ea5", letter="M"):
    rrect([PAD, yy, PAD + CW, yy + 150], 30, fill=ITEM, outline=ITEM_LN)
    rrect([PAD + 24, yy + 28, PAD + 24 + 88, yy + 28 + 88], 18, fill="#0d1219", outline=ITEM_LN)
    d.text([PAD + 24 + 44, yy + 28 + 44], letter, font=font(44, bold=True), fill="#cfe3d0", anchor="mm")
    tx = PAD + 24 + 88 + 24
    d.text((tx, yy + 30), title, font=font(26, bold=True), fill=TEXT)
    d.text((tx, yy + 68), meta, font=font(24), fill=MUTED)
    if sub: d.text((tx, yy + 102), sub, font=font(24), fill=MUTED)
    if chiplabel: chip(PAD + CW - d.textlength(chiplabel, font=font(20, bold=True)) - 28 - 14, yy + 53, chiplabel, chipkind)
    return yy + 150 + 16

# ---------- the OpenTag mark (same geometry as tools/build_icon.py) ----------
def mark(x, y, size):
    tile = int(size * 0.68)
    tx, ty = x + (size - tile) // 2, y + (size - tile) // 2
    rrect([tx, ty, tx + tile, ty + tile], int(tile * 0.33), fill=PANEL, outline=LINE)
    ss = 4
    ts = tile * ss
    m = Image.new("L", (ts, ts), 0)
    md = ImageDraw.Draw(m)
    RCX, RCY, RO, RI = 49.0, 60.0, 23.5, 10.6
    NOTCH, HALF = math.radians(-45), math.radians(18)
    sc = ts / 108.0
    cx, cy = RCX * sc, RCY * sc
    md.ellipse([cx - RO * sc, cy - RO * sc, cx + RO * sc, cy + RO * sc], fill=255)
    md.ellipse([cx - RI * sc, cy - RI * sc, cx + RI * sc, cy + RI * sc], fill=0)
    a1, a2 = NOTCH - HALF, NOTCH + HALF
    wedge = [(cx, cy)] + [(cx + RO * sc * 1.2 * math.cos(a), cy + RO * sc * 1.2 * math.sin(a))
                          for a in [a1 + (a2 - a1) * i / 24 for i in range(25)]]
    md.polygon(wedge, fill=0)
    ring = Image.new("RGBA", (ts, ts), (0, 0, 0, 0))
    ring.paste(Image.new("RGBA", (ts, ts), MINT), (0, 0), m)
    r_small = ring.resize((tile, tile), Image.LANCZOS)
    img.paste(r_small, (tx, ty), r_small)
    dx, dy = cx + RO * sc * math.cos(NOTCH), cy + RO * sc * math.sin(NOTCH)
    dot = Image.new("RGBA", (ts, ts), (0, 0, 0, 0))
    ImageDraw.Draw(dot).ellipse([dx - 6.2 * sc, dy - 6.2 * sc, dx + 6.2 * sc, dy + 6.2 * sc], fill=VIOLET)
    dd = dot.resize((tile, tile), Image.LANCZOS)
    img.paste(dd, (tx, ty), dd)

# ---------- brand ----------
mark(PAD, 48, 80)
d.text((PAD + 104, 54), "opentag", font=font(40, bold=True), fill=TEXT)
d.text((PAD + 106, 100), "D E V I C E   S Y N C", font=font(18, bold=True), fill=MUTED)
y = 172
for i, ln in enumerate(["Your tags,", "ready to tap."]):
    d.text((PAD, y + i * 74), ln, font=font(66, bold=True), fill=TEXT)
y += 170
y = para(PAD, y, "Send a text note or web URL to your OpenTag over Bluetooth LE, upload your own "
                 "NTAG215 amiibo backups to the on-device library, emulate any of them from the "
                 "device’s menu or right here in the app — and browse the full amiibo database with "
                 "box art below, no connection needed.",
         font(26), MUTED, CW, 12) + 26

# ---------- card 1: device connection ----------
cy = y + 8
gradient_card(cy, 780)
iy = cy + 40
d.text((PAD + 40, iy), "Device connection", font=font(30, bold=True), fill=TEXT)
d.text((PAD + 40, iy + 44), "OpenTag · OLED handheld · battery 85%", font=font(24), fill=MUTED)
d.ellipse([PAD + CW - 210, iy + 8, PAD + CW - 210 + 42, iy + 8 + 42], fill="#46b6f36a")
d.ellipse([PAD + CW - 210 + 10, iy + 18, PAD + CW - 210 + 32, iy + 40], fill=MINT)
d.text((PAD + CW - 150, iy + 16), "Connected", font=font(26), fill=MUTED)
iy += 92
button(PAD + 40, iy, 460, "Connect over BLE", primary=True, enabled=False)
button(PAD + 40 + 480, iy, CW - 80 - 480, "Disconnect", primary=False, enabled=True)
iy += 84 + 28
d.rectangle([PAD + 40, iy, PAD + 46, iy + 170], fill=VIOLET)
iy = para(PAD + 58, iy - 4, "Power the device on (the handheld advertises as OpenTag, the screenless "
          "compact build as OpenTag Mini — the Mini pairs without a PIN), then tap Connect over BLE. "
          "Android asks for Bluetooth permissions on first use — they are required for scanning. "
          "Everything the device answers appears in the log below.", font(24), MUTED, CW - 100, 12) + 16
rrect([PAD + 40, iy, PAD + CW - 40, iy + 344], 26, fill=LOG_BG, outline=LOG_LN)
ly = iy + 20
for ln, c in [("Scanning for OpenTag…", MUTED), ("Found OpenTag, connecting…", MUTED),
              ("OK:connected  MTU 247", LOG_TX), ("INFO", LOG_TX),
              ("OK:info:standard:3985:0", MINT), ("AMI:LIST", LOG_TX),
              ("AMI:0 mario-smash", LOG_TX),
              ("AMI:1 0000000000340102", LOG_TX), ("OK:ami-list 2 entries", MINT),
              ("KEY:STATUS", LOG_TX), ("OK:keys:ready", LOG_TX)]:
    d.text((PAD + 66, ly), ln, font=font(24, mono=True), fill=c); ly += 38
y = cy + 780 + 32

# ---------- card 2: NDEF note ----------
gradient_card(y, 980)
iy = y + 40
d.text((PAD + 40, iy), "NDEF note", font=font(30, bold=True), fill=TEXT)
d.text((PAD + 40 + d.textlength("NDEF note", font=font(30, bold=True)) + 12, iy + 7),
       "— a short text or link the tag carries", font=font(24), fill=MUTED)
iy = para(PAD + 40, iy + 48, "An NDEF note is the standard “message” stored on NFC tags: up to 200 bytes "
          "of plain text or an http(s) link. Send it to the device, and the device menu’s Write / Emulate / "
          "Clear NDEF note items act on it — a blank NTAG215 can then carry it, or the device can emulate "
          "it as a virtual tag.", font(24), MUTED, CW - 80, 12) + 22
d.text((PAD + 40, iy), "Content", font=font(24, bold=True), fill=MUTED); iy += 38
rrect([PAD + 40, iy, PAD + CW - 40, iy + 82], 24, fill=INPUT_BG, outline=MINT)
d.text((PAD + 64, iy + 40), "https://opentag.example", font=font(26), fill=TEXT, anchor="lm")
iy += 82 + 24
button(PAD + 40, iy, 400, "Send note now")
button(PAD + 40 + 416, iy, CW - 80 - 416, "Clear device note", primary=False, enabled=True)
iy += 84 + 14
button(PAD + 40, iy, 400, "Write to NTAG215", primary=False, enabled=True)
button(PAD + 40 + 416, iy, CW - 80 - 416, "Emulate note", primary=False, enabled=True)
iy += 84 + 22
iy = para(PAD + 40, iy, "Write to NTAG215 arms the device — tap a blank tag on its antenna and the note "
          "is written and read-back verified. Emulate note presents it as a virtual tag instead.",
          font(24), MUTED, CW - 80, 12) + 14
iy = para(PAD + 40, iy, "Text starting with http:// or https:// is sent as a URL record, everything else "
          "as plain text. The note survives reboots.", font(24), MUTED, CW - 80, 12) + 16
y = y + 980 + 32

# ---------- card 3: amiibo library ----------
gradient_card(y, 1330)
iy = y + 40
d.text((PAD + 40, iy), "Amiibo library", font=font(30, bold=True), fill=TEXT)
iy = para(PAD + 40, iy + 46, "Pick a .zip, loose .bin dumps, or Flipper Zero .nfc files from your own "
          "collection — any layout (532/540/572 bytes, padded, or with an N2-style header) is normalized "
          "on the phone, validated, then copied to the device over BLE. Kirby Air Riders v3 dumps "
          "(1024-byte NTAG I²C page images) are accepted as-is for emulation. Nothing is uploaded to any "
          "server.", font(24), MUTED, CW - 80, 12) + 22
button(PAD + 40, iy, 430, "Choose .zip / .bin / .nfc", primary=True, enabled=True)
button(PAD + 40 + 446, iy, CW - 80 - 446, "Upload all", primary=False, enabled=True)
iy += 84 + 26
iy = row(iy, "mario-smash.bin", "Mario · Super Smash Bros. · Figure · 2014", "540 B · UID 04:D4:3C:52:1A:6F:9B", "mint", "NTAG215", "#c0504d", "M")
iy = row(iy, "bandana-dee-v3.bin", "Bandana Dee · Kirby Air Riders · 2025", "1024 B · UID 04:91:E2:0C:44:1F:03", "mint", "V3 · I2C", "#4f81bd", "B")
iy = row(iy, "corrupt-dump.bin", "no metadata match", "blank UID (data-only export)", "bad", "INVALID", "#777777", "?")
d.rectangle([PAD + 40, iy, PAD + CW - 40, iy + 2], fill=CARD_LN); iy += 34
d.text((PAD + 40, iy), "On device", font=font(26, bold=True), fill=TEXT); iy += 44
button(PAD + 40, iy, 380, "Refresh list", primary=False, enabled=True)
button(PAD + 40 + 396, iy, CW - 80 - 396, "Stop emulation", primary=False, enabled=True)
iy += 84 + 14
button(PAD + 40, iy, 380, "Back up a tag", primary=False, enabled=True)
button(PAD + 40 + 396, iy, CW - 80 - 396, "Inspect a tag", primary=False, enabled=True)
iy += 84 + 22
iy = para(PAD + 40, iy, "Back up arms the reader: tap your own amiibo on the device antenna and it is "
          "saved to the library under its UID. Inspect identifies any tag — the UID, chip type and any "
          "NDEF text appear in the log.", font(24), MUTED, CW - 80, 12) + 18
iy = row(iy, "mario-smash.bin", "Mario · Super Smash Bros. · Figure · 2014", None, "violet", "BIN", "#c0504d", "M")
iy = row(iy, "0000000000340102.bin", "Mario · Super Smash Bros. · Figure · 2014", None, "violet", "BIN", "#c0504d", "M")
y = y + 1330 + 32

# ---------- card 4: amiibo database ----------
gradient_card(y, 690)
iy = y + 40
d.text((PAD + 40, iy), "Amiibo database", font=font(30, bold=True), fill=TEXT)
d.text((PAD + 40 + d.textlength("Amiibo database", font=font(30, bold=True)) + 12, iy + 7),
       "— browse before you sync", font=font(24), fill=MUTED)
iy = para(PAD + 40, iy + 48, "Every figure in the open AmiiboAPI database, with box art, series, type and "
          "release year — the same data your dumps are matched against above. Needs no device connection.",
          font(24), MUTED, CW - 80, 12) + 22
rrect([PAD + 40, iy, PAD + CW - 40, iy + 82], 24, fill=INPUT_BG, outline="#303a49")
d.text((PAD + 64, iy + 40), "Search name, series or game — e.g. link, animal crossing…",
       font=font(24), fill="#9ba7b7", anchor="lm")
iy += 82 + 22
d.text((PAD + 40, iy), "showing the first 120 matches of 932 amiibo — keep typing to narrow it down",
       font=font(24), fill=MUTED)
iy += 40
iy = row(iy, "Link", "The Legend of Zelda · Figure · 2014", None, None, None, "#4f81bd", "L")
iy = row(iy, "Link - Archer", "The Legend of Zelda: Breath of the Wild · Figure · 2017", None, None, None, "#6fa860", "L")
y = y + 690 + 32

# ---------- card 5: signing keys ----------
gradient_card(y, 700)
iy = y + 40
d.text((PAD + 40, iy), "Amiibo signing keys", font=font(30, bold=True), fill=TEXT)
d.text((PAD + 40 + d.textlength("Amiibo signing keys", font=font(30, bold=True)) + 12, iy + 7),
       "(advanced)", font=font(24), fill=MUTED)
iy = para(PAD + 40, iy + 48, "OpenTag emulates amiibo — no physical tag writing needed. These keys are only "
          "for the optional advanced flow that survives on the device (long-press in its library writes to a "
          "real NTAG215, TagMo-style, using your unfixed-info.bin and locked-secret.bin). A combined "
          "160-byte retail key file also works. Files are identified by their embedded type string, stored "
          "in the device’s NVS, and never bundled with this project or sent anywhere else.", font(24), MUTED, CW - 80, 12) + 22
button(PAD + 40, iy, 430, "Choose key .bin files", primary=True, enabled=True)
button(PAD + 40 + 446, iy, CW - 80 - 446, "Upload keys", primary=False, enabled=True)
iy += 84 + 20
button(PAD + 40, iy, 380, "Check status", primary=False, enabled=True)
button(PAD + 40 + 396, iy, CW - 80 - 396, "Forget keys", primary=False, enabled=True)
iy += 84 + 22
iy = para(PAD + 40, iy, "Key state: ready — both key files loaded, writes enabled", font(24), TEXT, CW - 80, 10) + 14
para(PAD + 40, iy, "Only use key files you legally extracted yourself. Uploading them is your responsibility.",
     font(24), WARN, CW - 80, 10)
y = y + 700 + 32

# ---------- card 6: firmware updates over Wi-Fi ----------
gradient_card(y, 880)
iy = y + 40
d.text((PAD + 40, iy), "Firmware updates over Wi-Fi", font=font(30, bold=True), fill=TEXT)
iy = para(PAD + 40, iy + 48, "Save your Wi-Fi credentials once, then update the OpenTag without a computer: "
          "it fetches the newest firmware release from GitHub, flashes only the app partition and reboots — "
          "your amiibo library, keys and note survive every update. Credentials live in the device’s "
          "settings and are only used for updates. The wired one-command updater "
          "(tools/update_opentag.py) keeps working without any of this.", font(24), MUTED, CW - 80, 12) + 22
d.text((PAD + 40, iy), "Wi-Fi name (SSID)", font=font(24, bold=True), fill=MUTED); iy += 38
rrect([PAD + 40, iy, PAD + CW - 40, iy + 82], 24, fill=INPUT_BG, outline="#303a49")
d.text((PAD + 64, iy + 40), "MyNetwork", font=font(26), fill=TEXT, anchor="lm")
iy += 82 + 22
d.text((PAD + 40, iy), "Password", font=font(24, bold=True), fill=MUTED); iy += 38
rrect([PAD + 40, iy, PAD + CW - 40, iy + 82], 24, fill=INPUT_BG, outline="#303a49")
d.text((PAD + 64, iy + 40), "••••••••", font=font(26), fill=MUTED, anchor="lm")
iy += 82 + 24
bw = (CW - 80 - 16) // 3
button(PAD + 40, iy, bw, "Save Wi-Fi", primary=True, enabled=True)
button(PAD + 40 + bw + 8, iy, bw, "Check status", primary=False, enabled=True)
button(PAD + 40 + 2 * (bw + 8), iy, bw, "Update now", primary=False, enabled=True)
iy += 84 + 22
para(PAD + 40, iy, "Updating needs battery or a charger connected — a power loss during flashing falls back "
     "to the previous app on the next boot, but finishing an update is nicer. Progress streams into the "
     "sync log; the device’s button cancels (BACK, or OK on the Mini).",
     font(24), WARN, CW - 80, 10)
y = y + 880 + 20

para(PAD, y + 14, "OpenTag is an independent open-source project. NFC writes replace the existing NDEF "
     "area of the selected NTAG215. Confirm on the device before writing; only use tags you own or are "
     "authorized to program.", font(22), MUTED, CW, 12)

img = img.crop((0, 0, W, y + 180))
img.save("docs/app-ui-preview.png")
print("saved docs/app-ui-preview.png", img.size)
