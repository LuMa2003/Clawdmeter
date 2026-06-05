#pragma once

// Auto-sleep / idle screen-off configuration.
// All tunables live here so nothing is hard-coded in main.cpp / idle.cpp.

#define IDLE_TIMEOUT_MS             (30UL * 60UL * 1000UL)  // 30 min user-input timeout (existing)
#define IDLE_FADE_OUT_MS            400      // fade-to-black duration
#define IDLE_FADE_IN_MS             180      // wake fade-in (snappier)
#define IDLE_FADE_STEP_MS           20       // tick interval per fade step

#define DISPLAY_DEFAULT_BRIGHTNESS  200      // active-screen brightness

// ---- Activity-driven idle (Phase B) ----
// Screen-off threshold when no integer change in s/w during work hours.
// 20 min picked because Anthropic returns 1%-resolution utilization and on
// Max plan that integer can stall 20-30 min during real coding.
#define IDLE_DATA_TIMEOUT_MS        (20UL * 60UL * 1000UL)

// Spinner freeze threshold — stops animating after this long with no delta.
// Shorter than IDLE_DATA_TIMEOUT_MS so the visual cue lands while the
// screen is still on.
#define IDLE_ANIM_FREEZE_MS         (5UL * 60UL * 1000UL)

// Work window. Python weekday() — Mon=0 .. Sun=6. Window is [START, END)
// hours, so 7..18 == 07:00 inclusive through 17:59 inclusive.
#define IDLE_WORK_HOUR_START        7
#define IDLE_WORK_HOUR_END          18

// When false, the device never enters sleep while USB power is present (also
// wakes from sleep when USB is plugged back in). Useful when sitting on a
// desk plugged in — also covers battery-less hardware that's always on USB.
// Set true to sleep regardless of power source.
#define IDLE_SLEEP_WHEN_CHARGING    false
