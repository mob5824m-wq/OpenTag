#include <Arduino.h>
#include <SPI.h>
#include <Preferences.h>
#include <NimBLEDevice.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1351.h>
#include <Adafruit_PN532.h>
#include <cstring>
#include <string>

// OpenTag device prototype: ESP32-S3 + SSD1351 OLED + PN532 over shared SPI.
// The NFC feature is intentionally limited to ordinary NDEF text/URL records.
namespace Pins {
constexpr int SCK = 12, MISO = 13, MOSI = 11;
constexpr int OLED_CS = 10, OLED_DC = 9, OLED_RST = 8;
constexpr int PN532_CS = 14;
constexpr int BUTTON_UP = 4, BUTTON_DOWN = 5, BUTTON_OK = 6;
}

namespace BleIds {
static const char *SERVICE = "9b9e0001-9d2a-4b72-9a0c-6a8f6f4d0001";
static const char *WRITE_VALUE = "9b9e0002-9d2a-4b72-9a0c-6a8f6f4d0001";
static const char *STATUS = "9b9e0003-9d2a-4b72-9a0c-6a8f6f4d0001";
}

Adafruit_SSD1351 oled(128, 128, &SPI, Pins::OLED_CS, Pins::OLED_DC, Pins::OLED_RST);
Adafruit_PN532 nfc(Pins::PN532_CS, &SPI);
Preferences prefs;
NimBLECharacteristic *statusCharacteristic = nullptr;

static portMUX_TYPE pendingMux = portMUX_INITIALIZER_UNLOCKED;
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
static unsigned long lastDraw = 0;
static unsigned long lastNfcPoll = 0;

static const uint16_t BG = 0x1082, MINT = 0xB7EF, LAVENDER = 0xC5DF;
static const uint16_t WHITE = 0xFFFF, MUTED = 0x9CF3, RED = 0xF986;

class NoteWriteCallbacks final : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *characteristic, NimBLEConnInfo &) override {
    const std::string raw = characteristic->getValue();
    const size_t split = raw.find(':');
    if (split == std::string::npos || split == 0 || split > 4 || raw.size() <= split + 1 || raw.size() > 245) {
      if (statusCharacteristic) { statusCharacteristic->setValue("ERR:format-or-size"); statusCharacteristic->notify(); }
      return;
    }
    const std::string kind = raw.substr(0, split);
    const std::string value = raw.substr(split + 1);
    const bool isText = kind == "TEXT";
    const bool isUrl = kind == "URL";
    if ((!isText && !isUrl) || value.size() > 200 || value.find('\0') != std::string::npos ||
        value.find('\n') != std::string::npos || value.find('\r') != std::string::npos ||
        (isUrl && value.rfind("https://", 0) != 0 && value.rfind("http://", 0) != 0)) {
      if (statusCharacteristic) { statusCharacteristic->setValue("ERR:unsupported-payload"); statusCharacteristic->notify(); }
      return;
    }
    portENTER_CRITICAL(&pendingMux);
    memset(pendingValue, 0, sizeof(pendingValue));
    memcpy(pendingValue, value.data(), value.size());
    strlcpy(pendingKind, kind.c_str(), sizeof(pendingKind));
    pendingChanged = true;
    portEXIT_CRITICAL(&pendingMux);
    if (statusCharacteristic) { statusCharacteristic->setValue("OK:queued"); statusCharacteristic->notify(); }
  }
};

class ServerCallbacks final : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *, NimBLEConnInfo &) override { bleConnected = true; }
  void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) override {
    bleConnected = false;
    NimBLEDevice::startAdvertising();
  }
};

