#pragma once
#include <stdbool.h>
#include <stdint.h>

void idle_init(void);
void idle_tick(void);
void idle_note_activity(void);

// Set the "awake" brightness target (0..255). idle owns display brightness
// (it fades between this and 0), so user brightness control routes through
// here. Applied immediately if the screen is currently fully awake; otherwise
// picked up by the next fade-in. See brightness.{h,cpp}.
void idle_set_awake_brightness(uint8_t level);

// Returns true if this press was consumed as a wake-up (caller MUST skip the
// button's normal action). Returns false when already awake — also notes the
// activity, so callers don't need a separate idle_note_activity() call.
bool idle_consume_wake_press(void);

// Touch should NOT count as activity (avoids accidental wakes from pets,
// sleeves, etc.). Callers use this to silently drop touch events while the
// panel is dark.
bool idle_is_asleep(void);

// ---- Host-driven activity signals (set from main.cpp parse_json) ----

// Latest host wall-clock stamp. dow=Mon..Sun (0..6) per Python weekday();
// hour 0..23; minute 0..59. Pass any value <0 to mean "not yet known"
// (idle will refuse to screen-off until a real stamp lands).
void idle_set_clock(int8_t dow, int8_t hour, int8_t minute);

// Host PC lock state. Transitioning to locked while awake immediately
// triggers a fade-out to STATE_LIGHT_SLEEP_IDLE. Transitioning to
// unlocked while in that state wakes the device.
void idle_set_host_locked(bool locked);

// Called for every successful JSON payload. If the integer values of s or w
// differ from what we last saw, bumps "last data activity" and wakes
// STATE_LIGHT_SLEEP_IDLE. Equal values are silent (no FSM effect).
void idle_note_data_delta(int new_s_int, int new_w_int);

// True when no real delta in s/w has been seen for IDLE_ANIM_FREEZE_MS.
// ui_tick_anim() checks this to stop redrawing the spinner during idle.
bool idle_animation_should_freeze(void);

// ---- Deep sleep (Phase C) ----

// Pure helper: compute seconds from the last known host wall-clock to the
// next Mon-Fri 07:00. Mon 22:00 → ~9 h, Sat 17:00 → ~38 h, Sun 23:59 → ~7 h.
// Falls back to 12 hours when no clock has ever been seen (safe default —
// the deep-sleep entry only fires after a real payload was received anyway).
uint32_t idle_seconds_to_next_work_window(void);

// Tear everything down and enter ESP32 deep sleep. Does not return.
// Wake sources are configured by power_hal_enter_deep_sleep() — currently
// the timer + the board's BOOT/primary button as an RTC-GPIO EXT1 wake.
// `reason` is logged once over Serial before the radio goes dark.
void idle_enter_deep_sleep(const char* reason) __attribute__((noreturn));
