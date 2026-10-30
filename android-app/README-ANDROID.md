# OpenTag for Android

A native Android app (Kotlin, platform APIs only — zero external
dependencies) that connects to your OpenTag device over Bluetooth LE for
amiibo file dumping and emulation control.

## What it does

- **Scan & connect** to OpenTag (filters on the OpenTag service UUID,
  requests MTU 247, subscribes to the status characteristic).
- **Dump amiibo files from your phone**: pick `.bin` dumps or a `.zip` of
  them (any 532/540/572-byte NTAG215 container the device accepts). The app
  streams them with the device's chunked transfer protocol and the device
  normalizes + validates each one before storing it in its LittleFS library.
- **Metadata & box art**: bins are matched **by file name** against the
  bundled 932-entry amiibo database (`app/src/main/assets/amiibo.json`,
  generated from the community [AmiiboAPI] database) — first by a
  16-hex-character amiibo ID in the name (`0000000000340102.bin`), then by a
  fuzzy name match (`mario smash.bin`). Box art loads from AmiiboAPI's image
  CDN, with a colored-initial placeholder offline.
- **Emulate**: tap an on-device entry to start emulation on the OpenTag (the
  device presents the amiibo to a console — no physical tag writing).
- **NDEF note**: send a text/URL note to the device.
- Live status log of every BLE notification.

The amiibo ID *inside* a retail dump is AES-encrypted, which is why matching
happens by file name — the same approach collectors' naming conventions
support.

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
  [`keys/`](keys/README.md) — stable, so upgrades work, but public, so it
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
