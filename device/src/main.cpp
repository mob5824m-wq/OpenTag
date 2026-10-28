#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <LittleFS.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <cstring>
#include <string>

#include "ndef_tlv.h"
#include "ntag215_emu.h"
#include "pn532_uart.h"

// OpenTag device firmware: ESP32-S3 DevKitC-1 + SSD1351 OLED + PN532Killer
// (ST25R3916-class NFC emulator) on UART2.
//
// Features:
//   * Write the BLE-synced NDEF note onto a real NTAG215 tag (reader mode).
//   * Emulate the NDEF note as a virtual NTAG215 (Killer emulator slot 0).
//   * Amiibo library: upload .bin dumps of your own amiibo tags over BLE
//     (as a .zip via the web page or one-by-one), browse on the OLED and
//     emulate any of them by presenting the dump's own 7-byte UID.
//   * BLE service with a text command channel and a binary chunk channel.
//
// All tag data is user-supplied at runtime; nothing is bundled here.
namespace Pins {
constexpr int SCK = 12, MISO = 13, MOSI = 11;
constexpr int OLED_CS = 10, OLED_DC = 9, OLED_RST = 8;
constexpr int NFC_TX = 17, NFC_RX = 18;  // ESP32 TX -> Killer J4 RX
// Buttons sit on the J3 header row (board-east side of the carrier PCB):
constexpr int BUTTON_UP = 42, BUTTON_DOWN = 2, BUTTON_OK = 1;
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
static uint8_t recvBuf[572];        // largest accepted container (540 + slack)

// ---- emulation state ----
enum EmuMode : uint8_t { EMU_NONE = 0, EMU_NOTE = 1, EMU_AMIIBO = 2 };
static uint8_t emuMode = EMU_NONE;
static char emuName[Amiibo::kNameLen] = "";
static uint8_t emuUid[7] = {0x04, 0, 0, 0, 0, 0, 0};

// ---- amiibo library browser ----
static char amiNames[Amiibo::kMaxFiles][Amiibo::kNameLen];
static size_t amiCount = 0;
static size_t amiSelected = 0;
static size_t amiTop = 0;

enum UiState : uint8_t { UI_MENU, UI_WAIT_TAG, UI_TAG_READY, UI_LIBRARY, UI_EMULATE };
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
  explicit Button(int pinNumber) : pin(pinNumber) {}
  bool pressed() {
    const bool reading = digitalRead(pin);
    if (reading != lastRead) { lastRead = reading; changedAt = millis(); }
    if (millis() - changedAt > 28 && reading != lastStable) {
      lastStable = reading;
      if (lastStable == LOW) return true;
    }
    return false;
  }
};
Button btnUp(Pins::BUTTON_UP), btnDown(Pins::BUTTON_DOWN), btnOk(Pins::BUTTON_OK);

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
  return amiCount;
}

