#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <cstring>
#include <string>

#include "ndef_tlv.h"
#include "ntag215_emu.h"
#include "pn532_uart.h"
#include "amiibo_crypto.h"
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
// OpenTag device firmware: ESP32-S3 DevKitC-1 + SSD1351 OLED + PN532Killer
// (ST25R3916-class NFC emulator) on UART2.
//
// Features:
//   * Write the BLE-synced NDEF note onto a real NTAG215 tag (reader mode).
//   * Emulate the NDEF note as a virtual NTAG215 (Killer emulator slot 0).
//   * Amiibo library: upload .bin dumps of your own amiibo tags over BLE
//     (as a .zip via the web page or one-by-one), browse on the OLED and
//     emulate any of them by presenting the dump's own 7-byte UID.
//   * Four-button UI: UP/DOWN navigate, OK selects, BACK cancels or exits
//     (BACK = GPIO13 on J1-19, the repurposed SPI MISO pin).
//   * BLE service with a text command channel and a binary chunk channel.
//
// All tag data is user-supplied at runtime; nothing is bundled here.
namespace Pins {
constexpr int SCK = 12, MOSI = 11;  // no MISO: see BUTTON_BACK below
constexpr int OLED_CS = 10, OLED_DC = 9, OLED_RST = 8;
constexpr int NFC_TX = 17, NFC_RX = 18;  // ESP32 TX -> Killer J4 RX
// UP/DOWN/OK sit on the J3 header row (board-east side of the carrier PCB).
// BACK sits on J1-19 = GPIO13, the SPI MISO pin: the OLED bus is write-only,
// so MISO is unused and the pin is repurposed as a button input instead.
constexpr int BUTTON_UP = 42, BUTTON_DOWN = 2, BUTTON_OK = 1, BUTTON_BACK = 13;
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

Adafruit_SSD1351 oled(128, 128, &SPI, Pins::OLED_CS, Pins::OLED_DC, Pins::OLED_RST);
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
static bool waitingForTag = false;
static bool tagReady = false;
static uint8_t menuIndex = 0;
static String uiMessage;

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
static uint8_t recvBuf[ntag215::kV3ImageBytes];  // v2 containers (540 + slack) or a v3 page image
static bool recvIsKeys = false;     // transfer carries key files, not a dump

// ---- amiibo signing keys (user-supplied, stored in NVS, never bundled) ----
static amiibo::AmiiboKeys retailKeys;
static bool keysReady = false;
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
static unsigned long lockAskedAt = 0;

// ---- emulation state ----
enum EmuMode : uint8_t { EMU_NONE = 0, EMU_NOTE = 1, EMU_AMIIBO = 2 };
static uint8_t emuMode = EMU_NONE;
static char emuName[Amiibo::kNameLen] = "";
static uint8_t emuUid[7] = {0x04, 0, 0, 0, 0, 0, 0};
static bool emuV3 = false;          // emulating a v3 (Air Riders) personality
static uint8_t emuSlot = 0;        // live Killer emulator slot (0-7, rotates)

// ---- amiibo library browser ----
static char amiNames[Amiibo::kMaxFiles][Amiibo::kNameLen];
static bool amiV3[Amiibo::kMaxFiles];   // entry is a v3 (1024+ byte) dump
static size_t amiCount = 0;
static size_t amiSelected = 0;
static size_t amiTop = 0;

enum UiState : uint8_t { UI_MENU, UI_WAIT_TAG, UI_TAG_READY, UI_LIBRARY, UI_EMULATE,
                         UI_WRITE_TAG, UI_LOCK_CONFIRM, UI_POWEROFF };
static uint8_t uiState = UI_MENU;

static unsigned long lastDraw = 0;
static unsigned long lastNfcPoll = 0;

static const uint16_t BG = 0x1082, MINT = 0xB7EF, LAVENDER = 0xC5DF;
static const uint16_t WHITE = 0xFFFF, MUTED = 0x9CF3, RED = 0xF986;

void setStatus(const char *value) {
  if (statusCharacteristic) { statusCharacteristic->setValue(value); statusCharacteristic->notify(); }
}

struct Button {
  int pin;
  bool lastStable = HIGH;
  bool lastRead = HIGH;
  unsigned long changedAt = 0;
  unsigned long pressStart = 0;
  unsigned long pressDur = 0;
  bool heldFired = false;
  bool pressEdge = false;
  bool releaseEdge = false;
  explicit Button(int pinNumber) : pin(pinNumber) {}
  void update() {
    const bool reading = digitalRead(pin);
    if (reading != lastRead) { lastRead = reading; changedAt = millis(); }
    if (millis() - changedAt > 28 && reading != lastStable) {
      lastStable = reading;
      if (lastStable == LOW) {
        pressStart = millis();
        heldFired = false;
        pressEdge = true;
      } else {
        pressDur = millis() - pressStart;
        releaseEdge = true;
      }
    }
    if (lastStable == HIGH) heldFired = false;
  }
  bool pressed() {  // fires once per press, on the debounced press edge
    update();
    const bool edge = pressEdge;
    pressEdge = false;
    return edge;
  }
  // True once when the button has been held down for `ms` (long press).
  bool held(unsigned long ms) {
    update();
    if (lastStable == LOW && !heldFired && millis() - pressStart >= ms) {
      heldFired = true;
      return true;
    }
    return false;
  }
  // True on release when the press lasted less than `maxHoldMs`. A release
  // edge older than 250 ms (never polled because the UI state changed while
  // the button was down) is ignored.
  bool shortPress(unsigned long maxHoldMs) {
    update();
    if (!releaseEdge) return false;
    releaseEdge = false;
    if (millis() - changedAt > 250) return false;  // stale edge
    return pressDur < maxHoldMs;
  }
};
Button btnUp(Pins::BUTTON_UP), btnDown(Pins::BUTTON_DOWN), btnOk(Pins::BUTTON_OK),
       btnBack(Pins::BUTTON_BACK);

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

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override { bleConnected = true; }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

void beginBle() {
  NimBLEDevice::init("OpenTag");
  NimBLEDevice::setMTU(247);
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
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BleIds::SERVICE);
  advertising->start();
}

