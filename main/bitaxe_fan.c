#include "bitaxe_fan.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "emc2101_regs.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bitaxe.h"
#include "m45_config.h"
#include "m45_log_buffer.h"

#define EMC2101_FAN_CONFIG_DIRECT_SETTING 0x23
#define EMC2101_PWM_FREQ_MAX_RESOLUTION 0x1f
#define EMC2101_PWM_DIV_DEFAULT 0x01
#define EMC2101_FAN_SETTING_MAX 0x3f
#define FAN_TARGET_TEMP_C 62.0f
#define FAN_TARGET_MIN_TEMP_C 35.0f
#define FAN_TARGET_MAX_TEMP_C 66.0f
#define FAN_TEMP_DEADBAND_C 1.0f
#define FAN_COOL_SAMPLES_BEFORE_STEP_DOWN 24
#define FAN_COOL_SAMPLES_BEFORE_AUTO_OFF 80
#define FAN_TEMP_WINDOW_SAMPLES 24
#define FAN_RPM_WINDOW_SAMPLES 16
#define FAN_AUTO_MIN_PERCENT 30.0f
#define FAN_MAX_PERCENT 100.0f
#define FAN_BASE_PERCENT 45.0f
#define FAN_ASSUMED_AMBIENT_C 35.0f
#define FAN_COOLING_EXPONENT 0.50f
#define FAN_KI_PERCENT_PER_C 0.35f
#define FAN_INTEGRAL_LIMIT 25.0f
#define FAN_STEP_DOWN_PERCENT 4.0f
#define FAN_STEP_UP_PERCENT 12.0f
#define FAN_UPDATE_DEADBAND_PERCENT 3.0f
#define FAN_STEP_DOWN_COOL_MARGIN_C 2.0f
#define FAN_STEP_DOWN_TARGET_GAP_PERCENT 8.0f
#define FAN_AUTO_OFF_COOL_MARGIN_C 5.0f
#define FAN_AUTO_START_BLIP_MS 300
#define FAN_PWM_CLEAN_STEP_SETTING 4
#define FAN_FULL_SPEED_ABOVE_TARGET_C 2.0f
#define FAN_FULL_SPEED_MAX_TEMP_C 66.0f
#define FAN_FAILSAFE_ON_TEMP_C 67.0f
#define FAN_FAILSAFE_OFF_TEMP_C 60.0f
#define FAN_VR_FAILSAFE_ON_TEMP_C 100.0f
#define FAN_VR_FAILSAFE_OFF_TEMP_C 95.0f
#define FAN_UNSTICK_START_RPM 100
#define FAN_UNSTICK_RECOVER_RPM 250
#define FAN_UNSTICK_PULSE_MS 250
#define FAN_UNSTICK_MAX_PULSES_PER_TICK 2

static const char *TAG = "bitaxe_fan";
static float g_fan_integral;
static uint8_t g_fan_cool_samples;
static uint8_t g_fan_auto_off_samples;
static float g_fan_temp_window[FAN_TEMP_WINDOW_SAMPLES];
static uint8_t g_fan_temp_window_count;
static uint8_t g_fan_temp_window_next;
static uint16_t g_fan_rpm_window[FAN_RPM_WINDOW_SAMPLES];
static uint8_t g_fan_rpm_window_count;
static uint8_t g_fan_rpm_window_next;
static bool g_fan_failsafe_latched;
static bool g_fan_unstick_active;
static bool g_fan_auto_off_active;
static bool g_emc2101_ready;
static i2c_master_dev_handle_t g_emc2101_handle;

static esp_err_t emc2101_read_byte(uint8_t reg, uint8_t *value)
{
    return i2c_bitaxe_register_read(g_emc2101_handle, reg, value, 1);
}

static esp_err_t emc2101_write_byte(uint8_t reg, uint8_t value)
{
    return i2c_bitaxe_register_write_byte(g_emc2101_handle, reg, value);
}

