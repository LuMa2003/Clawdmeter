#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>

// Minimal stub — replace with real power management for your board.
//
// If your board has an AXP2101 or similar PMU, mirror
// boards/waveshare_amoled_216/power.cpp. If the PWR button is wired
// somewhere other than the PMU's PKEY pin (e.g. through an IO expander
// like the AMOLED-1.8 board), look at that port instead.
//
// If your board has no PMU and no PWR button, leave the stubs as below
// and set BOARD_HAS_BATTERY=0 in board.h — the UI honors caps.has_battery
// and hides the battery indicator.

void power_hal_init(void) {}
void power_hal_tick(void) {}

int  power_hal_battery_pct(void) { return -1; }
bool power_hal_is_charging(void) { return false; }
bool power_hal_is_vbus_in(void)  { return false; }
bool power_hal_pwr_pressed(void) { return false; }
// Hold-to-pair gesture signals. Mirror the 216 (PMU PKEY long/positive IRQs)
// or the 1.8" (software hold-timing off a polled GPIO) port. Stub = no gesture.
bool power_hal_pwr_long_pressed(void) { return false; }
bool power_hal_pwr_released(void) { return false; }

// Configure wake sources + enter deep sleep. Mirror the 2.16 port:
//   esp_sleep_enable_timer_wakeup((uint64_t)wake_after_seconds * 1000000ULL);
//   rtc_gpio_pullup_en((gpio_num_t)BTN_BACK_GPIO);
//   esp_sleep_enable_ext1_wakeup(1ULL << BTN_BACK_GPIO, ESP_EXT1_WAKEUP_ALL_LOW);
//   esp_deep_sleep_start();
// Stub: scheduled deep sleep disabled.
void power_hal_enter_deep_sleep(uint32_t wake_after_seconds) {
    (void)wake_after_seconds;
}
