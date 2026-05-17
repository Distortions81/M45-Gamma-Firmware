#include "bitaxe_hw.h"

#include "bitaxe_fan.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "bm1370.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_log.h"
#include "asic_frequency_transition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bitaxe.h"
#include "m45_config.h"
#include "mining.h"
#include "asic_serial.h"
#include "tps546.h"
#include "utils.h"

#define ASIC_RESET_GPIO CONFIG_GPIO_ASIC_RESET
#define ASIC_JOB_SLOTS 128
#define TPS546_STATUS_FAULT_MASK                                                                  \
    (TPS546_STATUS_OFF | TPS546_STATUS_VOUT_OV | TPS546_STATUS_IOUT_OC | TPS546_STATUS_VIN_UV |   \
     TPS546_STATUS_TEMP | TPS546_STATUS_PGOOD)
#define TPS546_VOUT_TOLERANCE_VOLTS 0.075f
#define TPS546_MONITOR_INTERVAL_MS 250
#define ASIC_FAN_MONITOR_INTERVAL_MS 250
#define TPS546_POWER_WINDOW_SAMPLES 16
#define TPS546_OUTPUT_SETTLE_INITIAL_MS 10
#define TPS546_OUTPUT_SETTLE_POLL_MS 10
#define TPS546_OUTPUT_SETTLE_TIMEOUT_MS 150
#define ASIC_FREQUENCY_SETTLE_MS 50
#define ASIC_TEMP_VOLTAGE_UPDATE_DEADBAND_MV 5
#define TPS546_MAX_SAFE_TEMP_C 98
#define TPS546_EXPECTED_MAX_TEMP_C 85.0f
#define ASIC_TEMP_SHUTDOWN_C 69.0f
#define ASIC_TEMP_EXPECTED_MAX_C 60.0f
#define ASIC_VOLTAGE_MIN_SHUTDOWN_VOLTS 0.700f
#define ASIC_VOLTAGE_SHUTDOWN_VOLTS 1.400f
#define INPUT_VOLTAGE_SHUTDOWN_VOLTS 5.500f
#define INPUT_VOLTAGE_MIN_VOLTS 4.500f
#define INPUT_VOLTAGE_EXPECTED_MIN_VOLTS 4.800f
#define INPUT_VOLTAGE_EXPECTED_MAX_VOLTS 5.400f
#define TPS546_IOUT_WARN_AMPS 25.0f
#define TPS546_IOUT_FAULT_AMPS 30.0f
static const char *TAG = "bitaxe_hw";
static uint8_t g_chip_count = 0;
static char g_hw_status[64] = "boot";
static bool g_i2c_ready = false;
static bool g_power_monitor_started = false;
static bool g_regulator_enabled = false;
static bool g_tps546_ready = false;
static uint16_t g_commanded_voltage_mv = 0;
static portMUX_TYPE g_power_snapshot_lock = portMUX_INITIALIZER_UNLOCKED;
static bitaxe_gamma602_power_snapshot_t g_power_snapshot = {0};
static float g_power_vin_window[TPS546_POWER_WINDOW_SAMPLES];
static float g_power_vout_window[TPS546_POWER_WINDOW_SAMPLES];
static float g_power_iout_window[TPS546_POWER_WINDOW_SAMPLES];
static float g_power_temp_window[TPS546_POWER_WINDOW_SAMPLES];
static float g_power_vin_sum;
static float g_power_vout_sum;
static float g_power_iout_sum;
static float g_power_temp_sum;
static uint8_t g_power_window_count;
static uint8_t g_power_window_next;

static void set_hw_status(const char *status)
{
    strlcpy(g_hw_status, status, sizeof(g_hw_status));
}

static TPS546_CONFIG gamma_tps546_config(void)
{
    TPS546_CONFIG config = {0};
    config.TPS546_INIT_PHASE = TPS546_INIT_PHASE_SINGLE;
    config.TPS546_INIT_VIN_ON = INPUT_VOLTAGE_EXPECTED_MIN_VOLTS;
    config.TPS546_INIT_VIN_OFF = INPUT_VOLTAGE_MIN_VOLTS;
    config.TPS546_INIT_VIN_UV_WARN_LIMIT = 0.0f;
    config.TPS546_INIT_VIN_OV_FAULT_LIMIT = INPUT_VOLTAGE_SHUTDOWN_VOLTS;
    config.TPS546_INIT_SCALE_LOOP = 0.25f;
    config.TPS546_INIT_VOUT_MIN = 0.5f;
    config.TPS546_INIT_VOUT_MAX = ASIC_VOLTAGE_SHUTDOWN_VOLTS;
    config.TPS546_INIT_VOUT_COMMAND = 1.2f;
    config.TPS546_INIT_IOUT_OC_WARN_LIMIT = TPS546_IOUT_WARN_AMPS;
    config.TPS546_INIT_IOUT_OC_FAULT_LIMIT = TPS546_IOUT_FAULT_AMPS;
    config.TPS546_INIT_STACK_CONFIG = 0x0000;
    config.TPS546_INIT_SYNC_CONFIG = 0x10;
    return config;
}