static esp_err_t emc2101_configure_fan_pwm(void)
{
    ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_FAN_CONFIG, EMC2101_FAN_CONFIG_DIRECT_SETTING),
                        TAG, "EMC2101 fan config failed");
    ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_PWM_FREQ, EMC2101_PWM_FREQ_MAX_RESOLUTION),
                        TAG, "EMC2101 fan PWM frequency failed");
    ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_PWM_DIV, EMC2101_PWM_DIV_DEFAULT),
                        TAG, "EMC2101 fan PWM divider failed");
    return ESP_OK;
}

static esp_err_t emc2101_attach_and_check(void)
{
    if (g_emc2101_handle == NULL) {
        ESP_RETURN_ON_ERROR(i2c_bitaxe_add_device(EMC2101_I2CADDR_DEFAULT, &g_emc2101_handle,
                                                  "EMC2101"),
                            TAG, "EMC2101 I2C add failed");
    }

    uint8_t part_id = 0;
    ESP_RETURN_ON_ERROR(emc2101_read_byte(EMC2101_REG_PARTID, &part_id), TAG,
                        "EMC2101 part ID read failed");
    ESP_RETURN_ON_FALSE(part_id == EMC2101_CHIP_ID || part_id == EMC2101_ALT_CHIP_ID, ESP_FAIL,
                        TAG, "unexpected EMC2101 part ID 0x%02x", part_id);
    return ESP_OK;
}

static esp_err_t emc2101_set_fan_percent(float percent)
{
    percent = fminf(FAN_MAX_PERCENT, fmaxf(0.0f, percent));
    const uint8_t setting = (uint8_t)lroundf((percent / 100.0f) * (float)EMC2101_FAN_SETTING_MAX);
    return emc2101_write_byte(EMC2101_REG_FAN_SETTING, setting);
}

static esp_err_t emc2101_set_boot_max_pwm(void)
{
    ESP_RETURN_ON_ERROR(emc2101_attach_and_check(), TAG, "EMC2101 attach failed");
    ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_REG_CONFIG, 0x04), TAG,
                        "EMC2101 TACH input config failed");
    ESP_RETURN_ON_ERROR(emc2101_configure_fan_pwm(), TAG, "EMC2101 fan PWM config failed");
    ESP_RETURN_ON_ERROR(emc2101_set_fan_percent(FAN_MAX_PERCENT), TAG,
                        "EMC2101 startup fan max failed");
    return ESP_OK;
}

static float fan_percent_from_setting(uint8_t setting)
{
    return ((float)setting / (float)EMC2101_FAN_SETTING_MAX) * 100.0f;
}

static float fan_clean_percent(float percent)
{
    percent = fminf(FAN_MAX_PERCENT, fmaxf(FAN_AUTO_MIN_PERCENT, percent));
    if (percent >= FAN_MAX_PERCENT) {
        return FAN_MAX_PERCENT;
    }

    uint8_t setting = (uint8_t)lroundf((percent / 100.0f) * (float)EMC2101_FAN_SETTING_MAX);
    setting = (uint8_t)(((setting + (FAN_PWM_CLEAN_STEP_SETTING / 2)) /
                         FAN_PWM_CLEAN_STEP_SETTING) *
                        FAN_PWM_CLEAN_STEP_SETTING);
    setting = (uint8_t)fminf((float)EMC2101_FAN_SETTING_MAX,
                             fmaxf((float)FAN_PWM_CLEAN_STEP_SETTING, (float)setting));
    return fan_percent_from_setting(setting);
}

static float fan_target_temp_c_from_config(const m45_config_t *config)
{
    if (config == NULL || !config->fan_target_override_enabled) {
        return FAN_TARGET_TEMP_C;
    }
    return fminf(FAN_TARGET_MAX_TEMP_C,
                 fmaxf(FAN_TARGET_MIN_TEMP_C, (float)config->fan_target_temp_c));
}

static float fan_target_temp_c(void)
{
    return fan_target_temp_c_from_config(m45_config_get());
}

static bool fan_no_fan_configured(const m45_config_t *config)
{
    return config != NULL && config->fan_override_enabled && config->fan_override_percent == 0;
}

static bool fan_no_fan_selected(void)
{
    return fan_no_fan_configured(m45_config_get());
}

