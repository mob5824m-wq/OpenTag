// ---------------------------------------------------------------------------
// OpenTag Mini — the screenless build of the OpenTag firmware.
//
// Same hardware platform as the handheld (ESP32-S3 DevKitC-1 + PN532Killer
// on UART2 + TP4056/LiPo power), minus the SSD1351 OLED and three of the
// four buttons. There is no screen to show a pairing passkey on, so the
// Mini pairs just-works (bonded, secure connections, no PIN entry) and
// every screen the handheld had is replaced by a BLE command instead:
//
//   NOTE:WRITE     arm "write the synced note on the next tag tap"
//   NOTE:EMU       emulate the synced note (virtual NTAG215)
//   AMI:BACKUP     arm "dump the next amiibo tap into the library"
//   AMI:RAND:name  toggle per-entry UID randomization
//   AMI:LOCK       lock the tag just written (TagMo writeLockInfo)
//   TAG:INSPECT    arm "identify the next tag tap" (answer over BLE)
//   USB:START/STOP dock/eject the read-only USB library drive
//   INFO           model + battery millivolts + charge state
//
// …on top of the full shared protocol (TEXT/URL notes, AMI:PUT/LIST/DEL/EMU/
// WRITE, KEY:PUT/STATUS/CLEAR, WIFI/OTA, STAT, STOP). The Android app and
// the web sync page drive both devices with the same command set; they tell
// them apart by the advertised name ("OpenTag" vs "OpenTag Mini") and the
// INFO reply ("standard" vs "mini").
//
// Hardware differences from the handheld:
//   * no OLED / no SPI bus — those GPIOs stay unused
//   * one button (OK, GPIO1, an RTC pin): short press chirps the battery
//     level (1 chirp per 20 %), triple press toggles travel mode (BLE
//     advertising off), hold 1.5 s powers off, hold 1 s at wake boots
//   * everything else (piezo chirps, charge sense, battery divider,
//     low-battery protection, auto-sleep, USB MSC, Wi-Fi OTA) is identical
//
// All tag data is user-supplied at runtime; nothing is bundled here.
// ---------------------------------------------------------------------------

#include <Arduino.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Update.h>
#include <USB.h>
#include <USBMSC.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <cstring>
#include <string>

#include "../ndef_tlv.h"
#include "../ntag215_emu.h"
#include "../pn532_uart.h"
#include "../amiibo_crypto.h"
#include <mbedtls/aes.h>
#include <mbedtls/md.h>

// ---- amiibo crypto backends (mbedtls; algorithm tested against amiitool) ----

extern "C" void otAes128CtrXor(const uint8_t *key, const uint8_t *iv,
                               uint8_t *data, size_t len) {
  mbedtls_aes_context aes;
  mbedtls_aes_init(&aes);
  if (mbedtls_aes_setkey_enc(&aes, key, 128) == 0) {
    unsigned char nonceCounter[16], streamBlock[16];
    size_t ncOff = 0;
    memcpy(nonceCounter, iv, 16);
    memset(streamBlock, 0, sizeof(streamBlock));
    mbedtls_aes_crypt_ctr(&aes, len, &ncOff, nonceCounter, streamBlock, data, data);
  }
  mbedtls_aes_free(&aes);
}

extern "C" void otHmacSha256(const uint8_t *key, size_t keyLen,
                             const uint8_t *data, size_t len, uint8_t *out32) {
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  mbedtls_md_hmac(info, key, keyLen, data, len, out32);
}

// ---------------------------------------------------------------------------
// Pins / constants (mirrors the handheld everywhere except the display bus)
// ---------------------------------------------------------------------------

namespace Pins {
constexpr int NFC_TX = 17, NFC_RX = 18;  // ESP32 TX -> Killer J4 RX
constexpr int BUTTON_OK = 1;             // the single button (RTC pin: wake)
constexpr int CHG_SENSE = 21;            // TP4056 /CHRG via 22k, active low
constexpr int BATT_SENSE = 4;            // 100k/100k divider midpoint
constexpr int PIEZO = 5;                 // passive buzzer
}

namespace BleIds {
static const char *SERVICE = "9b9e0001-9d2a-4b72-9a0c-6a8f6f4d0001";
static const char *WRITE_VALUE = "9b9e0002-9d2a-4b72-9a0c-6a8f6f4d0001";
static const char *STATUS = "9b9e0003-9d2a-4b72-9a0c-6a8f6f4d0001";
static const char *DATA = "9b9e0004-9d2a-4b72-9a0c-6a8f6f4d0001";
}

namespace Amiibo {
constexpr size_t kMaxFiles = 128;
constexpr size_t kNameLen = 41;
constexpr const char *kDir = "/amiibo";
}

constexpr uint32_t kAutoSleepMs = 3UL * 60 * 1000;   // idle, not connected
constexpr const char *kModel = "mini";

Pn532Uart nfc;
Preferences prefs;
HardwareSerial& nfcSerial(Serial2);
NimBLECharacteristic *statusCharacteristic = nullptr;

static portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
static char pendingValue[241] = {0};
static char pendingKind[5] = "TEXT";
static bool pendingChanged = false;
static bool pendingPresent = false;
static bool bleConnected = false;
static bool nfcReady = false;

// Charge sense state (debounced in loop()). True = TP4056 reporting charge.
static bool charging = false;
static uint8_t chgDebounce = 0;
static uint32_t chgPollMs = 0;

// Battery telemetry (optional divider on BATT_SENSE).
static float battVolts = 0;          // 0 = no divider fitted
static uint32_t battPollMs = 0;
static uint32_t battLowSinceMs = 0;

// Wi-Fi OTA state (triggered from the app/web page after provisioning).
static bool otaRequested = false;
static volatile bool otaCancelled = false;

// ---- BLE command mailbox (consumed in loop()) ----
static char bleCommand[248] = {0};
static volatile bool bleCommandPending = false;

// ---- binary transfer state (filled from the BLE task, finalized in loop()) ----
static volatile bool recvActive = false;
static volatile bool recvComplete = false;
static uint16_t recvSeq = 0;        // next expected chunk sequence
static uint16_t recvAckPending = 0; // seq of last chunk (0 = none)
static bool recvAckWanted = false;
static size_t recvSize = 0;
static size_t recvLen = 0;
static char recvName[Amiibo::kNameLen] = {0};
static uint8_t recvBuf[ntag215::kV3ImageBytes];  // v2 containers or a v3 image
static bool recvIsKeys = false;     // transfer carries key files, not a dump

// ---- amiibo signing keys (user-supplied, stored in NVS, never bundled) ----
static amiibo::AmiiboKeys retailKeys;
static bool keysReady = false;
// Travel mode: BLE advertising off (triple-press the button).
static bool travelMode = false;
static void refreshKeyState() {
  uint8_t data[amiibo::kKeyFileSize] = {0};
  uint8_t tag[amiibo::kKeyFileSize] = {0};
  const bool haveData = prefs.getBytes("keys-data", data, sizeof(data)) == sizeof(data);
  const bool haveTag = prefs.getBytes("keys-tag", tag, sizeof(tag)) == sizeof(tag);
  keysReady = haveData && haveTag &&
              amiibo::loadKeyPair(data, sizeof(data), tag, sizeof(tag), &retailKeys);
}

// ---- write-to-tag flow (TagMo "WRITE TAG (AUTO)" equivalent) ----
static char writeName[Amiibo::kNameLen] = "";
static uint8_t writeImage[540];     // armed dump, re-signed at tap time
static bool writeArmed = false;

// ---- armed tap action (the Mini's replacement for screen flows) ----
enum ArmedAction : uint8_t {
  ARM_NONE = 0,
  ARM_NOTE_WRITE,    // write the synced note onto the tapped NTAG215
  ARM_BACKUP,        // dump the tapped amiibo into the library
  ARM_INSPECT,       // identify the tapped tag, answer over BLE
  ARM_WRITE_TAG,     // write the armed amiibo onto the tapped blank
};
static ArmedAction armedAction = ARM_NONE;

// ---- emulation state ----
enum : uint8_t { EMU_NONE = 0, EMU_NOTE, EMU_AMIIBO };
static uint8_t emuMode = EMU_NONE;
static char emuName[Amiibo::kNameLen] = "";
static uint8_t emuUid[7] = {0x04, 0, 0, 0, 0, 0, 0};
static bool emuV3 = false;          // emulating a v3 (Air Riders) personality
static uint8_t emuSlot = 0;        // live Killer emulator slot

// ---- idle bookkeeping ----
static uint32_t lastActivityMs = 0;
static void noteActivity() { lastActivityMs = millis(); }

// ---- amiibo library ----
static char amiNames[Amiibo::kMaxFiles][Amiibo::kNameLen];
static bool amiV3[Amiibo::kMaxFiles];
static bool amiRandom[Amiibo::kMaxFiles];
static size_t amiCount = 0;
static unsigned long lastNfcPoll = 0;

// ---------------------------------------------------------------------------
// Feedback: the piezo is the Mini's only local output, so every screen the
// handheld had maps to a chirp pattern here (and to a BLE status line).
// ---------------------------------------------------------------------------

static void chirp(uint16_t freq, uint16_t ms) { tone(Pins::PIEZO, freq, ms); }

