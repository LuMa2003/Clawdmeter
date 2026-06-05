#include <Arduino.h>
#include <Wire.h>
#include <lvgl.h>
#include <ArduinoJson.h>
#include <esp_heap_caps.h>
#include <esp_sleep.h>

#include "data.h"
#include "ui.h"
#include "ble.h"
#include "splash.h"
#include "usage_rate.h"
#include "idle.h"
#include "idle_cfg.h"
#include "brightness.h"

#include "hal/board_caps.h"
#include "hal/display_hal.h"
#include "hal/input_hal.h"
#include "hal/power_hal.h"
#include "hal/imu_hal.h"

static UsageData usage = {};

// ---- LVGL draw buffers (partial render mode) ----
// PSRAM-equipped boards (S3) can comfortably hold larger strips. PSRAM-free
// boards (e.g. ESP32-C6) allocate from internal SRAM, so we shrink the strip
// — 480×20 RGB565 = 19 KB × 2 buffers = 38 KB, fits beside everything else.
#ifdef BOARD_HAS_PSRAM
#define BUF_LINES 40
#define LV_BUF_CAPS (MALLOC_CAP_SPIRAM)
#else
#define BUF_LINES 20
#define LV_BUF_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
#endif
static uint16_t* buf1 = nullptr;
static uint16_t* buf2 = nullptr;

static uint32_t my_tick(void) { return millis(); }

static void my_flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    int32_t w = area->x2 - area->x1 + 1;
    int32_t h = area->y2 - area->y1 + 1;
    display_hal_draw_bitmap(area->x1, area->y1, w, h, (uint16_t*)px_map);
    lv_display_flush_ready(disp);
}

static void rounder_cb(lv_event_t* e) {
    lv_area_t* area = (lv_area_t*)lv_event_get_param(e);
    display_hal_round_area(&area->x1, &area->y1, &area->x2, &area->y2);
}

// Parse a JSON line into UsageData.
static bool parse_json(const char* json, UsageData* out) {
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, json);
    if (err) {
        Serial.printf("JSON parse error: %s\n", err.c_str());
        return false;
    }

    out->session_pct = doc["s"] | 0.0f;
    out->session_reset_mins = doc["sr"] | -1;
    out->weekly_pct = doc["w"] | 0.0f;
    out->weekly_reset_mins = doc["wr"] | -1;
    strlcpy(out->status, doc["st"] | "unknown", sizeof(out->status));
    out->ok = doc["ok"] | false;
    out->dow = doc["dow"] | -1;
    out->hour = doc["hour"] | -1;
    out->min = doc["min"] | -1;
    out->host_locked = doc["locked"] | false;
    out->valid = true;
    return true;
}

// ---- Serial command buffer ----
#define CMD_BUF_SIZE 64
static char cmd_buf[CMD_BUF_SIZE];
static int cmd_pos = 0;

static void send_screenshot() {
#ifndef BOARD_HAS_PSRAM
    // A full RGB565 framebuffer doesn't fit in internal SRAM on PSRAM-free
    // boards (e.g. 480×480×2 = 460 KB). Capture is unsupported there.
    Serial.println("SCREENSHOT_UNSUPPORTED");
    return;
#else
    const uint32_t w = board_caps().width;
    const uint32_t h = board_caps().height;
    const uint32_t row_bytes = w * 2;
    const uint32_t buf_size = row_bytes * h;
    uint8_t* sbuf = (uint8_t*)heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM);
    if (!sbuf) {
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, w, h, LV_COLOR_FORMAT_RGB565, row_bytes, sbuf, buf_size);

    lv_result_t res = lv_snapshot_take_to_draw_buf(lv_screen_active(), LV_COLOR_FORMAT_RGB565, &draw_buf);
    if (res != LV_RESULT_OK) {
        heap_caps_free(sbuf);
        Serial.println("SCREENSHOT_ERR");
        return;
    }

    Serial.printf("SCREENSHOT_START %lu %lu %lu\n",
        (unsigned long)w, (unsigned long)h, (unsigned long)buf_size);
    Serial.flush();
    Serial.write(sbuf, buf_size);
    Serial.flush();
    Serial.println();
    Serial.println("SCREENSHOT_END");
    heap_caps_free(sbuf);