static float fan_model_percent(float asic_temp_c, float target_temp_c)
{
    const float current_rise_c = fmaxf(1.0f, asic_temp_c - FAN_ASSUMED_AMBIENT_C);
    const float target_rise_c = fmaxf(1.0f, target_temp_c - FAN_ASSUMED_AMBIENT_C);

    return FAN_BASE_PERCENT * powf(current_rise_c / target_rise_c, 1.0f / FAN_COOLING_EXPONENT);
}

static esp_err_t emc2101_read_fan_rpm(uint16_t *rpm)
{
    uint8_t tach_lsb = 0;
    uint8_t tach_msb = 0;

    ESP_RETURN_ON_ERROR(emc2101_read_byte(EMC2101_TACH_LSB, &tach_lsb), TAG,
                        "EMC2101 fan tach LSB read failed");
    ESP_RETURN_ON_ERROR(emc2101_read_byte(EMC2101_TACH_MSB, &tach_msb), TAG,
                        "EMC2101 fan tach MSB read failed");

    const uint16_t tach_count = (uint16_t)tach_lsb | ((uint16_t)tach_msb << 8);
    if (tach_count == 0 || tach_count == 0xffff) {
        *rpm = 0;
        return ESP_OK;
    }

    const uint32_t computed_rpm = EMC2101_FAN_RPM_NUMERATOR / tach_count;
    *rpm = computed_rpm > UINT16_MAX ? UINT16_MAX : (uint16_t)computed_rpm;
    if (*rpm == 82) {
        *rpm = 0;
    }
    return ESP_OK;
}

static esp_err_t fan_sample_rpm(GlobalState *state, uint16_t *raw_rpm, uint16_t *avg_rpm)
{
    uint16_t rpm = 0;
    ESP_RETURN_ON_ERROR(emc2101_read_fan_rpm(&rpm), TAG, "EMC2101 fan tach read failed");

    g_fan_rpm_window[g_fan_rpm_window_next] = rpm;
    g_fan_rpm_window_next = (g_fan_rpm_window_next + 1) % FAN_RPM_WINDOW_SAMPLES;
    if (g_fan_rpm_window_count < FAN_RPM_WINDOW_SAMPLES) {
        ++g_fan_rpm_window_count;
    }

    uint32_t total = 0;
    for (uint8_t i = 0; i < g_fan_rpm_window_count; ++i) {
        total += g_fan_rpm_window[i];
    }

    const uint16_t averaged = (uint16_t)((total + (g_fan_rpm_window_count / 2)) /
                                         g_fan_rpm_window_count);
    state->POWER_MANAGEMENT_MODULE.fan_rpm = averaged;
    if (raw_rpm != NULL) {
        *raw_rpm = rpm;
    }
    if (avg_rpm != NULL) {
        *avg_rpm = averaged;
    }
    return ESP_OK;
}

static void fan_reset_rpm_average(GlobalState *state, uint16_t rpm)
{
    g_fan_rpm_window[0] = rpm;
    g_fan_rpm_window_count = 1;
    g_fan_rpm_window_next = 1;
    state->POWER_MANAGEMENT_MODULE.fan_rpm = rpm;
}

static bool fan_unstick_needed(uint16_t avg_rpm)
{
    if (g_fan_unstick_active) {
        return avg_rpm < FAN_UNSTICK_RECOVER_RPM;
    }
    return g_fan_rpm_window_count >= FAN_RPM_WINDOW_SAMPLES && avg_rpm < FAN_UNSTICK_START_RPM;
}