static esp_err_t asic_reset_level(int level)
{
    ESP_RETURN_ON_ERROR(gpio_reset_pin(ASIC_RESET_GPIO), TAG, "reset pin reset failed");
    ESP_RETURN_ON_ERROR(gpio_set_direction(ASIC_RESET_GPIO, GPIO_MODE_OUTPUT), TAG,
                        "reset pin direction failed");
    ESP_RETURN_ON_ERROR(gpio_set_level(ASIC_RESET_GPIO, level), TAG, "reset pin set failed");
    return ESP_OK;
}

static esp_err_t asic_reset_pulse(void)
{
    ESP_RETURN_ON_ERROR(asic_reset_level(0), TAG, "reset low failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    ESP_RETURN_ON_ERROR(gpio_set_level(ASIC_RESET_GPIO, 1), TAG, "reset high failed");
    vTaskDelay(pdMS_TO_TICKS(100));
    return ESP_OK;
}

static void store_power_snapshot(const TPS546_StatusSnapshot *snapshot)
{
    const bitaxe_gamma602_power_snapshot_t compact = {
        .valid = true,
        .status_word = snapshot->status_word,
        .operation = snapshot->operation,
        .vout_command = snapshot->vout_command,
        .read_vout = snapshot->read_vout,
        .read_vin = snapshot->read_vin,
        .read_iout = snapshot->read_iout,
        .read_temp_c = snapshot->read_temp1,
    };

    portENTER_CRITICAL(&g_power_snapshot_lock);
    g_power_snapshot = compact;
    portEXIT_CRITICAL(&g_power_snapshot_lock);
}

static void clear_power_snapshot(void)
{
    portENTER_CRITICAL(&g_power_snapshot_lock);
    memset(&g_power_snapshot, 0, sizeof(g_power_snapshot));
    portEXIT_CRITICAL(&g_power_snapshot_lock);
}

static void reset_power_average_window(void)
{
    memset(g_power_vin_window, 0, sizeof(g_power_vin_window));
    memset(g_power_vout_window, 0, sizeof(g_power_vout_window));
    memset(g_power_iout_window, 0, sizeof(g_power_iout_window));
    memset(g_power_temp_window, 0, sizeof(g_power_temp_window));
    g_power_vin_sum = 0.0f;
    g_power_vout_sum = 0.0f;
    g_power_iout_sum = 0.0f;
    g_power_temp_sum = 0.0f;
    g_power_window_count = 0;
    g_power_window_next = 0;
}

static void update_power_state(GlobalState *state, const TPS546_StatusSnapshot *snapshot)
{
    state->POWER_MANAGEMENT_MODULE.voltage = snapshot->read_vin;
    state->POWER_MANAGEMENT_MODULE.current = snapshot->read_iout;
    state->POWER_MANAGEMENT_MODULE.core_voltage = snapshot->read_vout;
    state->POWER_MANAGEMENT_MODULE.power = snapshot->read_vout * snapshot->read_iout;
    state->POWER_MANAGEMENT_MODULE.vr_temp = (float)snapshot->read_temp1;
}

static void update_power_average(const TPS546_StatusSnapshot *raw, TPS546_StatusSnapshot *averaged)
{
    const uint8_t index = g_power_window_next;
    if (g_power_window_count == TPS546_POWER_WINDOW_SAMPLES) {
        g_power_vin_sum -= g_power_vin_window[index];
        g_power_vout_sum -= g_power_vout_window[index];
        g_power_iout_sum -= g_power_iout_window[index];
        g_power_temp_sum -= g_power_temp_window[index];
    } else {
        ++g_power_window_count;
    }

    g_power_vin_window[index] = raw->read_vin;
    g_power_vout_window[index] = raw->read_vout;
    g_power_iout_window[index] = raw->read_iout;
    g_power_temp_window[index] = (float)raw->read_temp1;
    g_power_vin_sum += g_power_vin_window[index];
    g_power_vout_sum += g_power_vout_window[index];
    g_power_iout_sum += g_power_iout_window[index];
    g_power_temp_sum += g_power_temp_window[index];
    g_power_window_next = (uint8_t)((index + 1) % TPS546_POWER_WINDOW_SAMPLES);

    const float samples = (float)g_power_window_count;
    *averaged = *raw;
    averaged->read_vin = g_power_vin_sum / samples;
    averaged->read_vout = g_power_vout_sum / samples;
    averaged->read_iout = g_power_iout_sum / samples;
    averaged->read_temp1 = (int)lroundf(g_power_temp_sum / samples);
}

static esp_err_t capture_power_snapshot(TPS546_StatusSnapshot *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));
    ESP_RETURN_ON_ERROR(TPS546_snapshot_status(snapshot), TAG, "TPS546 snapshot failed");
    store_power_snapshot(snapshot);
    return ESP_OK;
}