#endif
}

static void check_serial_cmd() {
    while (Serial.available()) {
        char c = Serial.read();
        if (c == '\n' || c == '\r') {
            cmd_buf[cmd_pos] = '\0';
            if (strcmp(cmd_buf, "screenshot") == 0) send_screenshot();
            cmd_pos = 0;
        } else if (cmd_pos < CMD_BUF_SIZE - 1) {
            cmd_buf[cmd_pos++] = c;
        }
    }
}

// Each board provides this. Must bring up the shared I2C bus (Wire.begin
// with the board's SDA/SCL pins) and any board-private hardware that has
// to settle before display/touch (e.g. an IO expander gating the LCD
// reset line). Called exactly once at the start of setup().
extern "C" void board_init(void);

void setup() {
    Serial.begin(115200);
    delay(300);

    // Drop the CPU clock from the 240 MHz default to 80 MHz. The dashboard is
    // bursty light work — LVGL ticks, an occasional BLE callback, software
    // pixel rotation for the CO5300. 80 MHz handles all of it comfortably and
    // saves ~10-15 mA of active draw. 80 MHz is also the minimum stable clock
    // with BLE peripheral active — going lower drops connections. APB scales
    // with the CPU clock, so QSPI pixel pushes get slightly slower too;
    // imperceptible on a status display.
    setCpuFrequencyMhz(80);

    // Capture the wake cause BEFORE doing anything else — the value is only
    // meaningful until the next esp_*_sleep call clears it. Used to decide
    // boot screen (cold boot → splash, wake from deep sleep → usage view).
    const esp_sleep_wakeup_cause_t wake_cause = esp_sleep_get_wakeup_cause();
    const bool waking_from_deep_sleep =
        (wake_cause == ESP_SLEEP_WAKEUP_TIMER || wake_cause == ESP_SLEEP_WAKEUP_EXT1);
    Serial.printf("{\"ready\":true,\"wake_cause\":%d}\n", (int)wake_cause);

    board_init();

    display_hal_init();
    display_hal_begin();
    idle_init();        // takes over panel brightness and starts the idle timer
    brightness_init();  // load the user's saved brightness level and apply via idle

    power_hal_init();
    imu_hal_init();

    // ---- LVGL ----
    const int W = board_caps().width;
    const int H = board_caps().height;

    lv_init();
    lv_tick_set_cb(my_tick);

    buf1 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);
    buf2 = (uint16_t*)heap_caps_malloc(W * BUF_LINES * 2, LV_BUF_CAPS);

    lv_display_t* disp = lv_display_create(W, H);
    lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(disp, my_flush_cb);
    lv_display_set_buffers(disp, buf1, buf2, W * BUF_LINES * 2,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_add_event_cb(disp, rounder_cb, LV_EVENT_INVALIDATE_AREA, NULL);

    // No LVGL input device — the device is display-only. Touch hardware
    // (CST9220) is no longer driven; its INT line stays floating.

    ble_init();
    input_hal_init();

    ui_init();
    ui_update_ble_status(ble_get_state(), ble_get_device_name(), ble_get_mac_address());
    ui_update_battery(power_hal_battery_pct(), power_hal_is_charging());
    // Cold boot lands on splash (its long-standing role as the "look! a
    // device!" intro screen). Wake-from-deep-sleep is functional — the
    // user is already familiar with the device and wants the dashboard
    // back where it was. Wake-cause was captured at the top of setup().
    ui_show_screen(waking_from_deep_sleep ? SCREEN_USAGE : SCREEN_SPLASH);

    Serial.printf("Dashboard ready (%s, %dx%d), waiting for data on BLE...\n",
        board_caps().name, W, H);
}

