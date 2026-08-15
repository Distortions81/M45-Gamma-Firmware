#include "m45_config.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "nvs.h"

#define M45_CONFIG_NAMESPACE "m45"
#define M45_DEFAULT_HOSTNAME_BASE "M45-Firmware"
#define M45_LEGACY_HOSTNAME_BASE "m45-bitaxe"
#define M45_HOSTNAME_SUFFIX_HEX_CHARS 6
#define M45_HOSTNAME_SUFFIX_CHARS (1 + M45_HOSTNAME_SUFFIX_HEX_CHARS)
#define M45_FAN_OVERRIDE_MIN_PERCENT 35
#define M45_FAN_OVERRIDE_MAX_PERCENT 100
#define M45_ASIC_VOLTAGE_MIN_MV 500
#define M45_ASIC_VOLTAGE_MAX_MV 1370
#define M45_ASIC_TEMP_COMP_MIN_C 25.0f
#define M45_ASIC_TEMP_COMP_MAX_C 70.0f
#define M45_ASIC_TEMP_COMP_REFERENCE_C 60.0f
#define M45_ASIC_TEMP_COMP_MV_PER_C 5.5f
#define M45_ASIC_TEMP_COMP_STEP_MV 5.0f
#define M45_OC_VOLTAGE_OFFSET_MIN_MV (-500)
#define M45_OC_VOLTAGE_OFFSET_MAX_MV 300
#define M45_STRATUM_TARGET_SHARES_PER_MIN 14ULL
#define M45_STRATUM_DIFF_HASHES 4294967296ULL

static const char *TAG = "m45_config";
#ifndef CONFIG_M45_BITAXE_STRATUM_BACKUP_HOST
#define CONFIG_M45_BITAXE_STRATUM_BACKUP_HOST "public-pool.io"
#endif
#ifndef CONFIG_M45_BITAXE_STRATUM_BACKUP_PORT
#define CONFIG_M45_BITAXE_STRATUM_BACKUP_PORT 3333
#endif
#ifdef CONFIG_M45_BITAXE_STRATUM_TLS
#define M45_DEFAULT_POOL_TLS true
#else
#define M45_DEFAULT_POOL_TLS false
#endif
#ifdef CONFIG_M45_BITAXE_STRATUM_BACKUP_TLS
#define M45_DEFAULT_BACKUP_POOL_TLS true
#else
#define M45_DEFAULT_BACKUP_POOL_TLS false
#endif
#ifdef CONFIG_M45_BITAXE_STRATUM_DIFFICULTY_AUTO
#define M45_DEFAULT_POOL_DIFFICULTY_AUTO true
#else
#define M45_DEFAULT_POOL_DIFFICULTY_AUTO false
#endif

static m45_config_t g_config;
static pthread_mutex_t g_config_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local m45_config_t g_config_snapshot;

static void config_store_runtime(const m45_config_t *config)
{
    pthread_mutex_lock(&g_config_lock);
    g_config = *config;
    pthread_mutex_unlock(&g_config_lock);
}

static void config_copy_runtime(m45_config_t *config)
{
    pthread_mutex_lock(&g_config_lock);
    *config = g_config;
    pthread_mutex_unlock(&g_config_lock);
}

static esp_err_t store_optional_password(nvs_handle_t nvs, const char *key,
                                         const char *password)
{
    if (password == NULL || password[0] == '\0' || strcmp(password, "x") == 0) {
        const esp_err_t err = nvs_erase_key(nvs, key);
        return err == ESP_ERR_NVS_NOT_FOUND ? ESP_OK : err;
    }
    return nvs_set_str(nvs, key, password);
}

static const char *configured_hostname_base(void)
{
    if (CONFIG_M45_BITAXE_HOSTNAME[0] == '\0' ||
        strcmp(CONFIG_M45_BITAXE_HOSTNAME, M45_LEGACY_HOSTNAME_BASE) == 0) {
        return M45_DEFAULT_HOSTNAME_BASE;
    }
    return CONFIG_M45_BITAXE_HOSTNAME;
}

static void set_default_hostname(char *hostname, size_t hostname_size)
{
    uint8_t mac[6] = {0};
    char base[M45_HOSTNAME_MAX + 1];
    strlcpy(base, configured_hostname_base(), sizeof(base));

    if (esp_efuse_mac_get_default(mac) != ESP_OK) {
        strlcpy(hostname, base, hostname_size);
        return;
    }

    if (strlen(base) + M45_HOSTNAME_SUFFIX_CHARS > M45_HOSTNAME_MAX) {
        base[M45_HOSTNAME_MAX - M45_HOSTNAME_SUFFIX_CHARS] = '\0';
    }
    snprintf(hostname, hostname_size, "%s-%02x%02x%02x", base, mac[3], mac[4],
             mac[5]);
}

static void set_default_safety_limits(m45_config_t *config)
{
    config->safety_input_voltage_min_mv = M45_SAFETY_INPUT_VOLTAGE_MIN_DEFAULT_MV;
    config->safety_input_voltage_expected_min_mv =
        M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MIN_DEFAULT_MV;
    config->safety_input_voltage_expected_max_mv =
        M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MAX_DEFAULT_MV;
    config->safety_input_voltage_max_mv = M45_SAFETY_INPUT_VOLTAGE_MAX_DEFAULT_MV;
    config->safety_asic_voltage_min_mv = M45_SAFETY_ASIC_VOLTAGE_MIN_DEFAULT_MV;
    config->safety_asic_voltage_max_mv = M45_SAFETY_ASIC_VOLTAGE_MAX_DEFAULT_MV;
    config->safety_asic_temp_expected_max_c =
        M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_DEFAULT_C;
    config->safety_asic_temp_max_c = M45_SAFETY_ASIC_TEMP_MAX_DEFAULT_C;
    config->safety_tps546_temp_expected_max_c =
        M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_DEFAULT_C;
    config->safety_tps546_temp_max_c = M45_SAFETY_TPS546_TEMP_MAX_DEFAULT_C;
    config->safety_iout_warn_deciamps = M45_SAFETY_IOUT_WARN_DEFAULT_DA;
    config->safety_iout_fault_deciamps = M45_SAFETY_IOUT_FAULT_DEFAULT_DA;
}

