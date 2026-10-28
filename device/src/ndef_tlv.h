// OpenTag: NDEF file TLV builder for NFC Forum Type 2 tags.
//
// Pure, host-testable C++ (no Arduino dependencies). The device only ever
// encodes ordinary NDEF text records ("en" language, UTF-8) and HTTP(S) URL
// records with no URI-prefix abbreviation, wrapped in an NDEF Message TLV
// (0x03 ... 0xFE) sized for the NTAG215 user area.
#ifndef OPENTAG_NDEF_TLV_H_
#define OPENTAG_NDEF_TLV_H_

#include <stddef.h>
#include <stdint.h>
#include <string.h>

// Builds the NDEF file TLV for a "TEXT"/"en"/<value> or "URL"/<http(s) value>
// record. Returns false for unsupported kinds, empty or oversized values.
// The encoded TLV (including the 0xFE terminator) is written to out and its
// length stored in *outLength.
inline bool makeNdefTlv(const char *kind, const char *value, uint8_t *out,
                        size_t capacity, size_t *outLength) {
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

#endif  // OPENTAG_NDEF_TLV_H_
