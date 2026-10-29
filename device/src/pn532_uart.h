// OpenTag: PN532 / PN532Killer HSU (high-speed UART) driver.
//
// The PN532Killer presents the classic PN532 UART framing on its 3.3 V TTL
// header (J4: RX, TX, GND, VCC). In its "PN532 mode" it accepts the standard
// PN532 host command set (firmware version, passive target polling, data
// exchange), plus vendor commands that push a page image into one of its
// eight emulator slots and start/stop emulation:
//
//   SetWorkMode      0xAC [mode type index]   mode: 01 PN532, 02 emulator,
//                                                03 sniffer; emulator type 02
//                                                = NTAG/MFU; index = slot 0-7
//   WriteEmulator    0x1E type slot sector block data..   (MFU: sector 0,
//                                                block = page, 4 bytes/page;
//                                                sector FF FF ends the upload)
//   ReadEmulator     0x1C type slot sector block
//
// Host frame:  00 00 FF LEN LCS D4 <payload> DCS 00   (LEN = 1 + payload len)
// ACK:         00 00 FF 00 FF 00
// Response:    00 00 FF LEN LCS D5 <code+1> <payload> DCS 00
#ifndef OPENTAG_PN532_UART_H_
#define OPENTAG_PN532_UART_H_

#include <Arduino.h>

#ifndef OPENTAG_NFC_UART_RX_BUF
#define OPENTAG_NFC_UART_RX_BUF 512
#endif

// Work modes (PN532Killer SetWorkMode).
enum KillerWorkMode : uint8_t {
  KILLER_MODE_PN532 = 0x01,
  KILLER_MODE_EMULATOR = 0x02,
  KILLER_MODE_SNIFFER = 0x03,
};

// Emulator tag types (GUI enum).
enum KillerTagType : uint8_t {
  KILLER_TAG_MIFARE_CLASSIC = 0x01,
  KILLER_TAG_NTAG_MFU = 0x02,
  KILLER_TAG_EM4100 = 0x03,
  KILLER_TAG_ISO15693 = 0x04,
  KILLER_TAG_T5557 = 0x05,
};

class Pn532Uart {
 public:
  typedef bool (*MillisFn)(uint32_t&);

  bool begin(Stream& serial, uint32_t ackTimeoutMs = 40) {
    serial_ = &serial;
    ackTimeoutMs_ = ackTimeoutMs;
    return wake();
  }

  // --- standard PN532 commands -------------------------------------------

  // Returns 0 unless a valid response was received (PN532Killer answers with
  // a PN532-compatible version block).
  uint32_t getFirmwareVersion() {
    const uint8_t cmd[1] = {0x02};  // GetFirmwareVersion
    if (!transact(cmd, sizeof(cmd), 100)) return 0;
    if (frameLen_ < 1 || frame_[0] != 0x03) return 0;
    uint32_t v = 0;
    for (size_t i = 1; i < 5 && i < frameLen_; i++) v = (v << 8) | frame_[i];
    return v;
  }

  bool samConfig() {
    const uint8_t cmd[4] = {0x14, 0x01, 0x14, 0x01};  // normal mode, timeouts
    return transact(cmd, sizeof(cmd), 100) && frame_[0] == 0x15;
  }

  // Bounds the time InListPassiveTarget spends polling before giving up.
  bool setPassiveActivationRetries(uint8_t retries) {
    const uint8_t cmd[5] = {0x32, 0x05, 0x00, 0x00, retries};  // RFConfiguration
    return transact(cmd, sizeof(cmd), 100) && frame_[0] == 0x33;
  }

  // Polls for one Type-A target at 106 kbps. `timeoutMs` bounds the host-side
  // wait; the poll itself is bounded by the activation retry count.
  bool readPassiveTarget(uint8_t uid[7], uint8_t* uidLen, uint16_t timeoutMs) {
    const uint8_t cmd[3] = {0x4A, 0x01, 0x00};  // InListPassiveTarget, Type A
    if (!sendFrame(cmd, sizeof(cmd))) return false;
    if (!waitAck(timeoutMs)) return false;
    if (!readResponseFrame(0x4B, timeoutMs + 100)) return false;
    if (frameLen_ < 2 || frame_[1] == 0x00) return false;  // nbTg == 0: none found
    // [nbTg][sensRes 2][selRes 1][idLen][id]...
    size_t i = 2 + 2 + 1;
    if (frameLen_ <= i) return false;
    uint8_t n = frame_[i++];
    if (n > 7 || i + n > frameLen_) return false;
    memcpy(uid, &frame_[i], n);
    *uidLen = n;
    return n > 0;
  }

  // Raw ISO14443 exchange (e.g. NTAG2xx READ/WRITE commands). `rx` must have
  // room for 265 bytes; the status byte is stripped from the payload.
  bool inDataExchange(const uint8_t* tx, size_t txLen, uint8_t* rx, size_t* rxLen) {
    uint8_t cmd[270];
    if (txLen > 265) return false;
    cmd[0] = 0x40;  // InDataExchange
    cmd[1] = 0x01;  // logical target 1
    memcpy(&cmd[2], tx, txLen);
    if (!transact(cmd, txLen + 2, 150)) return false;
    if (frameLen_ < 2 || frame_[1] != 0x00) return false;
    size_t n = frameLen_ - 2;
    if (n > 0) memmove(rx, &frame_[2], n);
    *rxLen = n;
    return true;
  }

