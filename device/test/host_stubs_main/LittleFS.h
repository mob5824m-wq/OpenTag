#pragma once
#include "Arduino.h"
class File {
 public:
  operator bool() const { return false; }
  size_t write(const uint8_t *, size_t) { return 0; }
  size_t read(uint8_t *, size_t) { return 0; }
  size_t size() const { return 0; }
  void close() {}
  bool isDirectory() const { return false; }
  const char *name() const { return ""; }
  File openNextFile() { return File(); }
};
#define FILE_WRITE "w"
class LittleFSClass {
 public:
  bool begin(bool) { return true; }
  bool mkdir(const char *) { return true; }
  File open(const char *, const char * = "r") { return File(); }
  bool remove(const char *) { return true; }
  bool exists(const char *) { return false; }
};
extern LittleFSClass LittleFS;
