#pragma once

#include "esp_err.h"
#include "global_state.h"

esp_err_t m45_oled_init(void);
void m45_oled_start_status_task(GlobalState *state);
void m45_oled_show_factory_reset(void);
