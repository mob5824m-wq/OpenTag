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
  through the web page or the **Android app**, browse them on the OLED, and
  tap a console with the dump's own 7-byte UID presented by the NFC emulator.
  **OpenTag emulates amiibo — that's the product**; bins show metadata and
  box art in the web page and app, matched by file name against a bundled
  932-entry amiibo database.
- **Android app** ([`android-app/`](android-app/)) — native Kotlin app
  (no dependencies) for BLE file dumping: pick `.bin` dumps or a `.zip` of
  them, upload to the device, browse the on-device library, and trigger
  emulation. Every merge to `main` auto-builds a fresh signed `.apk`
  (`app-v*` releases on the Releases page) — download and sideload it, no
  Android Studio needed.
- **Wireless charging (optional)** — a Qi receiver coil inside the back
  plate charges the OpenTag from any Qi pad through the existing TP4056
  (an engraved ring on the back marks the spot); with the optional charge
  -sense wire, a bolt icon on the OLED confirms the pad is coupled.
- **Tag backup (reader mode)** — from the device menu, tap any amiibo
  against the back window and the firmware reads, validates, and stores the
  dump in the library (named by the tag's UID; full 1024-byte images for
  V3/NTAG I²C tags). No phone needed.
- **Tag inspector** — the third menu item turns the OpenTag into a tag
  identifier: tap any tag on the back window and the OLED shows its UID,
  chip type (NTAG213/215/216, MIFARE UL…) and any NDEF text/URL payload.
  Nothing is written; it's the safe "what is this tag?" answer.
- **Battery gauge + low-battery protection (optional)** — a 100k/100k
  divider on B+ feeds GPIO4; the header shows a charge percentage, turns
  red at/below 15 %, warns once at 3.35 V and (with the divider fitted)
  powers the device off cleanly if the cell sags below 3.25 V for 30 s.
- **BLE pairing with a PIN** — the first phone to connect runs the standard
  numeric-comparison pairing flow: the OLED shows a random 6-digit code,
  the phone asks to confirm it, and the bond is stored (a failed exchange
  is disconnected immediately). Re-pair by deleting the bond on the phone.
- **USB drive mode (read-only)** — docked in the menu, the DevKit's USB-C
  port enumerates as a USB mass-storage drive exposing a read-only snapshot
  of the amiibo library (plus a README) — drag bins off the device with no
  app at all. Loading new dumps still goes over BLE (the drive is a
  snapshot; OK/BACK ejects it).
- **Wi-Fi OTA updates (optional)** — provision Wi-Fi credentials over BLE
  (`WIFI:<ssid>|<pass>`), then trigger an update from the web page's
  Settings (or `OTA:START`): the device fetches the latest GitHub release,
  flashes only the app partition and reboots — library and settings survive,
  exactly like the wired one-command updater.
- **Piezo feedback (optional)** — a passive buzzer on GPIO5 chirps on tag
  backup, verified writes, pairing and the low-battery warning. Unwired,
  the device stays silent, as before.
- **One-command firmware updates** ([`tools/update_opentag.py`](tools/update_opentag.py))
  — plug the OpenTag into a computer and run the script: it downloads the
  latest firmware release from GitHub (CI builds and attaches it on every
  `v*` tag) and flashes **only the app partition** — your amiibo library,
  settings, and note survive every update.
- **Advanced: write amiibo to physical NTAG215 tags (TagMo-style)** — with
  your own `unfixed-info.bin` / `locked-secret.bin` key files uploaded over
  BLE, the device can re-sign a dump for a blank tag's own UID, write and
  verify it, and optionally lock it (irreversible, long-press confirm on the
  device). Emulation needs none of this; the flow remains available behind a
  long-press in the device library.
- **BLE sync** — Web Bluetooth page with note management, .zip upload, device
  library listing with metadata + box art, remote emulate/delete.