  // --- NTAG2xx helpers over InDataExchange -------------------------------

  // NTAG READ returns 4 pages (16 bytes) starting at `page`.
  bool ntagReadPage(uint8_t page, uint8_t out[16]) {
    const uint8_t read[2] = {0x30, page};
    uint8_t rx[32];
    size_t rxLen = 0;
    if (!inDataExchange(read, sizeof(read), rx, &rxLen) || rxLen < 16) return false;
    memcpy(out, rx, 16);
    return true;
  }

  // NTAG WRITE programs one 4-byte page and verifies the ACK.
  bool ntagWritePage(uint8_t page, const uint8_t data[4]) {
    const uint8_t write[6] = {0xA2, page, data[0], data[1], data[2], data[3]};
    uint8_t rx[32];
    size_t rxLen = 0;
    if (!inDataExchange(write, sizeof(write), rx, &rxLen)) return false;
    return rxLen == 1 && rx[0] == 0x0A;  // ACK
  }

  // --- PN532Killer vendor commands ----------------------------------------

  // Switch the board between PN532, emulator and sniffer personalities. The
  // board requires a pass through PN532 mode when changing personalities.
  bool killerSetWorkMode(KillerWorkMode mode, uint8_t type = 0, uint8_t slot = 0) {
    const uint8_t cmd[4] = {0xAC, (uint8_t)mode, type, slot};
    return transact(cmd, sizeof(cmd), 300) && frame_[0] == 0xAD && frame_[1] == 0x00;
  }

  // Queues one MFU page (4 bytes) in the given emulator slot.
  bool killerWriteEmulatorPage(KillerTagType type, uint8_t slot, uint8_t page,
                               const uint8_t data[4]) {
    const uint8_t cmd[9] = {0x1E, (uint8_t)type, slot, 0x00, page,
                            data[0], data[1], data[2], data[3]};
    return transact(cmd, sizeof(cmd), 150) && frame_[0] == 0x1F &&
           frame_[frameLen_ - 1] == 0x00;
  }

  // Closes the upload: the emulator re-sizes itself from the CC.
  bool killerWriteEmulatorDone(KillerTagType type, uint8_t slot) {
    const uint8_t cmd[5] = {0x1E, (uint8_t)type, slot, 0xFF, 0xFF};
    return transact(cmd, sizeof(cmd), 150) && frame_[0] == 0x1F;
  }

  // Reads back one MFU page from an emulator slot (verify helper). The
  // response echoes the request and ends with the 4 data bytes.
  bool killerReadEmulatorPage(KillerTagType type, uint8_t slot, uint8_t page,
                              uint8_t out[4]) {
    const uint8_t cmd[5] = {0x1C, (uint8_t)type, slot, 0x00, page};
    if (!transact(cmd, sizeof(cmd), 150) || frame_[0] != 0x1D) return false;
    if (frameLen_ < 4) return false;
    memcpy(out, &frame_[frameLen_ - 4], 4);  // last 4 bytes = page data
    return true;
  }

  // Convenience: pushes a full 540-byte NTAG215 image (135 pages, UID pages
  // included) into `slot` and closes the upload. ~135 small frames at
  // 115200 baud, well under a second.
  bool killerLoadNtagImage(KillerTagType type, uint8_t slot, const uint8_t image[540]) {
    for (size_t page = 0; page < 135; page++) {
      if (!killerWriteEmulatorPage(type, slot, (uint8_t)page, &image[page * 4]))
        return false;
    }
    return killerWriteEmulatorDone(type, slot);
  }

  // v3 amiibo (Air Riders generation, NTAG I2C Plus 2K personality): loads
  // pages 0x00-0xFF — everything the tag presents without a sector select.
  // Best effort by design: the Killer's documented profiles are NTAG213/215/
  // 216, so firmware that rejects pages past the NTAG215 range fails here
  // (returning false) and the UI reports that this build can't do v3 tags.
  bool killerLoadNtagImageV3(uint8_t slot, const uint8_t image[1024]) {
    for (size_t page = 0; page < 256; page++) {
      if (!killerWriteEmulatorPage(KILLER_TAG_NTAG_MFU, slot, (uint8_t)page,
                                   &image[page * 4]))
        return false;
    }
    return killerWriteEmulatorDone(KILLER_TAG_NTAG_MFU, slot);
  }

  // --- plumbing -----------------------------------------------------------

  // Flushes the input and probes for a live PN532/Killer a few times.
  bool wake() {
    while (serial_->available()) serial_->read();
    for (int attempt = 0; attempt < 3; attempt++) {
      if (getFirmwareVersion() != 0) return true;
      delay(120);
    }
    return false;
  }

