#pragma once
#include "Arduino.h"
#include <string>
#define NIMBLE_PROPERTY_WRITE 1
#define NIMBLE_PROPERTY_READ 2
#define NIMBLE_PROPERTY_NOTIFY 4
namespace NIMBLE_PROPERTY {
enum Prop : int { WRITE = 1, READ = 2, NOTIFY = 4, WRITE_NR = 8 };
}
class NimBLEConnInfo {
 public:
  bool isBonded() const { return false; }
  uint16_t getConnHandle() const { return 0; }
};
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
  virtual void onAuthenticationComplete(NimBLEConnInfo &) {}
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
  void disconnect(uint16_t) {}
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
  static void stopAdvertising() {}
  static NimBLEServer *getServer() { return new NimBLEServer(); }
  static bool startSecurity(uint16_t, int * = nullptr) { return true; }
  static void setSecurityAuth(bool, bool, bool) {}
  static void setSecurityIOCap(uint8_t) {}
  static void setSecurityPasskey(uint32_t) {}
};