static uint16_t default_if_outside(uint16_t value, uint16_t default_value,
                                   uint16_t min_value, uint16_t max_value)
{
    return value < min_value || value > max_value ? default_value : value;
}

static uint16_t clamp_u16(uint16_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return value;
}

static bool hostname_is_unsuffixed_default(const char *hostname)
{
    return hostname[0] == '\0' || strcmp(hostname, CONFIG_M45_BITAXE_HOSTNAME) == 0 ||
           strcmp(hostname, M45_DEFAULT_HOSTNAME_BASE) == 0 ||
           strcmp(hostname, M45_LEGACY_HOSTNAME_BASE) == 0;
}

static void set_defaults(m45_config_t *config)
{
    memset(config, 0, sizeof(*config));
    strlcpy(config->wifi_ssid, CONFIG_M45_BITAXE_WIFI_SSID, sizeof(config->wifi_ssid));
    strlcpy(config->wifi_password, CONFIG_M45_BITAXE_WIFI_PASSWORD,
            sizeof(config->wifi_password));
    set_default_hostname(config->hostname, sizeof(config->hostname));
    strlcpy(config->pool_host, CONFIG_M45_BITAXE_STRATUM_HOST, sizeof(config->pool_host));
    strlcpy(config->backup_pool_host, CONFIG_M45_BITAXE_STRATUM_BACKUP_HOST,
            sizeof(config->backup_pool_host));
    strlcpy(config->pool_user, CONFIG_M45_BITAXE_STRATUM_USER, sizeof(config->pool_user));
    strlcpy(config->pool_pass, CONFIG_M45_BITAXE_STRATUM_PASS, sizeof(config->pool_pass));
    config->pool_port = CONFIG_M45_BITAXE_STRATUM_PORT;
    config->backup_pool_port = CONFIG_M45_BITAXE_STRATUM_BACKUP_PORT;
    config->pool_tls = M45_DEFAULT_POOL_TLS;
    config->backup_pool_tls = M45_DEFAULT_BACKUP_POOL_TLS;
    config->multi_pool_enabled = false;
    config->pool_difficulty = CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY;
    config->pool_difficulty_auto = M45_DEFAULT_POOL_DIFFICULTY_AUTO;
    config->overclock_enabled = false;
    config->auto_clock_enabled = false;
    config->auto_domain_reboot_enabled = false;
    config->auto_clock_target_temp_c = M45_AUTO_CLOCK_TARGET_DEFAULT_C;
    config->auto_clock_max_watts_enabled = true;
    config->auto_clock_max_watts = M45_AUTO_CLOCK_MAX_WATTS_DEFAULT;
    config->asic_frequency_mhz = CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ;
    config->asic_voltage_mv = CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV;
    config->overclock_voltage_offset_mv = 0;
    config->asic_voltage_temp_compensation_enabled = true;
    config->fan_override_enabled = false;
    config->fan_override_percent = M45_FAN_OVERRIDE_MAX_PERCENT;
    config->fan_auto_off_allowed = false;
    config->fan_target_override_enabled = false;
    config->fan_target_temp_c = M45_FAN_TARGET_DEFAULT_C;
    config->display_screensaver_enabled = true;
    config->display_sleep_minutes = M45_DISPLAY_SLEEP_DEFAULT_MINUTES;
    config->safety_limits_unrestricted = false;
    set_default_safety_limits(config);
    config->best_diff = 0.0;
}

static void load_string(nvs_handle_t nvs, const char *key, char *value, size_t value_size)
{
    size_t required = value_size;
    esp_err_t err = nvs_get_str(nvs, key, value, &required);
    if (err != ESP_OK) {
        value[value_size - 1] = '\0';
    }
}

static void load_u16(nvs_handle_t nvs, const char *key, uint16_t *value)
{
    uint16_t stored = 0;
    if (nvs_get_u16(nvs, key, &stored) == ESP_OK) {
        *value = stored;
    }
}

static void load_i16(nvs_handle_t nvs, const char *key, int16_t *value)
{
    int16_t stored = 0;
    if (nvs_get_i16(nvs, key, &stored) == ESP_OK) {
        *value = stored;
    }
}

static void load_u8(nvs_handle_t nvs, const char *key, uint8_t *value)
{
    uint8_t stored = 0;
    if (nvs_get_u8(nvs, key, &stored) == ESP_OK) {
        *value = stored;
    }
}

static void load_double(nvs_handle_t nvs, const char *key, double *value)
{
    double stored = 0.0;
    size_t size = sizeof(stored);
    if (nvs_get_blob(nvs, key, &stored, &size) == ESP_OK && size == sizeof(stored)) {
        *value = stored;
    }
}

void m45_config_apply_auto_clock_policy(m45_config_t *config)
{
    if (config == NULL) {
        return;
    }

    /* Disabled overclock is stock mode; scrub stale overclock-panel values. */
    if (!config->overclock_enabled) {
        config->auto_clock_enabled = false;
        config->auto_domain_reboot_enabled = false;
        config->auto_clock_target_temp_c = M45_AUTO_CLOCK_TARGET_DEFAULT_C;
        config->auto_clock_max_watts_enabled = true;
        config->auto_clock_max_watts = M45_AUTO_CLOCK_MAX_WATTS_DEFAULT;
        config->asic_frequency_mhz = CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ;
        config->asic_voltage_mv = CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV;
        config->overclock_voltage_offset_mv = 0;
        config->asic_voltage_temp_compensation_enabled = true;
        config->safety_limits_unrestricted = false;
        set_default_safety_limits(config);
        return;
    }

    if (!config->auto_clock_enabled) {
        config->auto_clock_enabled = false;
        return;
    }

    /* Auto clock owns clock/voltage at runtime; persisted presets stay stock. */
    config->asic_frequency_mhz = CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ;
    config->asic_voltage_mv = CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV;

    if (!config->fan_override_enabled) {
        config->fan_override_enabled = true;
        config->fan_override_percent = M45_FAN_OVERRIDE_MAX_PERCENT;
    }
    if (config->fan_override_percent != 0 &&
        (config->fan_override_percent < M45_FAN_OVERRIDE_MIN_PERCENT ||
         config->fan_override_percent > M45_FAN_OVERRIDE_MAX_PERCENT)) {
        config->fan_override_percent = M45_FAN_OVERRIDE_MAX_PERCENT;
    }
    config->fan_auto_off_allowed = false;
}

