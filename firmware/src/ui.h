#pragma once
#include "data.h"
#include "ble.h"

void ui_init(void);
void ui_update(const UsageData* data);
void ui_update_ble_status(ble_state_t state, const char* name, const char* mac);
void ui_update_battery(int percent, bool charging);