// ---------------------------------------------------------------------------
// Storage
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

// Library display name: strips ".bin", turns - _ . into spaces and
// capitalizes the first letter of every word — "kirby-air-riders.bin"
// shows as "Kirby Air Riders", "Allycomplete(Air Riders).bin" keeps its
// parentheses. Only the display changes; files keep their real names.
static void formatAmiiboName(const char *in, char *out, size_t cap) {
  size_t len = strlen(in);
  if (len > 4 && strcasecmp(in + len - 4, ".bin") == 0) len -= 4;
  size_t o = 0;
  bool wordStart = true;
  for (size_t i = 0; i < len && o + 1 < cap; i++) {
    char c = in[i];
    if (c == '-' || c == '_' || c == '.') {
      if (o && out[o - 1] != ' ') out[o++] = ' ';
      wordStart = true;
      continue;
    }
    if (wordStart && c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    out[o++] = c;
    wordStart = c == '(' || c == ' ';
  }
  while (o && out[o - 1] == ' ') o--;
  out[o] = '\0';
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
  // v3 (Air Riders) dumps are stored as 1024-byte page images, v2 files are
  // always 540 bytes — size is the format tell. Checked in sorted order so
  // the flags line up with the names after the qsort.
  char path[64];
  for (size_t i = 0; i < amiCount; i++) {
    amiiboPath(amiNames[i], path, sizeof(path));
    File f = LittleFS.open(path, "r");
    amiV3[i] = f && f.size() >= ntag215::kV3ImageBytes;
    if (f) f.close();
  }
  return amiCount;
}

// Reads /amiibo/<name>.bin, validates it and normalizes it: v2 dumps become
// 540-byte NTAG215 page images; v3 (Air Riders generation) dumps are kept as
// their full 1024-byte page image with *isV3 set.
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
  // Nothing meaningful may follow the tag image in the file.
  if (got > 572) return false;
  if (!ntag215::validateDump(buf, got)) return false;
  ntag215::normalizeDump(buf, got, image);
  memcpy(emuUid, image, 7);
  return true;
}

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawHeader() {
  oled.setTextColor(MINT); oled.setCursor(8, 7); oled.print("OPENTAG");
  oled.setTextColor(MUTED); oled.setCursor(84, 7); oled.print(bleConnected ? "BLE ON" : "BLE");
  oled.drawFastHLine(8, 21, 112, 0x4208);
}