static bool fan_unstick_pulse(GlobalState *state, float raw_asic_temp_c)
{
    if (fan_no_fan_selected()) {
        ESP_LOGI(TAG, "fan unstuck pulse skipped; no fan configured");
        return true;
    }
    if (raw_asic_temp_c >= FAN_FAILSAFE_ON_TEMP_C) {
        return false;
    }

    g_fan_unstick_active = true;
    g_fan_integral = fmaxf(0.0f, g_fan_integral);
    g_fan_cool_samples = 0;
    g_fan_auto_off_samples = 0;

    for (uint8_t i = 0; i < FAN_UNSTICK_MAX_PULSES_PER_TICK; ++i) {
        ESP_LOGW(TAG, "fan unstuck pulse %u: 0%% then 100%%", (unsigned)(i + 1));
        esp_err_t err = emc2101_set_fan_percent(0.0f);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan unstuck 0%% pulse failed: %s", esp_err_to_name(err));
            return true;
        }
        state->POWER_MANAGEMENT_MODULE.fan_perc = 0.0f;
        vTaskDelay(pdMS_TO_TICKS(FAN_UNSTICK_PULSE_MS));

        err = emc2101_set_fan_percent(FAN_MAX_PERCENT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan unstuck 100%% pulse failed: %s", esp_err_to_name(err));
            return true;
        }
        state->POWER_MANAGEMENT_MODULE.fan_perc = FAN_MAX_PERCENT;
        vTaskDelay(pdMS_TO_TICKS(FAN_UNSTICK_PULSE_MS));

        uint16_t raw_rpm = 0;
        uint16_t avg_rpm = 0;
        err = fan_sample_rpm(state, &raw_rpm, &avg_rpm);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan unstuck tach read failed: %s", esp_err_to_name(err));
            return true;
        }
        ESP_LOGW(TAG, "fan unstuck tach raw %u RPM avg %u RPM", raw_rpm, avg_rpm);
        if (raw_rpm >= FAN_UNSTICK_RECOVER_RPM || avg_rpm >= FAN_UNSTICK_RECOVER_RPM) {
            g_fan_unstick_active = false;
            fan_reset_rpm_average(state, raw_rpm);
            return true;
        }
    }

    return true;
}

static esp_err_t fan_auto_start_blip(GlobalState *state, float raw_asic_temp_c,
                                     float control_temp_c, float target_temp_c)
{
    esp_err_t err = emc2101_set_fan_percent(FAN_MAX_PERCENT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fan auto start blip failed: %s", esp_err_to_name(err));
        return err;
    }
    state->POWER_MANAGEMENT_MODULE.fan_perc = FAN_MAX_PERCENT;
    m45_log_buffer_append_verbose(
        TAG, "fan auto raw %.1f C ctl %.1f C: 0%% -> 100%% start blip near %.0f C target",
        raw_asic_temp_c, control_temp_c, target_temp_c);
    vTaskDelay(pdMS_TO_TICKS(FAN_AUTO_START_BLIP_MS));
    return ESP_OK;
}

static bool fan_apply_override(GlobalState *state, float current_percent)
{
    const m45_config_t *config = m45_config_get();
    if (!config->fan_override_enabled) {
        return false;
    }

    const float override_percent = (float)config->fan_override_percent;
    if (fabsf(current_percent - override_percent) >= 0.5f) {
        esp_err_t err = emc2101_set_fan_percent(override_percent);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan override %.0f%% update failed: %s", override_percent,
                     esp_err_to_name(err));
            return true;
        }
        state->POWER_MANAGEMENT_MODULE.fan_perc = override_percent;
        ESP_LOGI(TAG, "fan override %.0f%%", override_percent);
    }
    g_fan_cool_samples = 0;
    g_fan_auto_off_samples = 0;
    g_fan_integral = 0.0f;
    g_fan_unstick_active = false;
    g_fan_auto_off_active = false;
    return true;
}

esp_err_t bitaxe_fan_boot_max(void)
{
    ESP_LOGI(TAG, "setting EMC2101 fan to %.0f%% PWM for early boot", FAN_MAX_PERCENT);
    ESP_RETURN_ON_ERROR(emc2101_set_boot_max_pwm(), TAG, "early boot fan max failed");
    ESP_LOGI(TAG, "early boot fan forced to %.0f%% PWM", FAN_MAX_PERCENT);
    return ESP_OK;
}

