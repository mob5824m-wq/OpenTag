// Host stub of WiFiClientSecure (TLS client).
#pragma once
#include "Arduino.h"

class WiFiClientSecure : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
  void setInsecure() {}
};