static esp_err_t shutdown_regulator(GlobalState *state, const char *reason)
{
    ESP_LOGE(TAG, "%s; turning TPS546 output off", reason);
    bitaxe_fan_force_max_if_allowed(state, reason);
    set_hw_status("regulator shutdown");
    g_regulator_enabled = false;
    g_commanded_voltage_mv = 0;
    g_chip_count = 0;
    clear_power_snapshot();

    esp_err_t reset_err = asic_reset_level(0);
    if (reset_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to hold ASIC reset low during regulator shutdown: %s",
                 esp_err_to_name(reset_err));
    }

    if (!g_tps546_ready) {
        return ESP_OK;
    }

    return TPS546_set_vout(0.0f);
}

static void flag_safety_fault(GlobalState *state, const char *reason)
{
    state->SYSTEM_MODULE.power_fault = 1;
    state->SYSTEM_MODULE.hardware_fault = true;
    strlcpy(state->SYSTEM_MODULE.hardware_fault_msg, reason,
            sizeof(state->SYSTEM_MODULE.hardware_fault_msg));
    state->ASIC_initalized = false;
    bitaxe_gamma602_clear_jobs(state);
}

static void safety_shutdown(GlobalState *state, const char *reason)
{
    flag_safety_fault(state, reason);
    esp_err_t shutdown_err = shutdown_regulator(state, reason);
    if (shutdown_err != ESP_OK) {
        ESP_LOGE(TAG, "regulator shutdown failed after %s: %s", reason,
                 esp_err_to_name(shutdown_err));
    }
    set_hw_status("safety shutdown");
}

static esp_err_t shutdown_and_return(GlobalState *state, const char *reason, esp_err_t result)
{
    esp_err_t shutdown_err = shutdown_regulator(state, reason);
    if (shutdown_err != ESP_OK) {
        ESP_LOGE(TAG, "regulator shutdown failed after %s: %s", reason,
                 esp_err_to_name(shutdown_err));
    }
    return result;
}

static esp_err_t fail_regulator_safety(GlobalState *state, const char *reason,
                                       const TPS546_StatusSnapshot *snapshot)
{
    TPS546_log_snapshot(snapshot);
    safety_shutdown(state, reason);
    return ESP_FAIL;
}

static esp_err_t validate_regulator_safety(GlobalState *state,
                                           const TPS546_StatusSnapshot *snapshot,
                                           float target_vout,
                                           bool require_target_vout)
{
    if (snapshot->status_word & TPS546_STATUS_FAULT_MASK) {
        return fail_regulator_safety(state, "TPS546 status fault", snapshot);
    }

    if ((snapshot->status_word & TPS546_STATUS_INPUT) &&
        (snapshot->st_input & TPS546_STATUS_VIN_OVF)) {
        return fail_regulator_safety(state, "TPS546 VIN overvoltage fault", snapshot);
    }

    if ((snapshot->operation & OPERATION_ON) == 0) {
        return fail_regulator_safety(state, "TPS546 operation bit is not ON", snapshot);
    }

    if (snapshot->read_vin < INPUT_VOLTAGE_MIN_VOLTS) {
        return fail_regulator_safety(state, "TPS546 VIN below safe range", snapshot);
    }

    if (snapshot->read_vin >= INPUT_VOLTAGE_SHUTDOWN_VOLTS) {
        return fail_regulator_safety(state, "input voltage at or above 5.5 V", snapshot);
    }

    if (snapshot->read_vout < ASIC_VOLTAGE_MIN_SHUTDOWN_VOLTS) {
        return fail_regulator_safety(state, "ASIC voltage below 700 mV while output is enabled",
                                     snapshot);
    }

    if (snapshot->read_vout >= ASIC_VOLTAGE_SHUTDOWN_VOLTS) {
        return fail_regulator_safety(state, "ASIC voltage at or above 1400 mV", snapshot);
    }

    if (require_target_vout && fabsf(snapshot->read_vout - target_vout) > TPS546_VOUT_TOLERANCE_VOLTS) {
        return fail_regulator_safety(state, "TPS546 VOUT outside tolerance", snapshot);
    }

    if (snapshot->read_temp1 >= TPS546_MAX_SAFE_TEMP_C) {
        return fail_regulator_safety(state, "TPS546 temperature too high", snapshot);
    }

    return ESP_OK;
}