static void sanitize_config(m45_config_t *config)
{
    if (hostname_is_unsuffixed_default(config->hostname)) {
        set_default_hostname(config->hostname, sizeof(config->hostname));
    }
    if (config->pool_host[0] == '\0') {
        strlcpy(config->pool_host, CONFIG_M45_BITAXE_STRATUM_HOST, sizeof(config->pool_host));
    }
    if (config->backup_pool_host[0] == '\0') {
        strlcpy(config->backup_pool_host, CONFIG_M45_BITAXE_STRATUM_BACKUP_HOST,
                sizeof(config->backup_pool_host));
    }
    if (!config->multi_pool_enabled && config->pool_user[0] == '\0') {
        strlcpy(config->pool_user, CONFIG_M45_BITAXE_STRATUM_USER, sizeof(config->pool_user));
    }
    if (!config->multi_pool_enabled && config->pool_pass[0] == '\0') {
        strlcpy(config->pool_pass, "x", sizeof(config->pool_pass));
    }
    if (config->pool_port < 1) {
        config->pool_port = CONFIG_M45_BITAXE_STRATUM_PORT;
    }
    if (config->backup_pool_port < 1) {
        config->backup_pool_port = CONFIG_M45_BITAXE_STRATUM_BACKUP_PORT;
    }
    for (size_t i = 0; i < M45_AUX_POOL_MAX; ++i) {
        m45_aux_pool_t *aux = &config->aux_pools[i];
        aux->host[M45_POOL_HOST_MAX] = '\0';
        aux->ip[M45_POOL_IP_MAX] = '\0';
        aux->user[M45_POOL_USER_MAX] = '\0';
        aux->pass[M45_POOL_PASS_MAX] = '\0';
        if (aux->host[0] == '\0') {
            aux->ip[0] = '\0';
            aux->user[0] = '\0';
            aux->pass[0] = '\0';
            aux->port = 0;
            aux->tls = false;
            aux->enabled = false;
            aux->share_percent = 0;
            continue;
        }
        if (aux->port < 1) {
            aux->port = CONFIG_M45_BITAXE_STRATUM_PORT;
        }
        if (!config->multi_pool_enabled || !aux->enabled) {
            continue;
        }
        if (aux->share_percent < M45_POOL_WEIGHT_MIN) {
            aux->enabled = false;
            aux->share_percent = 0;
            continue;
        }
        if (aux->share_percent > M45_POOL_WEIGHT_MAX) {
            aux->share_percent = M45_POOL_WEIGHT_MAX;
        }
    }
    if (config->pool_difficulty < 1) {
        config->pool_difficulty = CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY;
    }
    config->pool_ip[M45_POOL_IP_MAX] = '\0';
    config->backup_pool_ip[M45_POOL_IP_MAX] = '\0';
    if (config->asic_frequency_mhz < M45_ASIC_FREQUENCY_MIN_MHZ ||
        config->asic_frequency_mhz > M45_ASIC_FREQUENCY_MAX_MHZ) {
        config->asic_frequency_mhz = CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ;
    }
    if (config->asic_voltage_mv < M45_ASIC_VOLTAGE_MIN_MV ||
        config->asic_voltage_mv > M45_ASIC_VOLTAGE_MAX_MV) {
        config->asic_voltage_mv = CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV;
    }
    if (config->overclock_voltage_offset_mv < M45_OC_VOLTAGE_OFFSET_MIN_MV ||
        config->overclock_voltage_offset_mv > M45_OC_VOLTAGE_OFFSET_MAX_MV) {
        config->overclock_voltage_offset_mv = 0;
    }
    if (config->fan_override_percent != 0 &&
        (config->fan_override_percent < M45_FAN_OVERRIDE_MIN_PERCENT ||
         config->fan_override_percent > M45_FAN_OVERRIDE_MAX_PERCENT)) {
        config->fan_override_percent = M45_FAN_OVERRIDE_MAX_PERCENT;
    }
    if (config->fan_target_temp_c < M45_FAN_TARGET_MIN_C ||
        config->fan_target_temp_c > M45_FAN_TARGET_MAX_C) {
        config->fan_target_temp_c = M45_FAN_TARGET_DEFAULT_C;
    }
    if (config->auto_clock_target_temp_c < M45_AUTO_CLOCK_TARGET_MIN_C ||
        config->auto_clock_target_temp_c > M45_AUTO_CLOCK_TARGET_MAX_C) {
        config->auto_clock_target_temp_c = M45_AUTO_CLOCK_TARGET_DEFAULT_C;
    }
    if (config->auto_clock_max_watts < M45_AUTO_CLOCK_MAX_WATTS_MIN ||
        config->auto_clock_max_watts > M45_AUTO_CLOCK_MAX_WATTS_MAX) {
        config->auto_clock_max_watts = M45_AUTO_CLOCK_MAX_WATTS_DEFAULT;
    }
    m45_config_apply_auto_clock_policy(config);
    if (!config->safety_limits_unrestricted) {
        config->safety_input_voltage_min_mv =
            default_if_outside(config->safety_input_voltage_min_mv,
                               M45_SAFETY_INPUT_VOLTAGE_MIN_DEFAULT_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MIN_MAX_MV);
        config->safety_input_voltage_max_mv =
            default_if_outside(config->safety_input_voltage_max_mv,
                               M45_SAFETY_INPUT_VOLTAGE_MAX_DEFAULT_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MAX_MIN_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV);
    }
    if (config->safety_input_voltage_min_mv >= config->safety_input_voltage_max_mv) {
        config->safety_input_voltage_min_mv = M45_SAFETY_INPUT_VOLTAGE_MIN_DEFAULT_MV;
        config->safety_input_voltage_max_mv = M45_SAFETY_INPUT_VOLTAGE_MAX_DEFAULT_MV;
    }
    if (!config->safety_limits_unrestricted) {
        config->safety_input_voltage_expected_min_mv =
            default_if_outside(config->safety_input_voltage_expected_min_mv,
                               M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MIN_DEFAULT_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV);
        config->safety_input_voltage_expected_max_mv =
            default_if_outside(config->safety_input_voltage_expected_max_mv,
                               M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MAX_DEFAULT_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                               M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV);
    }
    if (config->safety_input_voltage_expected_min_mv >=
            config->safety_input_voltage_expected_max_mv ||
        config->safety_input_voltage_expected_min_mv <
            config->safety_input_voltage_min_mv ||
        config->safety_input_voltage_expected_max_mv >
            config->safety_input_voltage_max_mv) {
        config->safety_input_voltage_expected_min_mv =
            clamp_u16(M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MIN_DEFAULT_MV,
                      config->safety_input_voltage_min_mv,
                      config->safety_input_voltage_max_mv);
        config->safety_input_voltage_expected_max_mv =
            clamp_u16(M45_SAFETY_INPUT_VOLTAGE_EXPECTED_MAX_DEFAULT_MV,
                      config->safety_input_voltage_min_mv,
                      config->safety_input_voltage_max_mv);
        if (config->safety_input_voltage_expected_min_mv >=
            config->safety_input_voltage_expected_max_mv) {
            config->safety_input_voltage_expected_min_mv =
                config->safety_input_voltage_min_mv;
            config->safety_input_voltage_expected_max_mv =
                config->safety_input_voltage_max_mv;
        }
    }
    if (!config->safety_limits_unrestricted) {
        config->safety_asic_voltage_min_mv =
            default_if_outside(config->safety_asic_voltage_min_mv,
                               M45_SAFETY_ASIC_VOLTAGE_MIN_DEFAULT_MV,
                               M45_SAFETY_ASIC_VOLTAGE_MIN_MIN_MV,
                               M45_SAFETY_ASIC_VOLTAGE_MIN_MAX_MV);
        config->safety_asic_voltage_max_mv =
            default_if_outside(config->safety_asic_voltage_max_mv,
                               M45_SAFETY_ASIC_VOLTAGE_MAX_DEFAULT_MV,
                               M45_SAFETY_ASIC_VOLTAGE_MAX_MIN_MV,
                               M45_SAFETY_ASIC_VOLTAGE_MAX_MAX_MV);
    }
    if (config->safety_asic_voltage_min_mv >= config->safety_asic_voltage_max_mv) {
        config->safety_asic_voltage_min_mv = M45_SAFETY_ASIC_VOLTAGE_MIN_DEFAULT_MV;
        config->safety_asic_voltage_max_mv = M45_SAFETY_ASIC_VOLTAGE_MAX_DEFAULT_MV;
    }
    if (!config->safety_limits_unrestricted) {
        config->safety_asic_temp_max_c =
            default_if_outside(config->safety_asic_temp_max_c,
                               M45_SAFETY_ASIC_TEMP_MAX_DEFAULT_C,
                               M45_SAFETY_ASIC_TEMP_MAX_MIN_C,
                               M45_SAFETY_ASIC_TEMP_MAX_MAX_C);
        config->safety_asic_temp_expected_max_c =
            default_if_outside(config->safety_asic_temp_expected_max_c,
                               M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_DEFAULT_C,
                               M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_MIN_C,
                               M45_SAFETY_ASIC_TEMP_MAX_MAX_C);
    }
    if (config->safety_asic_temp_expected_max_c > config->safety_asic_temp_max_c) {
        config->safety_asic_temp_expected_max_c =
            config->safety_asic_temp_max_c;
    }
    if (!config->safety_limits_unrestricted) {
        config->safety_tps546_temp_max_c =
            default_if_outside(config->safety_tps546_temp_max_c,
                               M45_SAFETY_TPS546_TEMP_MAX_DEFAULT_C,
                               M45_SAFETY_TPS546_TEMP_MAX_MIN_C,
                               M45_SAFETY_TPS546_TEMP_MAX_MAX_C);
        config->safety_tps546_temp_expected_max_c =
            default_if_outside(config->safety_tps546_temp_expected_max_c,
                               M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_DEFAULT_C,
                               M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_MIN_C,
                               M45_SAFETY_TPS546_TEMP_MAX_MAX_C);
    }
    if (config->safety_tps546_temp_expected_max_c > config->safety_tps546_temp_max_c) {
        config->safety_tps546_temp_expected_max_c =
            config->safety_tps546_temp_max_c;
    }
    if (!config->safety_limits_unrestricted) {
        config->safety_iout_fault_deciamps =
            default_if_outside(config->safety_iout_fault_deciamps,
                               M45_SAFETY_IOUT_FAULT_DEFAULT_DA,
                               M45_SAFETY_IOUT_FAULT_MIN_DA,
                               M45_SAFETY_IOUT_FAULT_MAX_DA);
        config->safety_iout_warn_deciamps =
            default_if_outside(config->safety_iout_warn_deciamps,
                               M45_SAFETY_IOUT_WARN_DEFAULT_DA,
                               M45_SAFETY_IOUT_WARN_MIN_DA,
                               M45_SAFETY_IOUT_FAULT_MAX_DA);
    }
    if (config->safety_iout_warn_deciamps > config->safety_iout_fault_deciamps) {
        config->safety_iout_warn_deciamps = config->safety_iout_fault_deciamps;
    }
    if (!isfinite(config->best_diff) || config->best_diff < 0.0) {
        config->best_diff = 0.0;
    }
}

