#!/usr/bin/env python3
"""Builds docs/opentag-user-guide.pdf — the printed instruction booklet.

The cover art is generated from the same single sources of truth the device
uses: the OPENTAG pixel wordmark font and the lightning-bolt charge icon are
parsed straight out of hardware/cad/opentag_shell.cad (brand_font/bolt_cols),
so the booklet can never drift from the shell engraving or the OLED font.

Run:  pip install fpdf2  &&  python3 tools/make_booklet.py
(only fpdf2 beyond the stdlib; DejaVu Sans from the system font path)
"""

import re
import sys
from pathlib import Path

from fpdf import FPDF

ROOT = Path(__file__).resolve().parent.parent
CAD = ROOT / "hardware" / "cad" / "opentag_shell.cad"
OUT = ROOT / "docs" / "opentag-user-guide.pdf"
FONT_DIR = Path("/usr/share/fonts/truetype/dejavu")

# ---- palette: the device OLED's own RGB565 colours, unpacked ----------------

def rgb565(v):
    return (((v >> 11) & 0x1F) * 255 // 31, ((v >> 5) & 0x3F) * 255 // 63,
            (v & 0x1F) * 255 // 31)

BG = rgb565(0x1082)        # OLED background — near-black
MINT = rgb565(0xB7EF)      # selection / brand
LAVENDER = rgb565(0xC5DF)  # messages
WHITE = (255, 255, 255)
MUTED = rgb565(0x9CF3)     # secondary text
INK = (24, 24, 24)         # body text on paper
RED = rgb565(0xF986)       # warnings
PAPER = (255, 255, 255)
RULE = (210, 210, 214)
TIP_BG = (238, 240, 248)   # lavender-tinted tip box
TIP_EDGE = rgb565(0xC5DF)


def parse_cad_art():
    """brand_text + brand_font (A-Z 5x7 columns) + bolt_cols from the .cad."""
    import ast
    src = CAD.read_text()
    m = re.search(r'brand_text\s*=\s*"([^"]*)"', src)
    text = m.group(1) if m else "OPENTAG"
    m = re.search(r"brand_font\s*=\s*(\[.*?\]);", src, re.S)
    font = {e[0]: e[1] for e in ast.literal_eval(m.group(1))} if m else {}
    m = re.search(r"bolt_cols\s*=\s*(\[.*?\]);", src, re.S)
    bolt = ast.literal_eval(m.group(1)) if m else []
    return text, font, bolt


BRAND_TEXT, BRAND_FONT, BOLT = parse_cad_art()

# ---------------------------------------------------------------------------


class Book(FPDF):
    W, H = 148, 210          # A5 portrait

    def __init__(self):
        super().__init__(orientation="P", unit="mm", format=(self.W, self.H))
        self.set_margins(14, 14, 14)
        self.set_auto_page_break(True, margin=14)
        self.add_font("dv", "", FONT_DIR / "DejaVuSans.ttf")
        self.add_font("dv", "B", FONT_DIR / "DejaVuSans-Bold.ttf")
        self.set_title("OpenTag User Guide")
        self.set_creator("tools/make_booklet.py")

    # -- primitives ---------------------------------------------------------

    def sheet(self, title=None, footer=True):
        self.add_page()
        if title:
            self.set_font("dv", "B", 15)
            self.set_text_color(*MINT)
            self.set_draw_color(*MINT)
            self.set_line_width(0.5)
            y = self.get_y()
            self.cell(0, 8, title, new_x="LMARGIN", new_y="NEXT")
            self.line(self.l_margin, y + 9.5, self.W - self.r_margin, y + 9.5)
            self.set_y(y + 13)
            self.set_text_color(*INK)
        if footer:
            self.set_font("dv", "", 7.5)
            self.set_text_color(150, 150, 155)
            self.text(self.l_margin, self.H - 7, "OPENTAG user guide")
            s2 = "open source - see LICENSE"
            self.text(self.W - self.r_margin - self.get_string_width(s2),
                      self.H - 7, s2)

    def h2(self, s):
        self.set_font("dv", "B", 10.5)
        self.set_text_color(*INK)
        self.cell(0, 6, s, new_x="LMARGIN", new_y="NEXT")
        self.ln(0.5)

    def body(self, s, size=9, lh=4.6):
        self.set_font("dv", "", size)
        self.set_text_color(*INK)
        self.multi_cell(0, lh, s)
        self.ln(1)

    def bullets(self, items, bullet="\u2022", size=9, lh=4.6):
        self.set_font("dv", "", size)
        self.set_text_color(*INK)
        for it in items:
            self.cell(4, lh, bullet)
            x = self.get_x()
            self.multi_cell(self.W - self.l_margin - self.r_margin - x, lh,
                            it, new_x="LMARGIN", new_y="NEXT")
        self.ln(1)

    def tip(self, s, kind="tip"):
        edge = TIP_EDGE if kind == "tip" else RED
        fill = TIP_BG if kind == "tip" else (250, 235, 235)
        self.set_fill_color(*fill)
        self.set_draw_color(*edge)
        self.set_line_width(0.4)
        self.set_font("dv", "", 8.5)
        self.set_text_color(*INK)
        self.multi_cell(0, 4.4, s, border=1, fill=True)
        self.ln(2)

    def _wrap(self, s, w):
        """Greedy word wrap by measured width."""
        self.set_font("dv", "", 8.2)
        lines, cur = [], ""
        for word in s.split():
            t = (cur + " " + word).strip()
            if not cur or self.get_string_width(t) <= w:
                cur = t
            else:
                lines.append(cur)
                cur = word
        if cur:
            lines.append(cur)
        return lines or [""]

    def _table_head(self, header, widths, size):
        self.set_font("dv", "B", size)
        self.set_fill_color(*MINT)
        self.set_text_color(255, 255, 255)
        for h, w in zip(header, widths):
            self.cell(w, 6, h, border=1, fill=True)
        self.ln()

    def table(self, header, rows, widths, size=8.2, lh=4.3):
        self.set_draw_color(*RULE)
        self.set_line_width(0.2)
        self._table_head(header, widths, size)
        self.set_text_color(*INK)
        for row in rows:
            wrapped = [self._wrap(c, w - 3) for c, w in zip(row, widths)]
            rh = max(len(ls) for ls in wrapped) * lh + 1.8
            if self.get_y() + rh > self.H - 16:      # keep rows unsplit
                self.add_page()
                self._table_head(header, widths, size)
                self.set_text_color(*INK)
            x0, y0 = self.l_margin, self.get_y()
            for lines, w in zip(wrapped, widths):
                self.set_font("dv", "", size)
                for i, line in enumerate(lines):
                    self.set_xy(x0 + 1.5, y0 + 0.9 + i * lh)
                    self.cell(w - 3, lh, line)
                self.rect(x0, y0, w, rh)
                x0 += w
            self.set_xy(self.l_margin, y0 + rh)
        self.ln(2)

    # -- pixel art (same glyphs the device prints) ---------------------------

    def pixels(self, cols, x, y, px, r=8, color=None):
        """Draw 5/6-column pixel art (bit 0 = top row). Returns width."""
        color = color or MINT
        self.set_fill_color(*color)
        w = len(cols)
        for c in range(w):
            for rr in range(r):
                if (cols[c] >> rr) & 1:
                    self.rect(x + c * px, y + rr * px, px, px, style="F")
        return w * px

    def wordmark(self, x, y, px, color=None):
        """OPENTAG in the device's own 5x7 font. Returns end x."""
        cx = x
        for ch in BRAND_TEXT:
            if ch == " ":
                cx += px
                continue
            cols = BRAND_FONT.get(ch)
            if cols:
                self.pixels(cols, cx, y, px, r=8, color=color)
            cx += 6 * px
        return cx - px

    def bolt(self, x, y, px, color=None):
        return self.pixels(BOLT, x, y, px, r=8, color=color)

    def halo(self, cx, cy, r_out, lw, color=None):
        color = color or MINT
        self.set_draw_color(*color)
        self.set_line_width(lw)
        self.ellipse(cx - r_out, cy - r_out, 2 * r_out, 2 * r_out)


# ---------------------------------------------------------------------------


def cover(pdf):
    pdf.add_page()
    pdf.set_fill_color(*BG)
    pdf.rect(0, 0, pdf.W, pdf.H, style="F")

    # wordmark + halo, centred
    px = 3.1
    wm_w = len(BRAND_TEXT) * 6 * px - px
    pdf.wordmark((pdf.W - wm_w) / 2, 52, px, color=MINT)
    pdf.halo(pdf.W / 2, 105, 24, 1.2, color=MINT)
    pdf.halo(pdf.W / 2, 105, 18, 0.5, color=LAVENDER)

    pdf.set_font("dv", "", 11)
    pdf.set_text_color(*LAVENDER)
    pdf.cell(0, 8, "NFC tag writer + amiibo emulator", align="C",
             new_x="LMARGIN", new_y="NEXT")
    pdf.ln(4)
    pdf.set_font("dv", "B", 20)
    pdf.set_text_color(*WHITE)
    pdf.cell(0, 12, "User guide", align="C", new_x="LMARGIN", new_y="NEXT")

    # charge bolt badge, bottom
    bpx = 2.2
    bw = len(BOLT) * bpx
    pdf.bolt((pdf.W - bw) / 2, 150, bpx, color=LAVENDER)
    pdf.set_font("dv", "", 8.5)
    pdf.set_text_color(*MUTED)
    pdf.cell(0, 6, "charge on the bolt side", align="C",
             new_x="LMARGIN", new_y="NEXT")

    pdf.set_y(-24)
    pdf.set_font("dv", "", 8)
    pdf.set_text_color(*MUTED)
    pdf.multi_cell(0, 4,
                   "Print it, fold it, keep it in the box. Regenerate with\n"
                   "python3 tools/make_booklet.py", align="C")


def page_meet(pdf):
    pdf.sheet("Meet your OpenTag")
    pdf.body("OpenTag is a pocket handheld for NFC tags. It emulates amiibo "
             "with their own unique IDs, writes NDEF notes onto real tags, "
             "and emulates those too - no phone needed at tap time.")
    pdf.h2("The tour")
    pdf.table(
        ["Where", "What it is"],
        [["Front", "OLED screen, four buttons (UP, DOWN, OK, BACK)"],
         ["Back", "NFC tap window ringed by the halo - tap consoles and "
                  "phones flat against it"],
         ["Left side", "USB-C charge port under the engraved lightning bolt"],
         ["Right side", "Power slide switch (full off for storage)"]],
        [26, 94])
    pdf.tip("The engraved bolt marks the charging side. The charger's own "
            "indicator LEDs glow through the same port slot - so you can "
            "see charging with the case closed.")


def page_power(pdf):
    pdf.sheet("Power & charging")
    pdf.h2("Charging")
    pdf.bullets([
        "Plug any USB-C cable into the side port marked with the "
        "engraved lightning bolt.",
        "The charge LEDs on the charger module peek through the port slot: "
        "glowing = charging, dark = full or unplugged.",
        "Charge fully before first use. A full battery lasts many hours of "
        "emulation; the screen sleeps when the device is off.",
    ])
    pdf.h2("Turning it off")
    pdf.bullets([
        "Soft off: hold OK on the menu. BACK cancels within the countdown.",
        "Wake: hold OK for about a second.",
        "Storage / travel: use the power switch on the right side - soft "
        "off still drains a tiny current, the switch does not.",
    ])
    pdf.tip("Soft off is a convenience, not a battery disconnect. For a "
            "multi-week shelf life, flip the side switch.", kind="warn")


def page_buttons(pdf):
    pdf.sheet("Buttons & the menu")
    pdf.table(
        ["Input", "What it does"],
        [["UP / DOWN", "Move through lists and menu items"],
         ["OK (short)", "Select, start emulation, confirm"],
         ["OK (hold 1 s)", "At the menu: power off. In the library: the "
                           "advanced write-to-tag path"],
         ["BACK", "Cancel, stop emulation, go back one screen"],
         ["Any button", "Stops an active emulation"]],
        [30, 90])
    pdf.h2("The menu")
    pdf.table(
        ["Item", "What it's for"],
        [["Amiibo library", "Browse your amiibo, emulate one (opens first)"],
         ["BLE sync", "Connect the web page or Android app"],
         ["Write NDEF note", "Write your note to a physical NTAG215"],
         ["Emulate NDEF note", "Present the note as a virtual tag"],
         ["Clear NDEF note", "Erase the stored note"]],
        [34, 86])


def page_library(pdf):
    pdf.sheet("Your amiibo library")
    pdf.h2("Loading amiibo")
    pdf.bullets([
        "Open the BLE sync page or the Android app and upload your own "
        "NTAG215 dump files (.bin) or a .zip of them.",
        "Files are matched by name against a bundled 932-game amiibo "
        "database, so the library shows proper game titles and art in the "
        "browser; the device shows the tidied-up name.",
    ])
    pdf.h2("Emulating")
    pdf.bullets([
        "Pick an entry and press OK. The screen shows the tag name and its "
        "slot; the emulator presents the dump with its own unique ID.",
        "Tap the console or phone flat against the halo on the back.",
        "Press any button to stop.",
    ])
    pdf.h2("The V3 badge")
    pdf.body("Entries marked V3 (such as Kirby Air Riders figures) are the "
             "new-generation tags. They emulate perfectly on this device "
             "but cannot be written onto standard blank NTAG215 tags - "
             "there is no matching physical chip. That is a hardware "
             "limitation of the tag format, not a fault in your OpenTag.")


def page_write(pdf):
    pdf.sheet("Advanced: write to a physical tag")
    pdf.body("OpenTag can re-sign a dump for a blank NTAG215's own unique "
             "ID and write it, TagMo-style. This is optional - emulation "
             "needs none of it.")
    pdf.bullets([
        "Once, upload your key files (unfixed-info.bin, locked-secret.bin) "
        "over BLE - they are user-supplied and never bundled.",
        "In the library, hold OK on an entry to arm the write, then present "
        "a blank NTAG215 to the back window.",
        "The device re-signs, writes, and reads everything back to verify.",
    ])
    pdf.tip("Locking a tag makes it read-only forever. Locking always "
            "needs a second, deliberate long-press on the device - it can "
            "never happen remotely or by timeout.", kind="warn")


def page_ndef(pdf):
    pdf.sheet("NDEF notes")
    pdf.body("A note is a short text or https:// link the device keeps in "
             "memory and can stamp onto tags over and over.")
    pdf.table(
        ["Menu item", "What happens"],
        [["Write NDEF note", "Writes the note to a physical NTAG215 "
                             "(read-back verified)"],
         ["Emulate NDEF note", "Presents the note as a virtual, read-only "
                               "tag - phones read it like any sticker"],
         ["Clear NDEF note", "Erases the stored note"]],
        [36, 84])
    pdf.body("Set or change the note from the BLE sync page before "
             "writing. The note survives firmware updates.")


def page_sync(pdf):
    pdf.sheet("Sync & updates")
    pdf.h2("Web sync")
    pdf.body("Open the sync page in a Chromium browser, pick your OpenTag "
             "from the Bluetooth prompt, and you can upload amiibo dumps, "
             "manage the note, and browse the on-device library with "
             "artwork - remotely emulate or delete entries.")
    pdf.h2("Android app")
    pdf.body("The companion app does the same over BLE: pick .bin dumps or "
             "a .zip of them, upload, browse, and trigger emulation.")
    pdf.h2("Firmware updates")
    pdf.body("Plug the device into a computer over USB and run "
             "python tools/update_opentag.py - it downloads the latest "
             "release and flashes only the app partition. Your amiibo "
             "library, keys, settings and note survive every update.")


def page_help(pdf):
    pdf.sheet("Troubleshooting")
    pdf.table(
        ["Symptom", "Try this"],
        [["Console reads nothing",
          "Tap flat against the halo on the back; hold still a moment; "
          "check the emulation screen is showing"],
         ["'Killer fw lacks V3' on screen",
          "V3 emulation needs a current PN532Killer firmware - update the "
          "Killer board"],
         ["'V3: emulation only'",
          "By design: V3 amiibo cannot be written to physical tags"],
         ["'PN532Killer absent'",
          "Open the case and check the UART wiring (TX/RX swap is the "
          "usual cause)"],
         ["BLE can't find the device",
          "Exit any active emulation first; keep the phone within a few "
          "metres; toggle BLE off/on"],
         ["Won't wake",
          "Hold OK a full second; if truly dead, charge it, then check the "
          "side power switch"]],
        [36, 84])
    pdf.tip("Charging but the LEDs look dark? Look at the port slot from "
            "an angle in dim light - the LEDs sit just inside, next to "
            "the connector.")


def page_care(pdf):
    pdf.sheet("Care & safety")
    pdf.bullets([
        "The battery is a lithium-polymer cell: don't charge it unattended, "
        "don't puncture or squeeze the case, keep it off hot surfaces.",
        "If the case swells or the device gets unusually hot, stop using "
        "it and disconnect the battery (switch off, open the back).",
        "Wipe the case with a dry cloth; the tap window works best clean.",
        "Store with the side power switch off for long periods, battery "
        "about half full.",
    ])
    pdf.ln(2)
    pdf.h2("What's inside")
    pdf.body("An ESP32-S3 module, a PN532Killer NFC frontend, a 1.3-inch "
             "OLED, and a LiPo with a USB-C charge circuit - all on one "
             "open-source carrier board in a 3D-printed shell. Full "
             "schematics, CAD and firmware live in the project repository.")
    pdf.ln(1)
    pdf.set_font("dv", "", 8)
    pdf.set_text_color(150, 150, 155)
    pdf.multi_cell(0, 4,
                   "OpenTag is an open-source project. No Nintendo "
                   "proprietary data is included: signing keys are "
                   "user-supplied at runtime, and the amiibo database "
                   "contains only public metadata matched by file name.")


def main():
    pdf = Book()
    cover(pdf)
    page_meet(pdf)
    page_power(pdf)
    page_buttons(pdf)
    page_library(pdf)
    page_write(pdf)
    page_ndef(pdf)
    page_sync(pdf)
    page_help(pdf)
    page_care(pdf)
    OUT.parent.mkdir(exist_ok=True)
    pdf.output(OUT)
    print(f"wrote {OUT} ({OUT.stat().st_size // 1024} KiB, "
          f"{pdf.page_no()} pages)")


if __name__ == "__main__":
    sys.exit(main())
