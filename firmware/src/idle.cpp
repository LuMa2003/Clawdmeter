#include <Arduino.h>
#include "idle.h"
#include "idle_cfg.h"
#include "ble.h"
#include "hal/display_hal.h"
#include "hal/power_hal.h"

enum IdleState {
    STATE_AWAKE,
    STATE_FADING_OUT,
    STATE_ASLEEP,            // existing user-input timeout fade (30 min)
    STATE_LIGHT_SLEEP_IDLE,  // host-lock OR 20-min data-stall, BLE up, screen+display asleep
    STATE_FADING_IN,
};

static IdleState state = STATE_AWAKE;
static IdleState fade_out_target = STATE_ASLEEP;  // where to land on fade-out completion
static uint32_t last_activity_ms = 0;
static uint32_t fade_started_ms  = 0;
static uint32_t fade_last_step_ms = 0;
static uint8_t  fade_from = DISPLAY_DEFAULT_BRIGHTNESS;
static uint8_t  fade_to   = 0;
static uint8_t  awake_brightness = DISPLAY_DEFAULT_BRIGHTNESS;  // user-set "full" level (brightness.cpp)

// ---- Phase B state: host clock, lock, and data-delta tracking ----
// Clock sentinel: int8_t < 0 means "no stamp seen yet" — work-window check
// fails-safe to "keep screen on" in that case.
static int8_t   cur_dow  = -1;
static int8_t   cur_hour = -1;
static int8_t   cur_min  = -1;
static bool     cur_host_locked = false;
static int      last_s_int = -1;
static int      last_w_int = -1;
static uint32_t last_data_delta_ms = 0;
// Records why we entered STATE_LIGHT_SLEEP_IDLE so the auto-wake path can
// pick the right re-entry condition (host-lock only re-wakes on unlock,
// data-stall wakes on any real delta).
static bool     idle_entered_by_lock = false;

static void apply_brightness(uint8_t b) {
    display_hal_set_brightness(b);
}

static void begin_fade(uint8_t to, uint32_t now) {
    fade_from = (to == 0) ? awake_brightness : 0;
    fade_to   = to;
    fade_started_ms = now;
    fade_last_step_ms = now;
}

void idle_init(void) {
    state = STATE_AWAKE;
    last_activity_ms = millis();
    apply_brightness(awake_brightness);
}

void idle_set_awake_brightness(uint8_t level) {
    awake_brightness = level;
    // Apply now if fully awake so a button press is visible immediately;
    // during fades/sleep the next fade-in picks it up.
    if (state == STATE_AWAKE) apply_brightness(level);
}

void idle_note_activity(void) {
    last_activity_ms = millis();
    if (state == STATE_FADING_IN) return;
    if (state == STATE_AWAKE) return;
    // Asleep/fading-out shouldn't reach here in normal flow (callers gate via
    // idle_consume_wake_press first), but if it does: trigger a wake.
    begin_fade(awake_brightness, last_activity_ms);
    state = STATE_FADING_IN;
}

bool idle_consume_wake_press(void) {
    if (state == STATE_ASLEEP || state == STATE_LIGHT_SLEEP_IDLE || state == STATE_FADING_OUT) {
        uint32_t now = millis();
        last_activity_ms = now;
        // Treat an explicit user action as a fresh data-activity stamp too.
        // Without this, waking from a data-stall sleep (20-min %-unchanged
        // trigger in idle_tick) would re-evaluate the stall condition on the
        // very next loop iteration and immediately re-fade — making tap/BOOT
        // wake look broken. Mirrors what idle_set_host_locked(false) already
        // does on unlock.
        last_data_delta_ms = now;
        // Reverse the panel-sleep side effect when waking from a fully-dark
        // state. STATE_FADING_OUT mid-fade hasn't reached the sleep call yet.
        if (state == STATE_ASLEEP || state == STATE_LIGHT_SLEEP_IDLE) {
            display_hal_exit_sleep();
            power_hal_set_low_power(false);
        }
        // Explicit button press clears the "we entered via host-lock" flag
        // so subsequent data deltas can re-wake naturally.
        idle_entered_by_lock = false;
        begin_fade(awake_brightness, now);
        state = STATE_FADING_IN;
        return true;
    }
    if (state == STATE_FADING_IN) {
        // Mid-wake — still swallow this press; the user shouldn't get a
        // half-wake half-action surprise.
        last_activity_ms = millis();
        return true;
    }
    last_activity_ms = millis();
    return false;
}