// Battery telemetry: 100k/100k divider, 2 s cadence. A raw reading under
// ~0.1 V means the divider is not fitted -> batteryPercent() reports -1.
static void pollBattery() {
  const int raw = analogRead(Pins::BATT_SENSE);
  if (raw < 60) { battVolts = 0; battLowSinceMs = 0; return; }
  battVolts = raw * (3.3f / 4095.0f) * 2.0f;
}

static int batteryPercent() {
  if (battVolts <= 0.5f) return -1;
  int pct = (int)((battVolts - 3.30f) / 0.80f * 100.0f);
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// Short button press: chirp the battery level (1 chirp per 20 %, a long low
// tone when critical, two flat chirps when no divider is fitted).
static void announceBattery() {
  const int pct = batteryPercent();
  if (pct < 0) {
    chirp(2000, 60); delay(150); chirp(2000, 60);
    return;
  }
  const int beeps = (pct + 19) / 20;
  for (int i = 0; i < beeps; i++) { chirp(2400, 50); delay(160); }
  if (pct <= 15) { delay(120); chirp(1400, 300); }
}

void setStatus(const char *value) {
  if (statusCharacteristic) { statusCharacteristic->setValue(value); statusCharacteristic->notify(); }
}

// ---------------------------------------------------------------------------
// Storage (identical to the handheld: same file layout, same sidecars, so a
// library restored from one device works on the other)
// ---------------------------------------------------------------------------

static bool sanitizeName(const char *in, char *out, size_t cap) {
  size_t n = 0;
  for (const char *p = in; *p && n < cap - 1; ++p, ++n) {
    const char c = *p;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == ' ';
    if (!ok) return false;
    out[n] = c;
  }
  out[n] = 0;
  if (n == 0 || out[0] == '.') return false;
  return true;
}

static void amiiboPath(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s.bin", Amiibo::kDir, name);
}

// Per-entry "randomize UID" flag: a zero-byte sidecar next to the dump.
static void randomFlagPath(const char *name, char *out, size_t cap) {
  snprintf(out, cap, "%s/%s.rnd", Amiibo::kDir, name);
}
static bool amiiboRandomFlag(const char *name) {
  char path[Amiibo::kNameLen + 16];
  randomFlagPath(name, path, sizeof(path));
  return LittleFS.exists(path);
}
static void setAmiiboRandomFlag(const char *name, bool on) {
  char path[Amiibo::kNameLen + 16];
  randomFlagPath(name, path, sizeof(path));
  if (on) {
    File f = LittleFS.open(path, "w");
    if (f) f.close();
  } else {
    LittleFS.remove(path);
  }
}

static int compareNames(const void *a, const void *b) {
  return strcmp((const char *)a, (const char *)b);
}

static size_t listAmiibos() {
  amiCount = 0;
  File dir = LittleFS.open(Amiibo::kDir);
  if (!dir) return 0;
  for (File f = dir.openNextFile(); f && amiCount < Amiibo::kMaxFiles; f = dir.openNextFile()) {
    if (f.isDirectory()) continue;
    const char *base = strrchr(f.name(), '/');
    base = base ? base + 1 : f.name();
    size_t n = strlen(base);
    if (n < 5 || strcmp(base + n - 4, ".bin") != 0 || n > Amiibo::kNameLen - 1) continue;
    char name[Amiibo::kNameLen];
    strlcpy(name, base, n - 3);  // strip ".bin"
    strlcpy(amiNames[amiCount], name, Amiibo::kNameLen);
    amiCount++;
  }
  qsort(amiNames, amiCount, Amiibo::kNameLen, compareNames);
  char path[64];
  for (size_t i = 0; i < amiCount; i++) {
    amiiboPath(amiNames[i], path, sizeof(path));
    File f = LittleFS.open(path, "r");
    amiV3[i] = f && f.size() >= ntag215::kV3ImageBytes;
    if (f) f.close();
    amiRandom[i] = amiiboRandomFlag(amiNames[i]);
  }
  return amiCount;
}

// Reads /amiibo/<name>.bin, validates it and normalizes it: v2 dumps become
// 540-byte NTAG215 page images; v3 (Air Riders) dumps keep their full
// 1024-byte page image with *isV3 set.
static bool loadAmiiboImage(const char *name, uint8_t image[ntag215::kV3ImageBytes],
                            bool *isV3 = nullptr) {
  char path[64];
  amiiboPath(name, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t buf[ntag215::kV3ImageBytes];
  const size_t got = f.read(buf, sizeof(buf));
  f.close();
  if (isV3) *isV3 = false;
  memset(image, 0, ntag215::kV3ImageBytes);
  if (got < 532) return false;
  if (ntag215::validateV3Dump(buf, got)) {
    memcpy(image, buf, ntag215::kV3ImageBytes);
    if (isV3) *isV3 = true;
    memcpy(emuUid, image, 7);
    return true;
  }
  if (got > 572) return false;
  if (!ntag215::validateDump(buf, got)) return false;
  ntag215::normalizeDump(buf, got, image);
  memcpy(emuUid, image, 7);
  return true;
}

// TagMo-style "random tag": re-sign the dump for a fresh random UID.
static bool randomizeImageUid(uint8_t image[ntag215::kV3ImageBytes]) {
  uint8_t idPages[9];
  idPages[0] = 0x04;                                  // NXP manufacturer byte
  for (int i = 1; i < 8; i++) idPages[i] = (uint8_t)random(256);
  idPages[3] = 0x88 ^ idPages[0] ^ idPages[1] ^ idPages[2];  // BCC0
  idPages[8] = image[8];                              // keep page-2 byte 0
  if (!amiibo::resign(&retailKeys, image, idPages)) return false;
  memcpy(image, idPages, 8);
  memcpy(emuUid, image, 7);
  return true;
}

// Loads `name` for emulation, applying the per-entry random-UID re-sign when
// flagged (v2 dumps with key files loaded). Falls back to the untouched dump
// instead of failing the emulation — the same behaviour as the handheld,
// which explains itself on screen; here the flags are visible via AMI:RAND.
static bool loadAmiiboForEmulation(const char *name,
                                    uint8_t image[ntag215::kV3ImageBytes],
                                    bool *v3) {
  if (!loadAmiiboImage(name, image, v3)) return false;
  if (!*v3 && amiiboRandomFlag(name)) {
    if (keysReady) randomizeImageUid(image);
  }
  return true;
}

// ---------------------------------------------------------------------------
// Tag backup (reader mode) — the remote AMI:BACKUP counterpart
// ---------------------------------------------------------------------------

// Dumps a presented NTAG215 tag page by page and saves it as a library
// entry named after its UID. Amiibo (v2) tags read as 135 pages; Air
// Riders-generation tags keep reading through page 0xFF and store the full
// 1024-byte image. Blocking (~2-3 s).
static void runTagBackup() {
  uint8_t image[ntag215::kV3ImageBytes];
  memset(image, 0, sizeof(image));
  bool ok = true;
  for (uint16_t base = 0; base < 136 && ok; base += 4) {
    uint8_t page16[16];
    if (!nfc.ntagReadPage((uint8_t)base, page16)) ok = false;
    else memcpy(image + (size_t)base * 4, page16, 16);
  }
  size_t len = 0;
  if (ok) {
    uint8_t norm[540];
    if (ntag215::validateDump(image, 540)) {
      ntag215::normalizeDump(image, 540, norm);
      memcpy(image, norm, 540);       // files are stored normalized
      len = 540;
    } else {
      // v2 structure didn't verify: try the extended (v3) read.
      for (uint16_t base = 136; base < 256 && ok; base += 4) {
        uint8_t page16[16];
        if (!nfc.ntagReadPage((uint8_t)base, page16)) ok = false;
        else memcpy(image + (size_t)base * 4, page16, 16);
      }
      if (ok && ntag215::validateV3Dump(image, ntag215::kV3ImageBytes)) len = ntag215::kV3ImageBytes;
      else ok = false;
    }
  }
  if (!ok) {
    setStatus("ERR:backup:not-amiibo");
    chirp(1400, 250);
    return;
  }
  char name[Amiibo::kNameLen];
  snprintf(name, sizeof(name), "%02x%02x%02x%02x%02x%02x%02x",
           image[0], image[1], image[2], image[3], image[4], image[5], image[6]);
  // Deduplicate: same UID + identical content = already backed up; a changed
  // dump (e.g. a re-randomized figure) lands as name-2, name-3, ...
  char candidate[Amiibo::kNameLen];
  strlcpy(candidate, name, sizeof(candidate));
  for (int n = 1; ; n++) {
    char path[64];
    amiiboPath(candidate, path, sizeof(path));
    if (!LittleFS.exists(path)) break;
    File f = LittleFS.open(path, "r");
    bool same = false;
    if (f) {
      uint8_t have[ntag215::kV3ImageBytes];
      size_t got = f.read(have, len);
      f.close();
      same = got == len && memcmp(have, image, len) == 0;
    }
    if (same) {
      setStatus("ERR:backup:already-saved");   // the app refreshes its list
      chirp(1800, 60); delay(110); chirp(1800, 60);
      return;
    }
    snprintf(candidate, sizeof(candidate), "%s-%d", name, n + 1);
  }
  char path[64];
  amiiboPath(candidate, path, sizeof(path));
  File f = LittleFS.open(path, FILE_WRITE);
  bool saved = false;
  if (f) {
    saved = f.write(image, len) == len;
    f.close();
  }
  if (!saved) {
    setStatus("ERR:backup:io");
    return;
  }
  char status[80];
  snprintf(status, sizeof(status), "OK:backup:%s", candidate);
  setStatus(status);
  chirp(2000, 60);
}

// ---------------------------------------------------------------------------
// Tag inspector: identify any presented Type 2 tag (remote TAG:INSPECT)
// ---------------------------------------------------------------------------

static void inspectTag(const uint8_t *uid, uint8_t uidLen) {
  char kind[20] = "Type 2 tag";
  char ndef[40] = "";
  uint8_t page[16] = {0};
  if (nfc.ntagReadPage(3, page) && page[0] == 0xE1) {
    const uint8_t sz = page[2];
    const char *model =
        sz == 0x3E ? "NTAG215" : sz == 0x12 ? "NTAG213" :
        sz == 0x6D ? "NTAG216" : sz == 0x06 ? "MIFARE UL" : "NTAG2";
    snprintf(kind, sizeof(kind), "%s %u B", model, (unsigned)sz * 8);
  }
  // NDEF TLV starts at page 4 (Type 2 mapping): 0x03 <len> <record...>
  uint8_t tlv[32] = {0};
  if (nfc.ntagReadPage(4, tlv) && tlv[0] == 0x03) {
    uint16_t plen = tlv[1];
    const uint8_t *rec = tlv + 2;
    if (plen == 0xFF) { plen = ((uint16_t)tlv[2] << 8) | tlv[3]; rec = tlv + 4; }
    if (plen >= 3) {
      const uint8_t hdr = rec[0];
      const uint8_t typeLen = rec[1];
      const uint8_t idLen = (hdr & 0x08) ? rec[3] : 0;
      const size_t head = 3 + ((hdr & 0x08) ? 1 : 0) + typeLen + idLen;
      // Text/URI records carry a status byte before the payload.
      size_t skip = 0;
      if ((hdr & 0x07) == 1 && typeLen == 1) skip = 1;
      size_t n = 0;
      for (size_t i = head + skip; i < (size_t)2 + plen && i < sizeof(tlv) &&
                                  n < sizeof(ndef) - 1; ++i) {
        const char c = (char)tlv[i];
        if (c == 0) break;
        if (c >= 0x20 && c < 0x7F) ndef[n++] = c;
      }
      ndef[n] = 0;
    }
  }
  chirp(1800, 50);
  char uidHex[24] = "";
  const uint8_t len7 = uidLen < 7 ? uidLen : 7;
  for (uint8_t i = 0; i < len7; ++i) {
    char b[4];
    snprintf(b, sizeof(b), "%s%02X", i ? ":" : "", uid[i]);
    strlcat(uidHex, b, sizeof(uidHex));
  }
  char line[128];
  snprintf(line, sizeof(line), "OK:inspect:%s:%s:%s", uidHex, kind, ndef[0] ? ndef : "-");
  setStatus(line);
}

// ---------------------------------------------------------------------------
// USB drive mode: read-only FAT12 snapshot of the amiibo library
// ---------------------------------------------------------------------------

// A 1 MiB FAT12 volume is built in PSRAM from /amiibo when the mode starts,
// then served read-only over the DevKit's USB-C port (TinyUSB MSC). The
// on-device LittleFS stays mounted and live - the USB view is a snapshot,
// and BLE uploads are refused until the drive is ejected.
#define USB_DISK_SECTORS 2048         // 1 MiB
#define USB_FAT_SECTORS 6
#define USB_ROOT_SECTORS 8            // 128 entries
#define USB_DATA_START (1 + USB_FAT_SECTORS + USB_ROOT_SECTORS)

static uint8_t *usbImage = nullptr;
static USBMSC usbMsc;
static bool usbDriveActive = false;

static void fat12Set(uint8_t *fat, uint32_t cl, uint32_t val) {
  const uint32_t off = cl + cl / 2;
  if (cl & 1) {
    fat[off] = (fat[off] & 0x0F) | (uint8_t)((val & 0x0F) << 4);
    fat[off + 1] = (uint8_t)((val >> 4) & 0xFF);
  } else {
    fat[off] = (uint8_t)(val & 0xFF);
    fat[off + 1] = (fat[off + 1] & 0xF0) | (uint8_t)((val >> 8) & 0x0F);
  }
}

// 8.3 name for the FAT root directory; `variant` disambiguates collisions.
static void make83(const char *name, int variant, char out[12]) {
  char base[9] = "        ";
  char ext[4] = "   ";
  const char *dot = strrchr(name, '.');
  size_t bl = dot ? (size_t)(dot - name) : strlen(name);
  if (dot && dot[1]) strlcpy(ext, dot + 1, sizeof(ext));
  size_t n = 0;
  for (size_t i = 0; i < bl && n < (variant ? 6u : 8u); ++i) {
    char c = name[i];
    if (c >= 'a' && c <= 'z') c -= 32;
    if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) base[n++] = c;
  }
  if (variant) {
    base[n++] = '~';
    base[n++] = (char)('0' + (variant % 10));
  }
  memcpy(out, base, 8);
  memcpy(out + 8, ext, 3);
  out[11] = 0;
}

static bool buildFat12Image() {
  usbImage = (uint8_t *)malloc((size_t)USB_DISK_SECTORS * 512);
  if (!usbImage) return false;
  memset(usbImage, 0, (size_t)USB_DISK_SECTORS * 512);

  // Boot sector (FAT12, following the TinyUSB MSC example layout).
  uint8_t *bs = usbImage;
  bs[0] = 0xEB; bs[1] = 0x3C; bs[2] = 0x90;
  memcpy(bs + 3, "OPENTAG ", 8);                       // OEM
  bs[11] = 512 & 0xFF; bs[12] = 512 >> 8;             // bytes/sector
  bs[13] = 1;                                         // sectors/cluster
  bs[14] = 1; bs[15] = 0;                             // reserved sectors
  bs[16] = 1;                                         // FAT copies
  bs[17] = 128; bs[18] = 0;                           // root entries
  bs[19] = USB_DISK_SECTORS & 0xFF; bs[20] = USB_DISK_SECTORS >> 8;
  bs[21] = 0xF8;                                      // media descriptor
  bs[22] = USB_FAT_SECTORS; bs[23] = 0;               // sectors per FAT
  bs[24] = 1; bs[25] = 0;                             // sectors/track
  bs[26] = 1; bs[27] = 0;                             // heads
  bs[36] = 0x00;                                      // drive number
  bs[38] = 0x29;                                      // extended boot sig
  bs[39] = 0x4F; bs[40] = 0x54; bs[41] = 0x41; bs[42] = 0x47;  // volume id
  memcpy(bs + 43, "OPENTAG USB ", 11);                // volume label
  memcpy(bs + 54, "FAT12   ", 8);
  bs[510] = 0x55; bs[511] = 0xAA;

  uint8_t *fat = usbImage + 512;
  fat12Set(fat, 0, 0xFF8);
  fat12Set(fat, 1, 0xFFF);

  uint8_t *root = usbImage + (size_t)(1 + USB_FAT_SECTORS) * 512;
  // Volume label as the first root entry.
  memcpy(root, "OPENTAG USB ", 11);
  root[11] = 0x08;                                    // volume label attr

  static const char kReadme[] =
      "OpenTag Mini amiibo library (read-only snapshot).\r\n"
      "Copy your .bin dumps OFF the device here; load new ones over BLE.\r\n";
  const uint32_t firstCluster = 2;
  const char *files[Amiibo::kMaxFiles + 1];           // +1 for README
  const char *names[Amiibo::kMaxFiles + 1];
  size_t nFiles = 0;
  File dir = LittleFS.open(Amiibo::kDir);
  if (dir) {
    for (File f = dir.openNextFile(); f && nFiles < Amiibo::kMaxFiles;
         f = dir.openNextFile()) {
      if (!f.isDirectory() && !String(f.name()).endsWith(".rnd")) {
        names[nFiles] = strrchr(f.name(), '/');
        names[nFiles] = names[nFiles] ? names[nFiles] + 1 : f.name();
        files[nFiles++] = f.name();
      } else {
        f.close();
      }
    }
    dir.close();
  }
  uint32_t cluster = firstCluster;
  uint8_t *entry = root + 32;                         // after the label
  size_t entryCount = 1;
  for (size_t i = 0; i <= nFiles; ++i) {              // i == nFiles -> README
    size_t size;
    if (i == nFiles) {
      size = sizeof(kReadme) - 1;
    } else {
      File f = LittleFS.open(files[i], "r");
      if (!f) continue;
      size = f.size();
      if (size > 65536) size = 65536;
    }
    const uint32_t ncl = (uint32_t)((size + 511) / 512);
    if (cluster + ncl > USB_DISK_SECTORS - USB_DATA_START ||
        entryCount >= 128) break;                     // volume full
    char shortName[12];
    int variant = 0;
    bool clash;
    do {
      make83(i == nFiles ? "README.TXT" : names[i], variant++, shortName);
      clash = false;
      for (const uint8_t *e = root + 32; e < entry; e += 32)
        if (memcmp(e, shortName, 11) == 0) { clash = true; break; }
    } while (clash && variant < 10);
    memcpy(entry, shortName, 11);
    entry[11] = 0x20;                                 // archive
    entry[26] = cluster & 0xFF; entry[27] = cluster >> 8;
    entry[28] = size & 0xFF; entry[29] = (size >> 8) & 0xFF;
    entry[30] = (size >> 16) & 0xFF; entry[31] = (size >> 24) & 0xFF;
    uint8_t *data = usbImage + (size_t)(USB_DATA_START + cluster - 2) * 512;
    if (i == nFiles) {
      memcpy(data, kReadme, size);
    } else {
      File f = LittleFS.open(files[i], "r");
      if (f) { f.read(data, size); f.close(); }
    }
    for (uint32_t c = 0; c < ncl; ++c)
      fat12Set(fat, cluster + c, cluster + c + 1);
    fat12Set(fat, cluster + ncl - 1, 0xFFF);          // end of chain
    cluster += ncl;
    entry += 32;
    entryCount++;
  }
  return true;
}

static int32_t usbMscRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize) {
  if (!usbImage || lba >= USB_DISK_SECTORS) return -1;
  const size_t avail = (size_t)(USB_DISK_SECTORS - lba) * 512 - offset;
  const size_t n = bufsize < avail ? bufsize : avail;
  memcpy(buffer, usbImage + (size_t)lba * 512 + offset, n);
  return (int32_t)n;
}