- **Web preview of the device UI** ([`device/web-preview/`](device/web-preview/))
  — a browser simulation of the OLED interface (every screen, color, and
  pixel position ported from `drawScreen()`) with working buttons and
  long-press, for UI testing with no hardware attached.

## Set up your OpenTag

Three ways in, depending on what's on your desk. **A** needs nothing, **B**
is a working device on a bench in an afternoon, **C** is the finished
handheld.

### A · No hardware — try the interface (1 minute)

Open [`device/web-preview/index.html`](device/web-preview/index.html) in any
browser: a pixel-faithful simulation of the OLED menu with working buttons
and long-press — every screen the real device shows, nothing to install.

### B · Bench build — a working device today

Parts: **ESP32-S3-DevKitC-1** (16 MB flash), **PN532Killer**, **1.3"
SSD1351 OLED** (7-pin SPI), 4 × tactile buttons, Dupont jumpers, USB-C
cable. No PCB, no soldering — USB powers the whole bench setup.

1. Install [PlatformIO](https://platformio.org/) (VS Code extension or
   `pip install platformio`).
2. Flash the firmware and the library filesystem:
   ```sh
   cd device
   pio run -e opentag_s3 -t upload      # firmware (app partition only)
   pio run -e opentag_s3 -t uploadfs    # LittleFS: on-device amiibo storage
   ```
3. Wire the modules (full map and rationale in
   [`docs/HARDWARE.md`](docs/HARDWARE.md)):

   | Connection | DevKit pin (GPIO) | Module side |
   |---|---|---|
   | OLED VCC / GND | J1-1 (3V3) / GND | VCC / GND |
   | OLED SCK / MOSI | J1-18 (12) / J1-17 (11) | SCL / SDA |
   | OLED CS / DC / RST | J1-16 (10) / J1-15 (9) / J1-12 (8) | CS / DC / RES |
   | Killer 3V3 / GND (J4) | J1-2 (3V3) / GND | VCC / GND |
   | Killer RX / TX (J4) | J1-10 TX (17) / J1-11 RX (18) | crossed: TX → RX |
   | Buttons UP / DOWN / OK / BACK | J3-6 (42) / J3-5 (2) / J3-4 (1) / J1-19 (13) | one leg each; other legs to GND |

4. Plug in USB — the OLED shows the menu within a couple of seconds. Only
   the NFC actions need the Killer attached; trying one without it shows
   **PN532Killer absent** on screen. Wire it in and the whole menu is live.
5. Serve the sync page. Web Bluetooth only runs on a secure origin
   (localhost or HTTPS) — **double-clicking the file is not enough**:
   ```sh
   cd device/web-sync
   python3 -m http.server 8000        # then open http://localhost:8000
   ```
6. In Chrome or Edge: **Connect over BLE** (the device shows up as
   *OpenTag*) → pick a `.zip` of your own NTAG215 `.bin` dumps → they land
   in the on-device library with names and box art. Browse on the OLED,
   press OK on one, and tap your console.

Optional extras on the same hardware: send a text/URL **NDEF note** from the
sync page (the device menu can write it to a blank NTAG215 or emulate it —
a good first test of the tag pipeline), and — only if you write amiibo to
physical tags — upload your `unfixed-info.bin` / `locked-secret.bin` key
files. Emulation needs neither.

### C · Full handheld

1. **Order the PCB** — upload
   [`hardware/pcb/gerbers/opentag-gerbers.zip`](hardware/pcb/gerbers/) to any
   fab (JLCPCB, PCBWay, …). The zip is complete and machine-verified by its
   generator.
2. **Print the shell** — [`hardware/cad/stl/`](hardware/cad/stl/): front
   shell, back plate, 4 button caps (PLA/PETG, no supports needed).
3. **Assemble** per [`docs/WIRING.md`](docs/WIRING.md);
   parts and budget in [`docs/BOM.md`](docs/BOM.md) (≈ $72–102
   complete). Print [`docs/opentag-user-guide.pdf`](docs/opentag-user-guide.pdf)
   as the user manual.