void drawScreen(bool force = false) {
  if (!force && millis() - lastDraw < 180) return;
  lastDraw = millis();
  oled.fillScreen(BG);
  oled.setTextWrap(false);
  oled.setTextSize(1);
  drawHeader();

  if (uiState == UI_WAIT_TAG) {
    oled.setTextColor(WHITE); oled.setCursor(8, 37); oled.print("Ready to write");
    oled.setTextColor(MUTED); oled.setCursor(8, 55); oled.print("Tap an NTAG215 tag");
    oled.setCursor(8, 70); oled.print("to the NFC antenna.");
    oled.setTextColor(MINT); oled.setCursor(8, 105); oled.print("BACK: cancel");
  } else if (uiState == UI_TAG_READY) {
    oled.setTextColor(MINT); oled.setCursor(8, 35); oled.print("NTAG215 detected");
    oled.setTextColor(WHITE); oled.setCursor(8, 55); oled.print("OK to write NDEF");
    oled.setTextColor(MUTED); oled.setCursor(8, 73); oled.print(pendingKind);
    oled.setCursor(8, 87); oled.print(String(pendingValue).substring(0, 18));
    oled.setCursor(8, 99); oled.print("BACK: cancel");
    oled.setTextColor(RED); oled.setCursor(8, 111); oled.print("Existing data replaced");
  } else if (uiState == UI_LIBRARY) {
    oled.setTextColor(MINT); oled.setCursor(8, 27); oled.print("AMIIBO LIBRARY");
    if (amiCount == 0) {
      oled.setTextColor(MUTED); oled.setCursor(8, 50); oled.print("Empty.");
      oled.setCursor(8, 66); oled.print("Upload a .zip of your");
      oled.setCursor(8, 80); oled.print("dumps via BLE sync.");
      oled.setCursor(8, 96); oled.print("BACK: main menu");
    } else {
      const size_t visible = amiCount < 5 ? amiCount : 5;
      if (amiSelected < amiTop) amiTop = amiSelected;
      if (amiSelected >= amiTop + visible) amiTop = amiSelected - visible + 1;
      for (size_t i = 0; i < visible; ++i) {
        const size_t idx = amiTop + i;
        const int y = 34 + (int)i * 14;
        oled.setTextColor(idx == amiSelected ? MINT : MUTED);
        oled.setCursor(8, y); oled.print(idx == amiSelected ? "> " : "  ");
        char pretty[Amiibo::kNameLen];
        formatAmiiboName(amiNames[idx], pretty, sizeof(pretty));
        oled.print(String(pretty).substring(0, amiV3[idx] ? 13 : 16));
        if (amiV3[idx]) oled.print(" V3");
      }
      // The hint line doubles as the message line so feedback (missing keys,
      // V3 write refusal, absent Killer) is actually visible here.
      if (uiMessage.length()) {
        oled.setTextColor(LAVENDER); oled.setCursor(8, 101);
        oled.print(uiMessage.substring(0, 19));
      } else {
        oled.setTextColor(MUTED); oled.setCursor(8, 101);
        oled.print("OK:emul hold:write");
      }
      oled.setCursor(8, 113);
      oled.print((int)(amiSelected + 1)); oled.print('/'); oled.print((int)amiCount);
      oled.print(" BACK:menu");
    }
  } else if (uiState == UI_WRITE_TAG) {
    oled.setTextColor(MINT); oled.setCursor(8, 30); oled.print("WRITE AMIIBO");
    oled.setTextColor(WHITE); oled.setCursor(8, 48);
    {
      char pretty[Amiibo::kNameLen];
      formatAmiiboName(writeName, pretty, sizeof(pretty));
      oled.print(String(pretty).substring(0, 16));
    }
    oled.setTextColor(MUTED); oled.setCursor(8, 66); oled.print("Tap a BLANK NTAG215");
    oled.setCursor(8, 80); oled.print("to the NFC antenna.");
    oled.setTextColor(RED); oled.setCursor(8, 100); oled.print("Overwrites the tag");
    oled.setTextColor(MINT); oled.setCursor(8, 116);
    oled.print(uiMessage.length() ? uiMessage.substring(0, 19) : "BACK: cancel");
  } else if (uiState == UI_LOCK_CONFIRM) {
    oled.setTextColor(MINT); oled.setCursor(8, 26); oled.print("WRITE VERIFIED");
    oled.setTextColor(WHITE); oled.setCursor(8, 44); oled.print("Lock this tag?");
    oled.setTextColor(RED); oled.setCursor(8, 60); oled.print("IRREVERSIBLE!");
    oled.setTextColor(MUTED); oled.setCursor(8, 80); oled.print("HOLD OK: lock");
    oled.setCursor(8, 96); oled.print("BACK/short OK: skip");
    oled.setCursor(8, 112); oled.print("(auto-skips in 15 s)");
  } else if (uiState == UI_EMULATE) {
    oled.setTextColor(MINT); oled.setCursor(8, 32); oled.print("EMULATING");
    oled.setTextColor(WHITE); oled.setCursor(8, 48);
    {
      char pretty[Amiibo::kNameLen];
      formatAmiiboName(emuName, pretty, sizeof(pretty));
      oled.print(String(pretty).substring(0, 16));
    }
    oled.setTextColor(MUTED); oled.setCursor(8, 66); oled.print("UID ");
    for (int i = 0; i < 7; ++i) {
      if (i) oled.print(':');
      if (emuUid[i] < 0x10) oled.print('0');
      oled.print(emuUid[i], HEX);
    }
    oled.setCursor(8, 82); oled.print(emuMode == EMU_NOTE ? "NDEF note" : "Tap console to use");
    oled.setTextColor(LAVENDER); oled.setCursor(8, 100);
    {
      char line[24];
      snprintf(line, sizeof(line), emuV3 ? "V3 tag on slot %d" : "Killer slot %d active",
               (int)emuSlot);
      oled.print(line);
    }
    if (uiMessage.length()) {
      oled.setTextColor(LAVENDER); oled.setCursor(8, 108); oled.print(uiMessage.substring(0, 19));
    }
    oled.setTextColor(MINT); oled.setCursor(8, 116);
    oled.print(emuMode == EMU_AMIIBO && amiCount > 1 ? "UP/DN swap OK:stop" : "BACK: stop");
  } else if (uiState == UI_POWEROFF) {
    oled.setTextColor(WHITE); oled.setCursor(8, 52); oled.print("Powering off");
    oled.setTextColor(MINT); oled.setCursor(8, 76); oled.print("BACK: cancel");
  } else {
    const char *items[] = {"Amiibo library", "BLE sync", "Write NDEF note",
                           "Emulate NDEF note", "Clear NDEF note"};
    for (int i = 0; i < 5; ++i) {
      const int y = 28 + i * 16;
      oled.setTextColor(i == menuIndex ? MINT : MUTED);
      oled.setCursor(8, y); oled.print(i == menuIndex ? "> " : "  "); oled.print(items[i]);
    }
    oled.setTextColor(WHITE); oled.setCursor(8, 116);
    if (pendingPresent) { oled.print(pendingKind); oled.print(": "); oled.print(String(pendingValue).substring(0, 12)); }
    else oled.print("Hold OK: power off");
    if (uiMessage.length()) {
      oled.setTextColor(LAVENDER); oled.setCursor(8, 108); oled.print(uiMessage.substring(0, 19));
    }
  }
}

