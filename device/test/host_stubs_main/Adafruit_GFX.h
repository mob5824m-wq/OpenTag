#pragma once
#include "Arduino.h"
class Adafruit_GFX {
 public:
  Adafruit_GFX(int, int) {}
  virtual ~Adafruit_GFX() {}
  virtual void begin() {}
  void fillScreen(uint16_t) {}
  void setTextColor(uint16_t) {}
  void setTextSize(uint8_t) {}
  void setCursor(int, int) {}
  void setTextWrap(bool) {}
  void drawFastHLine(int, int, int, uint16_t) {}
  void drawPixel(int, int, uint16_t) {}
  template <typename T> void print(const T &) {}
  template <typename T> void print(const T &, int) {}
  template <typename T> void println(const T &) {}
  void println() {}
};
