# OpenTag for Android

A native Android app (Kotlin, platform APIs only — zero external
dependencies) that connects to your OpenTag device over Bluetooth LE for
amiibo file dumping and emulation control. The UI is a native replication
of the [web sync page](../device/web-sync/index.html) — same dark
mint/violet design, cards, status dot, log console and amiibo database —
built entirely from platform views and shape drawables (no WebView, no
libraries). A scroll-capture-style preview of the screen is in
[`app-ui-preview.png`](app-ui-preview.png) next to this file
(regenerate with `python3 tools/render_app_preview.py`).

The launcher icon is the OpenTag mark — a mint ring (the "O") with a
violet tag dot seated in its notch — drawn full-bleed as an adaptive icon
(color background + foreground + Android 13+ monochrome layers, so it
fills whatever shape the launcher masks instead of sitting in a square),
with legacy PNG rasters for Android 5–7. The geometry lives in
[`tools/build_icon.py`](../tools/build_icon.py); the web sync page's
header uses the same mark as an inline SVG.

## What it does

- **Scan & connect** to OpenTag (filters on the OpenTag service UUID,
  requests MTU 247, subscribes to the status characteristic); a live
  status dot + label tracks the connection like the web page's.
- **Dump amiibo files from your phone**: pick `.bin` dumps, Flipper Zero
  `.nfc` exports, or a `.zip` of them. Intake matches the web sync page
  exactly: any 532/540/572-byte NTAG215 container (zero/newline-padded or
  behind a 32-byte N2-style header) is normalized to a clean 540-byte page
  image on the phone; Kirby Air Riders **v3** dumps (1024-byte NTAG I²C
  page images) are accepted as-is for emulation. Each row shows its chip
  (`NTAG215` / `V3 · I2C` / `INVALID` + the reject reason) and UID.
- **Metadata & box art**: bins are matched **by file name** against the
  bundled 932-entry amiibo database
  (`android-app/app/src/main/assets/amiibo.json`,
  generated from the community [AmiiboAPI] database) — first by a
  16-hex-character amiibo ID in the name (`0000000000340102.bin`), then by a
  fuzzy name match (`mario smash.bin`). Box art loads from AmiiboAPI's image
  CDN, with a colored-initial placeholder offline.
- **Amiibo database browser**: search the whole database by name, series
  or game with box art — no device connection needed, just like the web
  page's database section.
- **Emulate**: tap an on-device entry to start emulation on the OpenTag (the
  device presents the amiibo to a console — no physical tag writing).
- **NDEF note**: send a text/URL note to the device, or clear the device's
  stored note remotely (`NOTE:CLEAR`, the same command the device menu
  runs).
- **Amiibo signing keys** (advanced): pick your `unfixed-info.bin` /
  `locked-secret.bin` (or one combined 160-byte retail file — files are
  identified by their embedded type string), upload them over the same
  chunked BLE stream, check the device's key state, or forget them
  (`KEY:PUT` / `KEY:STATUS` / `KEY:CLEAR`). Keys live in the device's NVS
  and enable the on-device TagMo-style write flow.
- **Firmware updates over Wi-Fi**: save Wi-Fi credentials
  (`WIFI:<ssid>|<pass>`), check that they're stored (`OTA:STATUS`), and
  start an over-the-air update (`OTA:START`) — the device fetches the
  newest firmware release from GitHub and flashes only the app partition.
- Live status log of every BLE notification.

The amiibo ID *inside* a retail dump is AES-encrypted, which is why matching
happens by file name — the same approach collectors' naming conventions
support.

## OpenTag Mini

The app also drives the **OpenTag Mini**, the screenless variant — and
tells the two apart for you. On connect the app sends `INFO` and reads the
reply (`OK:info:standard|mini:<battery mV>:<charging>`):

- the connection card's subtitle shows **OpenTag Mini · screenless · no
  pairing PIN** (the handheld shows **OpenTag · OLED handheld**), then
  settles to the model plus live **battery % / charging** readout — on the
  Mini this line is the only "screen" the device has;
- the Mini pairs with **no PIN** (just-works) — there's no numeric
  comparison to confirm, which is what makes the screenless design work;
- the remote action set covers everything the handheld's on-device menu
  does: per-amiibo **Emulate / Write to NTAG215 / Toggle random UID /
  Delete**, plus the tag tools **Backup tag** and **Inspect tag**, and
  **Write / Emulate NDEF note** — on the Mini these replace the menu
  entirely; on the handheld they're the same commands its menu runs, now
  available remotely;
- everything else (uploads, keys, Wi-Fi/OTA, USB drive, stop) is
  identical, over the same BLE protocol — one app, both devices.

## Build

1. Open Android Studio → **File ▸ Open** → select this `android-app/`
   directory and let Gradle sync (first sync downloads the Android Gradle
   plugin; the app itself has no dependencies).
2. **Run** on a phone with Android 6.0+ (API 23). BLE + the camera-less
   file picker are the only requirements.

Or from the command line:

```sh
cd android-app
gradle assembleDebug        # needs a local Gradle 8.7+ / Android SDK 34
```

The project deliberately avoids AndroidX and every third-party library so it
builds offline once the toolchain itself is present.

## Automatic releases

Every merge to `main` triggers [.github/workflows/android-app.yml](../.github/workflows/android-app.yml):
it builds a **signed release APK** and publishes it as a GitHub release tagged
`app-v<version>` (patch-bumped each time, `versionCode` = the run number).
Pull requests that touch `android-app/` only build — a compile gate.

- **Installing:** grab the newest `opentag-android-*.apk` from the repo's
  [Releases](https://github.com/mob5824m-wq/OpenTag/releases) (the `app-v*`
  entries), open it on your phone and allow installs from that source. Each
  release upgrades in place over the previous one.
- **Signing:** releases are signed with the committed *open* key in
  [`android-app/keys/`](../android-app/keys/) (see [`SIGNING-KEY.md`](SIGNING-KEY.md)) — stable, so upgrades work, but public, so it
  proves nothing about origin; trust the Releases page, not the signature.
  Swap in your own private key via the `OT_KEYSTORE_B64` /
  `OT_KEYSTORE_PASSWORD` repo secrets if you prefer a closed signature.
- App releases are deliberately **not** marked "Latest" in the repo — that
  badge stays on the firmware releases that `tools/update_opentag.py` and the
  device's OTA resolve automatically.

## Permissions

- Android 12+: `BLUETOOTH_SCAN` (neverForLocation) + `BLUETOOTH_CONNECT`,
  requested at runtime.
- Android 11 and older: `BLUETOOTH`, `BLUETOOTH_ADMIN`,
  `ACCESS_FINE_LOCATION` (required by the legacy BLE scan API), requested at
  runtime.
- `INTERNET` — only for fetching box-art images.

## Protocol

The app speaks the same BLE protocol as the web sync page — see the root
README's device section: control characteristic `…0002` (text commands),
status `…0003` (notifications), data `…0004` (`[seq_hi][seq_lo][len]` + up
to 240 payload bytes per chunk, sequence from 0, total = the size announced
in `AMI:PUT`).

[AmiiboAPI]: https://github.com/N3evin/AmiiboAPI