  void flushInput() {
    while (serial_->available()) serial_->read();
  }

  // Sends a frame and waits for ACK + response whose code must equal
  // payload[0] + 1. `timeoutMs` bounds the whole transaction.
  bool transact(const uint8_t* payload, size_t len, uint16_t timeoutMs) {
    if (!sendFrame(payload, len)) return false;
    if (!waitAck(timeoutMs)) return false;
    return readResponseFrame(payload[0] + 1, timeoutMs);
  }

  // Builds and writes the framed host message.
  bool sendFrame(const uint8_t* payload, size_t len) {
    if (len > 260) return false;
    uint8_t frame[8 + 260];
    size_t n = 0;
    const size_t pktLen = len + 1;  // D4 + payload
    frame[n++] = 0x00;
    frame[n++] = 0x00;
    frame[n++] = 0xFF;
    frame[n++] = (uint8_t)(pktLen & 0xFF);
    frame[n++] = (uint8_t)((~pktLen + 1) & 0xFF);
    frame[n++] = 0xD4;
    uint8_t dcs = 0xD4;
    for (size_t i = 0; i < len; i++) {
      frame[n++] = payload[i];
      dcs += payload[i];
    }
    frame[n++] = (uint8_t)((~dcs + 1) & 0xFF);
    frame[n++] = 0x00;
    serial_->write(frame, n);
    return true;
  }

  // Waits for the 6-byte ACK frame. Any leading garbage is skipped; a NACK
  // frame or timeout fails the transaction.
  bool waitAck(uint32_t timeoutMs) {
    uint32_t start = millis();
    uint8_t need = 6, got = 0;
    const uint8_t kAck[6] = {0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00};
    while (millis() - start < timeoutMs) {
      while (serial_->available()) {
        uint8_t b = (uint8_t)serial_->read();
        if (b == kAck[got]) {
          got++;
          if (got == need) return true;
        } else if (b == kAck[0]) {
          got = 1;
        } else {
          got = 0;
        }
      }
      delay(1);
    }
    return false;
  }

  // Collects a D5 response frame, verifying length and data checksums and
  // the expected response code. Result payload (code included) is left in
  // frame_/frameLen_.
  bool readResponseFrame(uint8_t expectedCode, uint32_t timeoutMs) {
    uint32_t start = millis();
    uint8_t hdr[5];  // 00 00 FF LEN LCS
    size_t hdrLen = 0;
    while (millis() - start < timeoutMs) {
      while (serial_->available()) {
        uint8_t b = (uint8_t)serial_->read();
        // Hunt for the preamble + start code.
        if (hdrLen < 2) {
          if (b == 0x00) {
            if (hdrLen == 1) hdrLen = 2;
            else hdrLen = 1;
          } else {
            hdrLen = 0;
          }
          continue;
        }
        hdr[hdrLen++] = b;  // fill hdr[2..4] = FF LEN LCS
        if (hdrLen == 5) {
          if (hdr[2] != 0xFF) {
            hdrLen = 0;
            continue;
          }
          size_t len = hdr[3];  // D5 + code + payload
          uint8_t lcs = hdr[4];
          if ((uint8_t)(len + lcs) != 0) {  // LCS check
            hdrLen = 0;
            continue;
          }
          if (len < 1 || len > OPENTAG_NFC_UART_RX_BUF - 1) {  // need the code
            hdrLen = 0;
            continue;
          }
          // D5 | code+payload (len bytes) | DCS | postamble
          uint8_t d5, dcs, post;
          if (!readExact(&d5, 1, 150)) return false;
          if (!readExact(frame_, len, 150)) return false;
          if (!readExact(&dcs, 1, 150)) return false;
          if (!readExact(&post, 1, 150)) return false;  // consume postamble
          uint8_t sum = 0xD5;  // the response checksum covers D5 too
          for (size_t i = 0; i < len; i++) sum += frame_[i];
          if ((uint8_t)(sum + dcs) != 0) {  // DCS check
            hdrLen = 0;
            continue;
          }
          frameLen_ = len;
          if (frame_[0] != expectedCode) return false;
          return true;
        }
      }
      delay(1);
    }
    return false;
  }

  // Reads exactly `n` bytes within the timeout (frame body + DCS + postamble).
  bool readExact(uint8_t* out, size_t n, uint32_t timeoutMs) {
    uint32_t start = millis();
    size_t got = 0;
    while (got < n && millis() - start < timeoutMs) {
      while (serial_->available() && got < n) out[got++] = (uint8_t)serial_->read();
      if (got < n) delay(1);
    }
    return got == n;
  }

  // Last response payload (first byte is the response code).
  const uint8_t* lastResponse() const { return frame_; }
  size_t lastResponseLen() const { return frameLen_; }

 private:
  Stream* serial_ = nullptr;
  uint32_t ackTimeoutMs_ = 40;
  uint8_t frame_[OPENTAG_NFC_UART_RX_BUF];
  size_t frameLen_ = 0;
};

#endif  // OPENTAG_PN532_UART_H_
