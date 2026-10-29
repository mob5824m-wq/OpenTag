// Host stub of the Arduino OTA Update API.
#pragma once
#include <cstddef>
#include "Arduino.h"

class UpdateClass {
 public:
  bool begin(size_t) { return true; }
  void onProgress(void (*)(size_t, size_t)) {}
  size_t writeStream(Stream &) { return 0; }
  bool end(bool = true) { return true; }
  void abort() {}
};
extern UpdateClass Update;