4. The device is identical to the bench build — continue from step 5 in
   path **B**.

Updating an assembled device later never touches your library:
`python tools/update_opentag.py` flashes only the app partition
([details](#updating-the-firmware-keeps-your-amiibos)).


ESP32-S3-DevKitC-1 + **PN532Killer** (ST25R3916-class NFC emulator; the
classic PN532 cannot present the 7-byte, signature-bound UIDs amiibo relies
on) + SSD1351 OLED + four buttons (UP / DOWN / OK / BACK) + LiPo power
(TP4056 + MT3608). The NFC
board talks HSU UART to the ESP32; a 540-byte page image per tag is all the
firmware pushes. The SW-corner tab is a lanyard / keyring loop (4.2 mm bore,
continuous through the front/back seam). See [`docs/HARDWARE.md`](docs/HARDWARE.md) for the full
interface map and [`hardware/`](hardware/) for:

| File | Contents |
|---|---|
| `hardware/pcb/opentag-carrier.kicad_pcb` | 90 × 60 mm 2-layer carrier, all through-hole |
| `hardware/pcb/generate_carrier_pcb.py` | Regenerates the PCB and self-checks connectivity + clearance |
| `hardware/pcb/gerbers/` | Fab-ready Gerber set + Excellon drill + `opentag-gerbers.zip` — the generator writes them and re-parses them to verify against the model |
| `docs/WIRING.md` | Assembly, power chain, signal map, bring-up |
| `docs/HARDWARE.md` | Device interface baseline — the contract the firmware implements (buttons, display, NFC, power, USB) |
| `hardware/cad/opentag_shell.cad` | Parametric 2-part handheld shell + button caps, with the ⚡-marked USB-C charge port on the west wall and a lanyard loop tab on the SW corner (OpenSCAD source; `.cad` is the project's extension) |
| `hardware/cad/stl/*.stl` | Print-ready binary STL: front shell, back plate, button cap (print 4 caps) |
| `tools/cad_to_stl.py` | Regenerates the STLs from the `.cad` without OpenSCAD (`pip install trimesh manifold3d scipy rtree`) and self-checks watertightness, features and front/back fit |
| `docs/BOM.md` | Parts list and budget (≈ $72–102) |
| `docs/opentag-user-guide.pdf` | The printed instruction booklet — 15 A5 pages covering charging, the desk stand, buttons, the library, V3 amiibo, BLE sync, writes, USB drive and troubleshooting |
| `docs/ANDROID-APP.md` | Android companion app — features, BLE protocol, releases |
| `docs/SIGNING-KEY.md` | The committed APK signing key: why it's public, and how to swap in a private one |
| `tools/make_booklet.py` | Regenerates the booklet (`pip install fpdf2`); its cover art is parsed straight from the shell's own wordmark font and bolt icon |

## Updating the firmware (keeps your amiibos)

Push a `v*` tag and CI (`.github/workflows/firmware.yml`) builds the firmware
and attaches it to a GitHub release. Then, on any computer with Python:

```sh
pip install esptool
python tools/update_opentag.py            # auto port, latest release
python tools/update_opentag.py --tag v1.2 # a specific release
python tools/update_opentag.py --bin firmware.bin --port COM5
```

The script reads the app partition offset from `device/partitions_16mb.csv`
and flashes **only that partition** — the LittleFS amiibo library, NVS
settings, and pending note are never touched. If no release asset exists it
falls back to building locally with `pio run`.

## Android app

Open [`android-app/`](android-app/) in Android Studio and run — see
[`docs/ANDROID-APP.md`](docs/ANDROID-APP.md). Platform
APIs only, no dependencies; minSdk 23.

## Tests and gates (host-native, no hardware)

The firmware build commands live in [Setup B](#b--bench-build--a-working-device-today).
Everything below runs on any machine with `g++` — no toolchain download, no
device attached.

Unit tests:

```sh
cd device/test
g++ -std=c++11 -Wall -Wextra -pedantic -I ../src test_ntag215_emu.cpp -o t1 && ./t1
g++ -std=c++11 -Wall -Wextra -pedantic -I host_stubs -I ../src test_pn532_uart.cpp -o t2 && ./t2
g++ -std=c++11 -Wall -Wextra -pedantic -I ../src test_amiibo_crypto.cpp -o t3 && ./t3
```

`test_amiibo_crypto` checks the signing pipeline byte-for-byte against
fixtures generated by the real
[amiitool](https://github.com/socram8888/amiitool) (MIT): its AES-128-CTR /
HMAC-SHA256 reference backends are themselves validated against FIPS-197,
FIPS-180-4 and RFC 4231 vectors first.

A firmware compile gate (syntax/type check of `src/main.cpp` against stub
Arduino/NimBLE/mbedtls headers — no toolchain download needed):

```sh
cd device/test
g++ -std=c++11 -Wall -Wextra -fsyntax-only -I host_stubs_main -I ../src ../src/main.cpp
```

## Web sync

Open `device/web-sync/index.html` from a secure origin (HTTPS or localhost)
in Chrome/Edge, connect over BLE, then:

- queue a text/URL note and send it;
- choose a `.zip`, loose `.bin` dumps, or Flipper Zero `.nfc` files of your
  NTAG215 backups — the page unzips locally with the browser's native
  `DecompressionStream`, accepts any container layout (532/540/572 B,
  zero/newline-padded, N2 Elite 32-byte header, Flipper page lists), validates
  each dump (UID BCC, NTAG215 capability container), and copies the normalized
  540-byte image to the device in BLE chunks;
- every bin — local and on-device — shows **amiibo metadata and box art**,
  matched by file name (16-hex amiibo ID first, then a fuzzy name match)
  against `device/web-sync/amiibo-db.js`, a 932-entry database generated from
  the community AmiiboAPI project. The amiibo ID inside a retail dump is
  AES-encrypted, so file names are the reliable key. Images load from
  AmiiboAPI's CDN with a colored-initial fallback offline;
- list, emulate, or delete library entries on the device — emulation is the
  point; there is no per-amiibo write button;
- (advanced) upload your **amiibo signing keys** (`unfixed-info.bin` +
  `locked-secret.bin`, or one combined 160-byte retail file) if you also use
  the device's long-press physical-tag write flow.

Nothing is uploaded to any server; all processing happens in your browser.

### Amiibo metadata database

[`amiibo.json`](amiibo.json) (repository root) is the canonical,
human-editable database — 932 entries mapping each 16-hex amiibo ID to
`[name, amiiboSeries, type, gameSeries, releaseYear]`, generated from the
community [AmiiboAPI] database. Edit it, then run:

```sh
python tools/gen_amiibo_db.py   # -> web-sync/amiibo-db.js + android assets
```

[AmiiboAPI]: https://github.com/N3evin/AmiiboAPI

## Writing an amiibo to a physical tag

The write flow is the TagMo "WRITE TAG (AUTO)" sequence, re-implemented in
portable C++ from the MIT-licensed amiitool reference:

1. Read the blank tag's pages 0–3 (UID) and check `GET_VERSION` says NTAG215
   and the static lock bytes are still clear.
2. Decrypt the stored retail dump with keys derived from its own UID, patch
   the new tag's UID + BCC bytes in, re-encrypt and re-sign both HMACs.
3. Program pages 3–129, then the PWD (133) and PACK (134) pages derived from
   the new UID.
4. Read everything back and compare; finally `PWD_AUTH` must answer with the
   written PACK.
5. Optionally lock the tag (page 2 `xx xx 0F E0`, page 130 `01 00 0F 00`,
   page 131 `00 00 00 04` = AUTH0 4, page 132 `5F 00 00 00`) — **this is
   irreversible** and always requires the physical long-press confirmation on
   the device; it never happens remotely or by timeout.

Trigger it with `AMI:WRITE:<name>` over BLE or by long-pressing OK on a
library entry.

## Charging

Charge through the **USB-C port on the left side**, marked by the engraved
lightning bolt — the TP4056's charge LEDs peek through the same slot, and a
**light pipe** (a ~5 mm stub of 2.5 mm acrylic rod seated in the tray wall
just below the port) carries the charge LED's glow out through the west
face, so charging is visible with the case closed even in bright rooms. The
optional printed **desk stand**
([`hardware/cad/stl/opentag_desk_stand.stl`](hardware/cad/stl/opentag_desk_stand.stl))
holds the OpenTag leaning back at ~10° with the screen up and readable —
OLED end down at the front lip, the back resting on the stand's tall rear
panel. The chin's charge port ends up just behind the panel's top edge, so
the device still charges while docked with the cable dropped behind. The
stand's front brace carries an engraved **OPENTAG** wordmark, and the back
of the rear panel carries an engraved ring + bolt: stick a 5 V Qi
transmitter coil (≤ 40 mm, ferrite-backed) on that mark and the docked
device charges wirelessly — the mark is centred under the device's
receiver coil as far up the panel as it fits, but the ~10° lean still
offsets the coils, so expect a gentle charge rate rather than full power. **Wireless:** with
the optional Qi receiver coil fitted (see `docs/WIRING.md` — it sits
under the battery on the engraved ring, east of the tap halo), any Qi pad
charges the device through the same TP4056, and the bolt icon at the
top-right of the screen confirms the pad is coupled. Soft-off still draws a few mA
(the DevKitC regulator and boost stay alive), so use the side switch when
storing the device.

## Device menu

`Amiibo library` (first — emulation is the main use, so the menu opens on
it) → `Backup tag` → `Inspect tag` → `BLE sync` → `Write NDEF note` →
`Emulate NDEF note` → `Clear NDEF note` → `USB drive` → `Factory reset`,
driven by UP/DOWN/OK with BACK to
cancel or exit. Holding UP or DOWN auto-repeats (first extra step after
450 ms, then accelerating), so long libraries scroll quickly. Emulation
starts on the PN532Killer's slot
0. **While an amiibo is emulating, UP/DOWN hot-swaps to the neighbouring
library entry without stopping** — each swap loads onto the next of the
Killer's 8 emulator slots (never rewriting the one being presented), so you
can tap console after console with different figures. OK/BACK stops the
emulation (any button stops NDEF-note emulation), as does a BLE `STOP`. In the library, short OK
emulates; long OK (1 s) is the advanced path that arms a write to a physical
tag. Long-pressing OK **at the menu** is soft power: a "Powering off" screen
appears (BACK cancels within ~0.7 s), then the OLED parks and the ESP32-S3
drops into deep sleep; holding OK for a full second wakes it back to the
menu. That's a convenience off, not a battery disconnect — the DevKitC's
regulator and the boost still draw a few mA asleep, so use the slide switch
for storage. **Auto-sleep:** after 3 minutes with no button, BLE or tag
activity the device powers off through the same flow (BACK still cancels
inside the window); mid-transfer BLE traffic resets the timer.

**Backup tag (reader mode):** the second menu item turns the OpenTag into a
reader — tap any amiibo (or other NTAG215) flat against the back window and
the firmware reads it, validates it (UID/BCC + capability container), and
saves a normalized 540-byte dump to the library under the tag's 7-byte UID
hex name (V3/NTAG I²C Plus tags save their full 1024-byte image instead).
An identical re-read says `Already saved` and just jumps to the entry; a
tag rewritten with different data but the same UID saves as `-2`, `-3`, …
BACK or OK leaves the reader screen.

**Inspect tag:** the third menu item identifies whatever you tap on the back
window — UID (hex), chip type from the capability container (NTAG213/215/216
or MIFARE UL, with the tag's byte capacity) and the first bytes of any NDEF
text/URL record, or `No NDEF data`. Tap again to re-scan another tag; it
never writes. Use it to sort blank tags from written ones, or to read what
an unknown sticker says before committing anything to it.

**USB drive (read-only snapshot):** menu item 8. The DevKit's own USB-C port
enumerates as a tiny USB mass-storage drive holding a *read-only snapshot*
of the amiibo library plus a README — copy bins off the device from any OS
with no app. It shares one USB port with the serial console, so the drive
replaces the console while docked (flash over the other USB-C port, or just
press OK/BACK to eject and get the console back). New dumps still go on
over BLE; the snapshot is rebuilt every time you dock.

**Battery telemetry:** with the optional 100k/100k divider fitted
(`docs/WIRING.md`), the header shows the cell's charge percentage
between the wordmark and the BLE indicator — grey normally, red at/below
15 %. Below 3.35 V the device warns once (`Battery low` + a chirp with the
piezo fitted); below 3.25 V sustained for 30 s it runs the clean
power-off flow to protect the cell (charging suspends the checks — the
terminal voltage rises while the TP4056 is active).

**Pairing & OTA:** the first BLE connection runs numeric-comparison pairing:
a random 6-digit PIN appears on the OLED, the phone shows the same code,
and the bond is stored. **Wi-Fi updates:** in the web page's Settings (or
via BLE) send `WIFI:<ssid>|<pass>` once, then `OTA:START` (or the web page's
Update button) — the device joins Wi-Fi, fetches the newest GitHub release,
flashes only the app partition and reboots. Library, keys and settings
survive, same as the wired updater; BACK cancels mid-download.

**Travel mode:** hold BACK (1 s) at the menu to toggle. On, the device stops
BLE advertising completely — it won't appear in phone scans — while
everything else (emulation, buttons, auto-sleep) keeps working; `BLE sync`
while it's on says `Travel: BLE off`. The setting persists across reboots
and survives firmware updates. Use it anywhere you'd put a phone in
airplane mode.

**Random-UID re-sign (per amiibo):** hold BACK on a library entry to flag it
(an `R` badge appears). Every emulation or hot-swap of a flagged entry then
re-signs the dump for a fresh random UID with the on-device amiitool crypto,
so consoles treat it as a brand-new figure each time — needs your key files
loaded (`Random needs keys` says so otherwise), v2 dumps only, and the flag
persists as a zero-byte `.rnd` sidecar next to the dump.

**Factory reset:** the last menu item wipes the amiibo library (including
`.rnd` flags), the signing keys, and all settings (travel mode included)
back to as-built. The confirm screen requires **holding OK for ~1.2 s**;
any short press cancels, and the screen gives up on its own after 15 s.
There is no undo — re-load your dumps from the web page afterwards.

Preview the whole interface (buttons, long-press, every screen,
power off/on) in your
browser with [`device/web-preview/index.html`](device/web-preview/index.html).

## Scope notes

- **Amiibo signing keys are user-supplied at runtime and never bundled.**
  The re-signing code (`device/src/amiibo_crypto.h`) implements the publicly
  documented amiitool (MIT) algorithm; no Nintendo proprietary data ships in
  this repository. Uploaded keys live in the device's NVS (not encrypted at
  rest — the device is assumed personal, like a phone running TagMo).
- The NDEF-note emulation personality uses a random, persisted 7-byte UID.
- **Wireless charging is a hardware option**: a ⌀ ≤ 40 mm ferrite-backed Qi
  receiver coil (under the battery, on the engraved ring) feeds the TP4056
  through a second Schottky — see `docs/WIRING.md`. No firmware
  change needed; the optional 22 kΩ sense wire (GPIO21) lights the bolt.
- `docs/HARDWARE.md` documents the interface baseline; the PCB is
  self-checked by its generator but has not been manufactured in this repo —
  review it in KiCad before ordering.

## License

MIT License; see [`LICENSE`](LICENSE).
