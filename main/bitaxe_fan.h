#pragma once

#include "esp_err.h"
#include "global_state.h"
#include "m45_config.h"

esp_err_t bitaxe_fan_boot_max(void);
esp_err_t bitaxe_fan_init(GlobalState *state);
esp_err_t bitaxe_fan_apply_config(GlobalState *state, const m45_config_t *config);
esp_err_t bitaxe_fan_start_for_asic(GlobalState *state);
esp_err_t bitaxe_fan_stop_for_asic(GlobalState *state);
esp_err_t bitaxe_fan_force_max_if_allowed(GlobalState *state, const char *reason);
esp_err_t bitaxe_fan_read_asic_temp_c(float *temp_c);
float bitaxe_fan_control_temp_c(float asic_temp_c);
void bitaxe_fan_update_auto(GlobalState *state, float raw_asic_temp_c, float control_temp_c,
                            float vr_temp_c);
