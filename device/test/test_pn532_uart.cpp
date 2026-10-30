// Host-native model-based tests for pn532_uart.h (PN532 / PN532Killer HSU
// framing and protocol). A fake serial echoes the PN532Killer's observable
// behaviour: ACK every well-formed host frame, then a D5 response.
// Build & run:
//   g++ -std=c++11 -Wall -Wextra -pedantic -I host_stubs -I ../src
//       test_pn532_uart.cpp && ./a.out
#include "Arduino.h"

#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>

#include "../src/pn532_uart.h"

static int failures = 0;
static int checks = 0;

#define CHECK(cond)                                                            \
  do {                                                                         \
    checks++;                                                                  \
    if (!(cond)) {                                                             \
      failures++;                                                              \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);              \
    }                                                                          \
  } while (0)

#define CHECK_EQ(a, b)                                                         \
  do {                                                                         \
    checks++;                                                                  \
    if (!((a) == (b))) {                                                       \
      failures++;                                                              \
      std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);       \
    }                                                                          \
  } while (0)

uint32_t g_fakeMillis = 0;

// ---------------------------------------------------------------------------
// Fake serial + PN532Killer behavioural model
// ---------------------------------------------------------------------------

struct KillerModel;

static KillerModel *g_model = nullptr;

struct KillerModel {
  std::deque<uint8_t> toHost;
  std::vector<uint8_t> captured;  // every host->device byte, for assertions
  size_t parsePos = 0;            // model-side cursor into `captured`

  // Behaviour switches.
  bool silent = false;             // ignore frames (timeout paths)
  bool corruptDcs = false;         // emit responses with a bad data checksum
  bool garbageBeforeAck = false;   // exercise ACK resynchronisation
  bool passiveTargetPresent = false;
  uint8_t targetUid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  uint8_t targetUidLen = 7;

  // Emulator slot model (NTAG/MFU, slot 0). v3Capable models firmware that
  // accepts the extended (Air Riders) page range 0x00-0xFF.
  uint8_t slotImage[1024];
  bool slotLoaded = false;
  bool v3Capable = true;
  int writePageCalls = 0;
  bool doneReceived = false;
  uint8_t lastWorkMode[3] = {0, 0, 0};  // mode, type, index

  void emit(const uint8_t *b, size_t n) {
    for (size_t i = 0; i < n; i++) toHost.push_back(b[i]);
  }
  void emitAck() {
    static const uint8_t ack[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    emit(ack, 6);
  }
  void emitResponse(uint8_t code, const uint8_t *payload, size_t n) {
    // 00 00 FF LEN LCS D5 <code payload> DCS 00
    uint8_t frame[300];
    size_t len = 1 + n;
    size_t k = 0;
    frame[k++] = 0x00; frame[k++] = 0x00; frame[k++] = 0xFF;
    frame[k++] = (uint8_t)len;
    frame[k++] = (uint8_t)(~len + 1);
    frame[k++] = 0xD5;
    uint8_t dcs = 0xD5;
    frame[k++] = code; dcs += code;
    for (size_t i = 0; i < n; i++) { frame[k++] = payload[i]; dcs += payload[i]; }
    if (corruptDcs) dcs ^= 0xFF;
    frame[k++] = (uint8_t)(~dcs + 1);
    frame[k++] = 0x00;
    emit(frame, k);
  }
};

struct FakeSerial final : Stream {
  int available() override { return (int)g_model->toHost.size(); }
  int read() override {
    if (g_model->toHost.empty()) return -1;
    int b = g_model->toHost.front();
    g_model->toHost.pop_front();
    return b;
  }
  size_t write(const uint8_t *buf, size_t size) override {
    for (size_t i = 0; i < size; i++) {
      g_model->captured.push_back(buf[i]);
    }
    parsePending();
    return size;
  }

  // Consumes complete frames from `captured` and reacts like the board.
  void parsePending() {
    std::vector<uint8_t>& in = g_model->captured;
    size_t i = g_model->parsePos;
    while (true) {
      // hunt for 00 00 FF
      while (in.size() - i >= 3 && !(in[i] == 0x00 && in[i + 1] == 0x00 && in[i + 2] == 0xFF)) i++;
      if (in.size() - i < 3) break;
      size_t len = in[i + 3];
      if (in.size() - i < 6u + len + 1) break;  // header(5)+D4+payload+DCS
      // header: 00 00 FF LEN LCS
      uint8_t lcs = in[i + 4];
      if ((uint8_t)(len + lcs) != 0) { i += 1; continue; }
      // in[i+5] = D4, payload starts at i+6
      uint8_t dcs = in[i + 5];
      for (size_t j = 0; j < len; j++) dcs += in[i + 6 + j];
      if ((uint8_t)(dcs + in[i + 6 + len]) != 0) { i += 1; continue; }
      const uint8_t *payload = &in[i + 6];
      size_t payloadLen = len - 1;
      handlePayload(payload, payloadLen);
      i += 6 + len + 1;  // past postamble byte
    }
    g_model->parsePos = i;
  }

