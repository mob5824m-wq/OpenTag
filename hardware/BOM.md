# OpenTag parts list (BOM)

## Core electronics

| # | Part | Qty | Notes |
|---|------|----|-------|
| 1 | ESP32-S3-DevKitC-1 (N8R2 or N16R8, 16 MB flash recommended) | 1 | The 16 MB layout assumed by `partitions_16mb.csv`; an 8 MB board works after switching to a smaller partition table |
| 2 | PN532Killer (NFC-funs, ST25R3916) | 1 | Replaces a PN532 entirely: presents a real 7-byte UID, 8 emulator slots; get the UART-firmware version |
| 3 | SSD1351 1.3" 128×128 OLED module, SPI, 7-pin | 1 | 3.3 V, GH1313-style header |
| 4 | LiPo cell 805040 (3.7 V, ~800 mAh, with protection + JST-PH lead) | 1 | Any 3.7 V cell that fits the shell bay; adjust `bat_*` in the `.cad` |
| 5 | TP4056 USB-C charging module **with protection** (DW01A + 8205A) | 1 | Must be the 6-pad version with B+/B−/OUT+/OUT− |
| 6 | MT3608 boost module (adjustable) | 1 | Set to 5.0–5.1 V before installation |
| 7 | 1N5819 Schottky diode (DO-41) | 1 | Series element on the boost output |
| 8 | SPDT mini toggle switch, PCB mount, ≥1 A (MTS-102 style, 3 pins @ 2.54 mm) | 1 | Master battery cut. The firmware's "hold OK" soft-off is deep sleep, not a disconnect — the DevKitC's regulator and the boost still draw a few mA, so flip this switch for storage |

## PCB & wiring

| # | Part | Qty | Notes |
|---|------|----|-------|
| 9 | OpenTag carrier PCB | 1 | `hardware/pcb/opentag-carrier.kicad_pcb`, 90 × 60 mm, 2 layers, 1.6 mm, HASL/ENIG |
| 10 | Female header 1×22, 2.54 mm | 2 | DevKit socket |
| 11 | Female header 1×7, 2.54 mm | 1 | OLED socket |
| 12 | Male header 1×4, 2.54 mm | 3 | Killer, TP4056, MT3608 |
| 13 | Tactile switch 6×6 mm, through-hole, ~8.5 mm seated height | 4 | Standard 6.5 × 4.5 mm pin pattern; UP / DOWN / OK / BACK |
| 14 | JST-PH-2 through-hole connector | 1 | Battery input |
| 15 | M3 screws + standoffs/nuts | 4 + 4 + 4 | PCB mount (6 mm) and shell closure (12 mm countersunk) |
| 16 | Dupont jumper wires, F–F, ~10 cm | 4 | ESP32 ⇄ PN532Killer (3V3, GND, TX, RX) |
| 17 | Hook-up wire 22–24 AWG | ~8 | TP4056/MT3608 to their headers |
| 18 | Double-sided foam tape / zip ties | — | Securing modules in the shell tray |

## Printed parts (`hardware/cad/opentag_shell.cad`)

Print-ready STLs are in `hardware/cad/stl/` (front shell, back plate, button
cap — print 4 caps). Regenerate them with OpenSCAD
(`openscad -o front.stl -D 'part="front"' opentag_shell.cad`, etc.) or with
`python3 tools/cad_to_stl.py`, which also self-checks the mesh.

The shell is a slim two-part clamshell (97.6 × 135.6 × 29.3 mm outside,
1.8 mm walls): rectangular OLED window with a chamfered bezel, four button
holes with captive caps, a **fully open** NFC tap window in the back plate
(no plastic over the Killer's antenna) ringed by an outside cradle that
centres a controller or phone, a service window over the DevKitC's BOOT/RST
pads, and a vented power-module tray. Branding is printed in, not added
later: an engraved `OPENTAG` wordmark on the front chin (below the buttons,
using the same pixel font as the OLED and the PCB silkscreen) and an
engraved halo ring around the NFC cradle marking the tap zone — both
0.4–0.6 mm deep, so they survive handling and need no supports.
The west wall carries the **charging port**: a slot over the tray's west
end (the tray wall is notched behind it) so the TP4056's USB-C jack and
charge LEDs are reachable with the case closed, marked by an engraved
lightning bolt above the slot — charge the device on that side. The
south-west corner carries a **lanyard / keyring loop**: a tab shared by both
halves with a continuous 4.2 mm bore through the seam.
Cutouts are placed so nothing needs
supports: both halves print as drawn (front face-down, plate flat-side-down).

| # | Part | Qty | Material |
|---|------|----|----------|
| 19 | Front shell | 1 | PLA/PETG — **not** carbon- or metal-filled near the tap zone |
| 20 | Back plate | 1 | PLA/PETG |
| 21 | Button cap | 4 | PLA/PETG or TPU |

## Where to buy (no affiliation)

- PN532Killer: pn532killer.com or the NFC-funs store on AliExpress/Taobao
- Everything else: any electronics distributor (Adafruit/SparkFun stock the
  DevKitC-1 and OLED; modules are ubiquitous on Amazon/AliExpress)

## Budget estimate

| Block | Cost (USD, approx.) |
|---|---|
| DevKitC-1 + OLED | $18–25 |
| PN532Killer | $30–40 |
| Battery + TP4056 + MT3608 + switch + diode | $10–14 |
| PCB (5 pcs from JLCPCB/PCBWay) | $5–10 |
| Headers, connectors, hardware | $6–8 |
| Filament | $3–5 |
| **Total** | **≈ $72–102** |