static int32_t usbMscWrite(uint32_t, uint32_t, uint8_t *, uint32_t) {
  return -1;                                          // read-only volume
}

static bool usbMscStartStop(uint8_t, bool, bool) { return true; }

static bool enterUsbDrive() {
  if (!buildFat12Image()) return false;
  usbMsc.vendorID("OpenTag");
  usbMsc.productID("amiibo library");
  usbMsc.productRevision("1.0");
  usbMsc.onRead(usbMscRead);
  usbMsc.onWrite(usbMscWrite);
  usbMsc.onStartStop(usbMscStartStop);
  usbMsc.mediaPresent(true);
  if (!usbMsc.begin(USB_DISK_SECTORS, 512)) {
    free(usbImage); usbImage = nullptr;
    return false;
  }
  usbDriveActive = true;
  chirp(2200, 60);
  return true;
}

static void exitUsbDrive() {
  usbMsc.end();
  usbMsc.mediaPresent(false);
  free(usbImage);
  usbImage = nullptr;
  usbDriveActive = false;
  chirp(1800, 60);
}

// ---------------------------------------------------------------------------
// Wi-Fi OTA: credentials provisioned over BLE, update started from the app
// or web page (OTA:START). Downloads the latest GitHub release into the
// inactive app slot and reboots into it; the filesystem is untouched.
// ---------------------------------------------------------------------------

