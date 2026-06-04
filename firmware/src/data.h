#pragma once
#include <Arduino.h>

struct UsageData {
    float session_pct;       // 5-hour window utilization (0-100)
    int session_reset_mins;  // minutes until session resets
    float weekly_pct;        // 7-day window utilization (0-100)
    int weekly_reset_mins;   // minutes until weekly resets
    char status[16];         // "allowed" or "limited"
    bool ok;                 // data parse succeeded
    bool valid;              // false until first successful parse
    int8_t dow;              // host day-of-week, 0=Mon..6=Sun, -1 = unknown
    int8_t hour;             // host local hour 0..23, -1 = unknown
    int8_t min;              // host local minute 0..59, -1 = unknown
    bool host_locked;        // host session is locked (Win+L / lock screen)
};
