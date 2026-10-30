// OpenTag: amiibo retail-dump re-signing (TagMo "WRITE TAG (AUTO)" equivalent).
//
// This is a portable re-implementation of the signing pipeline documented by
// socram8888/amiitool (MIT license, (c) 2017 Marcos Del Sol Vives), which is
// the same algorithm the TagMo Android app uses through its bundled library.
// The crypto primitives are injected by the integrator so the module stays
// host-testable with independent reference implementations:
//
//   otAes128CtrXor  AES-128 in CTR mode (16-byte big-endian counter starting
//                   at the IV), XORed in place over `len` bytes.
//   otHmacSha256    HMAC-SHA256 over one contiguous buffer.
//
// The two master key files ("unfixed-info.bin" and "locked-secret.bin") are
// 80 bytes each and are NEVER bundled with this project: they must be dumped
// by the user from their own tags or otherwise legally obtained, and are
// uploaded at runtime over BLE (see main.cpp).
//
// Data flow for writing a retail dump onto a blank NTAG215:
//   1. unpack:  540-byte retail tag -> 520-byte internal image, verifying
//               the tag and data HMACs with keys derived from the image UID.
//   2. patch:   write the target tag's UID (+BCC0) and BCC1 into the image.
//   3. pack:    regenerate HMACs, encrypt, convert back to 540 retail bytes.
//   4. derive the NTAG215 password for the new UID and set PWD/PACK pages.
//
// Pure, host-testable C++ (no Arduino dependencies).
#ifndef OPENTAG_AMIIBO_CRYPTO_H_
#define OPENTAG_AMIIBO_CRYPTO_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// ---- crypto backends (provided by the integrator) -------------------------

// AES-128-CTR keystream XOR: key/iv are 16 bytes; the IV is the initial
// counter value, incremented as a full 128-bit big-endian integer per block.
extern "C" void otAes128CtrXor(const uint8_t* key, const uint8_t* iv,
                               uint8_t* data, size_t len);

// HMAC-SHA256(key, keyLen, data, len) -> out32.
extern "C" void otHmacSha256(const uint8_t* key, size_t keyLen,
                             const uint8_t* data, size_t len, uint8_t* out32);

