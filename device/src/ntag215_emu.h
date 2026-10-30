// OpenTag: NTAG215 page-image builder and dump validation.
//
// The NFC front end (PN532Killer) emulates a complete NTAG215 from a 540-byte
// page image (135 pages x 4 bytes). This module builds such images for the
// NDEF note feature and validates uploaded amiibo dumps before they are
// stored or emulated.
//
// Layout (NXP NTAG21x):
//   page 0    UID0..UID2, BCC0 (BCC0 = 0x88 ^ UID0 ^ UID1 ^ UID2)
//   page 1    UID3..UID6 (7-byte UIDs; BCC1 = UID3^UID4^UID5^UID6 follows in
//             anticollision, not stored)
//   page 2    internal/RFUI, static lock bytes
//   page 3    Capability Container (E1 10 3E 00 = NDEF-mapped, 496 B)
//   pages 4..129  user area with the NDEF Message TLV
//   page 130  dynamic lock bytes + RFUI (0xBD)
//   page 131  CFG0 (AUTH0 at byte 3)
//   page 133  CFG1: PWD    page 134: PACK
//
// Pure, host-testable C++ (no Arduino dependencies).
#ifndef OPENTAG_NTAG215_EMU_H_
#define OPENTAG_NTAG215_EMU_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace ntag215 {

static const size_t kPageCount = 135;       // pages 0..134
static const size_t kPageSize = 4;           // 135 * 4 = 540 bytes
static const size_t kImageBytes = kPageCount * kPageSize;
static const size_t kUserFirstPage = 4;      // start of the 504-byte user area
static const size_t kUserLastPage = 129;
static const size_t kUserBytes = (kUserLastPage - kUserFirstPage + 1) * kPageSize;
static const size_t kUidBytes = 7;           // NTAG215 always has a 7-byte UID

