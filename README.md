# OpenTag

OpenTag is an independent, open-source NFC device project. The `device/` directory contains a standalone ESP32-S3 firmware prototype targeting 16 MB flash, BLE, a 1.4-inch SSD1351 OLED, Up/Down/OK buttons, a physical power switch, USB-C, and a PN532 NFC frontend.

> OpenTag is not affiliated with or endorsed by Nintendo, Galmiibo, or any tag/device manufacturer. The linked Drive folder's directory names can be used as catalog labels, but its 540-byte `.bin` tag images are not imported, parsed, written, or emulated by this project.

## Device firmware

The device prototype supports standard NDEF text and HTTP(S) URL records on NTAG215-sized tags. The user confirms each write on-device, and the firmware reads back written pages to verify the result. The PN532 is used in reader/writer mode only; tag emulation, raw tag-image import, proprietary tag data, and cryptographic key handling are not implemented.

### Build and flash

Requirements: PlatformIO Core, the Espressif32 PlatformIO platform, and an ESP32-S3 development board/module with 16 MB flash. The prototype pin assignment and parts baseline are in [`device/HARDWARE.md`](device/HARDWARE.md).

```sh
cd device
pio run -e opentag_s3
pio run -e opentag_s3 -t upload
```

Open `device/web-sync/index.html` from a secure origin (HTTPS or localhost) in a Web Bluetooth-capable browser such as Chrome or Edge. Pair with **OpenTag**, add or import a JSON notes library, and send a text or URL note. `.bin` files are deliberately rejected. The firmware persists the currently queued note in NVS; the browser keeps the notes library in local storage and can export/import the supported JSON format.

> `device/HARDWARE.md` is a design baseline, **not a tested, fabrication-ready KiCad project**. Confirm the exact display and NFC module, electrical details, RF matching, enclosure, and PCB layout before manufacturing.

## License

MIT License; see [`LICENSE`](LICENSE). Contributions are welcome within the NDEF-only, non-emulation scope.