// Reads /amiibo/<name>.bin, validates it and normalizes it to 540 pages.
static bool loadAmiiboImage(const char *name, uint8_t image[ntag215::kImageBytes]) {
  char path[64];
  amiiboPath(name, path, sizeof(path));
  File f = LittleFS.open(path, "r");
  if (!f) return false;
  uint8_t buf[572];
  const size_t got = f.read(buf, sizeof(buf));
  f.close();
  if (got < 532) return false;
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
    oled.setTextColor(MINT); oled.setCursor(8, 105); oled.print("UP/DOWN/OK: cancel");
  } else if (uiState == UI_TAG_READY) {
    oled.setTextColor(MINT); oled.setCursor(8, 35); oled.print("NTAG215 detected");
    oled.setTextColor(WHITE); oled.setCursor(8, 55); oled.print("OK to write NDEF");
    oled.setTextColor(MUTED); oled.setCursor(8, 73); oled.print(pendingKind);
    oled.setCursor(8, 87); oled.print(String(pendingValue).substring(0, 18));
    oled.setTextColor(RED); oled.setCursor(8, 111); oled.print("Existing data replaced");
  } else if (uiState == UI_LIBRARY) {
    oled.setTextColor(MINT); oled.setCursor(8, 27); oled.print("AMIIBO LIBRARY");
    if (amiCount == 0) {
      oled.setTextColor(MUTED); oled.setCursor(8, 50); oled.print("Empty.");
      oled.setCursor(8, 66); oled.print("Upload a .zip of your");
      oled.setCursor(8, 80); oled.print("dumps via BLE sync.");
    } else {
      const size_t visible = amiCount < 5 ? amiCount : 5;
      if (amiSelected < amiTop) amiTop = amiSelected;
      if (amiSelected >= amiTop + visible) amiTop = amiSelected - visible + 1;
      for (size_t i = 0; i < visible; ++i) {
        const size_t idx = amiTop + i;
        const int y = 34 + (int)i * 15;
        oled.setTextColor(idx == amiSelected ? MINT : MUTED);
        oled.setCursor(8, y); oled.print(idx == amiSelected ? "> " : "  ");
        oled.print(String(amiNames[idx]).substring(0, 16));
      }
      oled.setTextColor(MUTED); oled.setCursor(8, 114);
      oled.print((int)(amiSelected + 1)); oled.print('/'); oled.print((int)amiCount);
      oled.print(" OK: emulate");
    }
  } else if (uiState == UI_EMULATE) {
    oled.setTextColor(MINT); oled.setCursor(8, 32); oled.print("EMULATING");
    oled.setTextColor(WHITE); oled.setCursor(8, 48);
    oled.print(String(emuName).substring(0, 16));
    oled.setTextColor(MUTED); oled.setCursor(8, 66); oled.print("UID ");
    for (int i = 0; i < 7; ++i) {
      if (i) oled.print(':');
      if (emuUid[i] < 0x10) oled.print('0');
      oled.print(emuUid[i], HEX);
    }
    oled.setCursor(8, 82); oled.print(emuMode == EMU_NOTE ? "NDEF note" : "Tap console to use");
    oled.setTextColor(LAVENDER); oled.setCursor(8, 100);
    oled.print("Killer slot 0 active");
    oled.setTextColor(MINT); oled.setCursor(8, 116); oled.print("UP/DOWN/OK: stop");
  } else {
    const char *items[] = {"Write NDEF note", "Emulate NDEF note", "Amiibo library",
                           "BLE sync", "Clear note"};
    for (int i = 0; i < 5; ++i) {
      const int y = 28 + i * 16;
      oled.setTextColor(i == menuIndex ? MINT : MUTED);
      oled.setCursor(8, y); oled.print(i == menuIndex ? "> " : "  "); oled.print(items[i]);
    }
    oled.setTextColor(WHITE); oled.setCursor(8, 116);
    if (pendingPresent) { oled.print(pendingKind); oled.print(": "); oled.print(String(pendingValue).substring(0, 12)); }
    else oled.print("Sync a text or URL");
    if (uiMessage.length()) {
      oled.setTextColor(LAVENDER); oled.setCursor(8, 108); oled.print(uiMessage.substring(0, 19));
    }
  }
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

