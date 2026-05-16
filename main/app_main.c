#include <stdio.h>
#include <string.h>

#include "bitaxe_hw.h"
#include "esp_err.h"
#include "esp_log.h"
#include "m45_config.h"
#include "m45_log_buffer.h"
#include "m45_oled.h"
#include "nvs_flash.h"
#include "stratum_minimal.h"
#include "wifi_http.h"

static const char *TAG = "app";
static GlobalState g_state;

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

    bitaxe_gamma602_init_state(&g_state);

    ret = bitaxe_gamma602_start_fan(&g_state);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "early fan init failed: %s", esp_err_to_name(ret));
    }

    ret = m45_oled_init();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "OLED unavailable: %s", esp_err_to_name(ret));
    } else {
        m45_oled_start_status_task(&g_state);
    }

    ESP_ERROR_CHECK(wifi_http_start(&g_state));

    if (xTaskCreate(hardware_task, "hardware", 8192, &g_state, 8, NULL) != pdPASS) {
        ESP_LOGE(TAG, "failed to start hardware task");
    }
}