static esp_err_t update_asic_temperature(GlobalState *state, float *temp_c)
{
    esp_err_t err = bitaxe_fan_read_asic_temp_c(temp_c);
    if (err != ESP_OK) {
        safety_shutdown(state, "ASIC temperature read failed");
        return err;
    }

    *temp_c += (float)state->DEVICE_CONFIG.temp_offset;
    state->POWER_MANAGEMENT_MODULE.chip_temp_avg = *temp_c;
    if (!isfinite(*temp_c)) {
        safety_shutdown(state, "ASIC temperature invalid");
        return ESP_FAIL;
    }

    if (*temp_c >= ASIC_TEMP_SHUTDOWN_C) {
        ESP_LOGE(TAG, "ASIC temperature %.1f C reached shutdown limit %.1f C", *temp_c,
                 ASIC_TEMP_SHUTDOWN_C);
        safety_shutdown(state, "ASIC temperature at or above 69 C");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static bool read_asic_temp_for_voltage(GlobalState *state, float *temp_c)
{
    if (temp_c == NULL) {
        return false;
    }

    float raw_temp_c = 0.0f;
    if (bitaxe_fan_read_asic_temp_c(&raw_temp_c) != ESP_OK) {
        *temp_c = 0.0f;
        return false;
    }

    raw_temp_c += (float)state->DEVICE_CONFIG.temp_offset;
    if (!isfinite(raw_temp_c) || raw_temp_c <= 0.0f) {
        *temp_c = 0.0f;
        return false;
    }

    state->POWER_MANAGEMENT_MODULE.chip_temp_avg = raw_temp_c;
    *temp_c = raw_temp_c;
    return true;
}

static esp_err_t apply_temperature_voltage_compensation(GlobalState *state, float asic_temp_c)
{
    const uint16_t target_mv =
        m45_config_effective_asic_voltage_mv_for_temp(m45_config_get(), asic_temp_c);
    if (g_commanded_voltage_mv == 0) {
        g_commanded_voltage_mv = m45_config_effective_asic_voltage_mv(m45_config_get());
    }

    const int delta_mv = (int)target_mv - (int)g_commanded_voltage_mv;
    if (abs(delta_mv) < ASIC_TEMP_VOLTAGE_UPDATE_DEADBAND_MV) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "temperature voltage compensation %.1f C: %u -> %u mV", asic_temp_c,
             g_commanded_voltage_mv, target_mv);
    return bitaxe_gamma602_set_voltage_mv(state, target_mv);
}

static esp_err_t validate_regulator_startup_snapshot(GlobalState *state,
                                                     const TPS546_StatusSnapshot *snapshot,
                                                     bool *settled,
                                                     float target_vout)
{
    *settled = false;

    const uint16_t startup_fault_mask = TPS546_STATUS_FAULT_MASK & ~TPS546_STATUS_PGOOD;
    if (snapshot->status_word & startup_fault_mask) {
        return fail_regulator_safety(state, "TPS546 status fault during startup", snapshot);
    }

    if ((snapshot->status_word & TPS546_STATUS_INPUT) &&
        (snapshot->st_input & TPS546_STATUS_VIN_OVF)) {
        return fail_regulator_safety(state, "TPS546 VIN overvoltage fault", snapshot);
    }

    if ((snapshot->operation & OPERATION_ON) == 0) {
        return fail_regulator_safety(state, "TPS546 operation bit is not ON", snapshot);
    }

    if (snapshot->read_vin < INPUT_VOLTAGE_MIN_VOLTS) {
        return fail_regulator_safety(state, "TPS546 VIN below safe range", snapshot);
    }

    if (snapshot->read_vin >= INPUT_VOLTAGE_SHUTDOWN_VOLTS) {
        return fail_regulator_safety(state, "input voltage at or above 5.5 V", snapshot);
    }

    if (snapshot->read_vout >= ASIC_VOLTAGE_SHUTDOWN_VOLTS) {
        return fail_regulator_safety(state, "ASIC voltage at or above 1400 mV", snapshot);
    }

    if (snapshot->read_temp1 >= TPS546_MAX_SAFE_TEMP_C) {
        return fail_regulator_safety(state, "TPS546 temperature too high", snapshot);
    }

    if (snapshot->read_vout >= ASIC_VOLTAGE_MIN_SHUTDOWN_VOLTS &&
        fabsf(snapshot->read_vout - target_vout) <= TPS546_VOUT_TOLERANCE_VOLTS) {
        *settled = true;
    }

    return ESP_OK;
}

static esp_err_t wait_for_regulator_after_enable(GlobalState *state, float target_vout)
{
    const TickType_t started = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(TPS546_OUTPUT_SETTLE_TIMEOUT_MS);

    vTaskDelay(pdMS_TO_TICKS(TPS546_OUTPUT_SETTLE_INITIAL_MS));

    while (true) {
        TPS546_StatusSnapshot snapshot;
        esp_err_t err = capture_power_snapshot(&snapshot);
        if (err != ESP_OK) {
            safety_shutdown(state, "TPS546 validation read failed");
            return err;
        }
        update_power_state(state, &snapshot);

        bool settled = false;
        ESP_RETURN_ON_ERROR(validate_regulator_startup_snapshot(state, &snapshot, &settled,
                                                                target_vout),
                            TAG, "TPS546 startup safety validation failed");
        if (settled) {
            const uint32_t elapsed_ms =
                pdTICKS_TO_MS(xTaskGetTickCount() - started);
            ESP_LOGI(TAG,
                     "TPS546 ok after %lu ms: VIN %.2f V, VOUT %.3f V, IOUT %.2f A, temp %d C, status 0x%04x",
                     (unsigned long)elapsed_ms, snapshot.read_vin, snapshot.read_vout,
                     snapshot.read_iout, snapshot.read_temp1, snapshot.status_word);
            return ESP_OK;
        }

        if ((xTaskGetTickCount() - started) >= timeout_ticks) {
            return fail_regulator_safety(state, "TPS546 VOUT did not settle after enable",
                                         &snapshot);
        }

        vTaskDelay(pdMS_TO_TICKS(TPS546_OUTPUT_SETTLE_POLL_MS));
    }
}

static void power_monitor_task(void *arg)
{
    GlobalState *state = (GlobalState *)arg;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(ASIC_FAN_MONITOR_INTERVAL_MS));
        if (!g_regulator_enabled) {
            continue;
        }

        TPS546_StatusSnapshot snapshot;
        esp_err_t err = capture_power_snapshot(&snapshot);
        if (err != ESP_OK) {
            safety_shutdown(state, "TPS546 monitor read failed");
            continue;
        }

        if (validate_regulator_safety(
                state, &snapshot,
                (g_commanded_voltage_mv > 0
                     ? g_commanded_voltage_mv
                     : m45_config_effective_asic_voltage_mv(m45_config_get())) /
                    1000.0f,
                false) != ESP_OK) {
            continue;
        }

        TPS546_StatusSnapshot averaged_snapshot;
        update_power_average(&snapshot, &averaged_snapshot);
        store_power_snapshot(&averaged_snapshot);
        update_power_state(state, &averaged_snapshot);

        if (state->ASIC_initalized) {
            float asic_temp_c = 0.0f;
            if (update_asic_temperature(state, &asic_temp_c) != ESP_OK) {
                continue;
            }
            if (apply_temperature_voltage_compensation(state, asic_temp_c) != ESP_OK) {
                ESP_LOGW(TAG, "temperature voltage compensation update failed");
                continue;
            }
            bitaxe_fan_update_auto(state, asic_temp_c, bitaxe_fan_control_temp_c(asic_temp_c),
                                   (float)averaged_snapshot.read_temp1);
        }

        if (!state->SYSTEM_MODULE.hardware_fault) {
            state->SYSTEM_MODULE.power_fault = 0;
        }
    }
}