esp_err_t m45_config_load(void)
{
    m45_config_t loaded;
    set_defaults(&loaded);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    uint32_t stored_schema = 0;
    (void)nvs_get_u32(nvs, "cfg_schema", &stored_schema);
    if (stored_schema > M45_CONFIG_SCHEMA_VERSION) {
        ESP_LOGW(TAG, "stored config schema %lu is newer than supported schema %lu; erasing settings",
                 (unsigned long)stored_schema,
                 (unsigned long)M45_CONFIG_SCHEMA_VERSION);
        err = nvs_erase_all(nvs);
        if (err == ESP_OK) {
            err = nvs_set_u32(nvs, "cfg_schema", M45_CONFIG_SCHEMA_VERSION);
        }
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
        nvs_close(nvs);
        if (err != ESP_OK) {
            return err;
        }
        set_defaults(&loaded);
        loaded.wifi_ssid[0] = '\0';
        loaded.wifi_password[0] = '\0';
        config_store_runtime(&loaded);
        return ESP_OK;
    }

    load_string(nvs, "wifi_ssid", loaded.wifi_ssid, sizeof(loaded.wifi_ssid));
    load_string(nvs, "wifi_pass", loaded.wifi_password, sizeof(loaded.wifi_password));
    load_string(nvs, "hostname", loaded.hostname, sizeof(loaded.hostname));
    load_string(nvs, "pool_host", loaded.pool_host, sizeof(loaded.pool_host));
    load_string(nvs, "pool_bak_host", loaded.backup_pool_host,
                sizeof(loaded.backup_pool_host));
    load_string(nvs, "pool_ip", loaded.pool_ip, sizeof(loaded.pool_ip));
    load_string(nvs, "pool_bak_ip", loaded.backup_pool_ip,
                sizeof(loaded.backup_pool_ip));
    load_string(nvs, "pool_user", loaded.pool_user, sizeof(loaded.pool_user));
    load_string(nvs, "pool_pass", loaded.pool_pass, sizeof(loaded.pool_pass));
    load_u16(nvs, "pool_port", &loaded.pool_port);
    load_u16(nvs, "pool_bak_port", &loaded.backup_pool_port);
    uint8_t pool_tls = loaded.pool_tls ? 1 : 0;
    load_u8(nvs, "pool_tls", &pool_tls);
    loaded.pool_tls = pool_tls != 0;
    uint8_t backup_pool_tls = loaded.backup_pool_tls ? 1 : 0;
    load_u8(nvs, "pool_bak_tls", &backup_pool_tls);
    loaded.backup_pool_tls = backup_pool_tls != 0;
    uint8_t multi_pool_enabled = loaded.multi_pool_enabled ? 1 : 0;
    load_u8(nvs, "pool_multi", &multi_pool_enabled);
    loaded.multi_pool_enabled = multi_pool_enabled != 0;
    for (size_t i = 0; i < M45_AUX_POOL_MAX; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "aux%u_host", (unsigned)i);
        load_string(nvs, key, loaded.aux_pools[i].host,
                    sizeof(loaded.aux_pools[i].host));
        snprintf(key, sizeof(key), "aux%u_ip", (unsigned)i);
        load_string(nvs, key, loaded.aux_pools[i].ip,
                    sizeof(loaded.aux_pools[i].ip));
        snprintf(key, sizeof(key), "aux%u_user", (unsigned)i);
        load_string(nvs, key, loaded.aux_pools[i].user,
                    sizeof(loaded.aux_pools[i].user));
        snprintf(key, sizeof(key), "aux%u_pass", (unsigned)i);
        load_string(nvs, key, loaded.aux_pools[i].pass,
                    sizeof(loaded.aux_pools[i].pass));
        snprintf(key, sizeof(key), "aux%u_port", (unsigned)i);
        load_u16(nvs, key, &loaded.aux_pools[i].port);
        snprintf(key, sizeof(key), "aux%u_tls", (unsigned)i);
        uint8_t aux_tls = loaded.aux_pools[i].tls ? 1 : 0;
        load_u8(nvs, key, &aux_tls);
        loaded.aux_pools[i].tls = aux_tls != 0;
        snprintf(key, sizeof(key), "aux%u_en", (unsigned)i);
        uint8_t aux_enabled = loaded.aux_pools[i].enabled ? 1 : 0;
        load_u8(nvs, key, &aux_enabled);
        loaded.aux_pools[i].enabled = aux_enabled != 0;
        snprintf(key, sizeof(key), "aux%u_weight", (unsigned)i);
        uint8_t aux_weight = loaded.aux_pools[i].share_percent;
        load_u8(nvs, key, &aux_weight);
        loaded.aux_pools[i].share_percent = aux_weight;
    }
    load_u16(nvs, "pool_diff", &loaded.pool_difficulty);
    uint8_t pool_difficulty_auto = loaded.pool_difficulty_auto ? 1 : 0;
    load_u8(nvs, "pool_diff_auto", &pool_difficulty_auto);
    loaded.pool_difficulty_auto = pool_difficulty_auto != 0;
    uint8_t overclock_enabled = loaded.overclock_enabled ? 1 : 0;
    load_u8(nvs, "oc_en", &overclock_enabled);
    loaded.overclock_enabled = overclock_enabled != 0;
    uint8_t auto_clock_enabled = loaded.auto_clock_enabled ? 1 : 0;
    load_u8(nvs, "auto_clk", &auto_clock_enabled);
    loaded.auto_clock_enabled = auto_clock_enabled != 0;
    uint8_t auto_domain_reboot_enabled = loaded.auto_domain_reboot_enabled ? 1 : 0;
    load_u8(nvs, "dom_reboot", &auto_domain_reboot_enabled);
    loaded.auto_domain_reboot_enabled = auto_domain_reboot_enabled != 0;
    load_u16(nvs, "auto_clk_tc", &loaded.auto_clock_target_temp_c);
    uint8_t auto_clock_max_watts_enabled =
        loaded.auto_clock_max_watts_enabled ? 1 : 0;
    load_u8(nvs, "auto_clk_wen", &auto_clock_max_watts_enabled);
    loaded.auto_clock_max_watts_enabled = auto_clock_max_watts_enabled != 0;
    load_u16(nvs, "auto_clk_w", &loaded.auto_clock_max_watts);
    load_u16(nvs, "asic_freq", &loaded.asic_frequency_mhz);
    load_u16(nvs, "asic_mv", &loaded.asic_voltage_mv);
    load_i16(nvs, "oc_mv_off", &loaded.overclock_voltage_offset_mv);
    uint8_t asic_voltage_temp_compensation_enabled =
        loaded.asic_voltage_temp_compensation_enabled ? 1 : 0;
    load_u8(nvs, "asic_mv_tc", &asic_voltage_temp_compensation_enabled);
    loaded.asic_voltage_temp_compensation_enabled =
        asic_voltage_temp_compensation_enabled != 0;
    uint8_t fan_override_enabled = loaded.fan_override_enabled ? 1 : 0;
    load_u8(nvs, "fan_ovr_en", &fan_override_enabled);
    loaded.fan_override_enabled = fan_override_enabled != 0;
    load_u16(nvs, "fan_ovr_pct", &loaded.fan_override_percent);
    uint8_t fan_auto_off_allowed = loaded.fan_auto_off_allowed ? 1 : 0;
    load_u8(nvs, "fan_auto_off", &fan_auto_off_allowed);
    loaded.fan_auto_off_allowed = fan_auto_off_allowed != 0;
    uint8_t fan_target_override_enabled = loaded.fan_target_override_enabled ? 1 : 0;
    load_u8(nvs, "fan_tgt_en", &fan_target_override_enabled);
    loaded.fan_target_override_enabled = fan_target_override_enabled != 0;
    load_u16(nvs, "fan_tgt_c", &loaded.fan_target_temp_c);
    uint8_t display_screensaver_enabled = loaded.display_screensaver_enabled ? 1 : 0;
    load_u8(nvs, "screensaver", &display_screensaver_enabled);
    loaded.display_screensaver_enabled = display_screensaver_enabled != 0;
    load_u16(nvs, "sleep_min", &loaded.display_sleep_minutes);
    uint8_t safety_limits_unrestricted = loaded.safety_limits_unrestricted ? 1 : 0;
    load_u8(nvs, "lim_unres", &safety_limits_unrestricted);
    loaded.safety_limits_unrestricted = safety_limits_unrestricted != 0;
    load_u16(nvs, "lim_vin_min", &loaded.safety_input_voltage_min_mv);
    load_u16(nvs, "lim_vin_emin", &loaded.safety_input_voltage_expected_min_mv);
    load_u16(nvs, "lim_vin_emax", &loaded.safety_input_voltage_expected_max_mv);
    load_u16(nvs, "lim_vin_max", &loaded.safety_input_voltage_max_mv);
    load_u16(nvs, "lim_av_min", &loaded.safety_asic_voltage_min_mv);
    load_u16(nvs, "lim_av_max", &loaded.safety_asic_voltage_max_mv);
    load_u16(nvs, "lim_at_exp", &loaded.safety_asic_temp_expected_max_c);
    load_u16(nvs, "lim_at_max", &loaded.safety_asic_temp_max_c);
    load_u16(nvs, "lim_vrt_exp", &loaded.safety_tps546_temp_expected_max_c);
    load_u16(nvs, "lim_vrt_max", &loaded.safety_tps546_temp_max_c);
    load_u16(nvs, "lim_iw_da", &loaded.safety_iout_warn_deciamps);
    load_u16(nvs, "lim_if_da", &loaded.safety_iout_fault_deciamps);
    load_double(nvs, "best_diff", &loaded.best_diff);
    sanitize_config(&loaded);
    if (stored_schema < M45_CONFIG_SCHEMA_VERSION) {
        if (strcmp(loaded.pool_pass, "x") == 0) {
            (void)nvs_erase_key(nvs, "pool_pass");
        }
        for (size_t i = 0; i < M45_AUX_POOL_MAX; ++i) {
            if (strcmp(loaded.aux_pools[i].pass, "x") == 0) {
                char key[16];
                snprintf(key, sizeof(key), "aux%u_pass", (unsigned)i);
                (void)nvs_erase_key(nvs, key);
            }
        }
        err = nvs_set_u32(nvs, "cfg_schema", M45_CONFIG_SCHEMA_VERSION);
        if (err == ESP_OK) {
            err = nvs_commit(nvs);
        }
    }
    nvs_close(nvs);
    if (err == ESP_OK) {
        config_store_runtime(&loaded);
    }
    return err;
}