struct Button {
  int pin;
  bool lastStable = HIGH;
  bool lastRead = HIGH;
  unsigned long changedAt = 0;
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
Button btnUp{Pins::BUTTON_UP}, btnDown{Pins::BUTTON_DOWN}, btnOk{Pins::BUTTON_OK};

void setStatus(const char *value) {
  if (statusCharacteristic) { statusCharacteristic->setValue(value); statusCharacteristic->notify(); }
}

void drawScreen(bool force = false) {
  if (!force && millis() - lastDraw < 180) return;
  lastDraw = millis();
  oled.fillScreen(BG);
  oled.setTextWrap(false);
  oled.setTextSize(1);
  oled.setTextColor(MINT); oled.setCursor(8, 7); oled.print("OPENTAG");
  oled.setTextColor(MUTED); oled.setCursor(84, 7); oled.print(bleConnected ? "BLE ON" : "BLE");
  oled.drawFastHLine(8, 21, 112, 0x4208);

  if (waitingForTag && !tagReady) {
    oled.setTextColor(WHITE); oled.setCursor(8, 37); oled.print("Ready to write");
    oled.setTextColor(MUTED); oled.setCursor(8, 55); oled.print("Tap an NTAG215 tag");
    oled.setCursor(8, 70); oled.print("to the NFC antenna.");
    oled.setTextColor(MINT); oled.setCursor(8, 105); oled.print("UP/DOWN: cancel");
    oled.setCursor(8, 117); oled.print("OK: scan/cancel");
  } else if (tagReady) {
    oled.setTextColor(MINT); oled.setCursor(8, 35); oled.print("NTAG215 detected");
    oled.setTextColor(WHITE); oled.setCursor(8, 55); oled.print("OK to write NDEF");
    oled.setTextColor(MUTED); oled.setCursor(8, 73); oled.print(pendingKind);
    oled.setCursor(8, 87); oled.print(String(pendingValue).substring(0, 18));
    oled.setTextColor(RED); oled.setCursor(8, 111); oled.print("Existing data replaced");
  } else {
    const char *items[] = {"Write NDEF note", "BLE sync", "Clear pending note"};
    for (int i = 0; i < 3; ++i) {
      const int y = 37 + i * 23;
      oled.setTextColor(i == menuIndex ? MINT : MUTED);
      oled.setCursor(8, y); oled.print(i == menuIndex ? "> " : "  "); oled.print(items[i]);
    }
    oled.setTextColor(WHITE); oled.setCursor(8, 111);
    if (pendingPresent) { oled.print(pendingKind); oled.print(": "); oled.print(String(pendingValue).substring(0, 12)); }
    else oled.print("Sync a text or URL first");
    if (uiMessage.length()) { oled.setTextColor(LAVENDER); oled.setCursor(8, 96); oled.print(uiMessage.substring(0, 19)); }
  }
}

bool makeNdefTlv(const char *kind, const char *value, uint8_t *out, size_t capacity, size_t *outLength) {
  const size_t valueLen = strlen(value);
  const bool text = strcmp(kind, "TEXT") == 0;
  const bool url = strcmp(kind, "URL") == 0;
  if ((!text && !url) || valueLen == 0 || valueLen > 200) return false;
  const size_t payloadLen = text ? valueLen + 3 : valueLen + 1;
  const size_t ndefLen = 4 + payloadLen;
  const size_t total = 2 + ndefLen + 1;
  if (ndefLen > 254 || total > capacity || total > 504) return false;

  size_t i = 0;
  out[i++] = 0x03;                    // NDEF Message TLV
  out[i++] = static_cast<uint8_t>(ndefLen);
  out[i++] = 0xD1;                    // MB | ME | SR | well-known TNF
  out[i++] = 0x01;                    // type length
  out[i++] = static_cast<uint8_t>(payloadLen);
  out[i++] = text ? 'T' : 'U';
  if (text) {
    out[i++] = 0x02;                  // UTF-8, two-letter language code follows
    out[i++] = 'e'; out[i++] = 'n';
  } else {
    out[i++] = 0x00;                  // no URI prefix abbreviation
  }
  memcpy(out + i, value, valueLen); i += valueLen;
  out[i++] = 0xFE;                    // Terminator TLV
  *outLength = i;
  return true;
}

bool writeNtag215Ndef(const char *kind, const char *value) {
  uint8_t cc[4] = {0};
  // NTAG215 Capability Container is E1 10 3E 00 (504 bytes user memory).
  // Restrict writes to that NDEF-compatible tag profile; no tag-image import.
  if (!nfc.ntag2xx_ReadPage(3, cc) || cc[0] != 0xE1 || cc[2] != 0x3E) return false;
  uint8_t tlv[220] = {0};
  size_t length = 0;
  if (!makeNdefTlv(kind, value, tlv, sizeof(tlv), &length)) return false;

  const size_t pages = (length + 3) / 4;
  uint8_t page[4];
  for (size_t p = 0; p < pages; ++p) {
    memset(page, 0, sizeof(page));
    const size_t start = p * 4;
    const size_t count = min(static_cast<size_t>(4), length - start);
    memcpy(page, tlv + start, count);
    if (!nfc.ntag2xx_WritePage(static_cast<uint8_t>(4 + p), page)) return false;
  }
  // Read back every written page and compare before reporting success.
  for (size_t p = 0; p < pages; ++p) {
    uint8_t verify[4] = {0};
    if (!nfc.ntag2xx_ReadPage(static_cast<uint8_t>(4 + p), verify)) return false;
    for (size_t b = 0; b < 4; ++b) {
      const size_t pos = p * 4 + b;
      const uint8_t expected = pos < length ? tlv[pos] : 0;
      if (verify[b] != expected) return false;
    }
  }
  return true;
}

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
  writeValue->setCallbacks(new NoteWriteCallbacks());
  statusCharacteristic->setValue("READY");
  service->start();
  NimBLEAdvertising *advertising = NimBLEDevice::getAdvertising();
  advertising->addServiceUUID(BleIds::SERVICE);
  advertising->start();
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
}

