# OpenTag

OpenTag is an independent, open-source NFC handheld. `device/` contains the
ESP32-S3 firmware; `hardware/` contains the carrier PCB, enclosure CAD, wiring
guide, and parts list.

> OpenTag is not affiliated with or endorsed by Nintendo or any tag/device
> manufacturer. Amiibo is a trademark of Nintendo. **All amiibo tag data is
> user-supplied** — the project bundles, generates, and distributes none of
> it. Upload backups of tags you own, and only write to tags you are
> authorized to program.

## What it does

- **Write NDEF notes** — sync a text or HTTP(S) URL note over BLE and write it
  to a real NTAG215 (verified by read-back), from the device menu.
- **Emulate the NDEF note** — present the note as a virtual, read-only
  NTAG215; phones read it like any NDEF tag.
- **Amiibo library** — upload a `.zip` of your own NTAG215 `.bin` dumps
  through the web page (unpacked and validated in the browser), browse them
  on the OLED, and tap a console with the dump's own 7-byte UID presented by
  the NFC emulator.
- **BLE sync** — Web Bluetooth page with note management, .zip upload, device
  library listing, remote emulate/delete.

## Hardware architecture

ESP32-S3-DevKitC-1 + **PN532Killer** (ST25R3916-class NFC emulator; the
classic PN532 cannot present the 7-byte, signature-bound UIDs amiibo relies
on) + SSD1351 OLED + three buttons + LiPo power (TP4056 + MT3608). The NFC
board talks HSU UART to the ESP32; a 540-byte page image per tag is all the
firmware pushes. See [`device/HARDWARE.md`](device/HARDWARE.md) for the full
interface map and [`hardware/`](hardware/) for:

| File | Contents |
|---|---|
| `hardware/pcb/opentag-carrier.kicad_pcb` | 90 × 60 mm 2-layer carrier, all through-hole |
| `hardware/pcb/generate_carrier_pcb.py` | Regenerates the PCB and self-checks connectivity + clearance |
| `hardware/pcb/WIRING.md` | Assembly, power chain, signal map, bring-up |
| `hardware/cad/opentag_shell.scad` | Parametric 2-part handheld shell + button caps |
| `hardware/BOM.md` | Parts list and budget (≈ $72–102) |

## Build and flash

Requirements: PlatformIO Core + an ESP32-S3-DevKitC-1 with 16 MB flash.

```sh
cd device
pio run -e opentag_s3            # firmware
pio run -e opentag_s3 -t upload
pio run -e opentag_s3 -t uploadfs # LittleFS image (amiibo library storage)
```

Unit tests (host-native, no hardware needed):

```sh
cd device/test
g++ -std=c++11 -Wall -Wextra -pedantic -I ../src test_ntag215_emu.cpp -o t1 && ./t1
g++ -std=c++11 -Wall -Wextra -pedantic -I host_stubs -I ../src test_pn532_uart.cpp -o t2 && ./t2
```

## Web sync

Open `device/web-sync/index.html` from a secure origin (HTTPS or localhost)
in Chrome/Edge, connect over BLE, then:

- queue a text/URL note and send it;
- choose a `.zip` (or loose `.bin` files) of your NTAG215 dumps — the page
  unzips locally with the browser's native `DecompressionStream`, validates
  each dump (size 532–572 B, UID BCC, NTAG215 capability container), and
  copies them to the device in 240-byte BLE chunks;
- list, emulate, or delete library entries on the device.

Nothing is uploaded to any server; all processing happens in your browser.

## Device menu

`Write NDEF note` → `Emulate NDEF note` → `Amiibo library` → `BLE sync` →
`Clear note`, driven by UP/DOWN/OK. Emulation runs on the PN532Killer's slot
0 and stops on any button press or a BLE `STOP`.

## Scope notes

- No cryptographic key handling anywhere in the project (not needed for
  emulation of user-owned dumps).
- The NDEF-note emulation personality uses a random, persisted 7-byte UID.
- `device/HARDWARE.md` documents the interface baseline; the PCB is
  self-checked by its generator but has not been manufactured in this repo —
  review it in KiCad before ordering.

## License

MIT License; see [`LICENSE`](LICENSE).