namespace amiibo {

// ---- key files -------------------------------------------------------------

static const size_t kKeyFileSize = 80;

// 80-byte master key file layout (both "unfixed infos" and "locked secret").
struct MasterKeyFile {
  uint8_t hmacKey[16];       // 0x00
  char typeString[14];       // 0x10, NUL-terminated
  uint8_t rfu;               // 0x1E
  uint8_t magicBytesSize;    // 0x1F
  uint8_t magicBytes[16];    // 0x20
  uint8_t xorPad[32];        // 0x30
};

// Both key files together (160 bytes, "unfixed infos" first — the order
// TagMo and amiitool use for a combined retail key file).
struct AmiiboKeys {
  MasterKeyFile data;        // unfixed-info.bin
  MasterKeyFile tag;         // locked-secret.bin
};

enum KeyFileKind { KEY_UNKNOWN = 0, KEY_DATA, KEY_TAG };

// Identify a single 80-byte key file by its type string. Returns KEY_UNKNOWN
// for anything that is not a valid amiibo master key file.
inline KeyFileKind identifyKeyFile(const uint8_t* file, size_t len) {
  if (!file || len != kKeyFileSize) return KEY_UNKNOWN;
  const MasterKeyFile* k = reinterpret_cast<const MasterKeyFile*>(file);
  if (k->magicBytesSize > 16) return KEY_UNKNOWN;  // amiitool rejects this too
  if (memchr(k->typeString, 0, sizeof(k->typeString)) == NULL) return KEY_UNKNOWN;
  if (strcmp(k->typeString, "unfixed infos") == 0) return KEY_DATA;
  if (strcmp(k->typeString, "locked secret") == 0) return KEY_TAG;
  return KEY_UNKNOWN;
}

// Accepts the two key files in either order and fills `out`.
inline bool loadKeyPair(const uint8_t* a, size_t aLen,
                        const uint8_t* b, size_t bLen, AmiiboKeys* out) {
  if (!out) return false;
  KeyFileKind ka = identifyKeyFile(a, aLen);
  KeyFileKind kb = identifyKeyFile(b, bLen);
  if (ka == KEY_UNKNOWN || kb == KEY_UNKNOWN) return false;
  if (ka == kb) return false;  // must be one of each
  const uint8_t* data = (ka == KEY_DATA) ? a : b;
  const uint8_t* tag = (ka == KEY_TAG) ? a : b;
  memcpy(&out->data, data, kKeyFileSize);
  memcpy(&out->tag, tag, kKeyFileSize);
  return true;
}

// True when both halves of the pair are loaded.
inline bool keysLoaded(const AmiiboKeys* keys) {
  return identifyKeyFile(reinterpret_cast<const uint8_t*>(&keys->data),
                         kKeyFileSize) == KEY_DATA &&
         identifyKeyFile(reinterpret_cast<const uint8_t*>(&keys->tag),
                         kKeyFileSize) == KEY_TAG;
}

// ---- image layouts ----------------------------------------------------------

static const size_t kTagSize = 540;    // retail tag image (135 NTAG215 pages)
static const size_t kPlainSize = 520;  // amiitool "internal" plain image

// Retail (540) -> internal (520) and back. Scrambles the fixed positions of
// the amiibo format; offsets and lengths from amiitool's amiitool.c table.
// Only the crypto region (tag bytes 0x000..0x207) is converted; the NTAG215
// trailer pages 130..134 (dynamic lock, CFG0/CFG1, PWD, PACK) are handled by
// the tag writer, not by the signature.
inline void tagToInternal(const uint8_t* tag, uint8_t* intl) {
  memset(intl, 0, kPlainSize);
  memcpy(intl + 0x000, tag + 0x008, 8);      // lock bytes / settings
  memcpy(intl + 0x008, tag + 0x080, 0x20);   // data HMAC (encrypted)
  memcpy(intl + 0x028, tag + 0x010, 0x24);   // CC, settings, write counters
  memcpy(intl + 0x04C, tag + 0x0A0, 0x168);  // encrypted app data
  memcpy(intl + 0x1B4, tag + 0x034, 0x20);   // tag HMAC (encrypted)
  memcpy(intl + 0x1D4, tag + 0x000, 8);      // UID0..UID2, BCC0, UID3..UID6
  memcpy(intl + 0x1DC, tag + 0x054, 0x2C);   // BCC1, amiibo id, ...
}

inline void internalToTag(const uint8_t* intl, uint8_t* tag) {
  memcpy(tag + 0x000, intl + 0x1D4, 8);
  memcpy(tag + 0x008, intl + 0x000, 8);
  memcpy(tag + 0x010, intl + 0x028, 0x24);
  memcpy(tag + 0x034, intl + 0x1B4, 0x20);
  memcpy(tag + 0x054, intl + 0x1DC, 0x2C);
  memcpy(tag + 0x080, intl + 0x008, 0x20);
  memcpy(tag + 0x0A0, intl + 0x04C, 0x168);
}

// ---- per-image key derivation ----------------------------------------------

struct DerivedKeys {
  uint8_t aesKey[16];
  uint8_t aesIV[16];
  uint8_t hmacKey[16];
};

// Seed for the key derivation, taken from the *decrypted* internal image.
inline void calcSeed(const uint8_t* intl, uint8_t* seed) {
  memset(seed, 0, 64);
  memcpy(seed + 0x00, intl + 0x029, 2);
  memcpy(seed + 0x10, intl + 0x1D4, 8);
  memcpy(seed + 0x18, intl + 0x1D4, 8);
  memcpy(seed + 0x20, intl + 0x1E8, 0x20);
}

// DRBG from amiitool/drbg.c: HMAC-SHA256(hmacKey, be16(counter) || seed)
// with counter = 0, 1 -> 64 bytes, of which the first 48 are the derived keys.
inline void keygen(const MasterKeyFile* master, const uint8_t* seed,
                   DerivedKeys* out) {
  // prepared = typeString (through NUL) + seed[0 .. 16-magicSize) +
  //            magicBytes + seed[0x10..0x1F] + (seed[0x20..0x3F] ^ xorPad)
  uint8_t prepared[95];
  size_t n = 0;
  while (n < sizeof(master->typeString) && master->typeString[n] != 0) {
    prepared[n] = (uint8_t)master->typeString[n];
    n++;
  }
  prepared[n++] = 0;
  size_t left = 16 - master->magicBytesSize;
  memcpy(prepared + n, seed, left);
  n += left;
  memcpy(prepared + n, master->magicBytes, master->magicBytesSize);
  n += master->magicBytesSize;
  memcpy(prepared + n, seed + 0x10, 0x10);
  n += 0x10;
  for (size_t i = 0; i < 0x20; i++) prepared[n++] = seed[0x20 + i] ^ master->xorPad[i];

  uint8_t out48[48];
  uint8_t block[2 + 95];
  block[0] = 0;
  for (size_t half = 0; half < 2; half++) {
    block[1] = (uint8_t)half;
    memcpy(block + 2, prepared, n);
    otHmacSha256(master->hmacKey, sizeof(master->hmacKey), block, n + 2,
                 out48 + half * 32);
  }
  memcpy(out->aesKey, out48, 16);
  memcpy(out->aesIV, out48 + 16, 16);
  memcpy(out->hmacKey, out48 + 32, 16);
}

// AES-CTR over the encrypted span of the internal image (0x02C..0x1B3, 0x188
// bytes — deliberately not block-aligned). In place.
inline void cipher(const DerivedKeys* dk, uint8_t* intl) {
  otAes128CtrXor(dk->aesKey, dk->aesIV, intl + 0x02C, 0x188);
}

// ---- unpack / pack -----------------------------------------------------------

// Verify and decrypt a 540-byte retail dump. On success fills `plain` with the
// 520-byte internal image (HMAC fields at 0x008 and 0x1B4 regenerated).
// Returns false if either HMAC does not match (wrong keys or corrupt dump).
inline bool unpack(const AmiiboKeys* keys, const uint8_t* tag, uint8_t* plain) {
  uint8_t intl[kPlainSize];
  tagToInternal(tag, intl);

  uint8_t seed[64];
  calcSeed(intl, seed);
  DerivedKeys dataDk, tagDk;
  keygen(&keys->data, seed, &dataDk);
  keygen(&keys->tag, seed, &tagDk);

  memcpy(plain, intl, kPlainSize);
  cipher(&dataDk, plain);

  // Tag HMAC covers plain[0x1D4..0x207] and is stored (encrypted) at 0x1B4.
  otHmacSha256(tagDk.hmacKey, 16, plain + 0x1D4, 0x34, plain + 0x1B4);
  bool tagOk = memcmp(plain + 0x1B4, intl + 0x1B4, 32) == 0;

  // Data HMAC covers plain[0x029..0x207] (including the regenerated tag HMAC)
  // and is stored (encrypted) at 0x008.
  otHmacSha256(dataDk.hmacKey, 16, plain + 0x029, 0x1DF, plain + 0x008);
  bool dataOk = memcmp(plain + 0x008, intl + 0x008, 32) == 0;

  return tagOk && dataOk;
}

// Sign a 520-byte internal image into a 540-byte retail dump.
inline void pack(const AmiiboKeys* keys, const uint8_t* plain, uint8_t* tag) {
  uint8_t seed[64];
  calcSeed(plain, seed);
  DerivedKeys dataDk, tagDk;
  keygen(&keys->data, seed, &dataDk);
  keygen(&keys->tag, seed, &tagDk);

  uint8_t intl[kPlainSize];
  memcpy(intl, plain, kPlainSize);

  // Tag HMAC first: data HMAC input includes it.
  otHmacSha256(tagDk.hmacKey, 16, plain + 0x1D4, 0x34, intl + 0x1B4);
  uint8_t buf[0x1DF];
  memcpy(buf, plain + 0x029, 0x18B);
  memcpy(buf + 0x18B, intl + 0x1B4, 0x20);
  memcpy(buf + 0x1AB, plain + 0x1D4, 0x34);
  otHmacSha256(dataDk.hmacKey, 16, buf, sizeof(buf), intl + 0x008);

  cipher(&dataDk, intl);
  internalToTag(intl, tag);
}

// ---- re-sign for a target tag ------------------------------------------------

// Re-sign a retail dump in place for the NTAG215 whose pages 0-3 were just
// read: idPages = [UID0,UID1,UID2,BCC0,UID3..UID6][page2b0][...]. This mirrors
// TagMo's writeToTagAuto -> patchUid: the UID prefix (+BCC0) goes to
// plain[0x1D4], BCC1 is recomputed, and plain[0] is forced to the live tag's
// page-2 byte 0 so the signature matches the physical tag. PWD/PACK pages
// (133/134) are refreshed for the new UID. Returns false if the dump does not
// verify under the loaded keys.
inline bool resign(const AmiiboKeys* keys, uint8_t* tag, const uint8_t* idPages) {
  uint8_t plain[kPlainSize];
  if (!unpack(keys, tag, plain)) return false;

  const uint8_t* uidPrefix = idPages;
  memcpy(plain + 0x1D4, uidPrefix, 8);
  plain[0x1DC] = uidPrefix[4] ^ uidPrefix[5] ^ uidPrefix[6] ^ uidPrefix[7];  // BCC1
  plain[0x000] = idPages[8];  // live page-2 byte 0 (TagMo patchUid)

  pack(keys, plain, tag);

  // PWD/PACK keygen (TagMo NfcByte.kt). uid7 = [UID0,UID1,UID2,UID3,UID4,
  // UID5,UID6] = uidPrefix bytes 0,1,2,4,5,6,7 (skipping BCC0).
  const uint8_t* u = uidPrefix;
  uint8_t uid7[7] = {u[0], u[1], u[2], u[4], u[5], u[6], u[7]};
  uint8_t pwd[4] = {
      (uint8_t)(0xAA ^ uid7[1] ^ uid7[3]), (uint8_t)(0x55 ^ uid7[2] ^ uid7[4]),
      (uint8_t)(0xAA ^ uid7[3] ^ uid7[5]), (uint8_t)(0x55 ^ uid7[4] ^ uid7[6])};
  memcpy(tag + 0x214, pwd, 4);               // page 133 PWD
  tag[0x218] = 0x80; tag[0x219] = 0x80;      // page 134 PACK
  tag[0x21A] = 0x00; tag[0x21B] = 0x00;
  return true;
}

}  // namespace amiibo

#endif  // OPENTAG_AMIIBO_CRYPTO_H_
