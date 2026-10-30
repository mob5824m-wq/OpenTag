// Host stub for the arduino-esp32 2.0.x device-mode USB MSC class
// (cores/esp32/USBMSC.h) - API mirrored from the real header.
#pragma once
#include <cstdint>
#include <cstddef>

typedef bool (*msc_start_stop_cb)(uint8_t power_condition, bool start, bool load_eject);
typedef int32_t (*msc_read_cb)(uint32_t lba, uint32_t offset, void *buffer, uint32_t bufsize);
typedef int32_t (*msc_write_cb)(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t bufsize);

class USBMSC {
 public:
  USBMSC() {}
  ~USBMSC() {}
  bool begin(uint32_t, uint16_t) { return true; }
  void end() {}
  void vendorID(const char *) {}
  void productID(const char *) {}
  void productRevision(const char *) {}
  void mediaPresent(bool) {}
  void onStartStop(msc_start_stop_cb) {}
  void onRead(msc_read_cb) {}
  void onWrite(msc_write_cb) {}
};
