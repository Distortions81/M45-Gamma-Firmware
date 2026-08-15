#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "global_state.h"

esp_err_t wifi_http_start(GlobalState *state);
void wifi_http_set_recovery_ap(bool enabled);
bool wifi_http_connected(void);
const char *wifi_http_ip(void);
bool wifi_http_setup_active(void);
const char *wifi_http_setup_ssid(void);
const char *wifi_http_setup_ip(void);
bool wifi_http_retained_axeos_available(void);
esp_err_t wifi_http_select_retained_axeos(void);