esp_err_t bitaxe_fan_force_max_if_allowed(GlobalState *state, const char *reason)
{
    if (fan_no_fan_selected()) {
        ESP_LOGI(TAG, "emergency fan max skipped; no fan configured");
        return ESP_OK;
    }
    if (!g_emc2101_ready) {
        ESP_LOGW(TAG, "emergency fan max skipped; EMC2101 is not ready");
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = emc2101_set_fan_percent(FAN_MAX_PERCENT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "emergency fan max failed after %s: %s", reason != NULL ? reason : "fault",
                 esp_err_to_name(err));
        return err;
    }
    if (state != NULL) {
        state->POWER_MANAGEMENT_MODULE.fan_perc = FAN_MAX_PERCENT;
    }
    g_fan_cool_samples = 0;
    g_fan_auto_off_samples = 0;
    g_fan_integral = 0.0f;
    g_fan_failsafe_latched = true;
    g_fan_auto_off_active = false;
    ESP_LOGW(TAG, "emergency fan max after %s", reason != NULL ? reason : "fault");
    return ESP_OK;
}

esp_err_t bitaxe_fan_read_asic_temp_c(float *temp_c)
{
    uint8_t temp_msb = 0;
    uint8_t temp_lsb = 0;

    ESP_RETURN_ON_ERROR(emc2101_read_byte(EMC2101_EXTERNAL_TEMP_MSB, &temp_msb), TAG,
                        "EMC2101 external temp MSB read failed");
    ESP_RETURN_ON_ERROR(emc2101_read_byte(EMC2101_EXTERNAL_TEMP_LSB, &temp_lsb), TAG,
                        "EMC2101 external temp LSB read failed");

    uint16_t reading = ((uint16_t)temp_msb << 8) | temp_lsb;
    reading >>= 5;
    int16_t signed_reading = (int16_t)reading;
    if ((signed_reading & 0x0400) != 0) {
        signed_reading |= 0xf800;
    }

    *temp_c = ((float)signed_reading / 8.0f);
    return ESP_OK;
}

esp_err_t bitaxe_fan_init(GlobalState *state)
{
    if (g_emc2101_ready) {
        ESP_LOGI(TAG, "EMC2101 fan controller already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "initializing EMC2101 fan controller");
    ESP_RETURN_ON_ERROR(emc2101_set_boot_max_pwm(), TAG, "EMC2101 fan max init failed");
    g_emc2101_ready = true;
    state->POWER_MANAGEMENT_MODULE.fan_perc = FAN_MAX_PERCENT;
    g_fan_cool_samples = 0;
    g_fan_auto_off_samples = 0;
    g_fan_integral = 0.0f;
    g_fan_unstick_active = false;
    g_fan_auto_off_active = false;
    ESP_LOGI(TAG, "fan startup forced to %.0f%% PWM", FAN_MAX_PERCENT);

    if (state->DEVICE_CONFIG.emc_ideality_factor != 0x00) {
        ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_IDEALITY_FACTOR,
                                               state->DEVICE_CONFIG.emc_ideality_factor),
                            TAG, "EMC2101 ideality config failed");
        ESP_RETURN_ON_ERROR(emc2101_write_byte(EMC2101_BETA_COMPENSATION,
                                               state->DEVICE_CONFIG.emc_beta_compensation),
                            TAG, "EMC2101 beta config failed");
    }

    const m45_config_t *config = m45_config_get();
    const float target_temp_c = fan_target_temp_c_from_config(config);
    ESP_LOGI(TAG, "fan initialized at %.0f%% PWM; target %.0f C; tach sampling starts in monitor",
             FAN_MAX_PERCENT, target_temp_c);

    return ESP_OK;
}

