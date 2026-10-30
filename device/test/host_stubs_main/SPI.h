#pragma once
#include "Arduino.h"
class SPIClass {
 public:
  void begin(int, int, int) {}
};
extern SPIClass SPI;