static esp_err_t start_power_monitor(GlobalState *state)
{
    if (g_power_monitor_started) {
        return ESP_OK;
    }

    BaseType_t created =
        xTaskCreate(power_monitor_task, "tps546_mon", 4096, state, tskIDLE_PRIORITY + 2, NULL);
    if (created == pdPASS) {
        g_power_monitor_started = true;
        return ESP_OK;
    } else {
        ESP_LOGE(TAG, "failed to start TPS546 monitor task");
        return ESP_ERR_NO_MEM;
    }
}

void bitaxe_gamma602_init_state(GlobalState *state)
{
    memset(state, 0, sizeof(*state));

    state->DEVICE_CONFIG = (DeviceConfig) {
        .board_version = "602",
        .family = FAMILY_GAMMA,
        .EMC2101 = true,
        .emc_ideality_factor = 0x24,
        .emc_beta_compensation = 0x00,
        .TPS546 = true,
        .power_consumption_target = 22,
    };
    const m45_config_t *config = m45_config_get();
    state->POWER_MANAGEMENT_MODULE.frequency_value =
        (float)m45_config_effective_asic_frequency_mhz(config);
    state->POWER_MANAGEMENT_MODULE.actual_frequency = 0.0f;
    state->POWER_MANAGEMENT_MODULE.core_voltage =
        m45_config_effective_asic_voltage_mv(config) / 1000.0f;
    state->pool_difficulty = m45_config_effective_pool_difficulty(
        config, state->DEVICE_CONFIG.family.asic.small_core_count,
        state->DEVICE_CONFIG.family.asic_count);
    state->version_mask = STRATUM_DEFAULT_VERSION_MASK;
    state->SYSTEM_MODULE.pool_user = (char *)config->pool_user;
    state->SYSTEM_MODULE.pool_pass = (char *)config->pool_pass;
    state->SYSTEM_MODULE.pool_url = (char *)config->pool_host;
    state->SYSTEM_MODULE.pool_port = config->pool_port;
    state->send_uid = 10;
    state->stratum_mux = (portMUX_TYPE)portMUX_INITIALIZER_UNLOCKED;
    pthread_mutex_init(&state->valid_jobs_lock, NULL);

    state->ASIC_TASK_MODULE.active_jobs = calloc(ASIC_JOB_SLOTS, sizeof(bm_job *));
    state->valid_jobs = calloc(ASIC_JOB_SLOTS, sizeof(uint8_t));
    if (state->ASIC_TASK_MODULE.active_jobs == NULL || state->valid_jobs == NULL) {
        ESP_LOGE(TAG, "failed to allocate ASIC job tables");
        set_hw_status("job table alloc failed");
    }
}

