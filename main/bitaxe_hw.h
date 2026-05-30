#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "global_state.h"
#include "m45_config.h"

typedef struct {
    bool valid;
    uint16_t status_word;
    uint8_t operation;
    float vout_command;
    float read_vout;
    float read_vin;
    float read_iout;
    int read_temp_c;
} bitaxe_gamma602_power_snapshot_t;

typedef struct {
    float input_voltage_min_v;
    float input_voltage_expected_min_v;
    float input_voltage_expected_max_v;
    float input_voltage_max_v;
    float asic_voltage_min_v;
    float asic_voltage_expected_min_v;
    float asic_voltage_expected_max_v;
    float asic_voltage_max_v;
    float asic_voltage_target_v;
    float asic_temp_expected_max_c;
    float asic_temp_max_c;
    float tps546_temp_expected_max_c;
    float tps546_temp_max_c;
    float iout_warn_a;
    float iout_fault_a;
    float power_warn_w;
    float power_fault_w;
    float fan_expected_percent;
} bitaxe_gamma602_safety_limits_t;

void bitaxe_gamma602_init_state(GlobalState *state);
esp_err_t bitaxe_gamma602_prepare_io(void);
esp_err_t bitaxe_gamma602_boot_fan_max(void);
esp_err_t bitaxe_gamma602_start_fan(GlobalState *state);
esp_err_t bitaxe_gamma602_start_hardware(GlobalState *state);
bool bitaxe_gamma602_asic_power_enabled(void);
esp_err_t bitaxe_gamma602_set_asic_power(GlobalState *state, bool enabled, bool manage_fan);
esp_err_t bitaxe_gamma602_set_frequency_mhz(GlobalState *state, uint16_t frequency_mhz);
esp_err_t bitaxe_gamma602_set_voltage_mv(GlobalState *state, uint16_t voltage_mv);
esp_err_t bitaxe_gamma602_set_voltage_mv_for_config(GlobalState *state, uint16_t voltage_mv,
                                                    const m45_config_t *config);
esp_err_t bitaxe_gamma602_apply_safety_limits(const m45_config_t *config);
void bitaxe_gamma602_clear_jobs(GlobalState *state);
uint8_t bitaxe_gamma602_chip_count(void);
const char *bitaxe_gamma602_status(void);
const char *bitaxe_gamma602_tps_model(void);
bool bitaxe_gamma602_power_snapshot(bitaxe_gamma602_power_snapshot_t *snapshot);
void bitaxe_gamma602_safety_limits(bitaxe_gamma602_safety_limits_t *limits);