void savePending() {
  prefs.putString("kind", pendingKind);
  prefs.putString("value", pendingValue);
  pendingPresent = strlen(pendingValue) > 0;
}

void handleButtons() {
  if (tagReady) {
    if (btnUp.pressed() || btnDown.pressed()) { tagReady = false; waitingForTag = false; uiMessage = "Write cancelled"; drawScreen(true); return; }
    if (btnOk.pressed()) {
      const bool ok = pendingPresent && writeNtag215Ndef(pendingKind, pendingValue);
      tagReady = false; waitingForTag = false;
      uiMessage = ok ? "NDEF verified OK" : "Write/verify failed";
      setStatus(ok ? "OK:written-and-verified" : "ERR:write-failed-or-not-NTAG215");
      drawScreen(true);
      return;
    }
  } else if (waitingForTag) {
    if (btnUp.pressed() || btnDown.pressed() || btnOk.pressed()) {
      waitingForTag = false; uiMessage = "Scan cancelled"; drawScreen(true); return;
    }
  } else {
    if (btnUp.pressed()) { menuIndex = (menuIndex + 2) % 3; uiMessage = ""; drawScreen(true); }
    if (btnDown.pressed()) { menuIndex = (menuIndex + 1) % 3; uiMessage = ""; drawScreen(true); }
    if (btnOk.pressed()) {
      if (menuIndex == 0) {
        if (!pendingPresent) uiMessage = "Sync NDEF over BLE first";
        else if (!nfcReady) uiMessage = "PN532 not detected";
        else { waitingForTag = true; tagReady = false; uiMessage = ""; }
      } else if (menuIndex == 1) {
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

void setup() {
  Serial.begin(115200);
  pinMode(Pins::BUTTON_UP, INPUT_PULLUP);
  pinMode(Pins::BUTTON_DOWN, INPUT_PULLUP);
  pinMode(Pins::BUTTON_OK, INPUT_PULLUP);

  SPI.begin(Pins::SCK, Pins::MISO, Pins::MOSI);
  oled.begin();
  oled.fillScreen(BG);
  oled.setTextColor(MINT); oled.setTextSize(1); oled.setCursor(8, 20); oled.print("OPENTAG STARTING");

  nfc.begin();
  const uint32_t version = nfc.getFirmwareVersion();
  nfcReady = version != 0;
  if (nfcReady) nfc.SAMConfig();

  loadPending();
  beginBle();
  drawScreen(true);
}

void loop() {
  handleButtons();
  bool changed = false;
  char localValue[241] = {0};
  char localKind[5] = {0};
  portENTER_CRITICAL(&pendingMux);
  if (pendingChanged) {
    strlcpy(localValue, pendingValue, sizeof(localValue));
    strlcpy(localKind, pendingKind, sizeof(localKind));
    pendingChanged = false;
    changed = true;
  }
  portEXIT_CRITICAL(&pendingMux);
  if (changed) {
    strlcpy(pendingValue, localValue, sizeof(pendingValue));
    strlcpy(pendingKind, localKind, sizeof(pendingKind));
    savePending();
    uiMessage = "NDEF note synced";
    drawScreen(true);
  }

  if (waitingForTag && nfcReady && millis() - lastNfcPoll > 100) {
    lastNfcPoll = millis();
    uint8_t uid[7] = {0}; uint8_t uidLength = 0;
    if (nfc.readPassiveTargetID(PN532_MIFARE_ISO14443A, uid, &uidLength, 40)) {
      uint8_t cc[4] = {0};
      if (nfc.ntag2xx_ReadPage(3, cc) && cc[0] == 0xE1 && cc[2] == 0x3E) {
        tagReady = true;
        uiMessage = "";
        drawScreen(true);
      } else {
        uiMessage = "Not an NTAG215 NDEF tag";
        drawScreen(true);
      }
    }
  }
  drawScreen();
  delay(4);
}