esp_err_t bitaxe_gamma602_prepare_io(void)
{
    if (!g_i2c_ready) {
        ESP_RETURN_ON_ERROR(i2c_bitaxe_init(), TAG, "I2C init failed");
        g_i2c_ready = true;
    }
    ESP_RETURN_ON_ERROR(asic_reset_level(0), TAG, "ASIC reset hold failed");
    set_hw_status("reset held");
    return ESP_OK;
}

esp_err_t bitaxe_gamma602_boot_fan_max(void)
{
    set_hw_status("fan max");
    ESP_LOGI(TAG, "setting EMC2101 fan to max before NVS, display, network, and TPS546");
    return bitaxe_fan_boot_max();
}

esp_err_t bitaxe_gamma602_start_fan(GlobalState *state)
{
    set_hw_status("fan init");
    ESP_LOGI(TAG, "initializing EMC2101 fan before display, network, and TPS546");
    return bitaxe_fan_init(state);
}

esp_err_t bitaxe_gamma602_start_hardware(GlobalState *state)
{
    if (state->ASIC_TASK_MODULE.active_jobs == NULL || state->valid_jobs == NULL) {
        set_hw_status("job table missing");
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(bitaxe_gamma602_start_fan(state), TAG, "fan init failed");
    clear_power_snapshot();
    reset_power_average_window();

    set_hw_status("regulator init");
    ESP_LOGI(TAG, "initializing TPS546 for Gamma 602");
    esp_err_t ret = TPS546_init(gamma_tps546_config());
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "TPS546 init failed", ret);
    }
    g_tps546_ready = true;

    const m45_config_t *config = m45_config_get();
    float asic_temp_c = 0.0f;
    const bool have_startup_temp = read_asic_temp_for_voltage(state, &asic_temp_c);
    const uint16_t voltage_mv =
        m45_config_effective_asic_voltage_mv_for_temp(config, asic_temp_c);
    const uint16_t compensation_mv =
        m45_config_asic_voltage_temp_compensation_mv(config, asic_temp_c);
    const float volts = voltage_mv / 1000.0f;
    if (compensation_mv > 0) {
        ESP_LOGI(TAG, "ASIC temp %.1f C adds %u mV voltage compensation", asic_temp_c,
                 compensation_mv);
    } else if (config->overclock_enabled && config->asic_voltage_temp_compensation_enabled &&
               !have_startup_temp) {
        ESP_LOGW(TAG, "ASIC temp unavailable; starting without voltage compensation");
    }
    ESP_LOGI(TAG, "setting BM1370 core voltage to %.3f V", volts);
    ret = TPS546_set_vout(volts);
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "TPS546 set voltage failed", ret);
    }
    g_commanded_voltage_mv = voltage_mv;
    g_regulator_enabled = true;
    ret = wait_for_regulator_after_enable(state, volts);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = start_power_monitor(state);
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "TPS546 monitor start failed", ret);
    }

    set_hw_status("asic reset");
    ret = asic_reset_pulse();
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "ASIC reset failed", ret);
    }

    set_hw_status("serial init");
    ret = SERIAL_init();
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "ASIC serial init failed", ret);
    }

    set_hw_status("asic detect");
    g_chip_count = BM1370_init(state);
    if (g_chip_count == 0) {
        set_hw_status("no BM1370");
        ESP_LOGE(TAG, "no BM1370 chips detected");
        return shutdown_and_return(state, "no BM1370 detected", ESP_FAIL);
    }

    ret = SERIAL_set_baud(BM1370_set_max_baud());
    if (ret != ESP_OK) {
        return shutdown_and_return(state, "ASIC baud change failed", ret);
    }
    SERIAL_clear_buffer();
    state->ASIC_initalized = true;
    set_hw_status("ready");

    ESP_LOGI(TAG, "Gamma 602 ASIC ready: %u chip(s), %.0f MHz, %d mV",
             g_chip_count, state->POWER_MANAGEMENT_MODULE.actual_frequency,
             g_commanded_voltage_mv);
    return ESP_OK;
}