esp_err_t m45_config_save(const m45_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    m45_config_t clean = *config;
    m45_config_t current;
    config_copy_runtime(&current);
    sanitize_config(&clean);
    if (strcmp(clean.pool_host, current.pool_host) != 0) {
        clean.pool_ip[0] = '\0';
    }
    if (strcmp(clean.backup_pool_host, current.backup_pool_host) != 0) {
        clean.backup_pool_ip[0] = '\0';
    }
    for (size_t i = 0; i < M45_AUX_POOL_MAX; ++i) {
        if (strcmp(clean.aux_pools[i].host, current.aux_pools[i].host) != 0) {
            clean.aux_pools[i].ip[0] = '\0';
        }
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, "wifi_ssid", clean.wifi_ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_pass", clean.wifi_password);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "hostname", clean.hostname);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pool_host", clean.pool_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pool_bak_host", clean.backup_pool_host);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pool_ip", clean.pool_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pool_bak_ip", clean.backup_pool_ip);
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "pool_user", clean.pool_user);
    }
    if (err == ESP_OK) {
        err = store_optional_password(nvs, "pool_pass", clean.pool_pass);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "pool_port", clean.pool_port);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "pool_bak_port", clean.backup_pool_port);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "pool_tls", clean.pool_tls ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "pool_bak_tls", clean.backup_pool_tls ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "pool_multi", clean.multi_pool_enabled ? 1 : 0);
    }
    for (size_t i = 0; err == ESP_OK && i < M45_AUX_POOL_MAX; ++i) {
        char key[16];
        snprintf(key, sizeof(key), "aux%u_host", (unsigned)i);
        err = nvs_set_str(nvs, key, clean.aux_pools[i].host);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_ip", (unsigned)i);
        err = nvs_set_str(nvs, key, clean.aux_pools[i].ip);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_user", (unsigned)i);
        err = nvs_set_str(nvs, key, clean.aux_pools[i].user);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_pass", (unsigned)i);
        err = store_optional_password(nvs, key, clean.aux_pools[i].pass);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_port", (unsigned)i);
        err = nvs_set_u16(nvs, key, clean.aux_pools[i].port);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_tls", (unsigned)i);
        err = nvs_set_u8(nvs, key, clean.aux_pools[i].tls ? 1 : 0);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_en", (unsigned)i);
        err = nvs_set_u8(nvs, key, clean.aux_pools[i].enabled ? 1 : 0);
        if (err != ESP_OK) {
            break;
        }
        snprintf(key, sizeof(key), "aux%u_weight", (unsigned)i);
        err = nvs_set_u8(nvs, key, clean.aux_pools[i].share_percent);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "pool_diff", clean.pool_difficulty);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "pool_diff_auto", clean.pool_difficulty_auto ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "oc_en", clean.overclock_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "auto_clk", clean.auto_clock_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "dom_reboot",
                         clean.auto_domain_reboot_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "auto_clk_tc", clean.auto_clock_target_temp_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "auto_clk_wen",
                         clean.auto_clock_max_watts_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "auto_clk_w", clean.auto_clock_max_watts);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "asic_freq", clean.asic_frequency_mhz);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "asic_mv", clean.asic_voltage_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_i16(nvs, "oc_mv_off", clean.overclock_voltage_offset_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "asic_mv_tc",
                         clean.asic_voltage_temp_compensation_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "fan_ovr_en", clean.fan_override_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "fan_ovr_pct", clean.fan_override_percent);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "fan_auto_off", clean.fan_auto_off_allowed ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "fan_tgt_en", clean.fan_target_override_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "fan_tgt_c", clean.fan_target_temp_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "screensaver", clean.display_screensaver_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "sleep_min", clean.display_sleep_minutes);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(nvs, "lim_unres", clean.safety_limits_unrestricted ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vin_min", clean.safety_input_voltage_min_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vin_emin",
                          clean.safety_input_voltage_expected_min_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vin_emax",
                          clean.safety_input_voltage_expected_max_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vin_max", clean.safety_input_voltage_max_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_av_min", clean.safety_asic_voltage_min_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_av_max", clean.safety_asic_voltage_max_mv);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_at_exp",
                          clean.safety_asic_temp_expected_max_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_at_max", clean.safety_asic_temp_max_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vrt_exp",
                          clean.safety_tps546_temp_expected_max_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_vrt_max", clean.safety_tps546_temp_max_c);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_iw_da", clean.safety_iout_warn_deciamps);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(nvs, "lim_if_da", clean.safety_iout_fault_deciamps);
    }
    if (err == ESP_OK) {
        err = nvs_set_u32(nvs, "cfg_schema", M45_CONFIG_SCHEMA_VERSION);
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        config_copy_runtime(&current);
        clean.best_diff = current.best_diff;
        config_store_runtime(&clean);
    }
    return err;
}

