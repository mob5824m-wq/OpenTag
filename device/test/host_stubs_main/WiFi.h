// Host stub of the Arduino WiFi API (STA basics only).
#pragma once
#include "Arduino.h"

#define WL_CONNECTED 3
#define WIFI_STA 1

class WiFiClass {
 public:
  void mode(int) {}
  void begin(const char *, const char *) {}
  int status() { return WL_CONNECTED; }
  void disconnect() {}
};
extern WiFiClass WiFi;