bool bitaxe_gamma602_asic_power_enabled(void)
{
    return g_regulator_enabled;
}

esp_err_t bitaxe_gamma602_set_asic_power(GlobalState *state, bool enabled)
{
    if (state == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (enabled) {
        if (state->SYSTEM_MODULE.hardware_fault) {
            return ESP_ERR_INVALID_STATE;
        }
        if (state->ASIC_initalized && g_regulator_enabled) {
            return ESP_OK;
        }
        bitaxe_gamma602_clear_jobs(state);
        const esp_err_t err = bitaxe_gamma602_start_hardware(state);
        if (err != ESP_OK && !state->SYSTEM_MODULE.hardware_fault) {
            set_hw_status("asic off");
        }
        return err;
    }

    ESP_LOGI(TAG, "manual ASIC power off requested");
    set_hw_status("asic off");
    g_regulator_enabled = false;
    g_commanded_voltage_mv = 0;
    g_chip_count = 0;
    state->ASIC_initalized = false;
    if (!state->SYSTEM_MODULE.hardware_fault) {
        state->SYSTEM_MODULE.power_fault = 0;
    }
    bitaxe_gamma602_clear_jobs(state);
    clear_power_snapshot();
    state->POWER_MANAGEMENT_MODULE.power = 0.0f;
    state->POWER_MANAGEMENT_MODULE.current = 0.0f;
    state->POWER_MANAGEMENT_MODULE.chip_temp_avg = 0.0f;

    esp_err_t err = asic_reset_level(0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to hold ASIC reset low during manual power off: %s",
                 esp_err_to_name(err));
    }

    if (g_tps546_ready) {
        const esp_err_t vout_err = TPS546_set_vout(0.0f);
        if (vout_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to turn TPS546 output off manually: %s",
                     esp_err_to_name(vout_err));
            if (err == ESP_OK) {
                err = vout_err;
            }
        }
    }

    return err;
}

esp_err_t bitaxe_gamma602_set_frequency_mhz(GlobalState *state, uint16_t frequency_mhz)
{
    if (state == NULL || frequency_mhz < M45_ASIC_FREQUENCY_MIN_MHZ ||
        frequency_mhz > M45_ASIC_FREQUENCY_MAX_MHZ) {
        return ESP_ERR_INVALID_ARG;
    }

    state->POWER_MANAGEMENT_MODULE.frequency_value = (float)frequency_mhz;
    if (!state->ASIC_initalized) {
        return ESP_OK;
    }

    set_hw_status("clock update");
    SERIAL_clear_buffer();
    bitaxe_gamma602_clear_jobs(state);
    do_frequency_transition(state, BM1370_send_hash_frequency);
    BM1370_set_nonce_space(BM1370_NONCE_SPACE_PERCENT,
                           state->POWER_MANAGEMENT_MODULE.actual_frequency,
                           bitaxe_gamma602_chip_count(),
                           state->DEVICE_CONFIG.family.asic.core_count);
    vTaskDelay(pdMS_TO_TICKS(ASIC_FREQUENCY_SETTLE_MS));
    SERIAL_clear_buffer();
    bitaxe_gamma602_clear_jobs(state);
    set_hw_status("ready");
    return ESP_OK;
}