esp_err_t m45_config_set_runtime(const m45_config_t *config)
{
    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    m45_config_t clean = *config;
    sanitize_config(&clean);
    m45_config_t current;
    config_copy_runtime(&current);
    clean.best_diff = current.best_diff;
    config_store_runtime(&clean);
    return ESP_OK;
}

esp_err_t m45_config_factory_reset(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(nvs);
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_ssid", "");
    }
    if (err == ESP_OK) {
        err = nvs_set_str(nvs, "wifi_pass", "");
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        m45_config_t reset;
        set_defaults(&reset);
        reset.wifi_ssid[0] = '\0';
        reset.wifi_password[0] = '\0';
        config_store_runtime(&reset);
    }
    return err;
}

esp_err_t m45_config_set_pool_ip_cache(bool backup_pool, const char *expected_host,
                                       const char *ip)
{
    if (expected_host == NULL || ip == NULL || strlen(ip) > M45_POOL_IP_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    m45_config_t current;
    config_copy_runtime(&current);
    const char *active_host = backup_pool ? current.backup_pool_host : current.pool_host;
    char *active_ip = backup_pool ? current.backup_pool_ip : current.pool_ip;
    const char *nvs_key = backup_pool ? "pool_bak_ip" : "pool_ip";
    if (strcmp(active_host, expected_host) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(active_ip, ip) == 0) {
        return ESP_OK;
    }

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, nvs_key, ip);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        strlcpy(active_ip, ip, M45_POOL_IP_MAX + 1);
        config_store_runtime(&current);
    }
    return err;
}

