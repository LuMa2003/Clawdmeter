#include "../../hal/power_hal.h"
#include "board.h"
#include <Arduino.h>
#include <Wire.h>
#include <XPowersLib.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>

// PWR button comes from AXP2101 PKEY IRQs:
//   SHORT    — quick tap (cycle splash animations)
//   LONG     — ~1.5s mark, starts the hold-to-pair countdown
//   POSITIVE — release edge, completes/cancels the gesture

#define BATTERY_POLL_MS  2000
#define CHARGING_POLL_MS 500
#define PWR_POLL_MS      50

static XPowersPMU pmu;

static int      cached_pct        = -1;
static bool     cached_charging   = false;
static bool     cached_vbus       = false;
static bool     pwr_pressed_flag  = false;
static bool     pwr_long_flag     = false;
static bool     pwr_released_flag = false;
static uint32_t last_battery_ms   = 0;
static uint32_t last_charging_ms  = 0;
static uint32_t last_pwr_ms       = 0;

void power_hal_init(void) {
    if (!pmu.begin(Wire, AXP2101_ADDR, IIC_SDA, IIC_SCL)) {
        Serial.println("AXP2101 init failed");
        return;
    }
    Serial.println("AXP2101 init OK");

    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();
    pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ
                | XPOWERS_AXP2101_PKEY_LONG_IRQ
                | XPOWERS_AXP2101_PKEY_POSITIVE_IRQ);

    // AXP hardware force-shutdown threshold. We layer multiple gestures on
    // the PWR hold timeline:
    //   3.0s → pair-arm (release to pair)
    //   6.0s → pair-disarm
    //   7.0s → manual deep-sleep gesture
    //   ----  AXP cuts power here
    // The XPowersLib enum maxes out at 10S (4/6/8/10 only). 10S gives 3s of
    // margin past the deep-sleep trigger — at 7s the screen blanks, the user
    // sees it and releases; up to 10s held = still deep sleep, only past 10s
    // does the AXP force-shutdown the rails.
    pmu.setPowerKeyPressOffTime(XPOWERS_POWEROFF_10S);

    cached_charging = pmu.isCharging();
    cached_vbus     = pmu.isVbusIn();
    cached_pct = pmu.getBatteryPercent();
}

void power_hal_tick(void) {
    uint32_t now = millis();

    if (now - last_charging_ms >= CHARGING_POLL_MS) {
        last_charging_ms = now;
        cached_charging = pmu.isCharging();
        cached_vbus     = pmu.isVbusIn();
    }
    if (now - last_battery_ms >= BATTERY_POLL_MS) {
        last_battery_ms = now;
        cached_pct = pmu.getBatteryPercent();
    }
    if (now - last_pwr_ms >= PWR_POLL_MS) {
        last_pwr_ms = now;
        pmu.getIrqStatus();
        if (pmu.isPekeyShortPressIrq())    pwr_pressed_flag  = true;
        if (pmu.isPekeyLongPressIrq())     pwr_long_flag     = true;
        if (pmu.isPekeyPositiveIrq())      pwr_released_flag = true;
        pmu.clearIrqStatus();
    }
}

int  power_hal_battery_pct(void) { return cached_pct; }
bool power_hal_is_charging(void) { return cached_charging; }
bool power_hal_is_vbus_in(void)  { return cached_vbus; }

bool power_hal_pwr_pressed(void) {
    if (pwr_pressed_flag) { pwr_pressed_flag = false; return true; }
    return false;
}

bool power_hal_pwr_long_pressed(void) {
    if (pwr_long_flag) { pwr_long_flag = false; return true; }
    return false;
}

bool power_hal_pwr_released(void) {
    if (pwr_released_flag) { pwr_released_flag = false; return true; }
    return false;
}

void power_hal_enter_deep_sleep(uint32_t wake_after_seconds) {
    // Disable AXP IRQs so a stray PKEY edge doesn't accumulate status bits
    // that would fire spuriously on the next boot. Rails stay up — display,
    // I2C, and the chip itself all share the same VCC3V3, so cutting it
    // would brick the boot path.
    pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
    pmu.clearIrqStatus();

    // Timer wake (RTC counter). 0 = button-only, no timer.
    if (wake_after_seconds > 0) {
        esp_sleep_enable_timer_wakeup((uint64_t)wake_after_seconds * 1000000ULL);
    }

    // EXT1 wake on the BOOT/primary button. GPIO 0 is RTC-capable on
    // ESP32-S3. ALL_LOW = trigger when the masked pin goes low (button
    // presses short the line to GND through the external pull-up).
    const gpio_num_t wake_pin = (gpio_num_t)BTN_BACK_GPIO;
    rtc_gpio_pullup_en(wake_pin);
    esp_sleep_enable_ext1_wakeup(1ULL << wake_pin, ESP_EXT1_WAKEUP_ALL_LOW);

    esp_deep_sleep_start();  // never returns
}