esp_err_t bitaxe_gamma602_set_voltage_mv(GlobalState *state, uint16_t voltage_mv)
{
    if (state == NULL || voltage_mv < 500 || voltage_mv > 1370) {
        return ESP_ERR_INVALID_ARG;
    }

    const float volts = voltage_mv / 1000.0f;
    state->POWER_MANAGEMENT_MODULE.core_voltage = volts;
    if (!g_tps546_ready || !g_regulator_enabled) {
        g_commanded_voltage_mv = voltage_mv;
        return ESP_OK;
    }

    set_hw_status("voltage update");
    esp_err_t ret = TPS546_set_vout(volts);
    if (ret == ESP_OK) {
        ret = wait_for_regulator_after_enable(state, volts);
    }
    if (ret == ESP_OK) {
        g_commanded_voltage_mv = voltage_mv;
        set_hw_status("ready");
    }
    return ret;
}

void bitaxe_gamma602_clear_jobs(GlobalState *state)
{
    if (state->ASIC_TASK_MODULE.active_jobs == NULL || state->valid_jobs == NULL) {
        return;
    }

    pthread_mutex_lock(&state->valid_jobs_lock);
    for (int i = 0; i < ASIC_JOB_SLOTS; ++i) {
        state->valid_jobs[i] = 0;
        if (state->ASIC_TASK_MODULE.active_jobs[i] != NULL) {
            free_bm_job(state->ASIC_TASK_MODULE.active_jobs[i]);
            state->ASIC_TASK_MODULE.active_jobs[i] = NULL;
        }
    }
    pthread_mutex_unlock(&state->valid_jobs_lock);
}

uint8_t bitaxe_gamma602_chip_count(void)
{
    return g_chip_count;
}

const char *bitaxe_gamma602_status(void)
{
    return g_hw_status;
}

const char *bitaxe_gamma602_tps_model(void)
{
    return TPS546_model();
}

bool bitaxe_gamma602_power_snapshot(bitaxe_gamma602_power_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    portENTER_CRITICAL(&g_power_snapshot_lock);
    *snapshot = g_power_snapshot;
    portEXIT_CRITICAL(&g_power_snapshot_lock);

    return snapshot->valid;
}

void bitaxe_gamma602_safety_limits(bitaxe_gamma602_safety_limits_t *limits)
{
    if (limits == NULL) {
        return;
    }

    const uint16_t target_voltage_mv =
        g_commanded_voltage_mv > 0 ? g_commanded_voltage_mv
                                   : m45_config_effective_asic_voltage_mv(m45_config_get());
    const float target_vout = target_voltage_mv / 1000.0f;
    const float expected_vout_min =
        fmaxf(ASIC_VOLTAGE_MIN_SHUTDOWN_VOLTS, target_vout - TPS546_VOUT_TOLERANCE_VOLTS);
    const float expected_vout_max =
        fminf(ASIC_VOLTAGE_SHUTDOWN_VOLTS, target_vout + TPS546_VOUT_TOLERANCE_VOLTS);

    *limits = (bitaxe_gamma602_safety_limits_t){
        .input_voltage_min_v = INPUT_VOLTAGE_MIN_VOLTS,
        .input_voltage_expected_min_v = INPUT_VOLTAGE_EXPECTED_MIN_VOLTS,
        .input_voltage_expected_max_v = INPUT_VOLTAGE_EXPECTED_MAX_VOLTS,
        .input_voltage_max_v = INPUT_VOLTAGE_SHUTDOWN_VOLTS,
        .asic_voltage_min_v = ASIC_VOLTAGE_MIN_SHUTDOWN_VOLTS,
        .asic_voltage_expected_min_v = expected_vout_min,
        .asic_voltage_expected_max_v = expected_vout_max,
        .asic_voltage_max_v = ASIC_VOLTAGE_SHUTDOWN_VOLTS,
        .asic_voltage_target_v = target_vout,
        .asic_temp_expected_max_c = ASIC_TEMP_EXPECTED_MAX_C,
        .asic_temp_max_c = ASIC_TEMP_SHUTDOWN_C,
        .tps546_temp_expected_max_c = TPS546_EXPECTED_MAX_TEMP_C,
        .tps546_temp_max_c = (float)TPS546_MAX_SAFE_TEMP_C,
        .iout_warn_a = TPS546_IOUT_WARN_AMPS,
        .iout_fault_a = TPS546_IOUT_FAULT_AMPS,
        .power_warn_w = target_vout * TPS546_IOUT_WARN_AMPS,
        .power_fault_w = target_vout * TPS546_IOUT_FAULT_AMPS,
        .fan_expected_percent = 100.0f,
    };
}
