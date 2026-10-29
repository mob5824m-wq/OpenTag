#pragma once
#include "Arduino.h"
class Preferences {
 public:
  bool begin(const char *, bool) { return true; }
  String getString(const char *, const String &d = String()) { return d; }
  void putString(const char *, const String &) {}
  size_t getBytes(const char *, void *, size_t) { return 0; }
  void putBool(const char *, bool) {}
  bool getBool(const char *, bool d = false) { return d; }
  void clear() {}
  size_t putBytes(const char *, const void *, size_t len) { return len; }
  bool remove(const char *) { return true; }
};