static const char *kOtaApiUrl =
    "https://api.github.com/repos/mob5824m-wq/OpenTag/releases/latest";

static void otaProgressCb(size_t done, size_t total) {
  (void)done; (void)total;   // progress is implicit: the OK button cancels
  if (digitalRead(Pins::BUTTON_OK) == LOW) otaCancelled = true;
}

static void runOtaUpdate() {
  const String ssid = prefs.getString("wssid", "");
  const String pass = prefs.getString("wpass", "");
  if (!ssid.length()) { setStatus("ERR:ota-no-creds"); return; }
  otaCancelled = false;
  chirp(2400, 80); delay(120); chirp(2400, 80);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());
  const uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    if (otaCancelled || millis() - t0 > 20000) {
      WiFi.disconnect();
      setStatus(otaCancelled ? "ERR:ota-cancelled" : "ERR:ota-wifi");
      return;
    }
    delay(100);
  }

  WiFiClientSecure client;
  client.setInsecure();   // same trust level as the USB updater (see README)
  HTTPClient http;
  http.begin(client, kOtaApiUrl);
  http.addHeader("User-Agent", "OpenTag-OTA");
  const int code = http.GET();
  String body = code == 200 ? http.getString() : String();
  http.end();

  char url[192] = "";
  char fallback[192] = "";
  int pos = 0;
  while ((pos = body.indexOf("browser_download_url", pos)) >= 0) {
    const int q1 = body.indexOf('"', pos + 20);
    const int q2 = body.indexOf('"', q1 + 1);
    if (q1 < 0 || q2 < 0) break;
    const String asset = body.substring(q1 + 1, q2);
    // Releases carry two firmwares (handheld + mini); this device must
    // flash its own. Prefer the matching name, remember any .bin as a
    // fallback so pre-mini releases (single firmware.bin) still work.
    if (asset.endsWith(".bin") && asset.length() < sizeof(url)) {
      if (!fallback[0]) strlcpy(fallback, asset.c_str(), sizeof(fallback));
      if (asset.indexOf("opentag-mini-firmware-") >= 0) {
        strlcpy(url, asset.c_str(), sizeof(url));
        break;
      }
    }
    pos = q2;
  }
  if (!url[0] && fallback[0]) strlcpy(url, fallback, sizeof(fallback));
  if (!url[0]) {
    setStatus("ERR:ota-no-release");
    return;
  }

  http.begin(client, url);
  if (http.GET() != 200) {
    http.end();
    setStatus("ERR:ota-download");
    return;
  }
  const int total = http.getSize();
  if (total <= 0 || !Update.begin(total)) {
    http.end();
    setStatus("ERR:ota-begin");
    return;
  }
  Update.onProgress(otaProgressCb);
  const size_t written = Update.writeStream(*http.getStreamPtr());
  http.end();
  const bool ok = written > 0 && Update.end(true);
  if (otaCancelled || !ok) {
    setStatus(otaCancelled ? "ERR:ota-cancelled" : "ERR:ota-failed");
    return;
  }
  chirp(2600, 80); delay(140); chirp(2600, 80); delay(140); chirp(2600, 80);
  delay(1000);
  ESP.restart();                                       // boots the new slot
}

// ---------------------------------------------------------------------------
// NFC: note writer, emulation control
// ---------------------------------------------------------------------------

bool writeNtag215Ndef(const char *kind, const char *value) {
  uint8_t page16[16] = {0};
  // READ returns 4 pages starting at page 3; its first 4 bytes are the CC.
  if (!nfc.ntagReadPage(3, page16)) return false;
  if (page16[0] != 0xE1 || page16[2] != 0x3E) return false;  // NTAG215 CC
  uint8_t tlv[220] = {0};
  size_t length = 0;
  if (!makeNdefTlv(kind, value, tlv, sizeof(tlv), &length)) return false;

  const size_t pages = (length + 3) / 4;
  for (size_t p = 0; p < pages; ++p) {
    uint8_t page[4] = {0, 0, 0, 0};
    const size_t start = p * 4;
    const size_t count = min(static_cast<size_t>(4), length - start);
    memcpy(page, tlv + start, count);
    if (!nfc.ntagWritePage(static_cast<uint8_t>(4 + p), page)) return false;
  }
  // Read back every written page and compare before reporting success.
  for (size_t p = 0; p < pages; ++p) {
    uint8_t page16b[16];
    if (!nfc.ntagReadPage(static_cast<uint8_t>(4 + p), page16b)) return false;
    for (size_t b = 0; b < 4; ++b) {
      const size_t pos = p * 4 + b;
      const uint8_t expected = pos < length ? tlv[pos] : 0;
      if (page16b[b] != expected) return false;
    }
  }
  return true;
}

bool startEmulation(const uint8_t *image, const char *label, uint8_t mode, bool v3 = false) {
  if (!nfcReady) return false;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);  // required baseline between modes
  const bool loaded = v3 ? nfc.killerLoadNtagImageV3(0, image)
                         : nfc.killerLoadNtagImage(KILLER_TAG_NTAG_MFU, 0, image);
  if (!loaded) return false;
  if (!nfc.killerSetWorkMode(KILLER_MODE_EMULATOR, KILLER_TAG_NTAG_MFU, 0)) return false;
  emuMode = mode;
  emuSlot = 0;
  emuV3 = v3 && mode == EMU_AMIIBO;
  strlcpy(emuName, label, sizeof(emuName));
  chirp(2600, 60);
  char status[80];
  snprintf(status, sizeof(status), "OK:emulating:%s", label);
  setStatus(status);
  return true;
}

void stopEmulation() {
  if (emuMode == EMU_NONE) return;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);
  emuMode = EMU_NONE;
  emuV3 = false;
  emuSlot = 0;
  emuName[0] = 0;
  setStatus("OK:emulation-stopped");
}

bool startNoteEmulation() {
  uint8_t tlv[220];
  size_t tlvLen = 0;
  if (!makeNdefTlv(pendingKind, pendingValue, tlv, sizeof(tlv), &tlvLen)) return false;
  ntag215::ImageBuilder builder;
  builder.begin(emuUid);
  if (!builder.setNdefTlv(tlv, tlvLen)) return false;
  uint8_t image[540];
  builder.exportImage(image);
  return startEmulation(image, "NDEF note", EMU_NOTE);
}

