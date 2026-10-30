// Minimal Arduino-side stubs for host-native unit tests of portable driver
// code. Simulated clock: millis() advances only via delay(), so timeout paths
// terminate deterministically.
#pragma once

#include <cstdint>
#include <cstddef>
#include <deque>
#include <vector>

extern uint32_t g_fakeMillis;
inline uint32_t millis() { return g_fakeMillis; }
inline void delay(uint32_t ms) { g_fakeMillis += ms; }

class Stream {
 public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(const uint8_t *buf, size_t size) = 0;
};
