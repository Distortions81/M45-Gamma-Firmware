#pragma once

#include "esp_err.h"
#include "global_state.h"

esp_err_t m45_oled_init(void);
void m45_oled_start_status_task(GlobalState *state);
void m45_oled_show_wifi_recovery(void);
void m45_oled_show_factory_reset(void);

typedef enum {
    M45_OLED_RECOVERY_NONE = 0,
    M45_OLED_RECOVERY_FACTORY_RESET,
} m45_oled_recovery_action_t;

m45_oled_recovery_action_t m45_oled_recovery_action(void);