esp_err_t m45_config_set_aux_pool_ip_cache(size_t aux_index, const char *expected_host,
                                           const char *ip)
{
    if (aux_index >= M45_AUX_POOL_MAX || expected_host == NULL || ip == NULL ||
        strlen(ip) > M45_POOL_IP_MAX) {
        return ESP_ERR_INVALID_ARG;
    }

    m45_config_t current;
    config_copy_runtime(&current);
    m45_aux_pool_t *aux = &current.aux_pools[aux_index];
    if (strcmp(aux->host, expected_host) != 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (strcmp(aux->ip, ip) == 0) {
        return ESP_OK;
    }

    char nvs_key[16];
    snprintf(nvs_key, sizeof(nvs_key), "aux%u_ip", (unsigned)aux_index);

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(nvs, nvs_key, ip);
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);

    if (err == ESP_OK) {
        strlcpy(aux->ip, ip, sizeof(aux->ip));
        config_store_runtime(&current);
    }
    return err;
}

static esp_err_t write_best_diff(double best_diff)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(M45_CONFIG_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs, "best_diff", &best_diff, sizeof(best_diff));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t m45_config_set_best_diff(double best_diff)
{
    if (!isfinite(best_diff) || best_diff < 0.0) {
        return ESP_ERR_INVALID_ARG;
    }
    m45_config_t current;
    config_copy_runtime(&current);
    if (best_diff <= current.best_diff) {
        return ESP_OK;
    }

    esp_err_t err = write_best_diff(best_diff);
    if (err == ESP_OK) {
        config_copy_runtime(&current);
        if (best_diff > current.best_diff) {
            current.best_diff = best_diff;
            config_store_runtime(&current);
        }
    }
    return err;
}

esp_err_t m45_config_reset_best_diff(void)
{
    const double best_diff = 0.0;
    esp_err_t err = write_best_diff(best_diff);
    if (err == ESP_OK) {
        m45_config_t current;
        config_copy_runtime(&current);
        current.best_diff = best_diff;
        config_store_runtime(&current);
    }
    return err;
}

const m45_config_t *m45_config_get(void)
{
    config_copy_runtime(&g_config_snapshot);
    return &g_config_snapshot;
}

uint32_t m45_config_schema_version(void)
{
    return M45_CONFIG_SCHEMA_VERSION;
}