bool startAmiiboEmulation(const char *name) {
  uint8_t image[ntag215::kV3ImageBytes];
  bool v3 = false;
  if (!loadAmiiboForEmulation(name, image, &v3)) return false;
  return startEmulation(image, name, EMU_AMIIBO, v3);
}

// ---------------------------------------------------------------------------
// Amiibo write-to-tag (TagMo writeToTagAuto sequence)
// ---------------------------------------------------------------------------

enum WriteResult : uint8_t {
  WRES_OK = 0,
  WRES_IO,            // tag vanished mid-write
  WRES_NOT_NTAG215,   // GET_VERSION says this is not an NTAG215
  WRES_LOCKED,        // tag already holds a locked amiibo
  WRES_BAD_DUMP,      // dump did not verify under the loaded keys
  WRES_WRITE,         // page program failed
  WRES_VERIFY,        // readback mismatch
};

static bool tagIsNtag215() {
  const uint8_t getVersion[1] = {0x60};
  uint8_t rx[16];
  size_t rxLen = 0;
  if (!nfc.inDataExchange(getVersion, sizeof(getVersion), rx, &rxLen)) return false;
  return rxLen == 8 && rx[2] == 0x04 && rx[6] == 0x11;  // NTAG215, 504 bytes
}

// Writes the armed, re-signed image to the tag on the antenna. The tag must
// have been detected this loop iteration (it stays selected for a short
// while afterwards, which covers the whole ~2 s blocking flow).
static WriteResult writeArmedImage() {
  uint8_t idPages[16] = {0};
  if (!nfc.ntagReadPage(0, idPages)) return WRES_IO;           // pages 0..3
  if (!tagIsNtag215()) return WRES_NOT_NTAG215;
  // TagMo validateBlankTag: static lock bytes 0F E0 mean "already an amiibo".
  if (idPages[10] == 0x0F && idPages[11] == 0xE0) return WRES_LOCKED;

  uint8_t image[540];
  memcpy(image, writeImage, sizeof(image));
  if (!amiibo::resign(&retailKeys, image, idPages)) return WRES_BAD_DUMP;

  for (uint8_t page = 3; page <= 129; page++) {
    if (!nfc.ntagWritePage(page, &image[page * 4])) return WRES_WRITE;
  }
  // TagMo order: PACK first, then PWD.
  if (!nfc.ntagWritePage(134, &image[134 * 4])) return WRES_WRITE;  // PACK
  if (!nfc.ntagWritePage(133, &image[133 * 4])) return WRES_WRITE;  // PWD

  // Read back and compare pages 3..129 (4-page reads) plus PWD/PACK.
  for (uint8_t page = 3; page <= 127; page += 4) {
    uint8_t rb[16];
    if (!nfc.ntagReadPage(page, rb)) return WRES_VERIFY;
    for (uint8_t i = 0; i < 16; i++) {
      const uint8_t p = page + i;
      if (p > 129) break;
      if (rb[i] != image[p * 4 + i]) return WRES_VERIFY;
    }
  }
  uint8_t rb[16];
  if (!nfc.ntagReadPage(133, rb)) return WRES_VERIFY;
  if (memcmp(rb, &image[133 * 4], 8) != 0) return WRES_VERIFY;  // 133 + 134

  // PWD_AUTH must return the PACK we wrote.
  const uint8_t auth[5] = {0x1B, image[532], image[533], image[534], image[535]};
  uint8_t ack[16];
  size_t ackLen = 0;
  if (!nfc.inDataExchange(auth, sizeof(auth), ack, &ackLen) || ackLen < 2 ||
      ack[0] != 0x80 || ack[1] != 0x80)
    return WRES_VERIFY;
  return WRES_OK;
}

// Irreversible: locks the tag to the amiibo just written (TagMo writeLockInfo
// byte-for-byte). Requires the same tag still on the antenna.
static bool lockWrittenTag() {
  uint8_t idPages[16] = {0};
  if (!nfc.ntagReadPage(0, idPages)) return false;
  const uint8_t page2[4] = {idPages[8], idPages[9], 0x0F, 0xE0};  // static lock
  if (!nfc.ntagWritePage(2, page2)) return false;
  const uint8_t page130[4] = {0x01, 0x00, 0x0F, 0x00};  // dynamic lock
  if (!nfc.ntagWritePage(130, page130)) return false;
  const uint8_t page131[4] = {0x00, 0x00, 0x00, 0x04};  // AUTH0 = 4
  if (!nfc.ntagWritePage(131, page131)) return false;
  const uint8_t page132[4] = {0x5F, 0x00, 0x00, 0x00};  // CFG1
  if (!nfc.ntagWritePage(132, page132)) return false;
  return true;
}

// Arms the write flow: the dump is loaded now; re-signing happens at tap
// time against the tag's own UID. v3 (Air Riders) personalities live on
// NTAG I2C Plus tags that cannot be bought blank, so they are
// emulation-only and never reach the write path (the caller reports
// ERR:not-found, matching the handheld's remote behaviour).
static bool armAmiiboWrite(const char *name) {
  if (!keysReady) return false;
  uint8_t image[ntag215::kV3ImageBytes];
  bool v3 = false;
  if (!loadAmiiboImage(name, image, &v3)) return false;
  if (v3) return false;
  memcpy(writeImage, image, sizeof(writeImage));
  stopEmulation();
  strlcpy(writeName, name, sizeof(writeName));
  writeArmed = true;
  char status[96];
  snprintf(status, sizeof(status), "OK:write-armed:%s", name);
  setStatus(status);
  return true;
}

static void disarmAmiiboWrite() {
  writeArmed = false;
  writeName[0] = '\0';
  memset(writeImage, 0, sizeof(writeImage));
}

// While the note is being emulated, a freshly synced note is pushed into the
// emulator slot so the next tap serves the new content.
void adoptSyncedNoteDuringEmulation() {
  char localValue[241] = {0};
  char localKind[5] = {0};
  bool changed = false;
  portENTER_CRITICAL(&stateMux);
  if (pendingChanged) {
    strlcpy(localValue, pendingValue, sizeof(localValue));
    strlcpy(localKind, pendingKind, sizeof(localKind));
    pendingChanged = false;
    changed = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (!changed) return;
  strlcpy(pendingValue, localValue, sizeof(pendingValue));
  strlcpy(pendingKind, localKind, sizeof(pendingKind));
  prefs.putString("kind", pendingKind);
  prefs.putString("value", pendingValue);
  pendingPresent = strlen(pendingValue) > 0;
  if (emuMode == EMU_NOTE) startNoteEmulation();
}

// ---------------------------------------------------------------------------
// BLE command processing (loop context)
// ---------------------------------------------------------------------------

static void finalizeAmiiboUpload() {
  char name[Amiibo::kNameLen];
  size_t size;
  uint8_t buf[ntag215::kV3ImageBytes];
  portENTER_CRITICAL(&stateMux);
  strlcpy(name, recvName, sizeof(name));
  size = recvSize;
  memcpy(buf, recvBuf, size < sizeof(buf) ? size : sizeof(buf));
  recvActive = false;
  recvComplete = false;
  recvLen = 0;
  recvSeq = 0;
  recvSize = 0;
  portEXIT_CRITICAL(&stateMux);

  if (size < 532) { setStatus("ERR:ami-invalid:short"); return; }
  uint8_t image[540];
  const uint8_t *toWrite = nullptr;
  size_t writeLen = 0;
  if (ntag215::validateV3Dump(buf, size)) {
    toWrite = buf;                          // v3: keep the full page image
    writeLen = ntag215::kV3ImageBytes;
  } else if (ntag215::validateDump(buf, size)) {
    ntag215::normalizeDump(buf, size, image);
    toWrite = image;
    writeLen = sizeof(image);
  } else {
    setStatus("ERR:ami-invalid:dump");
    return;
  }
  char path[64];
  amiiboPath(name, path, sizeof(path));
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) { setStatus("ERR:ami-io"); return; }
  const size_t wrote = f.write(toWrite, writeLen);
  f.close();
  if (wrote != writeLen) {
    LittleFS.remove(path);
    setStatus("ERR:ami-io");
    return;
  }
  char status[80];
  snprintf(status, sizeof(status), "OK:ami-saved:%s", name);
  setStatus(status);
}

void handleAmiiboPut(char *rest) {
  char *colon = strchr(rest, ':');
  if (!colon) { setStatus("ERR:format"); return; }
  *colon = 0;
  const char *sizeStr = colon + 1;
  char *end = nullptr;
  const long size = strtol(sizeStr, &end, 10);
  // v2 containers (532-572 bytes) or a full v3 (Air Riders) page image.
  const bool sizeOk = (size >= 532 && size <= 572) ||
                      size == (long)ntag215::kV3ImageBytes;
  if (end == sizeStr || *end != '\0' || !sizeOk) {
    setStatus("ERR:ami-size");
    return;
  }
  char name[Amiibo::kNameLen];
  if (!sanitizeName(rest, name, sizeof(name))) {
    setStatus("ERR:ami-name");
    return;
  }
  portENTER_CRITICAL(&stateMux);
  recvActive = true;
  recvComplete = false;
  recvLen = 0;
  recvSeq = 0;
  recvAckWanted = false;
  recvSize = (size_t)size;
  recvIsKeys = false;
  strlcpy(recvName, name, sizeof(recvName));
  portEXIT_CRITICAL(&stateMux);
  char status[80];
  snprintf(status, sizeof(status), "OK:ami-put:%s", name);
  setStatus(status);
}