bool startEmulation(const uint8_t image[540], const char *label, uint8_t mode) {
  if (!nfcReady) return false;
  nfc.killerSetWorkMode(KILLER_MODE_PN532);  // required baseline between modes
  if (!nfc.killerLoadNtagImage(KILLER_TAG_NTAG_MFU, 0, image)) return false;
  if (!nfc.killerSetWorkMode(KILLER_MODE_EMULATOR, KILLER_TAG_NTAG_MFU, 0)) return false;
  emuMode = mode;
  strlcpy(emuName, label, sizeof(emuName));
  uiState = UI_EMULATE;
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
  uint8_t image[540];
  if (!loadAmiiboImage(name, image)) return false;
  return startEmulation(image, name, EMU_AMIIBO);
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
  uint8_t buf[572];
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
  if (!ntag215::validateDump(buf, size)) {
    setStatus("ERR:ami-invalid:dump");
    return;
  }
  uint8_t image[540];
  ntag215::normalizeDump(buf, size, image);
  char path[64];
  amiiboPath(name, path, sizeof(path));
  File f = LittleFS.open(path, FILE_WRITE);
  if (!f) { setStatus("ERR:ami-io"); return; }
  const size_t wrote = f.write(image, sizeof(image));
  f.close();
  if (wrote != sizeof(image)) {
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
  if (end == sizeStr || *end != '\0' || size < 532 || size > 572) {
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
  strlcpy(recvName, name, sizeof(recvName));
  portEXIT_CRITICAL(&stateMux);
  char status[80];
  snprintf(status, sizeof(status), "OK:ami-put:%s", name);
  setStatus(status);
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

  if (strncmp(cmd, "AMI:", 4) == 0) {
    char *rest = cmd + 4;
    if (strncmp(rest, "PUT:", 4) == 0) { handleAmiiboPut(rest + 4); return; }
    if (strcmp(rest, "ABORT") == 0) {
      portENTER_CRITICAL(&stateMux);
      recvActive = false;
      recvComplete = false;
      recvLen = 0;
      recvSeq = 0;
      portEXIT_CRITICAL(&stateMux);
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

  if (strcmp(cmd, "STOP") == 0) { stopEmulation(); return; }

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
    if (btnUp.pressed() || btnDown.pressed()) { tagReady = false; waitingForTag = false; uiState = UI_MENU; uiMessage = "Write cancelled"; drawScreen(true); return; }
    if (btnOk.pressed()) {
      const bool ok = pendingPresent && writeNtag215Ndef(pendingKind, pendingValue);
      tagReady = false; waitingForTag = false; uiState = UI_MENU;
      uiMessage = ok ? "NDEF verified OK" : "Write/verify failed";
      setStatus(ok ? "OK:written-and-verified" : "ERR:write-failed-or-not-NTAG215");
      drawScreen(true);
      return;
    }
  } else if (uiState == UI_WAIT_TAG) {
    if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed()) {
      waitingForTag = false; uiState = UI_MENU; uiMessage = "Scan cancelled"; drawScreen(true);
    }
  } else if (uiState == UI_LIBRARY) {
    if (btnUp.pressed() && amiCount) { amiSelected = (amiSelected + amiCount - 1) % amiCount; drawScreen(true); }
    if (btnDown.pressed() && amiCount) { amiSelected = (amiSelected + 1) % amiCount; drawScreen(true); }
    if (btnOk.pressed()) {
      if (amiCount == 0) { uiState = UI_MENU; drawScreen(true); return; }
      if (!nfcReady) { uiMessage = "PN532Killer absent"; drawScreen(true); return; }
      if (!startAmiiboEmulation(amiNames[amiSelected])) {
        uiMessage = "Load failed";
        setStatus("ERR:ami-emulate");
        drawScreen(true);
      }
    }
  } else if (uiState == UI_EMULATE) {
    if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed()) {
      stopEmulation();
      uiMessage = "Emulation stopped";
      drawScreen(true);
    }
  } else {  // UI_MENU
    if (btnUp.pressed()) { menuIndex = (menuIndex + 4) % 5; uiMessage = ""; drawScreen(true); }
    if (btnDown.pressed()) { menuIndex = (menuIndex + 1) % 5; uiMessage = ""; drawScreen(true); }
    if (btnOk.pressed()) {
      if (menuIndex == 0) {
        if (!pendingPresent) uiMessage = "Sync NDEF over BLE first";
        else if (!nfcReady) uiMessage = "PN532Killer absent";
        else { waitingForTag = true; tagReady = false; uiState = UI_WAIT_TAG; uiMessage = ""; }
      } else if (menuIndex == 1) {
        if (!pendingPresent) uiMessage = "Sync NDEF over BLE first";
        else if (!nfcReady) uiMessage = "PN532Killer absent";
        else { uiMessage = ""; if (!startNoteEmulation()) { uiMessage = "Emulation failed"; setStatus("ERR:emulate"); } }
      } else if (menuIndex == 2) {
        listAmiibos();
        amiSelected = 0; amiTop = 0;
        uiState = UI_LIBRARY;
        uiMessage = "";
      } else if (menuIndex == 3) {
        uiMessage = bleConnected ? "Connected: use web sync" : "OpenTag BLE is advertising";
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
}

void setup() {
  Serial.begin(115200);
  pinMode(Pins::BUTTON_UP, INPUT_PULLUP);
  pinMode(Pins::BUTTON_DOWN, INPUT_PULLUP);
  pinMode(Pins::BUTTON_OK, INPUT_PULLUP);

  SPI.begin(Pins::SCK, Pins::MISO, Pins::MOSI);
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

  // Finished a .bin transfer: validate and store.
  bool complete = false;
  portENTER_CRITICAL(&stateMux);
  if (recvComplete) { complete = true; recvComplete = false; }
  portEXIT_CRITICAL(&stateMux);
  if (complete) finalizeAmiiboUpload();

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

  drawScreen();
  delay(4);
}