bool idle_is_asleep(void) {
    return state == STATE_ASLEEP
        || state == STATE_LIGHT_SLEEP_IDLE
        || state == STATE_FADING_OUT;
}

// ---- Phase B helpers ----

static bool in_work_window(void) {
    if (cur_dow < 0 || cur_hour < 0) return false;       // no clock yet → fail-safe (stays on)
    if (cur_dow > 4) return false;                       // Sat/Sun
    return cur_hour >= IDLE_WORK_HOUR_START && cur_hour < IDLE_WORK_HOUR_END;
}

void idle_set_clock(int8_t dow, int8_t hour, int8_t minute) {
    cur_dow  = dow;
    cur_hour = hour;
    cur_min  = minute;
}

void idle_set_host_locked(bool locked) {
    if (locked == cur_host_locked) return;               // no edge
    cur_host_locked = locked;
    uint32_t now = millis();
    if (locked) {
        // Lock event — immediate light-sleep. Only fades from a fully-awake
        // state; if a fade is already in progress (user-input fade etc.)
        // let it complete, the timer-based paths will hand off.
        if (state == STATE_AWAKE) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
            fade_out_target = STATE_LIGHT_SLEEP_IDLE;
            idle_entered_by_lock = true;
        }
    } else {
        // Unlock — wake if we're in the dark path we entered.
        if (state == STATE_LIGHT_SLEEP_IDLE || state == STATE_FADING_OUT) {
            if (state == STATE_LIGHT_SLEEP_IDLE) {
                display_hal_exit_sleep();
                power_hal_set_low_power(false);
            }
            begin_fade(awake_brightness, now);
            state = STATE_FADING_IN;
            last_activity_ms = now;
            last_data_delta_ms = now;                    // unlock acts as a fresh activity stamp
            idle_entered_by_lock = false;
        }
    }
}

void idle_note_data_delta(int new_s_int, int new_w_int) {
    if (new_s_int == last_s_int && new_w_int == last_w_int) return;
    last_s_int = new_s_int;
    last_w_int = new_w_int;
    last_data_delta_ms = millis();
    // Wake from a data-driven dark state. Don't auto-wake if we went dark
    // because the host locked — only an unlock should bring us back in
    // that case (otherwise a polling daemon write would defeat lock-sleep).
    if (state == STATE_LIGHT_SLEEP_IDLE && !idle_entered_by_lock) {
        display_hal_exit_sleep();
        power_hal_set_low_power(false);
        begin_fade(awake_brightness, last_data_delta_ms);
        state = STATE_FADING_IN;
        last_activity_ms = last_data_delta_ms;
    }
}

