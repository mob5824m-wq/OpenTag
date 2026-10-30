// Host-side stub of the ESP-IDF deep-sleep API (subset used by main.cpp).
// Only enough for the compile gate; nothing here is executed. On the real
// build the toolchain's IDF header is used instead.
#pragma once

// Subset of the RTC-capable GPIO numbering (ESP32-S3: GPIO 0..21).
enum gpio_num_t {
  GPIO_NUM_0 = 0,
  GPIO_NUM_1 = 1,
  GPIO_NUM_2 = 2,
  GPIO_NUM_13 = 13,
  GPIO_NUM_42 = 42,
};

typedef enum {
  ESP_SLEEP_WAKEUP_UNDEFINED = 0,
  ESP_SLEEP_WAKEUP_EXT0 = 1,
  ESP_SLEEP_WAKEUP_EXT1 = 2,
} esp_sleep_wakeup_cause_t;

inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) {
  return ESP_SLEEP_WAKEUP_UNDEFINED;
}
inline void esp_sleep_enable_ext0_wakeup(gpio_num_t, int) {}
inline void esp_deep_sleep_start(void) {}