static ble_state_t last_ble_state = BLE_STATE_INIT;

// Hold-to-pair gesture: hold the PWR button ~3s, then RELEASE → clear all BLE
// bonds and re-advertise. Clearing on *release* (not while held) is deliberate:
// holding to power the device OFF (AXP hardware shutdown at 8s) must not wipe
// the bond — a power-off hold never releases before shutdown. To stop a
// "chicken-out" release just before 8s from pairing, the gesture disarms at 6s.
//
//   ~1.5s long-press edge → PENDING
//   3.0s (+1500)          → ARMED   (release from here clears bonds)
//   6.0s (+4500)          → DISARMED (no clear; AXP powers off at 8s)
#define PAIR_ARM_AFTER_LONG_MS    1500   // 3.0s total
#define PAIR_DISARM_AFTER_LONG_MS 4500   // 6.0s total
// PWR held past the pair-gesture disarm point becomes a deep-sleep gesture.
// 7.0s total = 1.0s after pair disarms, 3.0s before AXP force-shutdown at
// 10s (bumped from the default 8s for safety margin in power.cpp).
#define DEEP_SLEEP_AFTER_DISARM_MS 1000  // 7.0s total
enum pair_state_t { PAIR_IDLE, PAIR_PENDING, PAIR_ARMED };
static pair_state_t pair_state        = PAIR_IDLE;
static uint32_t     pair_long_seen_ms = 0;
// Set when pair_tick disarms via timeout (PAIR_ARMED → PAIR_IDLE because the
// user held past PAIR_DISARM_AFTER_LONG_MS without releasing). Zero otherwise.
// Read by deep_sleep_tick to drive the 7s manual-sleep gesture, cleared on
// the eventual PWR release. NOT set when pair disarms via release — that's a
// normal pair attempt, not a sleep intent.
static uint32_t     pair_disarmed_at_ms = 0;

static void pair_tick(void) {
    if (pair_state == PAIR_IDLE && power_hal_pwr_long_pressed()) {
        pair_state = PAIR_PENDING;
        pair_long_seen_ms = millis();
        (void)power_hal_pwr_released();  // drain any stale release edge
        Serial.println("PWR long-press: hold to ~3s then release to pair");
        return;
    }
    if (pair_state == PAIR_IDLE) return;

    if (power_hal_pwr_released()) {
        if (pair_state == PAIR_ARMED) {
            Serial.println("Pair: released in window — clearing bonds, advertising");
            ble_clear_bonds();
        } else {
            Serial.println("Pair: released too early — cancelled");
        }
        pair_state = PAIR_IDLE;
        return;
    }

    uint32_t held = millis() - pair_long_seen_ms;
    if (pair_state == PAIR_PENDING && held >= PAIR_ARM_AFTER_LONG_MS) {
        pair_state = PAIR_ARMED;
        Serial.println("Pair: armed — release to pair");
    } else if (pair_state == PAIR_ARMED && held >= PAIR_DISARM_AFTER_LONG_MS) {
        pair_state = PAIR_IDLE;            // power-off territory; don't pair
        pair_disarmed_at_ms = millis();    // hand the held PWR off to deep_sleep_tick
        Serial.println("Pair: disarmed (holding past 6s — keep holding ~1s for sleep, release for no-op)");
    }
}