// ---------------------------------------------------------------------------
// Soft power: long-press OK
// ---------------------------------------------------------------------------

// "Off" is deep sleep: the OLED panel is parked and the SoC draws microamps.
// The DevKitC's LDO/USB chip and the MT3608 boost still leak a few mA, so
// the slide switch remains the true battery disconnect for storage — this
// is a convenience off. OK (GPIO1, an RTC pin) is the wake button; the PCB
// has no external pullups, so the RTC pullup must hold the line high while
// the chip sleeps (the normal weak pullup is off in deep sleep).
void enterDeepSleep() {
  rtc_gpio_pullup_en(GPIO_NUM_1);
  rtc_gpio_pulldown_dis(GPIO_NUM_1);
  esp_sleep_enable_ext0_wakeup(GPIO_NUM_1, 0);   // wake on OK, active low
  esp_deep_sleep_start();                        // no return
}

void powerOffFromMenu() {
  uiState = UI_POWEROFF;
  drawScreen(true);
  const uint32_t t0 = millis();                  // short BACK-cancel window
  while (millis() - t0 < 700) {
    if (btnBack.pressed()) {
      uiState = UI_MENU;
      uiMessage = "Power off cancelled";
      drawScreen(true);
      return;
    }
    delay(10);
  }
  oled.enableDisplay(false);   // park panel + charge pump (3V3 stays live)
  enterDeepSleep();
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
    if (!nfc.ntagReadPage(static_cast<uint8_t>(4 + p), page16)) return false;
    for (size_t b = 0; b < 4; ++b) {
      const size_t pos = p * 4 + b;
      const uint8_t expected = pos < length ? tlv[pos] : 0;
      if (page16[b] != expected) return false;
    }
  }
  return true;
}