// NTAG215 identity: NXP vendor, NTAG product, 540-byte storage, Type 2 protocol.
static const uint8_t kVersionInfo[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
// Capability Container: NDEF-mapped, v1.0, 496 bytes of NDEF user memory, R/W.
static const uint8_t kCapabilityContainer[4] = {0xE1, 0x10, 0x3E, 0x00};

inline uint8_t bcc0(const uint8_t uid[7]) {
  return 0x88 ^ uid[0] ^ uid[1] ^ uid[2];
}
inline uint8_t bcc1(const uint8_t uid[7]) {
  return uid[3] ^ uid[4] ^ uid[5] ^ uid[6];
}

// Validates a raw NTAG215 dump of `len` bytes (532, 540 or 572-byte container
// formats are accepted; only the first 540 bytes are used). On failure,
// returns false and writes a short reason into `why` (may be null).
inline bool validateDump(const uint8_t *dump, size_t len, const char **why = nullptr) {
  static const char *kTooShort = "short";
  static const char *kTooLong = "long";
  static const char *kBcc0 = "bcc0";
  static const char *kCc = "cc";
  static const char *kVendor = "vendor";
  if (why) *why = nullptr;
  if (len < 532) {
    if (why) *why = kTooShort;
    return false;
  }
  if (len > 572) {
    if (why) *why = kTooLong;
    return false;
  }
  // BCC0 (byte 3) is stored; BCC1 only exists in anticollision frames, so it
  // cannot be checked here.
  if (bcc0(dump) != dump[3]) {
    if (why) *why = kBcc0;
    return false;
  }
  // page 3 must be an NTAG215 CC (E1 10 3E 00): NTAG216 or Classic dumps are
  // out of scope.
  if (memcmp(dump + 12, kCapabilityContainer, 4) != 0) {
    if (why) *why = kCc;
    return false;
  }
  if (dump[0] != 0x04) {  // NXP manufacturer ID
    if (why) *why = kVendor;
    return false;
  }
  return true;
}

// Copies a dump of any accepted length into a full 540-byte page image,
// zero-padding what is missing (532-byte dumps lack PWD/PACK).
inline void normalizeDump(const uint8_t *dump, size_t len, uint8_t out[kImageBytes]) {
  memset(out, 0, kImageBytes);
  const size_t n = (len < kImageBytes) ? len : kImageBytes;
  memcpy(out, dump, n);
}

// ---- v3 amiibo (Kirby Air Riders generation) -------------------------------
// The Air Riders figures are "amiibo v3" on NTAG I2C Plus 2K tags: same
// NXP/NFC Forum Type 2 command set, but a different memory map (64 extra
// bytes at 0x80, the encrypted block shifted to 0xA0, config pages 0xE2-0xE6
// and an SRAM buffer at 0xF0-0xFF). Research: N3evin/AmiiboAPI issue #243
// (xSke's teardown). Emulators work from a "dumb dump": the SRAM response
// pre-loaded at 0xF0-0xFF and page 0xED's NS_RED already in the ready state,
// which is what the dumps circulating in collections look like.
// Pages 0x00-0xFF (1024 bytes) cover everything reachable without a sector
// select; the trailing figure-ID byte is the format version (0x02 = v2,
// 0x03 = v3). Real rips carry whatever BCC their tag has, so only the
// vendor byte and the v3 marker are checked.
static const size_t kV3PageCount = 256;
static const size_t kV3ImageBytes = kV3PageCount * kPageSize;  // 1024
inline bool validateV3Dump(const uint8_t *dump, size_t len, const char **why = nullptr) {
  static const char *kTooShort = "short";
  static const char *kNotV3 = "v3";
  static const char *kVendor = "vendor";
  if (why) *why = nullptr;
  if (len < kV3ImageBytes) {
    if (why) *why = kTooShort;
    return false;
  }
  if (dump[0] != 0x04) {  // NXP manufacturer ID
    if (why) *why = kVendor;
    return false;
  }
  if (dump[0x5B] != 0x03) {  // figure ID (pages 0x15-0x16) format byte
    if (why) *why = kNotV3;
    return false;
  }
  return true;
}

// Builds a read-only NDEF presentation image: the given 7-byte UID, an
// NTAG215 CC, the NDEF Message TLV in the user area and an unprotected
// configuration (AUTH0 = 0xFF). `tlv` comes from makeNdefTlv() (ndef_tlv.h).
class ImageBuilder {
 public:
  void begin(const uint8_t uid[7]) {
    memset(pages_, 0, sizeof(pages_));
    // The 7-byte UID is split: UID0..2 in page 0 (followed by BCC0),
    // UID3..6 in page 1.
    memcpy(pages_[0], uid, 3);
    pages_[0][3] = bcc0(uid);
    memcpy(pages_[1], uid + 3, 4);
    // page 2: internal/RFUI + static lock bytes (0x0000 = nothing locked)
    memcpy(pages_[3], kCapabilityContainer, kPageSize);
    pages_[4][0] = 0x03;                    // empty NDEF Message TLV
    pages_[4][1] = 0x00;
    pages_[4][2] = 0xFE;
    pages_[130][3] = 0xBD;                  // dynamic lock + RFUI
    pages_[131][3] = 0xFF;                  // AUTH0: no PWD_AUTH required
    ndefLen_ = 3;
  }

  bool setNdefTlv(const uint8_t *tlv, size_t len) {
    if (len == 0 || len > kUserBytes) return false;
    memset(&pages_[kUserFirstPage][0], 0, kUserBytes);
    memcpy(&pages_[kUserFirstPage][0], tlv, len);
    ndefLen_ = len;
    return true;
  }

  void exportImage(uint8_t out[kImageBytes]) const {
    memcpy(out, &pages_[0][0], kImageBytes);
  }

  size_t ndefLength() const { return ndefLen_; }
  const uint8_t *page(size_t p) const { return pages_[p]; }

 private:
  uint8_t pages_[kPageCount][kPageSize];
  size_t ndefLen_ = 0;
};

}  // namespace ntag215

#endif  // OPENTAG_NTAG215_EMU_H_
