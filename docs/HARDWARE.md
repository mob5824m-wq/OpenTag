# OpenTag device hardware

A pocket handheld that writes and emulates NFC tags, built around three
modules on one carrier PCB (no custom RF or power electronics):

- **ESP32-S3-DevKitC-1** — MCU + BLE + USB (flashing via its own USB port)
- **PN532Killer** — NFC frontend: reader/writer *and* a real NTAG215 emulator
  (7-byte UID, 8 slots) connected over UART
- **SSD1351 1.3" OLED** on SPI
- **LiPo power**: TP4056 charger + MT3608 boost to 5 V into the DevKit's 5V pin

Fabrication assets:

- `../hardware/pcb/opentag-carrier.kicad_pcb` — 90 × 60 mm 2-layer carrier
  (regenerate/verify with `python3 hardware/pcb/generate_carrier_pcb.py`)
- `WIRING.md` — assembly, power chain, signal map, bring-up
- `../hardware/cad/opentag_shell.cad` — parametric 2-part handheld shell
  (print-ready STLs in `hardware/cad/stl/`; regenerate with OpenSCAD or
  `python3 tools/cad_to_stl.py`)
- `BOM.md` — parts list and budget

## Why the PN532Killer (and not the PN532)

Amiibo (and most signature-bound NTAG215 applications) bind their data to the
tag's **7-byte UID**. A classic PN532 in target mode can only present a 4-byte
UID hard-wired to start with `0x08`, and cannot emit Type-2 ACK/NAK frames —
it physically cannot emulate these tags. The PN532Killer (ST25R3916-class,
open firmware) presents the dump's own 7-byte UID and serves the full NTAG215
command set from an on-board emulator slot, so the ESP32 only has to push a
540-byte page image over UART and switch the board into emulator mode.

## Interfaces

| Function | Pins (DevKitC header pin / GPIO) |
|---|---|
| OLED SPI: SCK, MOSI | J1-18 / GPIO12, J1-17 / GPIO11 |
| OLED CS, DC, RST | J1-16 / GPIO10, J1-15 / GPIO9, J1-12 / GPIO8 |
| OLED MISO | unused (write-only bus) — GPIO13 is repurposed as the BACK button |
| NFC UART (Serial2, 115200 8N1) | TX J1-10 / GPIO17 → Killer RX; RX J1-11 / GPIO18 ← Killer TX |
| Buttons (active-low) | UP J3-6 / GPIO42, DOWN J3-5 / GPIO2, OK J3-4 / GPIO1, BACK J1-19 / GPIO13 |
| Power in | 5 V via J1-21 (after Schottky from the boost), 3V3 out at J1-1/2 for OLED + Killer |
| Charge sense (optional) | TP4056 /CHRG (active low) via 22 kΩ → J3-18 / GPIO21; lights the on-screen charge bolt |
| Battery sense (optional) | 100k/100k divider: B+ ─ 100k ─┬─ 100k ─ GND, midpoint → J1-4 / GPIO4; header % gauge + low-battery auto-off |
| Piezo (optional) | passive buzzer between J1-5 / GPIO5 and GND; chirps on backup / verified write / pairing / low battery |

Avoid on the S3: GPIO 0/3/45/46 (strapping), 19/20 (USB D−/D+), 26–32 (flash),
35–37 (octal PSRAM), 43/44 (UART0). The pin choices above dodge all of them.

BACK sits on GPIO13 (the SPI MISO pin): the OLED bus is write-only, so the
firmware calls `SPI.begin()` without MISO and uses the pin as a button input
instead. BACK is the universal "get me out of here": it cancels the tag-scan
and write screens, stops emulation, skips the lock confirmation, and exits
the amiibo library back to the menu.

## NFC notes

- The Killer mounts antenna-side-down in the shell's back bay under a **fully
  open** tap window (no plastic at all over the antenna — best read range),
  with a shallow outside cradle that centres a controller or phone on it.
  Keep the battery and boost module on the opposite side of the bay to
  reduce metal near the antenna.
- Emulation presents the dump's own UID; the NDEF-note personality uses a
  random persisted 7-byte UID (`04:…` NXP-style prefix).
- No cryptographic keys are handled by this project: reading or writing
  amiibo PWD/PACK is not implemented; dumps are user-supplied backups of tags
  the user owns.

## Power budget (approximate)

| Rail | Consumers | Typical / peak |
|---|---|---|
| 3V3 | OLED (~30 mA), Killer (~50–100 mA active field), DevKit internals | ~150 / 300 mA |
| 5V (from boost) | DevKitC LDO input incl. ESP32-S3 BLE peaks | ~200 / 450 mA |
| Battery (3.7 V) | via MT3608 (≈85% eff.) | ~250 / 550 mA |

An 805040 (~800 mAh) cell gives roughly 2–3 h of active emulation and days of
standby-ish menu use. The TP4056 charges at up to 1 A over USB-C, and an
optional Qi receiver coil (⌀ ≤ 40 mm, ferrite-backed) under the battery adds
pad charging through the same charger — the engraved ring on the back marks
the spot; a bolt icon shows on screen while charging (22 kΩ sense wire on
GPIO21, optional).

## Bring-up order

1. Bare board: continuity on GND/+5V/+3V3 rails, no shorts.
2. Power chain staged per `WIRING.md` (set the boost to 5.05 V first).
3. DevKit alone + firmware; OLED should show the menu.
4. Plug the Killer in via the Dupont cable; menu no longer shows
   "PN532Killer absent".
5. BLE sync from `device/web-sync/`, write a note to a real NTAG215, then try
   the amiibo library with a .zip of your own dumps.
