#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

esp_err_t wifi_swarm_init(void);
esp_err_t wifi_swarm_get_handler(httpd_req_t *req);
esp_err_t wifi_swarm_post_handler(httpd_req_t *req, const char *local_ip);
