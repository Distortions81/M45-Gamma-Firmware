#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"
#include "esp_http_server.h"
#include "global_state.h"

typedef struct {
    GlobalState *state;
    const char *page_token;
    const char *ip;
    const char *setup_ssid;
    const char *setup_ip;
    bool wifi_connected;
    bool setup_active;
    bool ota_supported;
    size_t retained_axeos_count;
    bool ota_preserve_axeos_possible;
    bool axeos_return_available;
} wifi_http_status_context_t;

esp_err_t wifi_http_status_send(httpd_req_t *req,
                                const wifi_http_status_context_t *context);