void handleKeyPut(char *rest) {
  char *end = nullptr;
  const long size = strtol(rest, &end, 10);
  if (end == rest || *end != '\0' || (size != 80 && size != 160)) {
    setStatus("ERR:key-size");
    return;
  }
  portENTER_CRITICAL(&stateMux);
  recvActive = true;
  recvComplete = false;
  recvLen = 0;
  recvSeq = 0;
  recvAckWanted = false;
  recvSize = (size_t)size;
  recvIsKeys = true;
  recvName[0] = '\0';
  portEXIT_CRITICAL(&stateMux);
  setStatus("OK:key-put");
}

// One 80-byte key file arrived: identify it by type string and stage it.
static void stageKeyFile(const uint8_t *file) {
  const amiibo::KeyFileKind kind = amiibo::identifyKeyFile(file, amiibo::kKeyFileSize);
  const char *blob = (kind == amiibo::KEY_DATA) ? "keys-data" : "keys-tag";
  prefs.putBytes(blob, file, amiibo::kKeyFileSize);
}

static void finalizeKeyUpload() {
  size_t size;
  uint8_t buf[160];
  portENTER_CRITICAL(&stateMux);
  size = recvSize;
  memcpy(buf, recvBuf, size < sizeof(buf) ? size : sizeof(buf));
  recvActive = false;
  recvComplete = false;
  recvLen = 0;
  recvSeq = 0;
  recvSize = 0;
  portEXIT_CRITICAL(&stateMux);

  if (size == 160) {
    amiibo::AmiiboKeys keys;
    if (!amiibo::loadKeyPair(buf, 80, buf + 80, 80, &keys)) {
      setStatus("ERR:key-invalid");
      return;
    }
    prefs.putBytes("keys-data", &keys.data, amiibo::kKeyFileSize);
    prefs.putBytes("keys-tag", &keys.tag, amiibo::kKeyFileSize);
  } else {  // 80: single file, identified by its type string
    if (amiibo::identifyKeyFile(buf, amiibo::kKeyFileSize) == amiibo::KEY_UNKNOWN) {
      setStatus("ERR:key-invalid");
      return;
    }
    stageKeyFile(buf);
  }
  refreshKeyState();
  setStatus(keysReady ? "OK:keys-saved" : "OK:keys-partial");
}

static bool takeBleCommand(char *out, size_t cap) {
  portENTER_CRITICAL(&stateMux);
  if (!bleCommandPending) { portEXIT_CRITICAL(&stateMux); return false; }
  strlcpy(out, bleCommand, cap);
  bleCommandPending = false;
  portEXIT_CRITICAL(&stateMux);
  return true;
}

void processBleCommands() {
  char cmd[sizeof(bleCommand)];
  if (!takeBleCommand(cmd, sizeof(cmd))) return;

  // The USB drive shares a snapshot of the library; refuse data writes.
  if (usbDriveActive &&
      (strncmp(cmd, "AMI:", 4) == 0 || strncmp(cmd, "KEY:", 4) == 0)) {
    setStatus("ERR:usb-mode");
    return;
  }

  // Wi-Fi credentials for over-the-air updates ("WIFI:<ssid>|<pass>").
  if (strncmp(cmd, "WIFI:", 5) == 0) {
    char *sep = strchr(cmd + 5, '|');
    if (!sep) { setStatus("ERR:format"); return; }
    *sep = 0;
    const size_t sl = strlen(cmd + 5), pl = strlen(sep + 1);
    if (sl == 0 || sl > 32 || pl > 64) { setStatus("ERR:format"); return; }
    prefs.putString("wssid", cmd + 5);
    prefs.putString("wpass", sep + 1);
    setStatus("OK:wifi-saved");
    return;
  }
  if (strcmp(cmd, "OTA:STATUS") == 0) {
    setStatus(prefs.getString("wssid", "").length() ? "OK:ota-creds"
                                                    : "ERR:ota-no-creds");
    return;
  }
  if (strcmp(cmd, "OTA:START") == 0) {
    if (!prefs.getString("wssid", "").length()) { setStatus("ERR:ota-no-creds"); return; }
    if (usbDriveActive) { setStatus("ERR:usb-mode"); return; }
    otaRequested = true;
    setStatus("OK:ota-started");
    return;
  }

  if (strncmp(cmd, "TEXT:", 5) == 0 || strncmp(cmd, "URL:", 4) == 0) {
    const size_t split = strcspn(cmd, ":");
    const char *value = cmd + split + 1;
    const bool isText = split == 4;
    const size_t valueLen = strlen(value);
    if (valueLen == 0 || valueLen > 200 || strpbrk(value, "\n\r")) {
      setStatus("ERR:unsupported-payload");
      return;
    }
    if (!isText && strncmp(value, "https://", 8) != 0 && strncmp(value, "http://", 7) != 0) {
      setStatus("ERR:unsupported-payload");
      return;
    }
    portENTER_CRITICAL(&stateMux);
    memset(pendingValue, 0, sizeof(pendingValue));
    memcpy(pendingValue, value, valueLen);
    strlcpy(pendingKind, isText ? "TEXT" : "URL", sizeof(pendingKind));
    pendingChanged = true;
    portEXIT_CRITICAL(&stateMux);
    setStatus("OK:queued");
    return;
  }

  // Remote counterpart of the handheld's "Clear NDEF note" menu item.
  if (strcmp(cmd, "NOTE:CLEAR") == 0) {
    portENTER_CRITICAL(&stateMux);
    memset(pendingValue, 0, sizeof(pendingValue));
    strlcpy(pendingKind, "TEXT", sizeof(pendingKind));
    pendingChanged = false;
    portEXIT_CRITICAL(&stateMux);
    prefs.remove("kind");
    prefs.remove("value");
    pendingPresent = false;
    setStatus("OK:note-cleared");
    return;
  }

  // Screenless counterparts of the handheld's note menu items: arm the
  // write-on-tap flow (the tap performs it, the answer goes over BLE) or
  // emulate the note.
  if (strcmp(cmd, "NOTE:WRITE") == 0) {
    if (!pendingPresent) { setStatus("ERR:no-note"); return; }
    if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
    stopEmulation();
    armedAction = ARM_NOTE_WRITE;
    setStatus("OK:write-armed:note");
    return;
  }
  if (strcmp(cmd, "NOTE:EMU") == 0) {
    if (!pendingPresent) { setStatus("ERR:no-note"); return; }
    armedAction = ARM_NONE;
    if (!startNoteEmulation()) { setStatus("ERR:emulate"); return; }
    return;   // startEmulation() already reported OK:emulating:…
  }

  // Device identity for the app / web page: model, battery millivolts
  // (0 = no divider fitted) and charge state.
  if (strcmp(cmd, "INFO") == 0) {
    char status[64];
    snprintf(status, sizeof(status), "OK:info:%s:%u:%u", kModel,
             (unsigned)(battVolts > 0.5f ? battVolts * 1000.0f + 0.5f : 0u),
             charging ? 1u : 0u);
    setStatus(status);
    return;
  }

  // USB library drive: dock/eject over BLE (the handheld does it from its
  // menu; a short button press also ejects).
  if (strcmp(cmd, "USB:START") == 0) {
    if (usbDriveActive) { setStatus("ERR:usb-mode"); return; }
    setStatus(enterUsbDrive() ? "OK:usb-started" : "ERR:usb-failed");
    return;
  }
  if (strcmp(cmd, "USB:STOP") == 0) {
    if (!usbDriveActive) { setStatus("ERR:not-usb-mode"); return; }
    exitUsbDrive();
    setStatus("OK:usb-stopped");
    return;
  }

  // Tag identifier: arms the inspector; the tap answer arrives as
  // OK:inspect:<uid>:<chip>:<ndef> (stays armed for retaps until STOP).
  if (strcmp(cmd, "TAG:INSPECT") == 0) {
    if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
    stopEmulation();
    armedAction = ARM_INSPECT;
    setStatus("OK:inspect-armed");
    return;
  }

  if (strncmp(cmd, "AMI:", 4) == 0) {
    char *rest = cmd + 4;
    if (strncmp(rest, "PUT:", 4) == 0) { handleAmiiboPut(rest + 4); return; }
    if (strncmp(rest, "WRITE:", 6) == 0) {
      char name[Amiibo::kNameLen];
      if (!sanitizeName(rest + 6, name, sizeof(name))) { setStatus("ERR:ami-name"); return; }
      if (!keysReady) { setStatus("ERR:keys-needed"); return; }
      if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
      if (!armAmiiboWrite(name)) { setStatus("ERR:not-found"); return; }
      armedAction = ARM_WRITE_TAG;
      return;
    }
    if (strcmp(rest, "ABORT") == 0) {
      portENTER_CRITICAL(&stateMux);
      recvActive = false;
      recvComplete = false;
      recvLen = 0;
      recvSeq = 0;
      portEXIT_CRITICAL(&stateMux);
      if (writeArmed) disarmAmiiboWrite();
      armedAction = ARM_NONE;
      setStatus("OK:ami-abort");
      return;
    }
    if (strcmp(rest, "LIST") == 0) {
      const size_t n = listAmiibos();
      for (size_t i = 0; i < n; ++i) {
        char status[80];
        snprintf(status, sizeof(status), "AMI:%u:%s", (unsigned)i, amiNames[i]);
        setStatus(status);
        delay(30);  // keep notifies ordered and lossless for the client
      }
      char status[32];
      snprintf(status, sizeof(status), "OK:ami-list:%u", (unsigned)n);
      setStatus(status);
      return;
    }
    if (strncmp(rest, "DEL:", 4) == 0) {
      char name[Amiibo::kNameLen];
      if (!sanitizeName(rest + 4, name, sizeof(name))) { setStatus("ERR:ami-name"); return; }
      char path[64];
      amiiboPath(name, path, sizeof(path));
      if (!LittleFS.remove(path)) { setStatus("ERR:not-found"); return; }
      setStatus("OK:ami-deleted");
      return;
    }
    if (strncmp(rest, "EMU:", 4) == 0) {
      char name[Amiibo::kNameLen];
      if (!sanitizeName(rest + 4, name, sizeof(name))) { setStatus("ERR:ami-name"); return; }
      stopEmulation();
      armedAction = ARM_NONE;
      if (nfcReady && startAmiiboEmulation(name)) return;
      setStatus("ERR:ami-emulate");
      return;
    }
    // Screenless counterpart of the library browser's random-UID toggle.
    if (strncmp(rest, "RAND:", 5) == 0) {
      char name[Amiibo::kNameLen];
      if (!sanitizeName(rest + 5, name, sizeof(name))) { setStatus("ERR:ami-name"); return; }
      char path[64];
      amiiboPath(name, path, sizeof(path));
      if (!LittleFS.exists(path)) { setStatus("ERR:not-found"); return; }
      const bool on = !amiiboRandomFlag(name);
      setAmiiboRandomFlag(name, on);
      setStatus(on ? "OK:ami-rand:on" : "OK:ami-rand:off");
      return;
    }
    // Dumps the next amiibo tap into the library (reader mode).
    if (strcmp(rest, "BACKUP") == 0) {
      if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
      stopEmulation();
      armedAction = ARM_BACKUP;
      setStatus("OK:backup-armed");
      return;
    }
    // Locks the tag that was just written (irreversible) — the handheld
    // asks this on its screen; here the phone asks it.
    if (strcmp(rest, "LOCK") == 0) {
      if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
      const bool ok = lockWrittenTag();
      if (ok) chirp(2600, 80);
      setStatus(ok ? "OK:locked" : "ERR:lock-failed");
      return;
    }
    setStatus("ERR:unknown-command");
    return;
  }

  if (strncmp(cmd, "KEY:", 4) == 0) {
    char *rest = cmd + 4;
    if (strncmp(rest, "PUT:", 4) == 0) { handleKeyPut(rest + 4); return; }
    if (strcmp(rest, "STATUS") == 0) {
      refreshKeyState();
      setStatus(keysReady ? "OK:keys:ready" : "OK:keys:none-or-partial");
      return;
    }
    if (strcmp(rest, "CLEAR") == 0) {
      prefs.remove("keys-data");
      prefs.remove("keys-tag");
      memset(&retailKeys, 0, sizeof(retailKeys));
      keysReady = false;
      setStatus("OK:keys-cleared");
      return;
    }
    setStatus("ERR:unknown-command");
    return;
  }

  if (strcmp(cmd, "STOP") == 0) {
    stopEmulation();
    if (writeArmed) disarmAmiiboWrite();
    if (armedAction != ARM_NONE) { armedAction = ARM_NONE; setStatus("OK:cancelled"); }
    return;
  }

  if (strcmp(cmd, "STAT") == 0) {
    char status[96];
    if (emuMode == EMU_AMIIBO) snprintf(status, sizeof(status), "OK:stat:emulating:%s", emuName);
    else if (emuMode == EMU_NOTE) snprintf(status, sizeof(status), "OK:stat:emulating:note");
    else snprintf(status, sizeof(status), "OK:stat:idle");
    setStatus(status);
    return;
  }

  setStatus("ERR:unknown-command");
}

