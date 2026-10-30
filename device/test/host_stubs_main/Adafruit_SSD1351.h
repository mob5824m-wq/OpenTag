#pragma once
#include "SPI.h"
#include "Adafruit_GFX.h"
class Adafruit_SSD1351 : public Adafruit_GFX {
 public:
  Adafruit_SSD1351(int w, int h, SPIClass *, int, int, int) : Adafruit_GFX(w, h) {}
  void enableDisplay(bool) {}   // real lib: DISPLAYOFF 0xAE / DISPLAYON 0xAF
};
