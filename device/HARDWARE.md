# OpenTag device hardware baseline

This is a **schematic-level prototype specification**, not a fabrication-ready PCB. It fixes the initial electrical interfaces for firmware development; verify the chosen module/display pinouts, RF layout, antenna tuning, thermal/power margins, and enclosure before ordering a board.

## Assumed parts

| Ref | Part / requirement | Notes |
|---|---|---|
| U1 | ESP32-S3-WROOM-1-N16R8 | 16 MB SPI flash, 8 MB PSRAM, BLE, native USB; use module variant with matching flash/PSRAM package. |
| U2 | PN532 NFC frontend/module, 3.3 V SPI | Reader/writer for standard NFC Forum Type 2 NDEF tags. The prototype writes ordinary NDEF text/URI records only. It has no tag-emulation mode. |
| J2 | 1.4-inch 128×128 OLED, SSD1351, 4-wire SPI | Confirm module pin order and voltage before layout. |
| SW1–SW3 | Up, Down, OK tactile switches | Active-low, GPIO input with pull-ups. |
| SW4 | Latching power switch | USB-C 5 V input is switched before the 3.3 V regulator. This is a true off switch, not a GPIO sleep button. |
| J1 | USB-C receptacle | 5 V input and USB 2.0 data for native USB programming/serial. Add 5.1 kΩ Rd on CC1 and CC2 for a sink-only device. |
| U3 | 3.3 V buck regulator, ≥1.5 A peak | Select a proven part and follow its reference layout. Size for ESP32-S3 BLE current peaks plus display and NFC frontend. |
| D1 | USB ESD protection | Protect D+/D− and any exposed connector lines; add input transient protection as appropriate. |
| J3 | Optional 4-pin debug/test header | 3V3, GND, UART TX/RX or JTAG, per final production/debug decision. |

No battery was specified, so this baseline is USB-powered and has no Li-ion charging circuit. Add a battery/charger/power-path design only after selecting a cell and runtime target.

## Pin assignment used by firmware

| Signal | ESP32-S3 GPIO |
|---|---:|
| SPI SCK / MISO / MOSI | 12 / 13 / 11 |
| OLED CS / DC / RESET | 10 / 9 / 8 |
| PN532 SPI CS | 14 |
| Up / Down / OK | 4 / 5 / 6 |
| USB D− / D+ | 19 / 20 (native USB; verify module/board routing) |

OLED and PN532 share the SPI bus and use separate chip-select lines. Keep CS lines pulled high during reset. Use a 3.3 V compatible PN532 module and display; do not connect 5 V logic to ESP32-S3 GPIOs. Button inputs are active-low and pulled up in firmware.

## PCB layout checklist

1. Use the ESP32-S3 module manufacturer's reference layout, antenna keep-out, and USB routing guidance. Keep copper/metal away from the module antenna zone.
2. Put the PN532 and its tuned 13.56 MHz antenna at the edge of the enclosure, away from the ESP32 antenna, switching regulator, display flex, and USB connector. A PN532 breakout with its own antenna simplifies the first carrier-board revision; retain the module's matching network unchanged.
3. Keep the buck converter hot loop short, use the manufacturer's recommended inductor/capacitors, and provide a continuous ground reference.
4. Route SPI as a short shared bus, with distinct CS lines and local decoupling at U1/U2/display.
5. Put the NFC antenna under a marked tap area. Verify read/write range with the final enclosure and battery/metalwork before release.
6. Add test pads for 5 V, 3V3, GND, SPI, and reset. Include USB ESD and CC resistors near J1.
7. Validate the board with current measurement, USB attach/detach, BLE range, NFC interoperability, and repeated NDEF write/readback tests before calling it production-ready.

## Scope boundary

The firmware and board baseline intentionally provide standard NFC polling and ordinary NDEF writing. They do not implement NTAG215 tag emulation, raw memory-image import, proprietary tag formats, or cryptographic key handling. Writing a note replaces the tag's existing NDEF user area after the user confirms on-device; only a tag advertising the NTAG215-sized NDEF capability container is accepted.