// ---------------------------------------------------------------------------
// BLE service
// ---------------------------------------------------------------------------

static void queueCommand(const std::string& raw) {
  portENTER_CRITICAL(&stateMux);
  if (bleCommandPending) {
    portEXIT_CRITICAL(&stateMux);
    setStatus("ERR:busy");
    return;
  }
  memset(bleCommand, 0, sizeof(bleCommand));
  raw.copy(bleCommand, sizeof(bleCommand) - 1);
  bleCommandPending = true;
  portEXIT_CRITICAL(&stateMux);
}

class CommandCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    const std::string raw = characteristic->getValue();
    if (raw.empty() || raw.size() > sizeof(bleCommand) - 1) {
      setStatus("ERR:format-or-size");
      return;
    }
    noteActivity();   // a synced client is talking: don't auto-sleep under it
    queueCommand(raw);
  }
};

// Binary chunks: [seq_hi][seq_lo][len][payload len bytes]. Reassembled in
// place; every 8th chunk is echoed back so the sender can pace itself.
class DataCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    const std::string raw = characteristic->getValue();
    if (raw.size() < 4 || raw.size() > 247) return;
    const uint16_t seq = (uint8_t)raw[0] << 8 | (uint8_t)raw[1];
    const uint8_t plen = (uint8_t)raw[2];
    if ((size_t)plen + 3 != raw.size()) return;
    noteActivity();   // transfer in flight: never mid-upload
    portENTER_CRITICAL(&stateMux);
    if (!recvActive || recvComplete) {
      portEXIT_CRITICAL(&stateMux);
      setStatus("ERR:ami-inactive");
      return;
    }
    if (seq != recvSeq || recvLen + plen > recvSize) {
      recvActive = false;
      portEXIT_CRITICAL(&stateMux);
      setStatus("ERR:ami-seq");
      return;
    }
    memcpy(recvBuf + recvLen, raw.data() + 3, plen);
    recvLen += plen;
    recvSeq++;
    if (recvSeq % 8 == 0) { recvAckPending = seq; recvAckWanted = true; }
    if (recvLen == recvSize) recvComplete = true;
    portEXIT_CRITICAL(&stateMux);
  }
};

// Just-works pairing: the Mini has no screen, so there is no passkey to
// show. The bond is still stored (reconnects are silent) and the link still
// uses LE secure connections; only the MITM numeric step is gone.
class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &ci) override {
    if (ci.isBonded()) {
      bleConnected = true;
    } else {
      bleConnected = false;
      NimBLEDevice::startSecurity(ci.getConnHandle());
    }
  }
  void onAuthenticationComplete(NimBLEConnInfo &ci) override {
    if (ci.isBonded()) {
      bleConnected = true;
      chirp(2400, 80);   // the Mini's "paired" screen
    } else {
      NimBLEDevice::getServer()->disconnect(ci.getConnHandle());
    }
  }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    if (!travelMode) NimBLEDevice::startAdvertising();
  }
};

void beginBle() {
  NimBLEDevice::init("OpenTag Mini");
  NimBLEDevice::setMTU(247);
  // Pairing: bond + secure connections, but no MITM passkey — the Mini has
  // no display, so the phone pairs "just works" with no code to enter.
  // Bonded phones reconnect silently. Factory-flashing clears the bonds.
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(3);   // NoInputNoOutput: just works
  NimBLEServer *server = NimBLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());
  NimBLEService *service = server->createService(BleIds::SERVICE);
  NimBLECharacteristic *writeValue = service->createCharacteristic(
      BleIds::WRITE_VALUE, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  statusCharacteristic = service->createCharacteristic(
      BleIds::STATUS, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  NimBLECharacteristic *data = service->createCharacteristic(
      BleIds::DATA, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR);
  writeValue->setCallbacks(new CommandCallbacks());
  data->setCallbacks(new DataCallbacks());
  statusCharacteristic->setValue("READY");
  service->start();
  if (!travelMode) {
    NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
    advertising->addServiceUUID(BleIds::SERVICE);
    advertising->start();
  }
}

// ---------------------------------------------------------------------------
// Soft power: hold the button
// ---------------------------------------------------------------------------

// "Off" is deep sleep: the SoC draws microamps. OK (GPIO1, an RTC pin) is
// the wake button; the RTC pullup holds the line high while the chip sleeps.
void enterDeepSleep() {
  rtc_gpio_pullup_en(GPIO_NUM_1);
  rtc_gpio_pulldown_dis(GPIO_NUM_1);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0);   // wake on OK, active low
  esp_deep_sleep_start();                        // no return
}

static void powerOff() {
  chirp(1800, 150);
  if (usbDriveActive) exitUsbDrive();   // eject cleanly before sleeping
  delay(200);
  enterDeepSleep();
}

// Triple press: BLE advertising off/on (travel mode). An existing connection
// is left alone; the change applies the next time it drops.
static void toggleTravelMode() {
  travelMode = !travelMode;
  prefs.putBool("travel", travelMode);
  if (travelMode) {
    NimBLEDevice::stopAdvertising();
    chirp(1200, 200);
  } else {
    NimBLEDevice::startAdvertising();
    chirp(2400, 80); delay(120); chirp(2400, 80);
  }
}