void idle_tick(void) {
    uint32_t now = millis();

    // While on USB power (if configured), the 30-min user-input timeout
    // doesn't put the device to sleep, and an already-sleeping device wakes
    // when power comes back. This DELIBERATELY does NOT override
    // STATE_LIGHT_SLEEP_IDLE — that state is reached only via a deliberate
    // signal (host lock, or 20-min Anthropic data-stall) where the user has
    // either walked away or explicitly stepped back from work; USB power
    // shouldn't undo that. STATE_ASLEEP is the legacy user-input timeout
    // and keeps its original "USB == stay on" semantics.
    if (!IDLE_SLEEP_WHEN_CHARGING && power_hal_is_vbus_in()) {
        last_activity_ms = now;
        if (state == STATE_ASLEEP
            || (state == STATE_FADING_OUT && fade_out_target == STATE_ASLEEP)) {
            if (state == STATE_ASLEEP) {
                display_hal_exit_sleep();
                power_hal_set_low_power(false);
            }
            begin_fade(awake_brightness, now);
            state = STATE_FADING_IN;
        }
    }

    // ---- Work-window-closed → deep sleep ----
    // Whenever we're in any non-fading state with a stale data window AND the
    // clock has moved out of the work window, drop straight into deep sleep.
    // Covers both "user closes laptop at 18:01 while device is awake" and
    // "device has been light-sleep-idle since 17:50 and the window just closed".
    // Fires only after we've seen at least one real data delta, so a fresh
    // boot before any daemon traffic can't accidentally sleep forever.
    if (last_data_delta_ms != 0
        && (now - last_data_delta_ms) >= IDLE_DATA_TIMEOUT_MS
        && !in_work_window()
        && (state == STATE_AWAKE || state == STATE_LIGHT_SLEEP_IDLE || state == STATE_ASLEEP)) {
        idle_enter_deep_sleep("work-window closed");  // does not return
    }

    switch (state) {
    case STATE_AWAKE:
        // Existing user-input timeout (30 min) → STATE_ASLEEP.
        if (now - last_activity_ms >= IDLE_TIMEOUT_MS) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
            fade_out_target = STATE_ASLEEP;
            break;
        }
        // Data-stall trigger: during work hours, screen-off after
        // IDLE_DATA_TIMEOUT_MS without an integer change in s/w. Requires
        // a real clock stamp and at least one data delta seen.
        if (in_work_window() && last_data_delta_ms != 0
            && (now - last_data_delta_ms) >= IDLE_DATA_TIMEOUT_MS) {
            begin_fade(0, now);
            state = STATE_FADING_OUT;
            fade_out_target = STATE_LIGHT_SLEEP_IDLE;
            idle_entered_by_lock = false;
        }
        break;

    case STATE_FADING_OUT:
    case STATE_FADING_IN: {
        if (now - fade_last_step_ms < IDLE_FADE_STEP_MS) break;
        fade_last_step_ms = now;
        uint32_t dur = (state == STATE_FADING_OUT) ? IDLE_FADE_OUT_MS : IDLE_FADE_IN_MS;
        uint32_t elapsed = now - fade_started_ms;
        if (elapsed >= dur) {
            apply_brightness(fade_to);
            if (state == STATE_FADING_OUT) {
                // Panel reaches 0 — issue the CO5300 sleep command so the
                // panel's internal boost converter and driver shut down,
                // not just the pixels. Slow PMU polling at the same moment
                // so the I2C bus stops chirping every 50 ms for a screen
                // the user can't see.
                display_hal_enter_sleep();
                power_hal_set_low_power(true);
                state = fade_out_target;
            } else {
                state = STATE_AWAKE;
            }
        } else {
            // Linear interpolation fade_from -> fade_to over dur ms.
            int32_t span = (int32_t)fade_to - (int32_t)fade_from;
            int32_t b = (int32_t)fade_from + (span * (int32_t)elapsed) / (int32_t)dur;
            if (b < 0) b = 0;
            if (b > 255) b = 255;
            apply_brightness((uint8_t)b);
        }
        break;
    }

    case STATE_ASLEEP:
    case STATE_LIGHT_SLEEP_IDLE:
        // Passive — wakes are event-driven: button (STATE_ASLEEP),
        // data delta or unlock (STATE_LIGHT_SLEEP_IDLE).
        break;
    }
}

// ---- Deep sleep (Phase C) ----

uint32_t idle_seconds_to_next_work_window(void) {
    if (cur_dow < 0 || cur_hour < 0 || cur_min < 0) {
        return 12UL * 3600UL;  // safe fallback — should never trigger in practice
    }
    const int mins_today        = (int)cur_hour * 60 + (int)cur_min;
    const int target_mins_today = IDLE_WORK_HOUR_START * 60;

    // Walk forward day by day (up to a full week) until we find the next
    // Mon-Fri at 07:00 that is strictly in the future. Plain loop avoids
    // any modular-arithmetic week-wrap subtleties.
    for (int d = 0; d <= 7; d++) {
        int target_dow = ((int)cur_dow + d) % 7;
        if (target_dow > 4) continue;                       // skip Sat/Sun
        if (d == 0 && target_mins_today <= mins_today) continue;  // 07:00 already passed today
        int total_mins;
        if (d == 0) total_mins = target_mins_today - mins_today;
        else        total_mins = d * 24 * 60 - mins_today + target_mins_today;
        return (uint32_t)total_mins * 60UL;
    }
    return 12UL * 3600UL;                                   // unreachable, but keeps the compiler happy
}

void idle_enter_deep_sleep(const char* reason) {
    const uint32_t secs = idle_seconds_to_next_work_window();
    Serial.printf("deep_sleep: %s, wake in %us\n", reason, secs);
    Serial.flush();

    // Order matters: stop writing to the panel BEFORE telling it to sleep,
    // disconnect BLE BEFORE the radio gets pulled out from under NimBLE.
    display_hal_set_brightness(0);
    display_hal_enter_sleep();

    ble_disconnect_all();
    delay(150);  // let NimBLE flush the disconnect PDU before deep sleep

    power_hal_enter_deep_sleep(secs);  // configures wake sources + esp_deep_sleep_start, never returns
    // Belt-and-braces — if a stub board's power_hal_enter_deep_sleep does
    // return, fall through to a software reset rather than spin in a broken
    // state. Shared code stays warning-clean about the noreturn attribute.
    esp_restart();
}
