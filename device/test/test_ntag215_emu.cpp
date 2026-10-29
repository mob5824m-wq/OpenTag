// Host-native tests for ntag215.h: page-image building, dump validation and
// normalization. Build & run:
//   g++ -std=c++11 -Wall -Wextra -pedantic -I ../src test_ntag215_emu.cpp && ./a.out
#include "../src/ntag215_emu.h"

#include <cstdio>
#include <cstring>

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

using namespace ntag215;

static void testImageBuilderBasics() {
  uint8_t uid[7] = {0x04, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66};
  ImageBuilder b;
  b.begin(uid);

  CHECK(memcmp(b.page(0), uid, 3) == 0);
  CHECK_EQ(b.page(0)[3], bcc0(uid));
  CHECK(memcmp(b.page(1), uid + 3, 4) == 0);
  CHECK(memcmp(b.page(2), "\x00\x00\x00\x00", 4) == 0);
  CHECK(memcmp(b.page(3), kCapabilityContainer, 4) == 0);
  CHECK(memcmp(b.page(4), "\x03\x00\xFE\x00", 4) == 0);      // empty TLV
  CHECK(memcmp(b.page(130), "\x00\x00\x00\xBD", 4) == 0);    // dynamic lock
  CHECK_EQ(b.page(131)[3], 0xFF);                            // AUTH0: no auth
  CHECK(memcmp(b.page(133), "\x00\x00\x00\x00", 4) == 0);    // PWD unused
  CHECK_EQ(b.ndefLength(), 3u);

  uint8_t image[kImageBytes];
  b.exportImage(image);
  CHECK_EQ(image[3], 0x88 ^ uid[0] ^ uid[1] ^ uid[2]);
  CHECK(memcmp(image + 4, uid + 3, 4) == 0);  // page 1 = UID3..6
  CHECK(memcmp(image + 12, kCapabilityContainer, 4) == 0);
}