  void handlePayload(const uint8_t *p, size_t n) {
    if (g_model->silent) return;
    if (g_model->garbageBeforeAck) {
      static const uint8_t junk[4] = {0x42, 0x00, 0x00, 0x99};
      g_model->emit(junk, 4);
      g_model->garbageBeforeAck = false;
    }
    g_model->emitAck();
    const uint8_t cmd = p[0];
    switch (cmd) {
      case 0x02: {  // GetFirmwareVersion
        const uint8_t v[4] = {0x32, 0x01, 0x06, 0x07};
        g_model->emitResponse(0x03, v, 4);
        break;
      }
      case 0x14: {  // SAMConfiguration
        g_model->emitResponse(0x15, nullptr, 0);
        break;
      }
      case 0x32: {  // RFConfiguration
        g_model->emitResponse(0x33, nullptr, 0);
        break;
      }
      case 0x4A: {  // InListPassiveTarget
        if (g_model->passiveTargetPresent) {
          uint8_t r[16] = {0x01, 0x44, 0x00, 0x00, 0x00};
          r[4] = g_model->targetUidLen;
          memcpy(&r[5], g_model->targetUid, g_model->targetUidLen);
          g_model->emitResponse(0x4B, r, 5 + g_model->targetUidLen);
        } else {
          const uint8_t none[1] = {0x00};
          g_model->emitResponse(0x4B, none, 1);
        }
        break;
      }
      case 0x40: {  // InDataExchange
        uint8_t status = 0x00;
        const uint8_t *data = p + 2;
        size_t dataLen = n - 2;
        if (dataLen >= 2 && data[0] == 0x30) {  // NTAG READ
          uint8_t page = data[1];
          uint8_t r[18] = {0};
          r[0] = status;
          for (int k = 0; k < 4; k++) r[1 + k * 4] = page + k;  // marker bytes
          g_model->emitResponse(0x41, r, 17);
        } else if (dataLen >= 2 && data[0] == 0xA2) {  // NTAG WRITE
          uint8_t r[2] = {status, 0x0A};               // ACK
          g_model->emitResponse(0x41, r, 2);
        } else {
          uint8_t r[1] = {status};
          g_model->emitResponse(0x41, r, 1);
        }
        break;
      }
      case 0xAC: {  // SetWorkMode
        g_model->lastWorkMode[0] = p[1];
        g_model->lastWorkMode[1] = p[2];
        g_model->lastWorkMode[2] = p[3];
        const uint8_t ok[1] = {0x00};
        g_model->emitResponse(0xAD, ok, 1);
        if (p[1] == 0x02) g_model->slotLoaded = g_model->doneReceived;
        break;
      }
      case 0x1E: {  // WriteEmulator
        if (p[3] == 0xFF && p[4] == 0xFF) {
          g_model->doneReceived = true;
          uint8_t r[6] = {p[1], p[2], p[3], p[4], 0x00, 0x00};
          g_model->emitResponse(0x1F, r, 6);
        } else {
          g_model->writePageCalls++;
          const uint8_t type = p[1], slot = p[2], sector = p[3], block = p[4];
          // Stock NTAG215-only firmware stays silent past page 134.
          if (block >= 135 && !g_model->v3Capable) break;
          if (type == 0x02 && slot == 0 && sector == 0 && n >= 9)
            memcpy(&g_model->slotImage[block * 4], p + 5, 4);
          uint8_t r[6] = {p[1], p[2], p[3], p[4], 0x00, 0x00};
          g_model->emitResponse(0x1F, r, 6);
        }
        break;
      }
      case 0x1C: {  // ReadEmulator: echo request, then page data
        uint8_t r[5 + 4] = {p[1], p[2], p[3], p[4], 0x00};
        memcpy(&r[5], &g_model->slotImage[p[4] * 4], 4);
        g_model->emitResponse(0x1D, r, 9);
        break;
      }
      default:
        break;
    }
  }
};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static FakeSerial serial;
static Pn532Uart nfc;

static void resetBoard() {
  delete g_model;
  g_model = new KillerModel();
  g_model->toHost.clear();
  g_model->captured.clear();
  g_model->parsePos = 0;
  g_fakeMillis = 0;
}

// Computes the expected wire bytes for a payload independently of the driver.
static std::vector<uint8_t> expectedFrame(const uint8_t *payload, size_t n) {
  std::vector<uint8_t> f;
  size_t len = n + 1;
  f.push_back(0x00); f.push_back(0x00); f.push_back(0xFF);
  f.push_back((uint8_t)len);
  f.push_back((uint8_t)(~len + 1));
  f.push_back(0xD4);
  uint8_t dcs = 0xD4;
  for (size_t i = 0; i < n; i++) { f.push_back(payload[i]); dcs += payload[i]; }
  f.push_back((uint8_t)(~dcs + 1));
  f.push_back(0x00);
  return f;
}

// The most recent host->device frame (begin() probes first, so compare tails).
static bool lastFrameMatches(const std::vector<uint8_t>& want) {
  const std::vector<uint8_t>& got = g_model->captured;
  if (got.size() < want.size()) return false;
  return memcmp(got.data() + got.size() - want.size(), want.data(), want.size()) == 0;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void testWakeAndFirmware() {
  resetBoard();
  CHECK(nfc.begin(serial));
  CHECK_EQ(nfc.getFirmwareVersion(), 0x32010607u);
  // wake() flushes, then GetFirmwareVersion: payload [02].
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\x02", 1);
  CHECK(lastFrameMatches(want));
}

static void testSamConfigAndRf() {
  resetBoard();
  CHECK(nfc.begin(serial));
  CHECK(nfc.samConfig());
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\x14\x01\x14\x01", 4);
  CHECK(lastFrameMatches(want));

  resetBoard();
  CHECK(nfc.begin(serial));
  CHECK(nfc.setPassiveActivationRetries(2));
  want = expectedFrame((const uint8_t *)"\x32\x05\x00\x00\x02", 5);
  CHECK(lastFrameMatches(want));
}

static void testReadPassiveTarget() {
  resetBoard();
  g_model->passiveTargetPresent = true;
  CHECK(nfc.begin(serial));
  uint8_t uid[7];
  uint8_t uidLen = 0;
  CHECK(nfc.readPassiveTarget(uid, &uidLen, 200));
  CHECK_EQ(uidLen, 7);
  CHECK(memcmp(uid, g_model->targetUid, 7) == 0);
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\x4A\x01\x00", 3);
  CHECK(lastFrameMatches(want));

  // No target in the field.
  resetBoard();
  CHECK(nfc.begin(serial));
  CHECK(!nfc.readPassiveTarget(uid, &uidLen, 200));
}

static void testInDataExchangeNtag() {
  resetBoard();
  CHECK(nfc.begin(serial));

  uint8_t page16[16];
  CHECK(nfc.ntagReadPage(3, page16));
  // Model fills each page with its own marker byte.
  CHECK_EQ(page16[0], 3); CHECK_EQ(page16[4], 4); CHECK_EQ(page16[8], 5); CHECK_EQ(page16[12], 6);
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\x40\x01\x30\x03", 4);
  CHECK(lastFrameMatches(want));

  resetBoard();
  CHECK(nfc.begin(serial));
  const uint8_t data[4] = {0xAA, 0xBB, 0xCC, 0xDD};
  CHECK(nfc.ntagWritePage(7, data));
  want = expectedFrame((const uint8_t *)"\x40\x01\xA2\x07\xAA\xBB\xCC\xDD", 8);
  CHECK(lastFrameMatches(want));
}

static void testKillerVendorCommands() {
  resetBoard();
  CHECK(nfc.begin(serial));

  CHECK(nfc.killerSetWorkMode(KILLER_MODE_EMULATOR, KILLER_TAG_NTAG_MFU, 0));
  CHECK_EQ(g_model->lastWorkMode[0], 0x02);
  CHECK_EQ(g_model->lastWorkMode[1], 0x02);
  CHECK_EQ(g_model->lastWorkMode[2], 0x00);
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\xAC\x02\x02\x00", 4);
  CHECK(lastFrameMatches(want));

  resetBoard();
  CHECK(nfc.begin(serial));
  const uint8_t page[4] = {0x04, 0x12, 0x34, 0x56};
  CHECK(nfc.killerWriteEmulatorPage(KILLER_TAG_NTAG_MFU, 0, 1, page));
  want = expectedFrame((const uint8_t *)"\x1E\x02\x00\x00\x01\x04\x12\x34\x56", 9);
  CHECK(lastFrameMatches(want));

  // Read-back helper.
  uint8_t rb[4];
  CHECK(nfc.killerReadEmulatorPage(KILLER_TAG_NTAG_MFU, 0, 1, rb));
  CHECK(memcmp(rb, page, 4) == 0);
}

static void testLoadNtagImage() {
  resetBoard();
  CHECK(nfc.begin(serial));

  // Distinctive 540-byte image (each page gets its low byte as marker).
  uint8_t image[540];
  for (size_t p = 0; p < 135; p++) {
    image[p * 4 + 0] = (uint8_t)p;
    image[p * 4 + 1] = 0xA0;
    image[p * 4 + 2] = 0x0A;
    image[p * 4 + 3] = (uint8_t)(p ^ 0xFF);
  }
  CHECK(nfc.killerLoadNtagImage(KILLER_TAG_NTAG_MFU, 0, image));
  CHECK_EQ(g_model->writePageCalls, 135);
  CHECK(g_model->doneReceived);
  CHECK(memcmp(g_model->slotImage, image, 540) == 0);
  // The last frame written is the "upload done" marker.
  std::vector<uint8_t> want = expectedFrame((const uint8_t *)"\x1E\x02\x00\xFF\xFF", 5);
  CHECK(lastFrameMatches(want));
}

static void testLoadNtagImageV3() {
  resetBoard();
  CHECK(nfc.begin(serial));

  // v3 (Air Riders) personality: 256 pages, including the pages past the
  // NTAG215 range (config 0xE2-0xE6, SRAM mirror 0xF0-0xFF).
  uint8_t image[1024];
  for (size_t p = 0; p < 256; p++) {
    image[p * 4 + 0] = (uint8_t)p;
    image[p * 4 + 1] = 0xB1;
    image[p * 4 + 2] = 0x1B;
    image[p * 4 + 3] = (uint8_t)(p ^ 0xFF);
  }
  CHECK(nfc.killerLoadNtagImageV3(0, image));
  CHECK_EQ(g_model->writePageCalls, 256);
  CHECK(g_model->doneReceived);
  CHECK(memcmp(g_model->slotImage, image, 1024) == 0);
}

static void testLoadNtagImageV3Rejected() {
  resetBoard();
  g_model->v3Capable = false;  // stock NTAG215-only Killer firmware
  CHECK(nfc.begin(serial));
  uint8_t image[1024] = {0};
  // Pages 0-134 load, page 135 gets no answer: the driver must give up and
  // report failure instead of half-loading a tag.
  CHECK(nfc.killerLoadNtagImageV3(0, image) == false);
}

static void testAckResync() {
  resetBoard();
  g_model->garbageBeforeAck = true;
  CHECK(nfc.begin(serial));  // probes with GetFirmwareVersion
}

static void testTimeouts() {
  resetBoard();
  g_model->silent = true;
  CHECK(nfc.begin(serial) == false);

  // After a silent phase, the board answers again.
  g_model->silent = false;
  CHECK(nfc.getFirmwareVersion() != 0);
}

static void testCorruptDataChecksum() {
  resetBoard();
  g_model->corruptDcs = true;
  // ACK arrives but the response frame's DCS is wrong: transaction must fail.
  const uint8_t cmd[1] = {0x02};
  CHECK(!nfc.transact(cmd, 1, 200));
}

static void testFrameSizeGuard() {
  resetBoard();
  CHECK(nfc.begin(serial));
  uint8_t big[261];
  CHECK(!nfc.sendFrame(big, sizeof(big)));
}

int main() {
  resetBoard();
  serial = FakeSerial();

  testWakeAndFirmware();
  testSamConfigAndRf();
  testReadPassiveTarget();
  testInDataExchangeNtag();
  testKillerVendorCommands();
  testLoadNtagImage();
  testLoadNtagImageV3();
  testLoadNtagImageV3Rejected();
  testAckResync();
  testTimeouts();
  testCorruptDataChecksum();
  testFrameSizeGuard();

  if (failures == 0) {
    std::printf("test_pn532_uart: %d checks passed\n", checks);
    return 0;
  }
  std::printf("test_pn532_uart: %d/%d checks FAILED\n", failures, checks);
  return 1;
}