bool startEmulation(const uint8_t *image, const char *label, uint8_t mode, bool v3 = false) {
  if (!nfcReady) return false;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);  // required baseline between modes
  // v3 (Air Riders) personalities span pages 0x00-0xFF; the load only
  // succeeds if the Killer firmware accepts the extended page range.
  const bool loaded = v3 ? nfc.killerLoadNtagImageV3(0, image)
                         : nfc.killerLoadNtagImage(KILLER_TAG_NTAG_MFU, 0, image);
  if (!loaded) return false;
  if (!nfc.killerSetWorkMode(KILLER_MODE_EMULATOR, KILLER_TAG_NTAG_MFU, 0)) return false;
  emuMode = mode;
  emuSlot = 0;
  emuV3 = v3 && mode == EMU_AMIIBO;
  strlcpy(emuName, label, sizeof(emuName));
  uiState = UI_EMULATE;
  char status[80];
  snprintf(status, sizeof(status), "OK:emulating:%s", label);
  setStatus(status);
  drawScreen(true);
  return true;
}

// Hot-swap while emulating: UP/DOWN in the EMULATE screen loads the next
// library entry into the ROTATING slot (never the one being presented) and
// switches presentation, so consoles can be tapped back-to-back without
// stopping the emulator. The Killer has 8 slots; OpenTag cycles through
// them all so a swap never rewrites a slot a console might still be polling.
bool emuHotSwap(const uint8_t *image, const char *label, bool v3) {
  const uint8_t slot = (emuSlot + 1) & 7;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);  // required baseline between modes
  const bool loaded = v3 ? nfc.killerLoadNtagImageV3(slot, image)
                         : nfc.killerLoadNtagImage(KILLER_TAG_NTAG_MFU, slot, image);
  if (!loaded) return false;
  if (!nfc.killerSetWorkMode(KILLER_MODE_EMULATOR, KILLER_TAG_NTAG_MFU, slot)) return false;
  emuSlot = slot;
  emuV3 = v3;
  strlcpy(emuName, label, sizeof(emuName));
  char status[80];
  snprintf(status, sizeof(status), "OK:emulating:%s", label);
  setStatus(status);
  drawScreen(true);
  return true;
}

