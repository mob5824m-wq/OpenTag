#pragma once
#include "Arduino.h"
#include <string>
#define NIMBLE_PROPERTY_WRITE 1
#define NIMBLE_PROPERTY_READ 2
#define NIMBLE_PROPERTY_NOTIFY 4
namespace NIMBLE_PROPERTY {
enum Prop : int { WRITE = 1, READ = 2, NOTIFY = 4, WRITE_NR = 8 };
}
class NimBLEConnInfo {};
class NimBLECharacteristic {
 public:
  void setValue(const std::string &) {}
  void setValue(const uint8_t *, size_t) {}
  std::string getValue() { return std::string(); }
  void notify() {}
  void setCallbacks(class NimBLECharacteristicCallbacks *) {}
};
class NimBLEService {
 public:
  NimBLECharacteristic *createCharacteristic(const char *, int) { return new NimBLECharacteristic(); }
  void start() {}
};
class NimBLEServer;
class NimBLEServerCallbacks {
 public:
  virtual ~NimBLEServerCallbacks() {}
  virtual void onConnect(NimBLEServer *, NimBLEConnInfo &) {}
  virtual void onDisconnect(NimBLEServer *, NimBLEConnInfo &, int) {}
};
class NimBLECharacteristicCallbacks {
 public:
  virtual ~NimBLECharacteristicCallbacks() {}
  virtual void onWrite(NimBLECharacteristic *, NimBLEConnInfo &) {}
};
class NimBLEServer {
 public:
  void setCallbacks(NimBLEServerCallbacks *) {}
  NimBLEService *createService(const char *) { return new NimBLEService(); }
};
class NimBLEAdvertising {
 public:
  void addServiceUUID(const char *) {}
  void start() {}
};
class NimBLEDevice {
 public:
  static void init(const char *) {}
  static void setMTU(uint16_t) {}
  static NimBLEServer *createServer() { return new NimBLEServer(); }
  static NimBLEAdvertising *getAdvertising() { return new NimBLEAdvertising(); }
  static void startAdvertising() {}
};