// Manual deep-sleep gesture: hold PWR for ~7s. The first 6s belong to the
// pair-tick state machine (1.5s → PENDING, 3s → ARMED, 6s → DISARMED). When
// pair_tick disarms via timeout (NOT via release), it records the disarm
// time in `pair_disarmed_at_ms`. We watch that and, if the user keeps
// holding for another second, fire `idle_enter_deep_sleep`. AXP force-shutdown
// at 10s caps the window — gives ~3s tolerance past the deep-sleep trigger.
//
// Must run AFTER pair_tick so the gesture composes correctly (a 3-6s hold
// completes the pair gesture and never reaches the disarmed-by-timeout state).
static void deep_sleep_tick(void) {
    if (pair_disarmed_at_ms == 0) return;

    // Release after disarm → no-op; user changed their mind. Clear the gesture.
    if (power_hal_pwr_released()) {
        pair_disarmed_at_ms = 0;
        Serial.println("PWR long-hold: released after disarm — no sleep");
        return;
    }

    if ((millis() - pair_disarmed_at_ms) >= DEEP_SLEEP_AFTER_DISARM_MS) {
        pair_disarmed_at_ms = 0;
        idle_enter_deep_sleep("manual long-hold");  // never returns
    }
}

void loop() {
    idle_tick();
    lv_timer_handler();
    ui_tick_anim();
    ble_tick();
    power_hal_tick();
    imu_hal_tick();
    splash_tick();
    // Rotation transition (blank + ramp) would fight the idle fade — skip
    // ticks while the panel is dark. A rotation that happens during sleep
    // is detected by the next tick after wake and ramped in then.
    if (!idle_is_asleep()) display_hal_tick();

    // ---- Physical buttons ----
    //   BOOT (left, GPIO 0) → wake-from-sleep only. No HID; the device is
    //       display-only. Polled via input_hal so a press during light sleep
    //       can drive idle_consume_wake_press; the same GPIO is configured as
    //       the deep-sleep EXT1 wake source in power_hal_enter_deep_sleep.
    //   PWR (middle, AXP PKEY) → short tap cycles brightness on the usage
    //       view; 3s hold + release clears BLE bonds (pair_tick); 7s hold
    //       enters deep sleep (deep_sleep_tick).
    {
        static bool primary_was = false;
        bool primary_now = input_hal_is_held(INPUT_BTN_PRIMARY);
        if (primary_now != primary_was) {
            if (primary_now) {
                (void)idle_consume_wake_press();  // wake from light sleep, no other action
            }
            primary_was = primary_now;
        }

        if (power_hal_pwr_pressed()) {
            if (!idle_consume_wake_press()) {
                brightness_cycle();
            }
        }

        pair_tick();
        deep_sleep_tick();  // must run AFTER pair_tick — composes via pair_disarmed_at_ms
    }

    ble_state_t bs = ble_get_state();
    if (bs != last_ble_state) {
        last_ble_state = bs;
        ui_update_ble_status(bs, ble_get_device_name(), ble_get_mac_address());
    }

    static int  last_pct      = -2;
    static bool last_charging = false;
    int  pct      = power_hal_battery_pct();
    bool charging = power_hal_is_charging();
    if (pct != last_pct || charging != last_charging) {
        last_pct = pct;
        last_charging = charging;
        ui_update_battery(pct, charging);
    }

    check_serial_cmd();

    if (ble_has_data()) {
        if (parse_json(ble_get_data(), &usage)) {
            // Feed the activity-driven idle FSM. Clock first (so the work-
            // window check is current); host-lock next (event-driven, may
            // fire an immediate fade-out); data-delta last (may wake from
            // a data-stall dark state).
            idle_set_clock(usage.dow, usage.hour, usage.min);
            idle_set_host_locked(usage.host_locked);
            idle_note_data_delta((int)usage.session_pct, (int)usage.weekly_pct);

            int g_before = usage_rate_group();
            usage_rate_sample(usage.session_pct);
            int g_after = usage_rate_group();
            if (g_after != g_before) {
                Serial.printf("usage rate: group %d -> %d (s=%.2f%%)\n",
                    g_before, g_after, usage.session_pct);
                if (splash_is_active()) splash_pick_for_current_rate();
            }
            ui_update(&usage);
            ble_send_ack();
        } else {
            ble_send_nack();
        }
    }

    delay(5);
}
