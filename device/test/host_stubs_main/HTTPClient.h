// Host stub of HTTPClient (enough for the OTA path's syntax check).
#pragma once
#include "Arduino.h"
#include "WiFiClientSecure.h"

class WiFiClient : public Stream {
 public:
  int available() override { return 0; }
  int read() override { return -1; }
  size_t write(const uint8_t *, size_t size) override { return size; }
};

class HTTPClient {
 public:
  void begin(WiFiClientSecure &, const char *) {}
  void addHeader(const char *, const char *) {}
  int GET() { return 200; }
  int getSize() { return 0; }
  String getString() { return String(); }
  WiFiClient *getStreamPtr() { return nullptr; }
  void end() {}
};
