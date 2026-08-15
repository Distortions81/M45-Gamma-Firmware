#include <stdio.h>
#include <string.h>

#include "bitaxe_hw.h"
#include "esp_err.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "m45_config.h"
#include "m45_fault_log.h"
#include "m45_log_buffer.h"
#include "m45_oled.h"
#include "nvs_flash.h"
#include "stratum_minimal.h"
#include "wifi_http.h"

static const char *TAG = "app";
static GlobalState g_state;
static bool g_ota_pending_verification;
#define M45_RTC_BOOT_MARKER 0x4d343542U
RTC_NOINIT_ATTR static uint32_t g_rtc_boot_marker;

static bool reset_button_recovery_requested(void)
{
    const bool requested = esp_reset_reason() == ESP_RST_POWERON &&
                           g_rtc_boot_marker == M45_RTC_BOOT_MARKER;
    g_rtc_boot_marker = M45_RTC_BOOT_MARKER;
    return requested;
}

static void detect_pending_ota(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state = ESP_OTA_IMG_UNDEFINED;
    g_ota_pending_verification =
        running != NULL && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY;
    if (g_ota_pending_verification) {
        ESP_LOGW(TAG, "OTA image pending hardware health verification");
    }
}

static bool accept_pending_ota(void)
{
    if (!g_ota_pending_verification) {
        return true;
    }
    const esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        g_ota_pending_verification = false;
        ESP_LOGI(TAG, "OTA image passed hardware health verification");
        return true;
    } else {
        ESP_LOGE(TAG, "failed to confirm OTA image: %s", esp_err_to_name(err));
        return false;
    }
}

static void hardware_task(void *arg)
{
    GlobalState *state = (GlobalState *)arg;
    esp_err_t ret = bitaxe_gamma602_start_hardware(state);
    if (ret != ESP_OK) {
        if (!state->SYSTEM_MODULE.hardware_fault) {
            state->SYSTEM_MODULE.hardware_fault = true;
            state->ASIC_initalized = false;
            snprintf(state->SYSTEM_MODULE.hardware_fault_msg,
                     sizeof(state->SYSTEM_MODULE.hardware_fault_msg),
                     "hardware init failed: %s", esp_err_to_name(ret));
        }
        ESP_LOGE(TAG, "hardware init failed: %s", esp_err_to_name(ret));
        (void)m45_fault_log_record(state->SYSTEM_MODULE.hardware_fault_msg);
        if (g_ota_pending_verification) {
            ESP_LOGE(TAG, "rolling back OTA image after hardware initialization failure");
            esp_ota_mark_app_invalid_rollback_and_reboot();
        }
        vTaskDelete(NULL);
    }

    if (!accept_pending_ota()) {
        vTaskDelete(NULL);
    }

    ret = stratum_minimal_start(state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "stratum start failed: %s", esp_err_to_name(ret));
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    m45_log_buffer_init();
    detect_pending_ota();
    const bool reset_recovery_ap = reset_button_recovery_requested();

    ESP_ERROR_CHECK(bitaxe_gamma602_prepare_io());
    esp_err_t ret = bitaxe_gamma602_boot_fan_max();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "early boot fan max failed: %s", esp_err_to_name(ret));
    }

    ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_ERROR_CHECK(m45_config_load());
    ESP_ERROR_CHECK(m45_fault_log_init());
    if (reset_recovery_ap) {
        ESP_LOGW(TAG, "RESET button recovery requested; starting temporary setup AP");
        wifi_http_set_recovery_ap(true);
    }

    bitaxe_gamma602_init_state(&g_state);

    ret = bitaxe_gamma602_start_fan(&g_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "early fan init failed: %s", esp_err_to_name(ret));
    }

    ret = m45_oled_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED unavailable: %s", esp_err_to_name(ret));
    } else {
        const m45_oled_recovery_action_t recovery = m45_oled_recovery_action();
        if (recovery == M45_OLED_RECOVERY_FACTORY_RESET) {
            ESP_LOGW(TAG, "BOOT button recovery requested; erasing saved settings");
            ESP_ERROR_CHECK(m45_config_factory_reset());
            m45_oled_show_factory_reset();
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        if (reset_recovery_ap) {
            m45_oled_show_wifi_recovery();
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
        m45_oled_start_status_task(&g_state);
    }

    ESP_ERROR_CHECK(wifi_http_start(&g_state));

    if (!m45_config_hardware_identity_allowed()) {
        g_state.SYSTEM_MODULE.hardware_fault = true;
        g_state.ASIC_initalized = false;
        snprintf(g_state.SYSTEM_MODULE.hardware_fault_msg,
                 sizeof(g_state.SYSTEM_MODULE.hardware_fault_msg),
                 "AxeOS board %s is not Gamma 602",
                 m45_config_imported_board_version());
        ESP_LOGE(TAG, "%s; ASIC startup blocked",
                 g_state.SYSTEM_MODULE.hardware_fault_msg);
        (void)m45_fault_log_record(g_state.SYSTEM_MODULE.hardware_fault_msg);
    } else if (xTaskCreate(hardware_task, "hardware", 8192, &g_state, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start hardware task");
    }
}
