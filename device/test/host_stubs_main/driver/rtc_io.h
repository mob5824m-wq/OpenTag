// Host-side stub of the ESP-IDF RTC GPIO API (subset used by main.cpp to
// keep the OK button pulled up while the chip sleeps — the PCB has no
// external pullups). Compile-gate only.
#pragma once

#include <esp_sleep.h>   // gpio_num_t

inline void rtc_gpio_pullup_en(gpio_num_t) {}
inline void rtc_gpio_pulldown_dis(gpio_num_t) {}