static void testSetNdefTlv() {
  uint8_t uid[7] = {0x04, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
  ImageBuilder b;
  b.begin(uid);

  uint8_t tlv[16];
  memset(tlv, 0xAB, sizeof(tlv));
  tlv[0] = 0x03;
  tlv[1] = 0x0D;  // 13-byte payload
  tlv[15] = 0xFE;
  CHECK(b.setNdefTlv(tlv, sizeof(tlv)));
  CHECK_EQ(b.ndefLength(), 16u);
  CHECK(memcmp(b.page(4), tlv, 16) == 0);  // spans pages 4..7
  CHECK(memcmp(b.page(8), "\x00\x00\x00\x00", 4) == 0);

  // Oversized TLV must be rejected (user area is 504 bytes).
  uint8_t big[kUserBytes + 1];
  CHECK(!b.setNdefTlv(big, sizeof(big)));
  // Previous content survives a rejected update.
  CHECK(memcmp(b.page(4), tlv, 16) == 0);

  // The exported image re-validates as a dump.
  uint8_t image[kImageBytes];
  b.exportImage(image);
  const char *why = nullptr;
  CHECK(validateDump(image, sizeof(image), &why));
  CHECK(why == nullptr);
}

// Builds a syntactically valid NTAG215 dump (amiibo-shaped).
static void makeDump(uint8_t out[kImageBytes]) {
  uint8_t uid[7] = {0x04, 0x1A, 0x2B, 0x3C, 0x4D, 0x5E, 0x6F};
  ImageBuilder b;
  b.begin(uid);
  b.exportImage(out);
}

// v3 (Air Riders generation) dumps: 1024-byte page images whose figure ID
// (pages 0x15-0x16, byte 0x5B) ends in 0x03. Real rips carry whatever BCC
// their tag has, so only the vendor byte and the v3 marker are checked.
static void makeV3Dump(uint8_t out[kV3ImageBytes]) {
  memset(out, 0, kV3ImageBytes);
  out[0] = 0x04;                       // NXP
  memcpy(out + 0x54, "\x1F\x00\x00\x00\x04\xC4\x1E\x03", 8);  // Kirby & Warp Star
  memcpy(out + 0x80, "\x02\x00\x7F\xCB\x95\x65\x61\xAF", 8);  // SRAM-style data
}

static void testValidateV3Dump() {
  uint8_t dump[kV3ImageBytes];
  makeV3Dump(dump);

  const char *why = nullptr;
  CHECK(validateV3Dump(dump, sizeof(dump), &why));
  CHECK(why == nullptr);

  why = "x";
  CHECK(!validateV3Dump(dump, 1023, &why));
  CHECK(why && std::strcmp(why, "short") == 0);

  CHECK(!validateV3Dump(dump, sizeof(dump) - 1, &why));

  dump[0x5B] = 0x02;  // v2 figure: not a v3 dump
  CHECK(!validateV3Dump(dump, sizeof(dump), &why));
  CHECK(why && std::strcmp(why, "v3") == 0);
  dump[0x5B] = 0x03;

  dump[0] = 0x11;  // not NXP
  CHECK(!validateV3Dump(dump, sizeof(dump), &why));
  CHECK(why && std::strcmp(why, "vendor") == 0);

  // A v2 (540-byte) dump is never a valid v3 dump.
  uint8_t v2[kImageBytes];
  makeDump(v2);
  CHECK(!validateV3Dump(v2, sizeof(v2)));
}

static void testValidateDump() {
  uint8_t dump[kImageBytes];
  makeDump(dump);

  const char *why = nullptr;
  CHECK(validateDump(dump, sizeof(dump), &why));
  CHECK(why == nullptr);

  // Truncated / oversized containers.
  why = "x";
  CHECK(!validateDump(dump, 531, &why));
  CHECK(why && std::strcmp(why, "short") == 0);
  CHECK(!validateDump(dump, 573, &why));
  CHECK(why && std::strcmp(why, "long") == 0);

  // 532-byte dumps (no PWD/PACK) and 572-byte dumps (N2-style, +32 trailer)
  // both validate.
  CHECK(validateDump(dump, 532, &why));
  CHECK(validateDump(dump, 572, &why));

  // UID checksum (BCC0 is stored at byte 3; BCC1 only exists in
  // anticollision frames and cannot be checked from a dump).
  uint8_t bad[kImageBytes];
  makeDump(bad);
  bad[3] ^= 1;
  CHECK(!validateDump(bad, sizeof(bad), &why));
  CHECK(why && std::strcmp(why, "bcc0") == 0);

  // Wrong capability container (NTAG216 etc.).
  makeDump(bad);
  bad[13] = 0x6D;
  CHECK(!validateDump(bad, sizeof(bad), &why));
  CHECK(why && std::strcmp(why, "cc") == 0);

  // Non-NXP vendor byte.
  makeDump(bad);
  bad[0] = 0x11;
  bad[3] = bcc0(bad);
  CHECK(!validateDump(bad, sizeof(bad), &why));
  CHECK(why && std::strcmp(why, "vendor") == 0);
}

static void testNormalizeDump() {
  uint8_t dump[kImageBytes];
  makeDump(dump);

  uint8_t out[kImageBytes];
  normalizeDump(dump, 532, out);
  CHECK(memcmp(out, dump, 532) == 0);
  CHECK(memcmp(out + 532, "\x00\x00\x00\x00\x00\x00\x00\x00", 8) == 0);

  // A 572-byte container: only the first 540 bytes are kept.
  uint8_t big[572];
  memcpy(big, dump, kImageBytes);
  memset(big + kImageBytes, 0xEE, 32);
  normalizeDump(big, sizeof(big), out);
  CHECK(memcmp(out, dump, kImageBytes) == 0);
}

static void testFacts() {
  CHECK_EQ(kPageCount, 135u);
  CHECK_EQ(kImageBytes, 540u);
  CHECK_EQ(kUserBytes, 504u);
  CHECK_EQ(kUidBytes, 7u);
  const uint8_t version[8] = {0x00, 0x04, 0x04, 0x02, 0x01, 0x00, 0x11, 0x03};
  CHECK(memcmp(kVersionInfo, version, 8) == 0);
  CHECK(memcmp(kCapabilityContainer, "\xE1\x10\x3E\x00", 4) == 0);
}

int main() {
  testFacts();
  testImageBuilderBasics();
  testSetNdefTlv();
  testValidateDump();
  testValidateV3Dump();
  testNormalizeDump();
  if (failures == 0) {
    std::printf("test_ntag215: %d checks passed\n", checks);
    return 0;
  }
  std::printf("test_ntag215: %d/%d checks FAILED\n", failures, checks);
  return 1;
}
