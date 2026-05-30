#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define M45_WIFI_SSID_MAX 32
#define M45_WIFI_PASSWORD_MAX 64
#define M45_HOSTNAME_MAX 32
#define M45_POOL_HOST_MAX 96
#define M45_POOL_IP_MAX 45
#define M45_POOL_USER_MAX 128
#define M45_POOL_PASS_MAX 64
#define M45_WALLET_ADDRESS_MAX 128
#define M45_FAN_TARGET_DEFAULT_C 62
#define M45_FAN_TARGET_MIN_C 35
#define M45_FAN_TARGET_MAX_C 66
#define M45_DISPLAY_SLEEP_DEFAULT_MINUTES 0
#define M45_DISPLAY_SLEEP_MAX_MINUTES UINT16_MAX
#define M45_ASIC_FREQUENCY_MIN_MHZ 1
#define M45_ASIC_FREQUENCY_MAX_MHZ 1500
#define M45_SAFETY_INPUT_VOLTAGE_MIN_DEFAULT_MV 4500
#define M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MIN_DEFAULT_MV 4800
#define M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MAX_DEFAULT_MV 5400
#define M45_SAFETY_INPUT_VOLTAGE_MAX_DEFAULT_MV 5500
#define M45_SAFETY_ASIC_VOLTAGE_MIN_DEFAULT_MV 700
#define M45_SAFETY_ASIC_VOLTAGE_MAX_DEFAULT_MV 1400
#define M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_DEFAULT_C 60
#define M45_SAFETY_ASIC_TEMP_MAX_DEFAULT_C 69
#define M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_DEFAULT_C 85
#define M45_SAFETY_TPS546_TEMP_MAX_DEFAULT_C 98
#define M45_SAFETY_IOUT_WARN_DEFAULT_DA 250
#define M45_SAFETY_IOUT_FAULT_DEFAULT_DA 300
#define M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV 4500
#define M45_SAFETY_INPUT_VOLTAGE_MIN_MAX_MV 5200
#define M45_SAFETY_INPUT_VOLTAGE_MAX_MIN_MV 5000
#define M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV 5500
#define M45_SAFETY_ASIC_VOLTAGE_MIN_MIN_MV 700
#define M45_SAFETY_ASIC_VOLTAGE_MIN_MAX_MV 1200
#define M45_SAFETY_ASIC_VOLTAGE_MAX_MIN_MV 800
#define M45_SAFETY_ASIC_VOLTAGE_MAX_MAX_MV 1400
#define M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_MIN_C 35
#define M45_SAFETY_ASIC_TEMP_MAX_MIN_C 45
#define M45_SAFETY_ASIC_TEMP_MAX_MAX_C 69
#define M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_MIN_C 40
#define M45_SAFETY_TPS546_TEMP_MAX_MIN_C 60
#define M45_SAFETY_TPS546_TEMP_MAX_MAX_C 98
#define M45_SAFETY_IOUT_WARN_MIN_DA 50
#define M45_SAFETY_IOUT_FAULT_MIN_DA 60
#define M45_SAFETY_IOUT_FAULT_MAX_DA 300

typedef struct {
    char wifi_ssid[M45_WIFI_SSID_MAX + 1];
    char wifi_password[M45_WIFI_PASSWORD_MAX + 1];
    char hostname[M45_HOSTNAME_MAX + 1];
    char pool_host[M45_POOL_HOST_MAX + 1];
    char backup_pool_host[M45_POOL_HOST_MAX + 1];
    char pool_ip[M45_POOL_IP_MAX + 1];
    char backup_pool_ip[M45_POOL_IP_MAX + 1];
    char pool_user[M45_POOL_USER_MAX + 1];
    char pool_pass[M45_POOL_PASS_MAX + 1];
    uint16_t pool_port;
    uint16_t backup_pool_port;
    uint16_t pool_difficulty;
    bool pool_difficulty_auto;
    bool overclock_enabled;
    uint16_t asic_frequency_mhz;
    uint16_t asic_voltage_mv;
    int16_t overclock_voltage_offset_mv;
    bool asic_voltage_temp_compensation_enabled;
    bool fan_override_enabled;
    uint16_t fan_override_percent;
    bool fan_auto_off_allowed;
    bool fan_target_override_enabled;
    uint16_t fan_target_temp_c;
    bool display_screensaver_enabled;
    uint16_t display_sleep_minutes;
    bool safety_limits_unrestricted;
    uint16_t safety_input_voltage_min_mv;
    uint16_t safety_input_voltage_expected_min_mv;
    uint16_t safety_input_voltage_expected_max_mv;
    uint16_t safety_input_voltage_max_mv;
    uint16_t safety_asic_voltage_min_mv;
    uint16_t safety_asic_voltage_max_mv;
    uint16_t safety_asic_temp_expected_max_c;
    uint16_t safety_asic_temp_max_c;
    uint16_t safety_tps546_temp_expected_max_c;
    uint16_t safety_tps546_temp_max_c;
    uint16_t safety_iout_warn_deciamps;
    uint16_t safety_iout_fault_deciamps;
    double best_diff;
} m45_config_t;

esp_err_t m45_config_load(void);
esp_err_t m45_config_save(const m45_config_t *config);
esp_err_t m45_config_set_runtime(const m45_config_t *config);
esp_err_t m45_config_factory_reset(void);
esp_err_t m45_config_set_pool_ip_cache(bool backup_pool, const char *expected_host,
                                       const char *ip);
esp_err_t m45_config_set_best_diff(double best_diff);
esp_err_t m45_config_reset_best_diff(void);
const m45_config_t *m45_config_get(void);
uint16_t m45_config_auto_pool_difficulty(uint16_t frequency_mhz,
                                         uint16_t small_core_count,
                                         uint8_t asic_count);
uint16_t m45_config_effective_pool_difficulty(const m45_config_t *config,
                                              uint16_t small_core_count,
                                              uint8_t asic_count);
uint16_t m45_config_effective_asic_frequency_mhz(const m45_config_t *config);
uint16_t m45_config_effective_asic_voltage_mv(const m45_config_t *config);
int16_t m45_config_asic_voltage_temp_compensation_mv(const m45_config_t *config,
                                                     float asic_temp_c);
uint16_t m45_config_effective_asic_voltage_mv_for_temp(const m45_config_t *config,
                                                       float asic_temp_c);
uint16_t m45_config_effective_fan_target_temp_c(const m45_config_t *config);