uint16_t m45_config_auto_pool_difficulty(uint16_t frequency_mhz,
                                         uint16_t small_core_count,
                                         uint8_t asic_count)
{
    if (frequency_mhz == 0 || small_core_count == 0 || asic_count == 0) {
        return CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY;
    }

    const uint64_t hashes_per_second = (uint64_t)frequency_mhz * 1000000ULL *
                                       (uint64_t)small_core_count *
                                       (uint64_t)asic_count;
    const uint64_t denominator =
        M45_STRATUM_TARGET_SHARES_PER_MIN * M45_STRATUM_DIFF_HASHES;
    uint64_t difficulty = (hashes_per_second * 60ULL) / denominator;
    if (difficulty < 1ULL) {
        difficulty = 1ULL;
    } else if (difficulty > UINT16_MAX) {
        difficulty = UINT16_MAX;
    }
    return (uint16_t)difficulty;
}

uint16_t m45_config_effective_pool_difficulty(const m45_config_t *config,
                                              uint16_t small_core_count,
                                              uint8_t asic_count)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    if (!active->pool_difficulty_auto) {
        return active->pool_difficulty >= 1
                   ? active->pool_difficulty
                   : CONFIG_M45_BITAXE_STRATUM_SUGGESTED_DIFFICULTY;
    }
    return m45_config_auto_pool_difficulty(
        m45_config_effective_asic_frequency_mhz(active), small_core_count, asic_count);
}

uint16_t m45_config_effective_asic_frequency_mhz(const m45_config_t *config)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    return active->overclock_enabled ? active->asic_frequency_mhz
                                     : CONFIG_M45_BITAXE_ASIC_FREQUENCY_MHZ;
}

uint16_t m45_config_effective_asic_voltage_mv(const m45_config_t *config)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    return active->overclock_enabled ? active->asic_voltage_mv
                                     : CONFIG_M45_BITAXE_ASIC_VOLTAGE_MV;
}

int16_t m45_config_asic_voltage_temp_compensation_mv(const m45_config_t *config,
                                                     float asic_temp_c)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    if (!active->overclock_enabled || !active->asic_voltage_temp_compensation_enabled ||
        !isfinite(asic_temp_c) || asic_temp_c < M45_ASIC_TEMP_COMP_MIN_C ||
        asic_temp_c > M45_ASIC_TEMP_COMP_MAX_C) {
        return 0;
    }

    const float extra_mv =
        (M45_ASIC_TEMP_COMP_REFERENCE_C - asic_temp_c) * M45_ASIC_TEMP_COMP_MV_PER_C;
    const int32_t stepped_mv =
        lroundf(extra_mv / M45_ASIC_TEMP_COMP_STEP_MV) * (int32_t)M45_ASIC_TEMP_COMP_STEP_MV;
    const uint16_t base_mv = m45_config_effective_asic_voltage_mv(active);
    const int32_t min_target_mv =
        active->safety_asic_voltage_min_mv > M45_ASIC_VOLTAGE_MIN_MV
            ? active->safety_asic_voltage_min_mv
            : M45_ASIC_VOLTAGE_MIN_MV;
    const int32_t max_target_mv =
        active->safety_asic_voltage_max_mv <= M45_ASIC_VOLTAGE_MAX_MV
            ? (int32_t)active->safety_asic_voltage_max_mv - 1
            : M45_ASIC_VOLTAGE_MAX_MV;

    if (stepped_mv > 0) {
        const int32_t available_mv = max_target_mv - (int32_t)base_mv;
        if (available_mv <= 0) {
            return 0;
        }
        return (int16_t)(stepped_mv > available_mv ? available_mv : stepped_mv);
    }
    if (stepped_mv < 0) {
        const int32_t available_mv = min_target_mv - (int32_t)base_mv;
        if (available_mv >= 0) {
            return 0;
        }
        return (int16_t)(stepped_mv < available_mv ? available_mv : stepped_mv);
    }
    return 0;
}

uint16_t m45_config_effective_asic_voltage_mv_for_temp(const m45_config_t *config,
                                                       float asic_temp_c)
{
    const uint16_t base_mv = m45_config_effective_asic_voltage_mv(config);
    const int16_t compensation_mv =
        m45_config_asic_voltage_temp_compensation_mv(config, asic_temp_c);
    const int32_t target_mv = (int32_t)base_mv + compensation_mv;
    if (target_mv > M45_ASIC_VOLTAGE_MAX_MV) {
        return M45_ASIC_VOLTAGE_MAX_MV;
    }
    if (target_mv < M45_ASIC_VOLTAGE_MIN_MV) {
        return M45_ASIC_VOLTAGE_MIN_MV;
    }
    return (uint16_t)target_mv;
}

uint16_t m45_config_effective_fan_target_temp_c(const m45_config_t *config)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    if (!active->fan_target_override_enabled) {
        return M45_FAN_TARGET_DEFAULT_C;
    }
    if (active->fan_target_temp_c < M45_FAN_TARGET_MIN_C) {
        return M45_FAN_TARGET_MIN_C;
    }
    if (active->fan_target_temp_c > M45_FAN_TARGET_MAX_C) {
        return M45_FAN_TARGET_MAX_C;
    }
    return active->fan_target_temp_c;
}

uint16_t m45_config_effective_auto_clock_target_temp_c(const m45_config_t *config)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    if (active->auto_clock_target_temp_c < M45_AUTO_CLOCK_TARGET_MIN_C) {
        return M45_AUTO_CLOCK_TARGET_MIN_C;
    }
    if (active->auto_clock_target_temp_c > M45_AUTO_CLOCK_TARGET_MAX_C) {
        return M45_AUTO_CLOCK_TARGET_MAX_C;
    }
    return active->auto_clock_target_temp_c;
}

uint16_t m45_config_effective_auto_clock_max_watts(const m45_config_t *config)
{
    const m45_config_t *active = config != NULL ? config : m45_config_get();
    if (active->auto_clock_max_watts < M45_AUTO_CLOCK_MAX_WATTS_MIN) {
        return M45_AUTO_CLOCK_MAX_WATTS_MIN;
    }
    if (active->auto_clock_max_watts > M45_AUTO_CLOCK_MAX_WATTS_MAX) {
        return M45_AUTO_CLOCK_MAX_WATTS_MAX;
    }
    return active->auto_clock_max_watts;
}