void bitaxe_fan_update_auto(GlobalState *state, float raw_asic_temp_c, float control_temp_c,
                            float vr_temp_c)
{
    if (!isfinite(raw_asic_temp_c) || raw_asic_temp_c <= 0.0f ||
        !isfinite(control_temp_c) || control_temp_c <= 0.0f ||
        state->SYSTEM_MODULE.hardware_fault) {
        return;
    }

    const float current_percent = state->POWER_MANAGEMENT_MODULE.fan_perc >= 0.0f
                                      ? state->POWER_MANAGEMENT_MODULE.fan_perc
                                      : fan_clean_percent(FAN_BASE_PERCENT);
    uint16_t avg_rpm = 0;
    esp_err_t rpm_err = fan_sample_rpm(state, NULL, &avg_rpm);
    if (rpm_err != ESP_OK) {
        ESP_LOGW(TAG, "fan RPM sample failed: %s", esp_err_to_name(rpm_err));
    }

    const bool have_vr_temp = isfinite(vr_temp_c) && vr_temp_c > 0.0f;
    if (raw_asic_temp_c >= FAN_FAILSAFE_ON_TEMP_C ||
        (have_vr_temp && vr_temp_c >= FAN_VR_FAILSAFE_ON_TEMP_C)) {
        g_fan_failsafe_latched = true;
    } else if (raw_asic_temp_c < FAN_FAILSAFE_OFF_TEMP_C &&
               (!have_vr_temp || vr_temp_c < FAN_VR_FAILSAFE_OFF_TEMP_C)) {
        g_fan_failsafe_latched = false;
    }

    const m45_config_t *config = m45_config_get();
    if (config->fan_override_enabled && config->fan_override_percent == 0) {
        g_fan_failsafe_latched = false;
        fan_apply_override(state, current_percent);
        return;
    }

    const float target_temp_c = fan_target_temp_c();
    const float full_speed_temp_c =
        fminf(FAN_FULL_SPEED_MAX_TEMP_C, target_temp_c + FAN_FULL_SPEED_ABOVE_TARGET_C);
    float target_percent = FAN_MAX_PERCENT;
    if (g_fan_failsafe_latched) {
        g_fan_cool_samples = 0;
        g_fan_auto_off_samples = 0;
        g_fan_integral = fmaxf(0.0f, g_fan_integral);
        g_fan_auto_off_active = false;
    } else if (control_temp_c < full_speed_temp_c) {
        float error_c = control_temp_c - target_temp_c;
        if (fabsf(error_c) < FAN_TEMP_DEADBAND_C) {
            error_c = 0.0f;
            g_fan_integral *= 0.8f;
        }
        g_fan_integral += error_c * FAN_KI_PERCENT_PER_C;
        g_fan_integral = fminf(FAN_INTEGRAL_LIMIT, fmaxf(-FAN_INTEGRAL_LIMIT, g_fan_integral));
        if (control_temp_c >= (target_temp_c - FAN_STEP_DOWN_COOL_MARGIN_C)) {
            g_fan_integral = fmaxf(0.0f, g_fan_integral);
        }
        target_percent = fan_model_percent(control_temp_c, target_temp_c) + g_fan_integral;
    }
    const float requested_percent = target_percent;
    target_percent = fan_clean_percent(target_percent);
    const float auto_min_percent = fan_clean_percent(FAN_AUTO_MIN_PERCENT);

    if (g_fan_failsafe_latched) {
        if (fan_no_fan_selected()) {
            g_fan_failsafe_latched = false;
            ESP_LOGI(TAG, "fan failsafe max skipped; no fan configured");
            return;
        }
        if (current_percent >= (FAN_MAX_PERCENT - 0.5f)) {
            return;
        }
        esp_err_t err = emc2101_set_fan_percent(FAN_MAX_PERCENT);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "fan failsafe PWM update failed: %s", esp_err_to_name(err));
            return;
        }
        state->POWER_MANAGEMENT_MODULE.fan_perc = FAN_MAX_PERCENT;
        ESP_LOGW(TAG, "fan failsafe asic %.1f C ctl %.1f C vr %.1f C: %.0f%% -> 100%%",
                 raw_asic_temp_c, control_temp_c, have_vr_temp ? vr_temp_c : 0.0f,
                 current_percent);
        return;
    }

    if (config->fan_override_enabled) {
        g_fan_auto_off_active = false;
        g_fan_auto_off_samples = 0;
        fan_apply_override(state, current_percent);
        return;
    }

    if (requested_percent < FAN_AUTO_MIN_PERCENT) {
        if (control_temp_c <= (target_temp_c - FAN_AUTO_OFF_COOL_MARGIN_C) &&
            current_percent <= (auto_min_percent + 0.5f)) {
            if (g_fan_auto_off_samples < FAN_COOL_SAMPLES_BEFORE_AUTO_OFF) {
                ++g_fan_auto_off_samples;
                return;
            }
            g_fan_cool_samples = 0;
            g_fan_integral = fminf(0.0f, g_fan_integral);
            g_fan_auto_off_active = true;
            if (current_percent <= 0.5f) {
                return;
            }
            esp_err_t err = emc2101_set_fan_percent(0.0f);
            if (err != ESP_OK) {
                ESP_LOGW(TAG, "fan auto-off update failed: %s", esp_err_to_name(err));
                return;
            }
            state->POWER_MANAGEMENT_MODULE.fan_perc = 0.0f;
            m45_log_buffer_append_verbose(
                TAG,
                "fan auto raw %.1f C ctl %.1f C: %.0f%% -> 0%% target %.0f%% below %.0f%% min",
                raw_asic_temp_c, control_temp_c, current_percent, requested_percent,
                FAN_AUTO_MIN_PERCENT);
            return;
        }
        g_fan_auto_off_samples = 0;
    } else {
        g_fan_auto_off_samples = 0;
    }
    const bool was_auto_off = g_fan_auto_off_active;
    g_fan_auto_off_active = false;
    if (was_auto_off &&
        fan_auto_start_blip(state, raw_asic_temp_c, control_temp_c, target_temp_c) != ESP_OK) {
        g_fan_auto_off_active = true;
        return;
    }

    if (!was_auto_off && rpm_err == ESP_OK && fan_unstick_needed(avg_rpm) &&
        fan_unstick_pulse(state, raw_asic_temp_c)) {
        return;
    }

    if (target_percent > current_percent) {
        g_fan_cool_samples = 0;
        g_fan_auto_off_samples = 0;
    } else if (target_percent < current_percent) {
        if (control_temp_c >= (target_temp_c - FAN_STEP_DOWN_COOL_MARGIN_C) ||
            (current_percent - target_percent) < FAN_STEP_DOWN_TARGET_GAP_PERCENT) {
            g_fan_cool_samples = 0;
            g_fan_integral = fmaxf(0.0f, g_fan_integral);
            return;
        }
        if (g_fan_cool_samples < FAN_COOL_SAMPLES_BEFORE_STEP_DOWN) {
            ++g_fan_cool_samples;
            return;
        }
    } else {
        g_fan_cool_samples = 0;
        g_fan_auto_off_samples = 0;
    }

    if (fabsf(target_percent - current_percent) < FAN_UPDATE_DEADBAND_PERCENT) {
        return;
    }
    const float max_step = target_percent > current_percent ? FAN_STEP_UP_PERCENT
                                                            : FAN_STEP_DOWN_PERCENT;
    const float delta = fminf(max_step, fabsf(target_percent - current_percent));
    const float next_percent =
        fan_clean_percent(current_percent + copysignf(delta, target_percent - current_percent));

    if (fabsf(next_percent - current_percent) < 0.5f) {
        return;
    }

    esp_err_t err = emc2101_set_fan_percent(next_percent);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "fan PWM update failed: %s", esp_err_to_name(err));
        return;
    }

    state->POWER_MANAGEMENT_MODULE.fan_perc = next_percent;
    if (next_percent < current_percent) {
        g_fan_cool_samples = 0;
    }
    m45_log_buffer_append_verbose(TAG,
                                  "fan auto raw %.1f C ctl %.1f C: %.0f%% -> %.0f%% target %.0f%%",
                                  raw_asic_temp_c, control_temp_c, current_percent,
                                  next_percent, target_percent);
}

float bitaxe_fan_control_temp_c(float asic_temp_c)
{
    if (!isfinite(asic_temp_c) || asic_temp_c <= 0.0f) {
        return asic_temp_c;
    }

    g_fan_temp_window[g_fan_temp_window_next] = asic_temp_c;
    g_fan_temp_window_next = (g_fan_temp_window_next + 1) % FAN_TEMP_WINDOW_SAMPLES;
    if (g_fan_temp_window_count < FAN_TEMP_WINDOW_SAMPLES) {
        ++g_fan_temp_window_count;
    }

    float total = 0.0f;
    for (uint8_t i = 0; i < g_fan_temp_window_count; ++i) {
        total += g_fan_temp_window[i];
    }
    return total / (float)g_fan_temp_window_count;
}