// The single button: short press = battery chirps, triple press = travel
// mode, hold 1.5 s = power off. (Short press also ejects the USB drive.)
static void pollButton() {
  static bool lastRead = HIGH, stable = HIGH, holdFired = false;
  static unsigned long changedAt = 0, pressStart = 0, windowStart = 0;
  static uint8_t presses = 0;
  const unsigned long now = millis();
  const bool reading = digitalRead(Pins::BUTTON_OK);
  if (reading != lastRead) { lastRead = reading; changedAt = now; }
  if (now - changedAt > 28 && reading != stable) {
    stable = reading;
    if (stable == LOW) {           // press edge
      noteActivity();
      pressStart = now;
      holdFired = false;
      if (presses == 0) windowStart = now;
      presses++;
    }
    // release edge: evaluated when the multi-press window expires
  }
  if (stable == LOW && !holdFired && presses && now - pressStart >= 1500) {
    holdFired = true;              // long hold: soft power off
    powerOff();
    return;
  }
  if (presses && stable == HIGH && now - windowStart > 700) {
    const uint8_t n = presses;
    presses = 0;
    if (usbDriveActive) {
      exitUsbDrive();
      setStatus("OK:usb-stopped");
      return;
    }
    if (n >= 3) toggleTravelMode();
    else announceBattery();
  }
}

// ---------------------------------------------------------------------------
// Armed tap actions: the Mini's replacement for the handheld's screen flows
// ---------------------------------------------------------------------------

static void runArmedAction() {
  uint8_t uid[7] = {0};
  uint8_t uidLen = 0;
  if (!nfc.readPassiveTarget(uid, &uidLen, 60)) return;
  noteActivity();   // a tag landing is user activity too

  switch (armedAction) {
    case ARM_NOTE_WRITE: {
      armedAction = ARM_NONE;
      uint8_t page16[16] = {0};
      if (!nfc.ntagReadPage(3, page16) || page16[0] != 0xE1 || page16[2] != 0x3E) {
        setStatus("ERR:write-failed-or-not-NTAG215");
        chirp(1400, 250);
        return;
      }
      const bool ok = writeNtag215Ndef(pendingKind, pendingValue);
      if (ok) chirp(2400, 80);
      else chirp(1400, 250);
      setStatus(ok ? "OK:written-and-verified" : "ERR:write-failed-or-not-NTAG215");
      return;
    }
    case ARM_BACKUP:
      armedAction = ARM_NONE;
      runTagBackup();
      return;
    case ARM_INSPECT:
      // stays armed: retap to rescan, exactly like the handheld's screen
      inspectTag(uid, uidLen);
      return;
    case ARM_WRITE_TAG: {
      armedAction = ARM_NONE;
      const WriteResult result = writeArmedImage();
      disarmAmiiboWrite();
      switch (result) {
        case WRES_OK:
          chirp(2400, 80);
          setStatus("OK:written-and-verified");
          break;
        case WRES_IO:
          setStatus("ERR:write-tag-lost"); chirp(1400, 250);
          break;
        case WRES_NOT_NTAG215:
          setStatus("ERR:write-failed-or-not-NTAG215"); chirp(1400, 250);
          break;
        case WRES_LOCKED:
          setStatus("ERR:write-tag-locked"); chirp(1400, 250);
          break;
        case WRES_BAD_DUMP:
          setStatus("ERR:write-dump"); chirp(1400, 250);
          break;
        case WRES_WRITE:
          setStatus("ERR:write-failed"); chirp(1400, 250);
          break;
        default:
          setStatus("ERR:write-verify"); chirp(1400, 250);
          break;
      }
      return;
    }
    default:
      armedAction = ARM_NONE;
      return;
  }
}

// ---------------------------------------------------------------------------
// Setup / loop
// ---------------------------------------------------------------------------

void loadPending() {
  prefs.begin("opentag", false);
  const String kind = prefs.getString("kind", "");
  const String value = prefs.getString("value", "");
  if (kind.length() && value.length()) {
    strlcpy(pendingKind, kind.c_str(), sizeof(pendingKind));
    strlcpy(pendingValue, value.c_str(), sizeof(pendingValue));
    pendingPresent = true;
  }
  refreshKeyState();
}

void setup() {
  Serial.begin(115200);
  pinMode(Pins::BUTTON_OK, INPUT_PULLUP);
  pinMode(Pins::CHG_SENSE, INPUT_PULLUP);   // unwired reads HIGH = no charge
  noteActivity();   // start the auto-sleep clock at boot

  // Soft power-on: if OK just woke us from deep sleep, it must be held for
  // a full second to boot — a short tap goes straight back to sleep.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    const uint32_t t0 = millis();
    while (digitalRead(Pins::BUTTON_OK) == LOW && millis() - t0 < 1000) delay(10);
    if (digitalRead(Pins::BUTTON_OK) == HIGH) enterDeepSleep();   // released early
    while (digitalRead(Pins::BUTTON_OK) == LOW) delay(10);        // wait for release
  }

  LittleFS.begin(true);
  LittleFS.mkdir(Amiibo::kDir);

  nfcSerial.begin(115200, SERIAL_8N1, Pins::NFC_RX, Pins::NFC_TX);
  nfcReady = nfc.begin(nfcSerial);
  if (nfcReady) {
    nfc.samConfig();
    nfc.setPassiveActivationRetries(2);
  }

  loadPending();
  travelMode = prefs.getBool("travel", false);
  beginBle();
  chirp(2600, 60);   // booted: one bright chirp
}

void loop() {
  pollButton();

  // Auto-sleep: idle for 3 minutes while nothing is connected -> the same
  // power-off as holding the button (a connected client keeps it awake).
  if (!bleConnected && millis() - lastActivityMs > kAutoSleepMs) {
    powerOff();
    return;
  }

  // Battery telemetry + low-battery protection (optional divider). While
  // charging the terminal voltage rises, so the low checks are suspended.
  if (millis() - battPollMs >= 2000) {
    battPollMs = millis();
    pollBattery();
    if (battVolts > 0.5f && !charging) {
      if (battVolts < 3.35f) {
        if (battLowSinceMs == 0) {
          battLowSinceMs = millis();
          chirp(1200, 200);
        } else if (battVolts < 3.25f && millis() - battLowSinceMs > 30000) {
          powerOff();        // protect the cell: it is nearly empty
          return;
        }
      } else {
        battLowSinceMs = 0;
      }
    } else {
      battLowSinceMs = 0;
    }
  }

  // Over-the-air update requested from the app/web page (blocks until done).
  if (otaRequested) {
    otaRequested = false;
    runOtaUpdate();
    return;
  }

  // Charge sense: two matching 250 ms samples flip the state; a rising
  // edge chirps (the Mini's charging bolt).
  if (millis() - chgPollMs >= 250) {
    chgPollMs = millis();
    const bool nowCharging = digitalRead(Pins::CHG_SENSE) == LOW;
    if (nowCharging != charging) {
      if (++chgDebounce >= 2) {
        chgDebounce = 0;
        charging = nowCharging;
        if (charging) chirp(2000, 60);
      }
    } else {
      chgDebounce = 0;
    }
  }

  // Transfer chunk ACKs (pacing for the client).
  if (recvAckWanted) {
    portENTER_CRITICAL(&stateMux);
    const uint16_t ack = recvAckPending;
    recvAckWanted = false;
    portEXIT_CRITICAL(&stateMux);
    char status[32];
    snprintf(status, sizeof(status), "OK:ami-chunk:%u", (unsigned)ack);
    setStatus(status);
  }

  // Finished a binary transfer: store the dump or the key files.
  bool complete = false;
  bool completeIsKeys = false;
  portENTER_CRITICAL(&stateMux);
  if (recvComplete) {
    complete = true;
    completeIsKeys = recvIsKeys;
    recvComplete = false;
  }
  portEXIT_CRITICAL(&stateMux);
  if (complete) {
    if (completeIsKeys) finalizeKeyUpload();
    else finalizeAmiiboUpload();
  }

  processBleCommands();
  adoptSyncedNoteDuringEmulation();

  // Note adoption while idle (keeps the stored note in sync).
  bool noteChanged = false;
  char localValue[241] = {0};
  char localKind[5] = {0};
  portENTER_CRITICAL(&stateMux);
  if (pendingChanged) {
    strlcpy(localValue, pendingValue, sizeof(localValue));
    strlcpy(localKind, pendingKind, sizeof(localKind));
    pendingChanged = false;
    noteChanged = true;
  }
  portEXIT_CRITICAL(&stateMux);
  if (noteChanged && emuMode == EMU_NONE) {
    strlcpy(pendingValue, localValue, sizeof(pendingValue));
    strlcpy(pendingKind, localKind, sizeof(localKind));
    prefs.putString("kind", pendingKind);
    prefs.putString("value", pendingValue);
    pendingPresent = strlen(pendingValue) > 0;
    chirp(1800, 50);   // "note synced" without a screen
  }

  // Armed tap action: poll for a tag on the antenna and run it.
  if (armedAction != ARM_NONE && nfcReady && millis() - lastNfcPoll > 120) {
    lastNfcPoll = millis();
    runArmedAction();
  }

  delay(4);
}
