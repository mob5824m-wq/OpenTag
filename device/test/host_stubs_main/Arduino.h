// Host-side Arduino environment for the main.cpp compile gate
// (test_compile_main.sh). Just enough API surface for a syntax/type check of
// the firmware; nothing here is executed.
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdio>
#include <string>

extern uint32_t g_fakeMillis;
inline uint32_t millis() { return g_fakeMillis; }
inline void delay(uint32_t ms) { g_fakeMillis += ms; }

#define HIGH 1
#define LOW 0
#define INPUT_PULLUP 2
#define HEX 16
inline void pinMode(int, int) {}
inline int digitalRead(int) { return HIGH; }
inline long random(long max) { return max / 2; }
inline int analogRead(int) { return 0; }
inline void tone(int, unsigned int, unsigned long) {}
inline void noTone(int) {}
struct EspStub { void restart() {} };
extern EspStub ESP;
#define SERIAL_8N1 0x06
template <typename A, typename B>
inline A min(A a, B b) { return a < (A)b ? a : (A)b; }

class Stream {
 public:
  virtual ~Stream() {}
  virtual int available() = 0;
  virtual int read() = 0;
  virtual size_t write(const uint8_t *buf, size_t size) = 0;
};

class HardwareSerial : public Stream {
 public:
  void begin(unsigned long) {}
  void begin(unsigned long, int, int, int) {}
  int available() override { return 0; }
  int read() override { return -1; }
  size_t write(const uint8_t *, size_t) override { return 0; }
  void print(const char *) {}
  void println(const char *) {}
};
extern HardwareSerial Serial;
extern HardwareSerial Serial2;

// Minimal String stand-in: supports the operations main.cpp uses.
class String {
 public:
  String() {}
  String(const char *s) : s_(s ? s : "") {}
  String(const std::string &s) : s_(s) {}
  size_t length() const { return s_.size(); }
  const char *c_str() const { return s_.c_str(); }
  String substring(unsigned from, unsigned to) const {
    if (from >= s_.size()) return String();
    if (to > s_.size()) to = s_.size();
    return String(s_.substr(from, to - from));
  }
  String substring(unsigned from) const { return substring(from, s_.size()); }
  bool operator==(const char *o) const { return s_ == o; }
  bool operator!=(const char *o) const { return s_ != o; }
  String &operator=(const char *o) { s_ = o ? o : ""; return *this; }
  String operator+(const String &o) const { return String(s_ + o.s_); }
  int indexOf(const char *needle, int from = 0) const {
    if (from < 0) from = 0;
    if ((size_t)from > s_.size()) return -1;
    const size_t pos = s_.find(needle ? needle : "", (size_t)from);
    return pos == std::string::npos ? -1 : (int)pos;
  }
  int indexOf(char c, int from = 0) const {
    return indexOf(std::string(1, c).c_str(), from);
  }
  bool endsWith(const String &o) const {
    return o.length() <= s_.size() &&
           s_.compare(s_.size() - o.length(), o.length(), o.s_) == 0;
  }
  bool endsWith(const char *o) const { return endsWith(String(o)); }
  std::string s_;
};

#ifndef strlcpy
inline size_t strlcpy(char *dst, const char *src, size_t cap) {
  size_t n = strlen(src);
  if (cap) { size_t c = n < cap ? n : cap - 1; memcpy(dst, src, c); dst[c] = 0; }
  return n;
}
#endif

#ifndef strlcat
inline size_t strlcat(char *dst, const char *src, size_t cap) {
  size_t d = strlen(dst);
  if (d >= cap) return d + strlen(src);
  return d + strlcpy(dst + d, src, cap - d);
}
#endif

typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
inline void portENTER_CRITICAL(portMUX_TYPE *) {}
inline void portEXIT_CRITICAL(portMUX_TYPE *) {}