void stopEmulation() {
  if (emuMode == EMU_NONE) return;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);
  emuMode = EMU_NONE;
  emuV3 = false;
  emuSlot = 0;
  emuName[0] = 0;
  uiState = UI_MENU;
  setStatus("OK:emulation-stopped");
  drawScreen(true);
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
  if (!loadAmiiboImage(name, image, &v3)) return false;
  if (!startEmulation(image, name, EMU_AMIIBO, v3)) {
    if (v3) uiMessage = "Killer fw lacks V3";  // extended pages rejected
    return false;
  }
  return true;
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
// emulation-only and never reach the write path.
static bool armAmiiboWrite(const char *name) {
  if (!keysReady) return false;
  uint8_t image[ntag215::kV3ImageBytes];
  bool v3 = false;
  if (!loadAmiiboImage(name, image, &v3)) return false;
  if (v3) {
    uiMessage = "V3: emulation only";
    drawScreen(true);
    return false;
  }
  memcpy(writeImage, image, sizeof(writeImage));
  stopEmulation();
  strlcpy(writeName, name, sizeof(writeName));
  writeArmed = true;
  uiState = UI_WRITE_TAG;
  uiMessage = "";
  char status[96];
  snprintf(status, sizeof(status), "OK:write-armed:%s", name);
  setStatus(status);
  drawScreen(true);
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
  uiMessage = "Amiibo synced";
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
  if (keysReady) {
    setStatus("OK:keys-saved");
    uiMessage = "Keys loaded";
  } else {
    setStatus("OK:keys-partial");
    uiMessage = "Need both key files";
  }
  if (uiState == UI_MENU) drawScreen(true);
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

  // Remote counterpart of the on-device "Clear NDEF note" menu item, so the
  // web page's clear button actually clears instead of giving advice.
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

  if (strncmp(cmd, "AMI:", 4) == 0) {
    char *rest = cmd + 4;
    if (strncmp(rest, "PUT:", 4) == 0) { handleAmiiboPut(rest + 4); return; }
    if (strncmp(rest, "WRITE:", 6) == 0) {
      char name[Amiibo::kNameLen];
      if (!sanitizeName(rest + 6, name, sizeof(name))) { setStatus("ERR:ami-name"); return; }
      if (!keysReady) { setStatus("ERR:keys-needed"); return; }
      if (!nfcReady) { setStatus("ERR:nfc-absent"); return; }
      if (!armAmiiboWrite(name)) { setStatus("ERR:not-found"); return; }
      return;
    }
    if (strcmp(rest, "ABORT") == 0) {
      portENTER_CRITICAL(&stateMux);
      recvActive = false;
      recvComplete = false;
      recvLen = 0;
      recvSeq = 0;
      portEXIT_CRITICAL(&stateMux);
      if (writeArmed) {
        disarmAmiiboWrite();
        uiState = UI_MENU;
        uiMessage = "Write cancelled";
        drawScreen(true);
      }
      setStatus("OK:ami-abort");
      return;
    }
    if (strcmp(rest, "LIST") == 0) {
      const size_t n = listAmiibos();
      for (size_t i = 0; i < n; ++i) {
        char status[80];
        snprintf(status, sizeof(status), "AMI:%u:%s", (unsigned)i, amiNames[i]);
        setStatus(status);
        delay(30);  // keep notifies ordered and lossless for the web page
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
      if (nfcReady && startAmiiboEmulation(name)) return;
      setStatus("ERR:ami-emulate");
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
    if (writeArmed) {
      disarmAmiiboWrite();
      uiState = UI_MENU;
      uiMessage = "Write cancelled";
      drawScreen(true);
    }
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
// UI
// ---------------------------------------------------------------------------

void handleButtons() {
  if (uiState == UI_TAG_READY) {
    if (btnUp.pressed() || btnDown.pressed() || btnBack.pressed()) { tagReady = false; waitingForTag = false; uiState = UI_MENU; uiMessage = "Write cancelled"; drawScreen(true); return; }
    if (btnOk.pressed()) {
      const bool ok = pendingPresent && writeNtag215Ndef(pendingKind, pendingValue);
      tagReady = false; waitingForTag = false; uiState = UI_MENU;
      uiMessage = ok ? "NDEF verified OK" : "Write/verify failed";
      setStatus(ok ? "OK:written-and-verified" : "ERR:write-failed-or-not-NTAG215");
      drawScreen(true);
      return;
    }
  } else if (uiState == UI_WAIT_TAG) {
    if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed() || btnBack.pressed()) {
      waitingForTag = false; uiState = UI_MENU; uiMessage = "Scan cancelled"; drawScreen(true);
    }
  } else if (uiState == UI_LIBRARY) {
    // BACK is the library's way out; before it existed a populated library
    // had no exit path at all.
    if (btnBack.pressed()) { uiState = UI_MENU; uiMessage = ""; drawScreen(true); return; }
    if (btnUp.pressed() && amiCount) { amiSelected = (amiSelected + amiCount - 1) % amiCount; drawScreen(true); }
    if (btnDown.pressed() && amiCount) { amiSelected = (amiSelected + 1) % amiCount; drawScreen(true); }
    // Long-press OK: write the selected amiibo to a physical NTAG215.
    if (btnOk.held(1000)) {
      if (amiCount == 0) { uiState = UI_MENU; drawScreen(true); return; }
      if (!nfcReady) { uiMessage = "PN532Killer absent"; drawScreen(true); return; }
      if (amiV3[amiSelected]) {
        // Air Riders-generation dumps are NTAG I2C Plus personalities;
        // no blank tag of that format is on the market.
        uiMessage = "V3: emulation only"; drawScreen(true); return;
      }
      if (!keysReady) { uiMessage = "Upload key files first"; drawScreen(true); return; }
      if (!armAmiiboWrite(amiNames[amiSelected])) {
        uiMessage = "Load failed";
        drawScreen(true);
      }
      return;
    }
    if (btnOk.shortPress(1000)) {
      if (amiCount == 0) { uiState = UI_MENU; drawScreen(true); return; }
      if (!nfcReady) { uiMessage = "PN532Killer absent"; drawScreen(true); return; }
      if (!startAmiiboEmulation(amiNames[amiSelected])) {
        if (!uiMessage.length()) uiMessage = "Load failed";
        setStatus("ERR:ami-emulate");
        drawScreen(true);
      }
    }
  } else if (uiState == UI_WRITE_TAG) {
    if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed() || btnBack.pressed()) {
      disarmAmiiboWrite();
      uiState = UI_MENU;
      uiMessage = "Write cancelled";
      drawScreen(true);
    }
  } else if (uiState == UI_LOCK_CONFIRM) {
    // Locking is irreversible, so it needs an explicit long press here.
    if (btnOk.held(1200)) {
      uiMessage = "Locking...";
      drawScreen(true);
      const bool ok = lockWrittenTag();
      uiState = UI_MENU;
      uiMessage = ok ? "Tag locked" : "Lock failed";
      setStatus(ok ? "OK:locked" : "ERR:lock-failed");
      drawScreen(true);
    } else if (btnOk.shortPress(1200) || btnUp.pressed() || btnDown.pressed() ||
               btnBack.pressed() || millis() - lockAskedAt > 15000) {
      uiState = UI_MENU;
      uiMessage = "Left unlocked";
      setStatus("OK:lock-skipped");
      drawScreen(true);
    }
  } else if (uiState == UI_EMULATE) {
    // Amiibo mode: UP/DOWN hot-swaps to the neighbouring library entry on a
    // rotating emulator slot; the emulation keeps running. OK/BACK stop.
    // Note mode: any button stops (there is nothing to swap to).
    if (emuMode == EMU_AMIIBO && amiCount > 1 &&
        (btnUp.pressed() || btnDown.pressed())) {
      amiSelected = btnUp.pressed() ? (amiSelected + amiCount - 1) % amiCount
                                    : (amiSelected + 1) % amiCount;
      uint8_t image[ntag215::kV3ImageBytes];
      bool v3 = false;
      if (!loadAmiiboImage(amiNames[amiSelected], image, &v3)) {
        uiMessage = "Load failed";
        drawScreen(true);
      } else if (!emuHotSwap(image, amiNames[amiSelected], v3)) {
        uiMessage = v3 ? "Killer fw lacks V3" : "Swap failed";
        drawScreen(true);
      }
    } else {
      const bool stopBtn = btnOk.pressed() || btnBack.pressed() ||
                           (emuMode != EMU_AMIIBO &&
                            (btnUp.pressed() || btnDown.pressed()));
      if (stopBtn) {
        stopEmulation();
        uiMessage = "Emulation stopped";
        drawScreen(true);
      }
    }
  } else {  // UI_MENU
    if (btnUp.pressed()) { menuIndex = (menuIndex + 4) % 5; uiMessage = ""; drawScreen(true); }
    if (btnDown.pressed()) { menuIndex = (menuIndex + 1) % 5; uiMessage = ""; drawScreen(true); }
    if (btnBack.pressed()) { uiMessage = "You're at the menu"; drawScreen(true); }
    // Long-press OK: soft power off. The menu action itself moved to
    // short-press so a hold never triggers it (fires on release, exactly
    // like the web preview).
    if (btnOk.held(1000)) { powerOffFromMenu(); return; }
    if (btnOk.shortPress(1000)) {
      if (menuIndex == 0) {
        listAmiibos();
        amiSelected = 0; amiTop = 0;
        uiState = UI_LIBRARY;
        uiMessage = "";
      } else if (menuIndex == 1) {
        uiMessage = bleConnected ? "Connected: use web sync" : "OpenTag BLE is advertising";
      } else if (menuIndex == 2) {
        if (!pendingPresent) uiMessage = "Sync NDEF over BLE first";
        else if (!nfcReady) uiMessage = "PN532Killer absent";
        else { waitingForTag = true; tagReady = false; uiState = UI_WAIT_TAG; uiMessage = ""; }
      } else if (menuIndex == 3) {
        if (!pendingPresent) uiMessage = "Sync NDEF over BLE first";
        else if (!nfcReady) uiMessage = "PN532Killer absent";
        else { uiMessage = ""; if (!startNoteEmulation()) { uiMessage = "Emulation failed"; setStatus("ERR:emulate"); } }
      } else {
        memset(pendingValue, 0, sizeof(pendingValue));
        strlcpy(pendingKind, "TEXT", sizeof(pendingKind));
        prefs.remove("kind"); prefs.remove("value");
        pendingPresent = false; uiMessage = "Pending note cleared";
      }
      drawScreen(true);
    }
  }
}

void loadPending() {
  prefs.begin("opentag", false);
  const String kind = prefs.getString("kind", "");
  const String value = prefs.getString("value", "");
  if ((kind == "TEXT" || kind == "URL") && value.length() <= 200) {
    strlcpy(pendingKind, kind.c_str(), sizeof(pendingKind));
    strlcpy(pendingValue, value.c_str(), sizeof(pendingValue));
    pendingPresent = value.length() > 0;
  }
  // Stable random 7-byte UID for the note-emulation personality (NXP-style
  // 04 prefix; the dump's own UID is used for amiibo emulation).
  uint8_t stored[6] = {0, 0, 0, 0, 0, 0};
  if (prefs.getBytes("emuid7", stored, sizeof(stored)) == sizeof(stored)) {
    memcpy(emuUid + 1, stored, sizeof(stored));
  } else {
    for (size_t i = 1; i < 7; i++) emuUid[i] = static_cast<uint8_t>(random(256));
    prefs.putBytes("emuid7", emuUid + 1, 6);
  }
  // Amiibo signing keys, if the user uploaded them over BLE.
  refreshKeyState();
}

void setup() {
  Serial.begin(115200);
  pinMode(Pins::BUTTON_UP, INPUT_PULLUP);
  pinMode(Pins::BUTTON_DOWN, INPUT_PULLUP);
  pinMode(Pins::BUTTON_OK, INPUT_PULLUP);
  pinMode(Pins::BUTTON_BACK, INPUT_PULLUP);

  // Soft power-on: if OK just woke us from deep sleep, it must be held for
  // a full second to boot — a short tap goes straight back to sleep. This
  // runs before SPI/OLED come up so the screen stays dark during the hold.
  if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    const uint32_t t0 = millis();
    while (digitalRead(Pins::BUTTON_OK) == LOW && millis() - t0 < 1000) delay(10);
    if (digitalRead(Pins::BUTTON_OK) == HIGH) enterDeepSleep();   // released early
    while (digitalRead(Pins::BUTTON_OK) == LOW) delay(10);        // wait for release
  }

  // No MISO: the OLED is write-only and GPIO13 (the MISO pin) is the BACK
  // button, so it must not be claimed by the SPI driver.
  SPI.begin(Pins::SCK, -1, Pins::MOSI);
  oled.begin();
  oled.fillScreen(BG);
  oled.setTextColor(MINT); oled.setTextSize(1); oled.setCursor(8, 20); oled.print("OPENTAG STARTING");

  LittleFS.begin(true);
  LittleFS.mkdir(Amiibo::kDir);

  nfcSerial.begin(115200, SERIAL_8N1, Pins::NFC_RX, Pins::NFC_TX);
  nfcReady = nfc.begin(nfcSerial);
  if (nfcReady) {
    nfc.samConfig();
    nfc.setPassiveActivationRetries(2);
  }

  loadPending();
  beginBle();
  drawScreen(true);
}

void loop() {
  handleButtons();

  // Transfer chunk ACKs (pacing for the web page).
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

  // Note adoption while at the menu (keeps the UI in sync without emulation).
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
    uiMessage = "NDEF note synced";
  }

  // Poll for a physical NTAG215 while the write flow is armed.
  if (uiState == UI_WAIT_TAG && nfcReady && millis() - lastNfcPoll > 120) {
    lastNfcPoll = millis();
    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;
    if (nfc.readPassiveTarget(uid, &uidLen, 60)) {
      uint8_t page16[16] = {0};
      if (nfc.ntagReadPage(3, page16) && page16[0] == 0xE1 && page16[2] == 0x3E) {
        tagReady = true;
        uiState = UI_TAG_READY;
        uiMessage = "";
      } else {
        uiMessage = "Not an NTAG215 NDEF tag";
      }
      drawScreen(true);
    }
  }

  // Poll for a tag while the amiibo write flow is armed, then run the
  // TagMo-style write (blocking, ~2 s: re-sign, 129 page writes, verify).
  if (uiState == UI_WRITE_TAG && nfcReady && millis() - lastNfcPoll > 120) {
    lastNfcPoll = millis();
    uint8_t uid[7] = {0};
    uint8_t uidLen = 0;
    if (nfc.readPassiveTarget(uid, &uidLen, 60)) {
      uiMessage = "Signing + writing";
      drawScreen(true);
      const WriteResult result = writeArmedImage();
      disarmAmiiboWrite();
      switch (result) {
        case WRES_OK:
          uiState = UI_LOCK_CONFIRM;
          lockAskedAt = millis();
          uiMessage = "";
          setStatus("OK:written-and-verified");
          break;
        case WRES_IO:
          uiState = UI_MENU; uiMessage = "Tag lost"; setStatus("ERR:write-tag-lost");
          break;
        case WRES_NOT_NTAG215:
          uiState = UI_MENU; uiMessage = "Not an NTAG215";
          setStatus("ERR:write-failed-or-not-NTAG215");
          break;
        case WRES_LOCKED:
          uiState = UI_MENU; uiMessage = "Tag already locked"; setStatus("ERR:write-tag-locked");
          break;
        case WRES_BAD_DUMP:
          uiState = UI_MENU; uiMessage = "Dump failed verify"; setStatus("ERR:write-dump");
          break;
        case WRES_WRITE:
          uiState = UI_MENU; uiMessage = "Write failed"; setStatus("ERR:write-failed");
          break;
        default:
          uiState = UI_MENU; uiMessage = "Verify failed"; setStatus("ERR:write-verify");
          break;
      }
      drawScreen(true);
    }
  }

  drawScreen();
  delay(4);
}
