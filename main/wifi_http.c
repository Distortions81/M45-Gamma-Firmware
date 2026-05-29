#include "wifi_http.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "bitaxe_hw.h"
#include "bitaxe_fan.h"
#include "build_info.h"
#include "cJSON.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_mac.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "m45_config.h"
#include "m45_log_buffer.h"
#include "m45_oled.h"
#include "stratum_minimal.h"
#include "web_assets.h"

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1
#define WIFI_TEST_CONNECTED_BIT BIT2
#define WIFI_TEST_FAIL_BIT BIT3
#define WIFI_MAX_RETRY 8
#define WIFI_RETRY_BACKOFF_MS 30000
#define WIFI_TEST_TIMEOUT_MS 15000
#define WIFI_TEST_RESTORE_DELAY_MS 250
#ifdef M45_ASIC_LOSS_METRICS
#define STATUS_JSON_BUFFER_SIZE 9200
#else
#define STATUS_JSON_BUFFER_SIZE 7900
#endif
#define SETTINGS_JSON_BUFFER_SIZE 3400
#define M45_DEVICE_NAME "M45-Bitaxe"
#define HTTP_URI_HANDLER_SLOTS 34
#define HTTP_HANDLER_WARN_MS 100
#define LOG_CAPTURE_TIMEOUT_MS 5000
#define SETUP_SCAN_MAX_APS 12
#define SETUP_AP_CHANNEL 6
#define SETUP_AP_MAX_CLIENTS 4
#define SETUP_AP_IP0 10
#define SETUP_AP_IP1 45
#define SETUP_AP_IP2 0
#define SETUP_AP_IP3 1

static const char *TAG = "wifi_http";
static EventGroupHandle_t g_wifi_events;
static SemaphoreHandle_t g_wifi_test_mutex;
static SemaphoreHandle_t g_wifi_scan_mutex;
static GlobalState *g_state;
static esp_netif_t *g_netif;
static esp_netif_t *g_ap_netif;
static int g_retry_count;
static bool g_connected;
static bool g_setup_ap_active;
static volatile bool g_wifi_retry_backoff_pending;
static volatile bool g_wifi_test_active;
static volatile bool g_wifi_test_waiting;
static volatile bool g_wifi_test_ignore_assoc_leave;
static volatile uint8_t g_wifi_test_disconnect_reason;
static char g_ip[16] = "";
static char g_setup_ssid[32] = "m-idf";
static char g_setup_ip[16] = "10.45.0.1";
static uint8_t g_setup_ip_octets[4] = {SETUP_AP_IP0, SETUP_AP_IP1, SETUP_AP_IP2, SETUP_AP_IP3};
static char g_page_token[17];
static bool g_last_wifi_test_ok;
static char g_last_wifi_test_ssid[M45_WIFI_SSID_MAX + 1];
static char g_last_wifi_test_password[M45_WIFI_PASSWORD_MAX + 1];
static bool g_wifi_scan_running;
static bool g_wifi_scan_valid;
static esp_err_t g_wifi_scan_err = ESP_ERR_INVALID_STATE;
static wifi_ap_record_t g_wifi_scan_records[SETUP_SCAN_MAX_APS];
static uint16_t g_wifi_scan_count;
static portMUX_TYPE g_wifi_test_state_mux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t g_wifi_test_next_id;

typedef struct {
    uint32_t id;
    bool running;
    bool done;
    esp_err_t err;
    int rssi;
    uint8_t reason;
    char ip[16];
} wifi_test_async_state_t;

typedef struct {
    uint32_t id;
    char ssid[M45_WIFI_SSID_MAX + 1];
    char password[M45_WIFI_PASSWORD_MAX + 1];
} wifi_test_task_args_t;

static wifi_test_async_state_t g_wifi_test_state;

typedef esp_err_t (*http_route_handler_t)(httpd_req_t *req);

typedef struct {
    const char *uri;
    httpd_method_t method;
    http_route_handler_t handler;
} timed_http_route_t;

static void reboot_task(void *arg);
static void factory_reset_reboot_task(void *arg);

static uint64_t http_now_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

static void log_http_handler_delay(const char *operation, uint64_t started_us)
{
    const uint64_t elapsed_us = http_now_us() - started_us;
    if (elapsed_us < ((uint64_t)HTTP_HANDLER_WARN_MS * 1000ULL)) {
        return;
    }

    ESP_LOGW(TAG, "%s held HTTP server task for %llu ms", operation,
             (unsigned long long)(elapsed_us / 1000ULL));
}

static const char *http_method_name(httpd_method_t method)
{
    switch (method) {
    case HTTP_GET:
        return "GET";
    case HTTP_POST:
        return "POST";
    default:
        return "HTTP";
    }
}

static esp_err_t timed_http_handler(httpd_req_t *req)
{
    const timed_http_route_t *route = (const timed_http_route_t *)req->user_ctx;
    if (route == NULL || route->handler == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"route missing\"}");
    }

    const uint64_t started_us = http_now_us();
    const esp_err_t err = route->handler(req);
    const uint64_t elapsed_us = http_now_us() - started_us;
    if (elapsed_us >= ((uint64_t)HTTP_HANDLER_WARN_MS * 1000ULL)) {
        const char *uri = req->uri[0] != '\0' ? req->uri : route->uri;
        ESP_LOGW(TAG, "%s %s held HTTP server task for %llu ms",
                 http_method_name(route->method), uri,
                 (unsigned long long)(elapsed_us / 1000ULL));
    }
    return err;
}

static const char *payout_status_name(uint8_t status)
{
    switch (status) {
    case STRATUM_PAYOUT_STATUS_OK:
        return "ok";
    case STRATUM_PAYOUT_STATUS_LOW:
        return "low";
    case STRATUM_PAYOUT_STATUS_MISSING:
        return "missing";
    case STRATUM_PAYOUT_STATUS_UNSUPPORTED_WALLET:
        return "unsupported";
    case STRATUM_PAYOUT_STATUS_PARSE_ERROR:
        return "parse_error";
    case STRATUM_PAYOUT_STATUS_UNCHECKED:
    default:
        return "unchecked";
    }
}

static void format_domain_hashrates_json(const stratum_minimal_stats_t *stats,
                                         uint8_t expected_asic_count,
                                         char *dest,
                                         size_t dest_len)
{
    size_t used = 0;
    size_t avail = 0;
    uint8_t asic_count = stats->domain_asic_count > 0 ? stats->domain_asic_count
                                                       : expected_asic_count;
    if (asic_count == 0) {
        asic_count = 1;
    }
    if (asic_count > STRATUM_HASHRATE_MAX_ASICS) {
        asic_count = STRATUM_HASHRATE_MAX_ASICS;
    }
    uint8_t domain_count = stats->domain_count > 0 ? stats->domain_count
                                                   : STRATUM_HASH_DOMAIN_COUNT;
    if (domain_count > STRATUM_HASH_DOMAIN_COUNT) {
        domain_count = STRATUM_HASH_DOMAIN_COUNT;
    }

    if (dest_len == 0) {
        return;
    }
    int written = snprintf(dest, dest_len, "[");
    if (written < 0) {
        dest[0] = '\0';
        return;
    }
    used = (size_t)written < dest_len ? (size_t)written : dest_len - 1;

    for (uint8_t asic = 0; asic < asic_count && used < dest_len - 1; ++asic) {
        avail = dest_len - used;
        if (avail <= 1) {
            break;
        }
        written = snprintf(dest + used, avail, "%s[", asic == 0 ? "" : ",");
        used += written > 0 && (size_t)written < avail ? (size_t)written : avail - 1;
        for (uint8_t domain = 0; domain < domain_count && used < dest_len - 1; ++domain) {
            const double value = asic < stats->domain_asic_count && domain < stats->domain_count
                                     ? stats->domain_hashrates_ghs[asic][domain]
                                     : 0.0;
            avail = dest_len - used;
            if (avail <= 1) {
                break;
            }
            written = snprintf(dest + used, avail, "%s%.2f",
                               domain == 0 ? "" : ",", value);
            used += written > 0 && (size_t)written < avail ? (size_t)written : avail - 1;
        }
        avail = dest_len - used;
        if (avail <= 1) {
            break;
        }
        written = snprintf(dest + used, avail, "]");
        used += written > 0 && (size_t)written < avail ? (size_t)written : avail - 1;
    }
    avail = dest_len - used;
    if (avail > 1) {
        snprintf(dest + used, avail, "]");
    } else {
        dest[dest_len - 1] = '\0';
    }
}

static void init_page_token(void)
{
    snprintf(g_page_token, sizeof(g_page_token), "%08" PRIx32 "%08" PRIx32, esp_random(),
             esp_random());
}

static void init_setup_identity(void)
{
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP) != ESP_OK) {
        ESP_LOGW(TAG, "failed to read softAP MAC; using default setup identity");
    } else {
        snprintf(g_setup_ssid, sizeof(g_setup_ssid), "m-%02X%02X", mac[4], mac[5]);
        g_setup_ip_octets[1] = (uint8_t)((mac[5] / 10U) % 10U);
        g_setup_ip_octets[2] = (uint8_t)(mac[5] % 10U);
    }
    snprintf(g_setup_ip, sizeof(g_setup_ip), "%u.%u.%u.%u", g_setup_ip_octets[0],
             g_setup_ip_octets[1], g_setup_ip_octets[2], g_setup_ip_octets[3]);
}

static esp_err_t configure_setup_ap_netif(void)
{
    if (g_ap_netif == NULL) {
        g_ap_netif = esp_netif_create_default_wifi_ap();
        ESP_RETURN_ON_FALSE(g_ap_netif != NULL, ESP_ERR_NO_MEM, TAG,
                            "setup AP netif create failed");
    }

    esp_netif_ip_info_t ip_info = {
        .ip = {.addr = ESP_IP4TOADDR(SETUP_AP_IP0, SETUP_AP_IP1, SETUP_AP_IP2, SETUP_AP_IP3)},
        .gw = {.addr = ESP_IP4TOADDR(SETUP_AP_IP0, SETUP_AP_IP1, SETUP_AP_IP2, SETUP_AP_IP3)},
        .netmask = {.addr = ESP_IP4TOADDR(255, 255, 255, 0)},
    };
    ip_info.ip.addr = ESP_IP4TOADDR(g_setup_ip_octets[0], g_setup_ip_octets[1],
                                    g_setup_ip_octets[2], g_setup_ip_octets[3]);
    ip_info.gw.addr = ip_info.ip.addr;
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_stop(g_ap_netif), TAG, "setup DHCP stop failed");
    ESP_RETURN_ON_ERROR(esp_netif_set_ip_info(g_ap_netif, &ip_info), TAG,
                        "setup AP IP config failed");
    ESP_RETURN_ON_ERROR(esp_netif_dhcps_start(g_ap_netif), TAG, "setup DHCP start failed");
    return ESP_OK;
}

static esp_err_t configure_setup_ap_wifi(void)
{
    wifi_config_t ap_config = {
        .ap = {
            .ssid = "",
            .ssid_len = 0,
            .channel = SETUP_AP_CHANNEL,
            .authmode = WIFI_AUTH_OPEN,
            .max_connection = SETUP_AP_MAX_CLIENTS,
        },
    };
    strlcpy((char *)ap_config.ap.ssid, g_setup_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(g_setup_ssid);
    return esp_wifi_set_config(WIFI_IF_AP, &ap_config);
}

static void setup_dns_task(void *arg)
{
    (void)arg;
    static const uint8_t answer_template[16] = {
        0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
        0x00, 0x3c, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00};
    uint8_t buffer[512];
    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "setup DNS socket create failed");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) != 0) {
        ESP_LOGE(TAG, "setup DNS socket bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        struct sockaddr_in source_addr;
        socklen_t source_len = sizeof(source_addr);
        const int len =
            recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr *)&source_addr,
                     &source_len);
        if (len < 12) {
            continue;
        }

        int question_end = 12;
        while (question_end < len && buffer[question_end] != 0) {
            question_end += buffer[question_end] + 1;
        }
        question_end += 5;
        if (question_end > len ||
            question_end + (int)sizeof(answer_template) > (int)sizeof(buffer)) {
            continue;
        }

        buffer[2] = 0x81;
        buffer[3] = 0x80;
        buffer[6] = 0x00;
        buffer[7] = 0x01;
        buffer[8] = 0x00;
        buffer[9] = 0x00;
        buffer[10] = 0x00;
        buffer[11] = 0x00;

        uint8_t *answer = buffer + question_end;
        memcpy(answer, answer_template, sizeof(answer_template));
        answer[12] = g_setup_ip_octets[0];
        answer[13] = g_setup_ip_octets[1];
        answer[14] = g_setup_ip_octets[2];
        answer[15] = g_setup_ip_octets[3];

        sendto(sock, buffer, question_end + sizeof(answer_template), 0,
               (struct sockaddr *)&source_addr, source_len);
    }
}

static void start_setup_dns(void)
{
    xTaskCreate(setup_dns_task, "setup_dns", 3072, NULL, tskIDLE_PRIORITY + 2, NULL);
}

static void set_no_store_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
}

static esp_err_t send_gzip_asset(httpd_req_t *req, const char *type,
                                 const unsigned char *data, unsigned int len)
{
    httpd_resp_set_type(req, type);
    set_no_store_headers(req);
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    return httpd_resp_send(req, (const char *)data, (ssize_t)len);
}

static esp_err_t send_gzip_page(httpd_req_t *req, const unsigned char *data, unsigned int len)
{
    return send_gzip_asset(req, "text/html; charset=utf-8", data, len);
}

static esp_err_t send_page_token_reload(httpd_req_t *req)
{
    char body[64];
    snprintf(body, sizeof(body), "{\"page_token\":\"%s\",\"reload\":true}", g_page_token);
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool request_has_bad_page_token(httpd_req_t *req)
{
    const size_t token_len = httpd_req_get_hdr_value_len(req, "X-Page-Token");
    if (token_len == 0) {
        return false;
    }
    if (token_len >= sizeof(g_page_token)) {
        return true;
    }

    char token[sizeof(g_page_token)];
    if (httpd_req_get_hdr_value_str(req, "X-Page-Token", token, sizeof(token)) != ESP_OK) {
        return true;
    }
    return strcmp(token, g_page_token) != 0;
}

static esp_err_t set_sta_config(const char *ssid, const char *password)
{
    wifi_config_t wifi_config = {0};
    strlcpy((char *)wifi_config.sta.ssid, ssid != NULL ? ssid : "",
            sizeof(wifi_config.sta.ssid));
    strlcpy((char *)wifi_config.sta.password, password != NULL ? password : "",
            sizeof(wifi_config.sta.password));
    wifi_config.sta.threshold.authmode = WIFI_AUTH_OPEN;
    return esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
}

static const char *wifi_disconnect_reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "authentication failed";
    case WIFI_REASON_NO_AP_FOUND:
        return "network not found";
    case WIFI_REASON_ASSOC_FAIL:
    case WIFI_REASON_CONNECTION_FAIL:
        return "association failed";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost during connection";
    default:
        return "connection failed";
    }
}

static void clear_wifi_test_result_locked(void)
{
    g_last_wifi_test_ok = false;
    g_last_wifi_test_ssid[0] = '\0';
    g_last_wifi_test_password[0] = '\0';
}

static bool wifi_test_result_matches(const char *ssid, const char *password)
{
    if (g_wifi_test_mutex == NULL ||
        xSemaphoreTake(g_wifi_test_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    const bool matches =
        g_last_wifi_test_ok &&
        strcmp(g_last_wifi_test_ssid, ssid != NULL ? ssid : "") == 0 &&
        strcmp(g_last_wifi_test_password, password != NULL ? password : "") == 0;
    xSemaphoreGive(g_wifi_test_mutex);
    return matches;
}

static esp_err_t run_wifi_connection_test(const char *ssid, const char *password, int *rssi,
                                          char *ip, size_t ip_size, uint8_t *reason)
{
    if (ssid == NULL || ssid[0] == '\0' || rssi == NULL || ip == NULL || ip_size == 0 ||
        reason == NULL || g_wifi_test_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(g_wifi_test_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    clear_wifi_test_result_locked();
    *rssi = 0;
    ip[0] = '\0';
    *reason = 0;

    const m45_config_t saved_config = *m45_config_get();
    esp_err_t test_err = ESP_OK;
    bool connected = false;
    g_wifi_test_disconnect_reason = 0;
    g_wifi_test_active = true;
    g_wifi_test_waiting = false;
    g_wifi_test_ignore_assoc_leave = true;
    xEventGroupClearBits(g_wifi_events, WIFI_TEST_CONNECTED_BIT | WIFI_TEST_FAIL_BIT);

    (void)esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(WIFI_TEST_RESTORE_DELAY_MS));

    test_err = set_sta_config(ssid, password);
    if (test_err == ESP_OK) {
        g_connected = false;
        g_ip[0] = '\0';
        g_retry_count = 0;
        g_wifi_test_waiting = true;
        xEventGroupClearBits(g_wifi_events, WIFI_TEST_CONNECTED_BIT | WIFI_TEST_FAIL_BIT);
        test_err = esp_wifi_connect();
        if (test_err != ESP_OK) {
            g_wifi_test_ignore_assoc_leave = false;
        }
    }

    if (test_err == ESP_OK) {
        const EventBits_t bits =
            xEventGroupWaitBits(g_wifi_events, WIFI_TEST_CONNECTED_BIT | WIFI_TEST_FAIL_BIT,
                                pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_TEST_TIMEOUT_MS));
        connected = (bits & WIFI_TEST_CONNECTED_BIT) != 0;
        if (!connected) {
            test_err = (bits & WIFI_TEST_FAIL_BIT) != 0 ? ESP_FAIL : ESP_ERR_TIMEOUT;
            *reason = g_wifi_test_disconnect_reason;
        }
    }

    g_wifi_test_waiting = false;
    g_wifi_test_ignore_assoc_leave = false;
    if (connected) {
        wifi_ap_record_t ap_info = {0};
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            *rssi = ap_info.rssi;
        }
        strlcpy(ip, g_ip, ip_size);
        g_last_wifi_test_ok = true;
        strlcpy(g_last_wifi_test_ssid, ssid, sizeof(g_last_wifi_test_ssid));
        strlcpy(g_last_wifi_test_password, password != NULL ? password : "",
                sizeof(g_last_wifi_test_password));
    }

    if (g_connected) {
        (void)esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(WIFI_TEST_RESTORE_DELAY_MS));
    }

    esp_err_t restore_err = set_sta_config(saved_config.wifi_ssid, saved_config.wifi_password);
    if (restore_err != ESP_OK) {
        ESP_LOGW(TAG, "failed to restore Wi-Fi config after test: %s",
                 esp_err_to_name(restore_err));
    }
    g_retry_count = 0;
    g_wifi_test_active = false;
    if (saved_config.wifi_ssid[0] != '\0') {
        xEventGroupClearBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
        restore_err = esp_wifi_connect();
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to reconnect after Wi-Fi test: %s",
                     esp_err_to_name(restore_err));
        }
    }

    xSemaphoreGive(g_wifi_test_mutex);
    return connected ? ESP_OK : test_err;
}

static bool wifi_test_state_snapshot(wifi_test_async_state_t *snapshot)
{
    if (snapshot == NULL) {
        return false;
    }

    portENTER_CRITICAL(&g_wifi_test_state_mux);
    *snapshot = g_wifi_test_state;
    portEXIT_CRITICAL(&g_wifi_test_state_mux);
    return snapshot->running || snapshot->done;
}

static bool wifi_test_begin(uint32_t *id)
{
    bool started = false;

    portENTER_CRITICAL(&g_wifi_test_state_mux);
    if (!g_wifi_test_state.running) {
        uint32_t next_id = ++g_wifi_test_next_id;
        if (next_id == 0) {
            next_id = ++g_wifi_test_next_id;
        }
        memset(&g_wifi_test_state, 0, sizeof(g_wifi_test_state));
        g_wifi_test_state.id = next_id;
        g_wifi_test_state.running = true;
        g_wifi_test_state.err = ESP_ERR_INVALID_STATE;
        if (id != NULL) {
            *id = next_id;
        }
        started = true;
    }
    portEXIT_CRITICAL(&g_wifi_test_state_mux);

    return started;
}

static void wifi_test_finish(uint32_t id, esp_err_t err, int rssi, const char *ip,
                             uint8_t reason)
{
    portENTER_CRITICAL(&g_wifi_test_state_mux);
    if (g_wifi_test_state.id == id) {
        g_wifi_test_state.running = false;
        g_wifi_test_state.done = true;
        g_wifi_test_state.err = err;
        g_wifi_test_state.rssi = rssi;
        g_wifi_test_state.reason = reason;
        strlcpy(g_wifi_test_state.ip, ip != NULL ? ip : "", sizeof(g_wifi_test_state.ip));
    }
    portEXIT_CRITICAL(&g_wifi_test_state_mux);
}

static void wifi_test_async_task(void *arg)
{
    wifi_test_task_args_t *task_args = (wifi_test_task_args_t *)arg;
    if (task_args == NULL) {
        vTaskDelete(NULL);
        return;
    }

    int rssi = 0;
    char ip[16] = "";
    uint8_t reason = 0;
    const esp_err_t err = run_wifi_connection_test(task_args->ssid, task_args->password,
                                                   &rssi, ip, sizeof(ip), &reason);
    wifi_test_finish(task_args->id, err, rssi, ip, reason);
    free(task_args);
    vTaskDelete(NULL);
}

static esp_err_t send_wifi_test_state(httpd_req_t *req,
                                      const wifi_test_async_state_t *state)
{
    if (state == NULL || (!state->running && !state->done)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        set_no_store_headers(req);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no Wi-Fi test\"}");
    }

    char response[160];
    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    if (state->running) {
        httpd_resp_set_status(req, "202 Accepted");
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"running\":true,\"id\":%lu}",
                 (unsigned long)state->id);
        return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    }

    if (state->err != ESP_OK) {
        const char *message =
            state->err == ESP_ERR_TIMEOUT ? "connection timed out"
                                          : wifi_disconnect_reason_text(state->reason);
        httpd_resp_set_status(req, "400 Bad Request");
        snprintf(response, sizeof(response),
                 "{\"ok\":false,\"running\":false,\"id\":%lu,\"error\":\"%s\",\"reason\":%u}",
                 (unsigned long)state->id, message, state->reason);
        return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    }

    snprintf(response, sizeof(response),
             "{\"ok\":true,\"running\":false,\"id\":%lu,\"connected\":true,\"ip\":\"%s\","
             "\"rssi_dbm\":%d}",
             (unsigned long)state->id, state->ip, state->rssi);
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static void wifi_reconnect_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(750));
    const m45_config_t *config = m45_config_get();
    if (g_netif != NULL) {
        esp_netif_set_hostname(g_netif, config->hostname);
    }
    if (g_setup_ap_active) {
        esp_wifi_set_mode(WIFI_MODE_APSTA);
    }
    esp_err_t err = set_sta_config(config->wifi_ssid, config->wifi_password);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to apply Wi-Fi config: %s", esp_err_to_name(err));
    }
    esp_wifi_disconnect();
    if (config->wifi_ssid[0] != '\0') {
        g_retry_count = 0;
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void schedule_wifi_reconnect(void)
{
    xTaskCreate(wifi_reconnect_task, "wifi_reconn", 3072, NULL, tskIDLE_PRIORITY + 1, NULL);
}

static void wifi_retry_backoff_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_BACKOFF_MS));
    g_wifi_retry_backoff_pending = false;

    if (g_wifi_test_active || g_connected || m45_config_get()->wifi_ssid[0] == '\0') {
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGW(TAG, "Wi-Fi still disconnected; retrying after backoff");
    g_retry_count = 0;
    esp_wifi_connect();
    vTaskDelete(NULL);
}

static void schedule_wifi_retry_backoff(void)
{
    if (g_wifi_retry_backoff_pending) {
        return;
    }

    g_wifi_retry_backoff_pending = true;
    if (xTaskCreate(wifi_retry_backoff_task, "wifi_retry", 3072, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        g_wifi_retry_backoff_pending = false;
        ESP_LOGW(TAG, "failed to schedule Wi-Fi retry backoff; retrying immediately");
        esp_wifi_connect();
    }
}

static void json_escape(char *dst, size_t dst_size, const char *src)
{
    if (dst_size == 0) {
        return;
    }

    size_t out = 0;
    for (size_t i = 0; src != NULL && src[i] != '\0' && out + 1 < dst_size; ++i) {
        const char ch = src[i];
        if ((ch == '"' || ch == '\\') && out + 2 < dst_size) {
            dst[out++] = '\\';
            dst[out++] = ch;
        } else if ((unsigned char)ch >= 0x20) {
            dst[out++] = ch;
        }
    }
    dst[out] = '\0';
}

static bool json_get_string(cJSON *root, const char *name, char *dst, size_t dst_size,
                            bool required)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) {
        return !required;
    }
    if (!cJSON_IsString(item) || item->valuestring == NULL ||
        strlen(item->valuestring) >= dst_size) {
        return false;
    }
    strlcpy(dst, item->valuestring, dst_size);
    return true;
}

static bool json_get_u16(cJSON *root, const char *name, uint16_t *dst, int min, int max)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        return false;
    }
    *dst = (uint16_t)item->valueint;
    return true;
}

static bool json_get_bool(cJSON *root, const char *name, bool *dst)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *dst = cJSON_IsTrue(item);
    return true;
}

static bool json_get_optional_bool(cJSON *root, const char *name, bool *dst)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsBool(item)) {
        return false;
    }
    *dst = cJSON_IsTrue(item);
    return true;
}

static bool json_get_optional_u16(cJSON *root, const char *name, uint16_t *dst, int min, int max)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        return false;
    }
    *dst = (uint16_t)item->valueint;
    return true;
}

static uint64_t logs_since_from_query(httpd_req_t *req)
{
    char query[48];
    char since_text[24];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "since", since_text, sizeof(since_text)) != ESP_OK) {
        return 0;
    }

    char *end = NULL;
    const uint64_t since = strtoull(since_text, &end, 10);
    return end != since_text ? since : 0;
}

static bool logs_verbose_from_query(httpd_req_t *req)
{
    char query[64];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "verbose", value, sizeof(value)) != ESP_OK) {
        return false;
    }

    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

static bool line_contains_text(const char *line, size_t line_len, const char *needle)
{
    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || line_len < needle_len) {
        return false;
    }

    for (size_t i = 0; i + needle_len <= line_len; ++i) {
        if (memcmp(line + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool log_line_is_verbose(const char *line, size_t line_len)
{
    return line_len >= 3 && line[0] == 'V' && line[1] == ' ' && line[2] == '(';
}

static size_t filter_normal_logs(char *body, size_t body_len)
{
    size_t read_pos = 0;
    size_t write_pos = 0;

    while (read_pos < body_len) {
        const size_t line_start = read_pos;
        while (read_pos < body_len && body[read_pos] != '\n') {
            ++read_pos;
        }
        if (read_pos < body_len && body[read_pos] == '\n') {
            ++read_pos;
        }

        const size_t line_len = read_pos - line_start;
        if (log_line_is_verbose(body + line_start, line_len) ||
            line_contains_text(body + line_start, line_len, "share accepted")) {
            continue;
        }
        if (write_pos != line_start) {
            memmove(body + write_pos, body + line_start, line_len);
        }
        write_pos += line_len;
    }

    body[write_pos] = '\0';
    return write_pos;
}

static bool settings_string_changed(const char *a, const char *b)
{
    return strcmp(a != NULL ? a : "", b != NULL ? b : "") != 0;
}

static bool wifi_credentials_changed(const m45_config_t *old_config,
                                     const m45_config_t *new_config)
{
    return settings_string_changed(old_config->wifi_ssid, new_config->wifi_ssid) ||
           settings_string_changed(old_config->wifi_password, new_config->wifi_password);
}

static uint16_t suggested_pool_difficulty_for_config(const m45_config_t *config)
{
    if (config == NULL || g_state == NULL) {
        return config != NULL ? config->pool_difficulty : 1;
    }
    return m45_config_effective_pool_difficulty(
        config, g_state->DEVICE_CONFIG.family.asic.small_core_count,
        g_state->DEVICE_CONFIG.family.asic_count);
}

static bool active_pool_settings_changed(const m45_config_t *old_config,
                                         const m45_config_t *new_config)
{
    stratum_minimal_stats_t stats;
    stratum_minimal_get_stats(&stats);

    if (settings_string_changed(old_config->pool_user, new_config->pool_user) ||
        settings_string_changed(old_config->pool_pass, new_config->pool_pass)) {
        return true;
    }

    if (stats.using_backup_pool) {
        return settings_string_changed(old_config->backup_pool_host,
                                       new_config->backup_pool_host) ||
               old_config->backup_pool_port != new_config->backup_pool_port;
    }

    return settings_string_changed(old_config->pool_host, new_config->pool_host) ||
           old_config->pool_port != new_config->pool_port;
}

static void runtime_reconnect_flags(const m45_config_t *old_config,
                                    const m45_config_t *new_config,
                                    bool *wifi_reconnect,
                                    bool *pool_reconnect)
{
    *wifi_reconnect = wifi_credentials_changed(old_config, new_config) ||
                      settings_string_changed(old_config->hostname, new_config->hostname);
    *pool_reconnect = active_pool_settings_changed(old_config, new_config);
}

static void apply_runtime_state(const m45_config_t *config)
{
    g_state->pool_difficulty = suggested_pool_difficulty_for_config(config);
    g_state->SYSTEM_MODULE.pool_user = (char *)config->pool_user;
    g_state->SYSTEM_MODULE.pool_pass = (char *)config->pool_pass;
}

static esp_err_t apply_hardware_settings(const m45_config_t *old_config,
                                         const m45_config_t *new_config)
{
    const float asic_temp_c = g_state != NULL ? g_state->POWER_MANAGEMENT_MODULE.chip_temp_avg
                                              : 0.0f;
    const uint16_t old_voltage_mv =
        m45_config_effective_asic_voltage_mv_for_temp(old_config, asic_temp_c);
    const uint16_t new_voltage_mv =
        m45_config_effective_asic_voltage_mv_for_temp(new_config, asic_temp_c);
    const uint16_t old_frequency_mhz = m45_config_effective_asic_frequency_mhz(old_config);
    const uint16_t new_frequency_mhz = m45_config_effective_asic_frequency_mhz(new_config);
    if (old_voltage_mv != new_voltage_mv) {
        const uint64_t started_us = http_now_us();
        esp_err_t err = bitaxe_gamma602_set_voltage_mv(g_state, new_voltage_mv);
        log_http_handler_delay("ASIC voltage apply", started_us);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (old_frequency_mhz != new_frequency_mhz) {
        const uint64_t started_us = http_now_us();
        stratum_minimal_pause_work();
        esp_err_t err = bitaxe_gamma602_set_frequency_mhz(g_state, new_frequency_mhz);
        stratum_minimal_resume_work();
        log_http_handler_delay("ASIC frequency apply", started_us);
        if (err != ESP_OK) {
            return err;
        }
    }
    if (old_config->fan_override_enabled != new_config->fan_override_enabled ||
        old_config->fan_override_percent != new_config->fan_override_percent) {
        const uint64_t started_us = http_now_us();
        esp_err_t err = bitaxe_fan_apply_config(g_state, new_config);
        log_http_handler_delay("fan override apply", started_us);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void apply_runtime_reconnects(bool wifi_reconnect, bool pool_reconnect)
{
    if (pool_reconnect) {
        stratum_minimal_reconnect();
    }
    if (wifi_reconnect) {
        schedule_wifi_reconnect();
    }
}

static esp_err_t apply_runtime_settings(const m45_config_t *old_config,
                                        const m45_config_t *new_config,
                                        bool *wifi_reconnect,
                                        bool *pool_reconnect)
{
    runtime_reconnect_flags(old_config, new_config, wifi_reconnect, pool_reconnect);

    esp_err_t err = apply_hardware_settings(old_config, new_config);
    if (err != ESP_OK) {
        return err;
    }

    apply_runtime_state(new_config);
    apply_runtime_reconnects(*wifi_reconnect, *pool_reconnect);
    return ESP_OK;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id,
                               void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START &&
        !g_wifi_test_active && m45_config_get()->wifi_ssid[0] != '\0') {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *event =
            (const wifi_event_sta_disconnected_t *)event_data;
        if (g_wifi_test_active) {
            const uint8_t reason = event != NULL ? event->reason : 0;
            g_connected = false;
            g_ip[0] = '\0';
            if (g_wifi_test_ignore_assoc_leave && reason == WIFI_REASON_ASSOC_LEAVE) {
                g_wifi_test_ignore_assoc_leave = false;
                return;
            }
            g_wifi_test_ignore_assoc_leave = false;
            if (g_wifi_test_waiting) {
                g_wifi_test_disconnect_reason = reason;
                xEventGroupSetBits(g_wifi_events, WIFI_TEST_FAIL_BIT);
            }
            return;
        }
        const bool has_wifi_config = m45_config_get()->wifi_ssid[0] != '\0';
        g_connected = false;
        g_ip[0] = '\0';
        if (has_wifi_config && g_retry_count < WIFI_MAX_RETRY) {
            ++g_retry_count;
            esp_wifi_connect();
            ESP_LOGW(TAG, "Wi-Fi disconnected, retry %d/%d", g_retry_count, WIFI_MAX_RETRY);
        } else if (has_wifi_config) {
            xEventGroupSetBits(g_wifi_events, WIFI_FAIL_BIT);
            schedule_wifi_retry_backoff();
            ESP_LOGW(TAG, "Wi-Fi disconnected after %d retries; backing off for %d ms",
                     g_retry_count, WIFI_RETRY_BACKOFF_MS);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
        snprintf(g_ip, sizeof(g_ip), IPSTR, IP2STR(&event->ip_info.ip));
        g_connected = true;
        g_retry_count = 0;
        g_wifi_test_ignore_assoc_leave = false;
        xEventGroupSetBits(g_wifi_events,
                           g_wifi_test_active && g_wifi_test_waiting
                               ? WIFI_TEST_CONNECTED_BIT
                               : WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "Wi-Fi connected: %s", g_ip);
    }
}

static esp_err_t root_handler(httpd_req_t *req)
{
    if (g_setup_ap_active) {
        return send_gzip_page(req, WEB_SETUP_HTML_GZ, WEB_SETUP_HTML_GZ_LEN);
    }

    return send_gzip_page(req, WEB_ROOT_HTML_GZ, WEB_ROOT_HTML_GZ_LEN);
}

static esp_err_t setup_page_handler(httpd_req_t *req)
{
    return send_gzip_page(req, WEB_SETUP_HTML_GZ, WEB_SETUP_HTML_GZ_LEN);
}

static esp_err_t styles_css_handler(httpd_req_t *req)
{
    return send_gzip_asset(req, "text/css; charset=utf-8", WEB_STYLES_CSS_GZ,
                           WEB_STYLES_CSS_GZ_LEN);
}

static esp_err_t captive_portal_redirect_handler(httpd_req_t *req)
{
    char location[64];
    if (g_setup_ap_active) {
        snprintf(location, sizeof(location), "http://%s/wifi", g_setup_ip);
    } else {
        strlcpy(location, "/", sizeof(location));
    }
    httpd_resp_set_status(req, "302 Temporary Redirect");
    httpd_resp_set_hdr(req, "Location", location);
    set_no_store_headers(req);
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_sendstr(req, "<html><body>Redirect to the captive portal</body></html>");
}

static esp_err_t redirect_handler(httpd_req_t *req)
{
    return captive_portal_redirect_handler(req);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    if (request_has_bad_page_token(req)) {
        return send_page_token_reload(req);
    }

    const m45_config_t *config = m45_config_get();
    stratum_minimal_stats_t stats;
    bitaxe_gamma602_power_snapshot_t power;
    bitaxe_gamma602_safety_limits_t limits;
    stratum_minimal_get_stats(&stats);
    const bool have_power = bitaxe_gamma602_power_snapshot(&power);
    const float asic_power_watts = have_power ? power.read_vout * power.read_iout : 0.0f;
    bitaxe_gamma602_safety_limits(&limits);
    wifi_ap_record_t ap_info = {0};
    const int wifi_rssi = g_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK
                              ? ap_info.rssi
                              : 0;
    const uint8_t chip_count = bitaxe_gamma602_chip_count();
    const float active_frequency_mhz = g_state->POWER_MANAGEMENT_MODULE.actual_frequency > 0.0f
                                           ? g_state->POWER_MANAGEMENT_MODULE.actual_frequency
                                           : (float)m45_config_effective_asic_frequency_mhz(config);
    const uint8_t expected_chip_count =
        chip_count > 0 ? chip_count : g_state->DEVICE_CONFIG.family.asic_count;
    const double expected_hashrate_ghs =
        (double)active_frequency_mhz *
        (double)g_state->DEVICE_CONFIG.family.asic.small_core_count *
        (double)expected_chip_count / 1000.0;
    const uint16_t suggested_pool_difficulty =
        suggested_pool_difficulty_for_config(config);
    const char *hardware_status = bitaxe_gamma602_status();
    const bool asic_power_enabled = bitaxe_gamma602_asic_power_enabled();
    const bool asic_power_off = !asic_power_enabled && strcmp(hardware_status, "asic off") == 0;
    const bool booting = !asic_power_off && !g_state->ASIC_initalized &&
                         !g_state->SYSTEM_MODULE.hardware_fault &&
                         strcmp(hardware_status, "ready") != 0;
    const bool fan_auto = !config->fan_override_enabled;
    const uint16_t asic_temp_target_c =
        fan_auto ? m45_config_effective_fan_target_temp_c(config) : M45_FAN_TARGET_DEFAULT_C;
    const float asic_temp_c = g_state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
    const uint16_t voltage_base_mv = m45_config_effective_asic_voltage_mv(config);
    const uint16_t voltage_compensation_mv =
        m45_config_asic_voltage_temp_compensation_mv(config, asic_temp_c);
    const uint16_t voltage_target_mv =
        m45_config_effective_asic_voltage_mv_for_temp(config, asic_temp_c);

    char wifi_ssid[80];
    char pool_host[160];
    char hardware_fault_msg[96];
    char tps546_model[24];
    char domain_hashrates_json[512];
#ifdef M45_ASIC_LOSS_METRICS
    char asic_loss_json[960];
#endif
    json_escape(wifi_ssid, sizeof(wifi_ssid), config->wifi_ssid);
    json_escape(pool_host, sizeof(pool_host),
                stats.pool_host[0] != '\0' ? stats.pool_host : config->pool_host);
    json_escape(hardware_fault_msg, sizeof(hardware_fault_msg),
                g_state->SYSTEM_MODULE.hardware_fault ? g_state->SYSTEM_MODULE.hardware_fault_msg
                                                       : "");
    json_escape(tps546_model, sizeof(tps546_model), bitaxe_gamma602_tps_model());
    format_domain_hashrates_json(&stats, expected_chip_count,
                                  domain_hashrates_json,
                                  sizeof(domain_hashrates_json));
#ifdef M45_ASIC_LOSS_METRICS
    snprintf(asic_loss_json, sizeof(asic_loss_json),
             "{"
             "\"job_sent\":%" PRIu64 ","
             "\"job_send_skipped\":%" PRIu64 ","
             "\"job_alloc_failed\":%" PRIu64 ","
             "\"job_build_total_us\":%" PRIu64 ","
             "\"job_build_max_us\":%" PRIu64 ","
             "\"job_send_total_us\":%" PRIu64 ","
             "\"job_send_max_us\":%" PRIu64 ","
             "\"dispatch_late_count\":%" PRIu64 ","
             "\"dispatch_late_total_us\":%" PRIu64 ","
             "\"dispatch_late_max_us\":%" PRIu64 ","
             "\"dispatch_missed_slots\":%" PRIu64 ","
             "\"rx_calls\":%" PRIu64 ","
             "\"rx_null\":%" PRIu64 ","
             "\"rx_timeouts\":%" PRIu64 ","
             "\"rx_wait_total_us\":%" PRIu64 ","
             "\"rx_wait_max_us\":%" PRIu64 ","
             "\"rx_nonce_results\":%" PRIu64 ","
             "\"rx_register_results\":%" PRIu64 ","
             "\"invalid_job_nonces\":%" PRIu64
             "}",
             stats.asic_loss.job_sent, stats.asic_loss.job_send_skipped,
             stats.asic_loss.job_alloc_failed, stats.asic_loss.job_build_total_us,
             stats.asic_loss.job_build_max_us, stats.asic_loss.job_send_total_us,
             stats.asic_loss.job_send_max_us, stats.asic_loss.dispatch_late_count,
             stats.asic_loss.dispatch_late_total_us, stats.asic_loss.dispatch_late_max_us,
             stats.asic_loss.dispatch_missed_slots, stats.asic_loss.rx_calls,
             stats.asic_loss.rx_null, stats.asic_loss.rx_timeouts,
             stats.asic_loss.rx_wait_total_us, stats.asic_loss.rx_wait_max_us,
             stats.asic_loss.rx_nonce_results, stats.asic_loss.rx_register_results,
             stats.asic_loss.invalid_job_nonces);
#endif

    char *body = malloc(STATUS_JSON_BUFFER_SIZE);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    const int body_len =
        snprintf(body, STATUS_JSON_BUFFER_SIZE,
                 "{"
                 "\"page_token\":\"%s\","
                 "\"device_name\":\"%s\","
                 "\"build_id\":\"%s\","
                 "\"build_time\":\"%s\","
                 "\"wifi_connected\":%s,"
                 "\"ip\":\"%s\","
                 "\"wifi_rssi_dbm\":%d,"
                 "\"hardware\":\"%s\","
                 "\"booting\":%s,"
                 "\"setup_mode\":%s,"
                 "\"setup_ssid\":\"%s\","
                 "\"setup_ip\":\"%s\","
                 "\"asic_ready\":%s,"
                 "\"asic_power_enabled\":%s,"
                 "\"model\":\"%s %s\","
                 "\"asic_model\":\"%s\","
                 "\"asic_chips\":%u,"
                 "\"frequency_mhz\":%.0f,"
                 "\"hashrate_ghs\":%.2f,"
                 "\"hashrate_nominal_ghs\":%.2f,"
                 "\"domain_hashrate_ghs\":%.2f,"
                 "\"domain_hashrates_ghs\":%s,"
                 "\"asic_error_rate_percent\":%.2f,"
                 "\"expected_hashrate_ghs\":%.2f,"
                 "\"wifi_ssid\":\"%s\","
                 "\"voltage_mv\":%u,"
                 "\"voltage_base_mv\":%u,"
                 "\"voltage_temp_compensation_enabled\":%s,"
                 "\"voltage_temp_compensation_mv\":%u,"
                 "\"overclock_enabled\":%s,"
                 "\"asic_temp_c\":%.1f,"
                 "\"fan_percent\":%.1f,"
                 "\"fan_rpm\":%u,"
                 "\"fan_auto\":%s,"
                 "\"fan_auto_off_allowed\":%s,"
                 "\"fan_target_temp_c\":%u,"
                 "\"tps546_valid\":%s,"
                 "\"tps546_read_vout\":%.3f,"
                 "\"tps546_read_vin\":%.3f,"
                 "\"tps546_read_iout\":%.3f,"
                 "\"tps546_temp_c\":%d,"
                 "\"tps546_model\":\"%s\","
                 "\"asic_power_watts\":%.2f,"
                 "\"power_fault\":%u,"
                 "\"hardware_fault\":%s,"
                 "\"hardware_fault_msg\":\"%s\","
                 "\"pool\":\"%s\","
                 "\"pool_port\":%u,"
                 "\"pool_using_backup\":%s,"
                 "\"stratum_connected\":%s,"
                 "\"stratum_connected_seconds\":%lu,"
                 "\"stratum_response_ms\":%lu,"
                 "\"work_received\":%lu,"
                 "\"shares_accepted\":%lu,"
                 "\"shares_rejected\":%lu,"
                 "\"valid_nonces\":%lu,"
                 "\"nonce_errors\":%lu,"
                 "\"best_diff\":%.2f,"
                 "\"pool_difficulty\":%.2f,"
                 "\"pool_difficulty_auto\":%s,"
                 "\"pool_suggested_difficulty\":%u,"
                 "\"payout_status\":\"%s\","
                 "\"payout_percent_x100\":%u,"
                 "\"block_alert_active\":%s,"
                 "\"block_alert_diff\":%.2f,"
#ifdef M45_ASIC_LOSS_METRICS
                 "\"asic_loss\":%s,"
#endif
                 "\"limits\":{"
                 "\"input_voltage_min_v\":%.3f,"
                 "\"input_voltage_expected_min_v\":%.3f,"
                 "\"input_voltage_expected_max_v\":%.3f,"
                 "\"input_voltage_max_v\":%.3f,"
                 "\"asic_voltage_min_v\":%.3f,"
                 "\"asic_voltage_expected_min_v\":%.3f,"
                 "\"asic_voltage_expected_max_v\":%.3f,"
                 "\"asic_voltage_max_v\":%.3f,"
                 "\"asic_voltage_target_v\":%.3f,"
                 "\"asic_temp_expected_max_c\":%.1f,"
                 "\"asic_temp_max_c\":%.1f,"
                 "\"tps546_temp_expected_max_c\":%.1f,"
                 "\"tps546_temp_max_c\":%.1f,"
                 "\"iout_warn_a\":%.1f,"
                 "\"iout_fault_a\":%.1f,"
                 "\"power_warn_w\":%.2f,"
                 "\"power_fault_w\":%.2f,"
                 "\"fan_expected_percent\":%.0f"
                 "}"
                 "}",
                 g_page_token, M45_DEVICE_NAME, APP_BUILD_ID, APP_BUILD_TIME_UTC,
                 g_connected ? "true" : "false", g_ip, wifi_rssi,
                 hardware_status, booting ? "true" : "false",
                 g_setup_ap_active ? "true" : "false", g_setup_ssid, g_setup_ip,
                 g_state->ASIC_initalized ? "true" : "false",
                 asic_power_enabled ? "true" : "false",
                 g_state->DEVICE_CONFIG.family.name, g_state->DEVICE_CONFIG.board_version,
                 g_state->DEVICE_CONFIG.family.asic.name, chip_count,
                 g_state->POWER_MANAGEMENT_MODULE.actual_frequency,
                 stats.measured_hashrate_ghs, stats.nominal_hashrate_ghs,
                 stats.domain_hashrate_ghs, domain_hashrates_json,
                 stats.asic_error_rate_percent,
                 expected_hashrate_ghs, wifi_ssid,
                 voltage_target_mv, voltage_base_mv,
                 config->asic_voltage_temp_compensation_enabled ? "true" : "false",
                 voltage_compensation_mv,
                 config->overclock_enabled ? "true" : "false",
                 asic_temp_c,
                 g_state->POWER_MANAGEMENT_MODULE.fan_perc,
                 g_state->POWER_MANAGEMENT_MODULE.fan_rpm,
                 fan_auto ? "true" : "false",
                 config->fan_auto_off_allowed ? "true" : "false", asic_temp_target_c,
                 have_power ? "true" : "false", have_power ? power.read_vout : 0.0f,
                 have_power ? power.read_vin : 0.0f, have_power ? power.read_iout : 0.0f,
                 have_power ? power.read_temp_c : 0, tps546_model, asic_power_watts,
                 g_state->SYSTEM_MODULE.power_fault,
                 g_state->SYSTEM_MODULE.hardware_fault ? "true" : "false", hardware_fault_msg,
                 pool_host, stats.pool_port > 0 ? stats.pool_port : config->pool_port,
                 stats.using_backup_pool ? "true" : "false",
                 stats.connected ? "true" : "false",
                 (unsigned long)stats.connected_seconds, (unsigned long)stats.response_time_ms,
                 (unsigned long)stats.work_received,
                 (unsigned long)stats.accepted, (unsigned long)stats.rejected,
                 (unsigned long)stats.valid_nonces, (unsigned long)stats.nonce_errors,
                 stats.best_diff, stats.pool_diff,
                 config->pool_difficulty_auto ? "true" : "false",
                 suggested_pool_difficulty, payout_status_name(stats.payout_status),
                 stats.payout_percent_x100,
                 stats.block_alert_active ? "true" : "false", stats.block_alert_diff,
#ifdef M45_ASIC_LOSS_METRICS
                 asic_loss_json,
#endif
                 limits.input_voltage_min_v,
                 limits.input_voltage_expected_min_v, limits.input_voltage_expected_max_v,
                 limits.input_voltage_max_v, limits.asic_voltage_min_v,
                 limits.asic_voltage_expected_min_v, limits.asic_voltage_expected_max_v,
                 limits.asic_voltage_max_v, limits.asic_voltage_target_v,
                 limits.asic_temp_expected_max_c, limits.asic_temp_max_c,
                 limits.tps546_temp_expected_max_c, limits.tps546_temp_max_c,
                 limits.iout_warn_a, limits.iout_fault_a, limits.power_warn_w,
                 limits.power_fault_w, limits.fan_expected_percent);
    if (body_len < 0 || body_len >= STATUS_JSON_BUFFER_SIZE) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    const esp_err_t err = httpd_resp_send(req, body, body_len);
    free(body);
    return err;
}

static bool json_get_optional_i16(cJSON *root, const char *name, int16_t *dst, int min, int max)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        return false;
    }
    *dst = (int16_t)item->valueint;
    return true;
}

static bool safety_settings_valid_for_tune(const m45_config_t *config)
{
    if (config->safety_input_voltage_min_mv >= config->safety_input_voltage_max_mv ||
        config->safety_input_voltage_expected_min_mv >=
            config->safety_input_voltage_expected_max_mv ||
        config->safety_input_voltage_expected_min_mv <
            config->safety_input_voltage_min_mv ||
        config->safety_input_voltage_expected_max_mv >
            config->safety_input_voltage_max_mv ||
        config->safety_asic_voltage_min_mv >= config->safety_asic_voltage_max_mv ||
        config->safety_asic_temp_expected_max_c > config->safety_asic_temp_max_c ||
        config->safety_tps546_temp_expected_max_c > config->safety_tps546_temp_max_c ||
        config->safety_iout_warn_deciamps > config->safety_iout_fault_deciamps) {
        return false;
    }

    const uint16_t effective_voltage_mv = m45_config_effective_asic_voltage_mv(config);
    return effective_voltage_mv >= config->safety_asic_voltage_min_mv &&
           effective_voltage_mv < config->safety_asic_voltage_max_mv;
}

static esp_err_t settings_get_handler(httpd_req_t *req)
{
    const m45_config_t *config = m45_config_get();
    char wifi_ssid[80];
    char hostname[80];
    char pool_host[160];
    char backup_pool_host[160];
    char pool_user[200];
    json_escape(wifi_ssid, sizeof(wifi_ssid), config->wifi_ssid);
    json_escape(hostname, sizeof(hostname), config->hostname);
    json_escape(pool_host, sizeof(pool_host), config->pool_host);
    json_escape(backup_pool_host, sizeof(backup_pool_host), config->backup_pool_host);
    json_escape(pool_user, sizeof(pool_user), config->pool_user);
    const uint16_t suggested_pool_difficulty =
        suggested_pool_difficulty_for_config(config);

    char *body = malloc(SETTINGS_JSON_BUFFER_SIZE);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    const int body_len =
        snprintf(body, SETTINGS_JSON_BUFFER_SIZE,
                 "{"
                 "\"wifi_ssid\":\"%s\","
                 "\"hostname\":\"%s\","
                 "\"pool_host\":\"%s\","
                 "\"pool_port\":%u,"
                 "\"backup_pool_host\":\"%s\","
                 "\"backup_pool_port\":%u,"
                 "\"pool_user\":\"%s\","
                 "\"wifi_password_set\":%s,"
                 "\"pool_password_set\":%s,"
                 "\"pool_difficulty\":%u,"
                 "\"pool_difficulty_auto\":%s,"
                 "\"pool_suggested_difficulty\":%u,"
                 "\"overclock_enabled\":%s,"
                 "\"asic_frequency_mhz\":%u,"
                 "\"asic_voltage_mv\":%u,"
                 "\"overclock_voltage_offset_mv\":%d,"
                 "\"asic_voltage_temp_compensation_enabled\":%s,"
                 "\"fan_override_enabled\":%s,"
                 "\"fan_override_percent\":%u,"
                 "\"fan_auto_off_allowed\":%s,"
                 "\"fan_target_override_enabled\":%s,"
                 "\"fan_target_temp_c\":%u,"
                 "\"display_screensaver_enabled\":%s,"
                 "\"display_sleep_minutes\":%u,"
                 "\"display_sleep_max_minutes\":%u,"
                 "\"limit_input_voltage_min_mv\":%u,"
                 "\"limit_input_voltage_expected_min_mv\":%u,"
                 "\"limit_input_voltage_expected_max_mv\":%u,"
                 "\"limit_input_voltage_max_mv\":%u,"
                 "\"limit_asic_voltage_min_mv\":%u,"
                 "\"limit_asic_voltage_max_mv\":%u,"
                 "\"limit_asic_temp_expected_max_c\":%u,"
                 "\"limit_asic_temp_max_c\":%u,"
                 "\"limit_tps546_temp_expected_max_c\":%u,"
                 "\"limit_tps546_temp_max_c\":%u,"
                 "\"limit_iout_warn_deciamps\":%u,"
                 "\"limit_iout_fault_deciamps\":%u"
                 "}",
                 wifi_ssid, hostname, pool_host, config->pool_port, backup_pool_host,
                 config->backup_pool_port, pool_user,
                 config->wifi_password[0] != '\0' ? "true" : "false",
                 config->pool_pass[0] != '\0' ? "true" : "false", config->pool_difficulty,
                 config->pool_difficulty_auto ? "true" : "false",
                 suggested_pool_difficulty, config->overclock_enabled ? "true" : "false",
                 config->asic_frequency_mhz, config->asic_voltage_mv,
                 config->overclock_voltage_offset_mv,
                 config->asic_voltage_temp_compensation_enabled ? "true" : "false",
                 config->fan_override_enabled ? "true" : "false",
                 config->fan_override_percent,
                 config->fan_auto_off_allowed ? "true" : "false",
                 config->fan_target_override_enabled ? "true" : "false",
                 config->fan_target_temp_c,
                 config->display_screensaver_enabled ? "true" : "false",
                 (unsigned)config->display_sleep_minutes,
                 (unsigned)M45_DISPLAY_SLEEP_MAX_MINUTES,
                 config->safety_input_voltage_min_mv,
                 config->safety_input_voltage_expected_min_mv,
                 config->safety_input_voltage_expected_max_mv,
                 config->safety_input_voltage_max_mv,
                 config->safety_asic_voltage_min_mv,
                 config->safety_asic_voltage_max_mv,
                 config->safety_asic_temp_expected_max_c,
                 config->safety_asic_temp_max_c,
                 config->safety_tps546_temp_expected_max_c,
                 config->safety_tps546_temp_max_c,
                 config->safety_iout_warn_deciamps,
                 config->safety_iout_fault_deciamps);
    if (body_len < 0 || body_len >= SETTINGS_JSON_BUFFER_SIZE) {
        free(body);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"settings too large\"}");
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    const esp_err_t err = httpd_resp_send(req, body, body_len);
    free(body);
    return err;
}

static esp_err_t setup_get_handler(httpd_req_t *req)
{
    const m45_config_t *config = m45_config_get();
    char wifi_ssid[80];
    char pool_host[160];
    char pool_user[200];
    json_escape(wifi_ssid, sizeof(wifi_ssid), config->wifi_ssid);
    json_escape(pool_host, sizeof(pool_host), config->pool_host);
    json_escape(pool_user, sizeof(pool_user), config->pool_user);
    const uint16_t suggested_pool_difficulty =
        suggested_pool_difficulty_for_config(config);

    char body[896];
    const int body_len =
        snprintf(body, sizeof(body),
                 "{"
                 "\"setup_mode\":%s,"
                 "\"setup_ssid\":\"%s\","
                 "\"setup_ip\":\"%s\","
                 "\"wifi_ssid\":\"%s\","
                 "\"pool_host\":\"%s\","
                 "\"pool_port\":%u,"
                 "\"pool_user\":\"%s\","
                 "\"pool_password_set\":%s,"
                 "\"pool_difficulty\":%u,"
                 "\"pool_difficulty_auto\":%s,"
                 "\"pool_suggested_difficulty\":%u"
                 "}",
                 g_setup_ap_active ? "true" : "false", g_setup_ssid, g_setup_ip,
                 wifi_ssid, pool_host, config->pool_port, pool_user,
                 config->pool_pass[0] != '\0' ? "true" : "false",
                 config->pool_difficulty,
                 config->pool_difficulty_auto ? "true" : "false",
                 suggested_pool_difficulty);
    if (body_len < 0 || body_len >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"setup too large\"}");
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    return httpd_resp_send(req, body, body_len);
}

static bool networks_refresh_requested(httpd_req_t *req)
{
    char query[32];
    char value[8];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "refresh", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0;
}

static void wifi_scan_task(void *arg)
{
    (void)arg;
    wifi_scan_config_t scan_config = {
        .show_hidden = false,
    };
    wifi_ap_record_t records[SETUP_SCAN_MAX_APS] = {0};
    uint16_t ap_count = SETUP_SCAN_MAX_APS;
    esp_err_t err = esp_wifi_scan_start(&scan_config, true);
    if (err == ESP_OK) {
        err = esp_wifi_scan_get_ap_records(&ap_count, records);
    } else {
        ap_count = 0;
    }

    if (g_wifi_scan_mutex != NULL &&
        xSemaphoreTake(g_wifi_scan_mutex, portMAX_DELAY) == pdTRUE) {
        if (ap_count > SETUP_SCAN_MAX_APS) {
            ap_count = SETUP_SCAN_MAX_APS;
        }
        g_wifi_scan_running = false;
        g_wifi_scan_err = err;
        g_wifi_scan_count = err == ESP_OK ? ap_count : 0;
        g_wifi_scan_valid = err == ESP_OK;
        if (err == ESP_OK) {
            memcpy(g_wifi_scan_records, records,
                   (size_t)g_wifi_scan_count * sizeof(g_wifi_scan_records[0]));
        }
        xSemaphoreGive(g_wifi_scan_mutex);
    }

    vTaskDelete(NULL);
}

static esp_err_t start_wifi_scan_async(void)
{
    if (g_wifi_scan_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (xSemaphoreTake(g_wifi_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_wifi_scan_running) {
        xSemaphoreGive(g_wifi_scan_mutex);
        return ESP_OK;
    }
    g_wifi_scan_running = true;
    g_wifi_scan_valid = false;
    g_wifi_scan_err = ESP_ERR_INVALID_STATE;
    g_wifi_scan_count = 0;
    xSemaphoreGive(g_wifi_scan_mutex);

    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, NULL, tskIDLE_PRIORITY + 1,
                    NULL) == pdPASS) {
        return ESP_OK;
    }

    if (xSemaphoreTake(g_wifi_scan_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        g_wifi_scan_running = false;
        g_wifi_scan_err = ESP_ERR_NO_MEM;
        xSemaphoreGive(g_wifi_scan_mutex);
    }
    return ESP_ERR_NO_MEM;
}

static esp_err_t networks_handler(httpd_req_t *req)
{
    const bool refresh = networks_refresh_requested(req);
    bool running = false;
    bool valid = false;
    esp_err_t err = ESP_OK;
    uint16_t ap_count = 0;
    wifi_ap_record_t records[SETUP_SCAN_MAX_APS] = {0};

    if (g_wifi_scan_mutex == NULL ||
        xSemaphoreTake(g_wifi_scan_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"scan unavailable\"}");
    }
    running = g_wifi_scan_running;
    valid = g_wifi_scan_valid;
    err = g_wifi_scan_err;
    ap_count = g_wifi_scan_count;
    if (ap_count > SETUP_SCAN_MAX_APS) {
        ap_count = SETUP_SCAN_MAX_APS;
    }
    memcpy(records, g_wifi_scan_records, (size_t)ap_count * sizeof(records[0]));
    xSemaphoreGive(g_wifi_scan_mutex);

    if (refresh || (!running && !valid)) {
        const uint64_t scan_started_us = http_now_us();
        err = start_wifi_scan_async();
        log_http_handler_delay("Wi-Fi network scan start", scan_started_us);
        if (err != ESP_OK) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            char error_body[80];
            snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}",
                     esp_err_to_name(err));
            return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
        }
        running = true;
        valid = false;
    }

    if (running) {
        httpd_resp_set_type(req, "application/json");
        set_no_store_headers(req);
        return httpd_resp_sendstr(req, "{\"scanning\":true,\"networks\":[]}");
    }

    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    char body[2048];
    size_t offset = 0;
    int written = snprintf(body, sizeof(body), "{\"scanning\":false,\"networks\":[");
    if (written < 0 || written >= (int)sizeof(body)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"networks too large\"}");
    }
    offset = (size_t)written;

    bool first = true;
    for (uint16_t i = 0; i < ap_count; ++i) {
        if (records[i].ssid[0] == '\0') {
            continue;
        }
        char ssid[80];
        json_escape(ssid, sizeof(ssid), (const char *)records[i].ssid);
        written = snprintf(body + offset, sizeof(body) - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"channel\":%u,"
                           "\"auth_open\":%s}",
                           first ? "" : ",", ssid, records[i].rssi,
                           (unsigned)records[i].primary,
                           records[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        if (written < 0 || written >= (int)(sizeof(body) - offset)) {
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"networks too large\"}");
        }
        offset += (size_t)written;
        first = false;
    }

    written = snprintf(body + offset, sizeof(body) - offset, "]}");
    if (written < 0 || written >= (int)(sizeof(body) - offset)) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"networks too large\"}");
    }
    offset += (size_t)written;

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    return httpd_resp_send(req, body, offset);
}

static uint32_t wifi_test_id_from_query(httpd_req_t *req)
{
    char query[40];
    char id_text[16];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK) {
        return 0;
    }

    char *end = NULL;
    const unsigned long id = strtoul(id_text, &end, 10);
    return end != id_text ? (uint32_t)id : 0;
}

static esp_err_t wifi_test_status_handler(httpd_req_t *req)
{
    wifi_test_async_state_t state = {0};
    if (!wifi_test_state_snapshot(&state)) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        set_no_store_headers(req);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"no Wi-Fi test\"}");
    }

    const uint32_t requested_id = wifi_test_id_from_query(req);
    if (requested_id != 0 && requested_id != state.id) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_set_type(req, "application/json");
        set_no_store_headers(req);
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Wi-Fi test not found\"}");
    }

    return send_wifi_test_state(req, &state);
}

static esp_err_t wifi_test_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid size\"}");
    }

    char *body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    }

    m45_config_t test_config = *m45_config_get();
    const bool ok =
        json_get_string(json, "wifi_ssid", test_config.wifi_ssid,
                        sizeof(test_config.wifi_ssid), true) &&
        json_get_string(json, "wifi_password", test_config.wifi_password,
                        sizeof(test_config.wifi_password), false);
    cJSON_Delete(json);

    if (!ok || test_config.wifi_ssid[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid Wi-Fi settings\"}");
    }

    wifi_test_async_state_t current = {0};
    if (wifi_test_state_snapshot(&current) && current.running) {
        return send_wifi_test_state(req, &current);
    }

    wifi_test_task_args_t *task_args = calloc(1, sizeof(*task_args));
    if (task_args == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    uint32_t test_id = 0;
    if (!wifi_test_begin(&test_id)) {
        free(task_args);
        wifi_test_state_snapshot(&current);
        return send_wifi_test_state(req, &current);
    }

    task_args->id = test_id;
    strlcpy(task_args->ssid, test_config.wifi_ssid, sizeof(task_args->ssid));
    strlcpy(task_args->password, test_config.wifi_password, sizeof(task_args->password));

    const uint64_t test_started_us = http_now_us();
    if (xTaskCreate(wifi_test_async_task, "wifi_test", 4096, task_args,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        wifi_test_finish(test_id, ESP_ERR_NO_MEM, 0, "", 0);
        free(task_args);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"test task failed\"}");
    }
    log_http_handler_delay("Wi-Fi connection test start", test_started_us);

    wifi_test_state_snapshot(&current);
    return send_wifi_test_state(req, &current);
}

static esp_err_t setup_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 1536) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid size\"}");
    }

    char *body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    }

    m45_config_t config = *m45_config_get();
    const bool ok =
        json_get_string(json, "wifi_ssid", config.wifi_ssid, sizeof(config.wifi_ssid), true) &&
        json_get_string(json, "wifi_password", config.wifi_password,
                        sizeof(config.wifi_password), false) &&
        json_get_string(json, "pool_host", config.pool_host, sizeof(config.pool_host), true) &&
        json_get_string(json, "pool_user", config.pool_user, sizeof(config.pool_user), true) &&
        json_get_string(json, "pool_pass", config.pool_pass, sizeof(config.pool_pass), false) &&
        json_get_u16(json, "pool_port", &config.pool_port, 1, 65535) &&
        json_get_optional_bool(json, "pool_difficulty_auto",
                               &config.pool_difficulty_auto) &&
        json_get_optional_u16(json, "pool_difficulty", &config.pool_difficulty, 1, 65535);
    cJSON_Delete(json);

    if (!ok || config.wifi_ssid[0] == '\0' || config.pool_host[0] == '\0' ||
        config.pool_user[0] == '\0') {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid setup\"}");
    }
    if (!wifi_test_result_matches(config.wifi_ssid, config.wifi_password)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"test Wi-Fi connection before saving\"}");
    }

    const uint64_t save_started_us = http_now_us();
    esp_err_t err = m45_config_save(&config);
    log_http_handler_delay("setup NVS save", save_started_us);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    if (xTaskCreate(reboot_task, "setup_reboot", 2048, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"reboot task failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    return httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 4096) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid size\"}");
    }

    char *body = malloc((size_t)req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    }

    m45_config_t config = *m45_config_get();
    const bool ok =
        json_get_string(json, "wifi_ssid", config.wifi_ssid, sizeof(config.wifi_ssid), true) &&
        json_get_string(json, "wifi_password", config.wifi_password,
                        sizeof(config.wifi_password), false) &&
        json_get_string(json, "hostname", config.hostname, sizeof(config.hostname), true) &&
        json_get_string(json, "pool_host", config.pool_host, sizeof(config.pool_host), true) &&
        json_get_string(json, "backup_pool_host", config.backup_pool_host,
                        sizeof(config.backup_pool_host), true) &&
        json_get_string(json, "pool_user", config.pool_user, sizeof(config.pool_user), true) &&
        json_get_string(json, "pool_pass", config.pool_pass, sizeof(config.pool_pass), false) &&
        json_get_u16(json, "pool_port", &config.pool_port, 1, 65535) &&
        json_get_u16(json, "backup_pool_port", &config.backup_pool_port, 1, 65535) &&
        json_get_optional_bool(json, "pool_difficulty_auto",
                               &config.pool_difficulty_auto) &&
        json_get_optional_u16(json, "pool_difficulty", &config.pool_difficulty, 1, 65535) &&
        json_get_optional_bool(json, "overclock_enabled", &config.overclock_enabled) &&
        json_get_u16(json, "asic_frequency_mhz", &config.asic_frequency_mhz,
                     M45_ASIC_FREQUENCY_MIN_MHZ, M45_ASIC_FREQUENCY_MAX_MHZ) &&
        json_get_u16(json, "asic_voltage_mv", &config.asic_voltage_mv, 500, 1370) &&
        json_get_optional_i16(json, "overclock_voltage_offset_mv",
                              &config.overclock_voltage_offset_mv, -500, 300) &&
        json_get_optional_bool(json, "asic_voltage_temp_compensation_enabled",
                               &config.asic_voltage_temp_compensation_enabled) &&
        json_get_bool(json, "fan_override_enabled", &config.fan_override_enabled) &&
        json_get_u16(json, "fan_override_percent", &config.fan_override_percent, 0, 100) &&
        json_get_optional_bool(json, "fan_auto_off_allowed",
                               &config.fan_auto_off_allowed) &&
        json_get_bool(json, "fan_target_override_enabled",
                      &config.fan_target_override_enabled) &&
        json_get_u16(json, "fan_target_temp_c", &config.fan_target_temp_c, 35, 66) &&
        json_get_optional_bool(json, "display_screensaver_enabled",
                               &config.display_screensaver_enabled) &&
        json_get_optional_u16(json, "display_sleep_minutes",
                              &config.display_sleep_minutes, 0,
                              M45_DISPLAY_SLEEP_MAX_MINUTES) &&
        json_get_optional_u16(json, "limit_input_voltage_min_mv",
                              &config.safety_input_voltage_min_mv,
                              M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                              M45_SAFETY_INPUT_VOLTAGE_MIN_MAX_MV) &&
        json_get_optional_u16(json, "limit_input_voltage_expected_min_mv",
                              &config.safety_input_voltage_expected_min_mv,
                              M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                              M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV) &&
        json_get_optional_u16(json, "limit_input_voltage_expected_max_mv",
                              &config.safety_input_voltage_expected_max_mv,
                              M45_SAFETY_INPUT_VOLTAGE_MIN_MIN_MV,
                              M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV) &&
        json_get_optional_u16(json, "limit_input_voltage_max_mv",
                              &config.safety_input_voltage_max_mv,
                              M45_SAFETY_INPUT_VOLTAGE_MAX_MIN_MV,
                              M45_SAFETY_INPUT_VOLTAGE_MAX_MAX_MV) &&
        json_get_optional_u16(json, "limit_asic_voltage_min_mv",
                              &config.safety_asic_voltage_min_mv,
                              M45_SAFETY_ASIC_VOLTAGE_MIN_MIN_MV,
                              M45_SAFETY_ASIC_VOLTAGE_MIN_MAX_MV) &&
        json_get_optional_u16(json, "limit_asic_voltage_max_mv",
                              &config.safety_asic_voltage_max_mv,
                              M45_SAFETY_ASIC_VOLTAGE_MAX_MIN_MV,
                              M45_SAFETY_ASIC_VOLTAGE_MAX_MAX_MV) &&
        json_get_optional_u16(json, "limit_asic_temp_expected_max_c",
                              &config.safety_asic_temp_expected_max_c,
                              M45_SAFETY_ASIC_TEMP_EXPECTED_MAX_MIN_C,
                              M45_SAFETY_ASIC_TEMP_MAX_MAX_C) &&
        json_get_optional_u16(json, "limit_asic_temp_max_c",
                              &config.safety_asic_temp_max_c,
                              M45_SAFETY_ASIC_TEMP_MAX_MIN_C,
                              M45_SAFETY_ASIC_TEMP_MAX_MAX_C) &&
        json_get_optional_u16(json, "limit_tps546_temp_expected_max_c",
                              &config.safety_tps546_temp_expected_max_c,
                              M45_SAFETY_TPS546_TEMP_EXPECTED_MAX_MIN_C,
                              M45_SAFETY_TPS546_TEMP_MAX_MAX_C) &&
        json_get_optional_u16(json, "limit_tps546_temp_max_c",
                              &config.safety_tps546_temp_max_c,
                              M45_SAFETY_TPS546_TEMP_MAX_MIN_C,
                              M45_SAFETY_TPS546_TEMP_MAX_MAX_C) &&
        json_get_optional_u16(json, "limit_iout_warn_deciamps",
                              &config.safety_iout_warn_deciamps,
                              M45_SAFETY_IOUT_WARN_MIN_DA,
                              M45_SAFETY_IOUT_FAULT_MAX_DA) &&
        json_get_optional_u16(json, "limit_iout_fault_deciamps",
                              &config.safety_iout_fault_deciamps,
                              M45_SAFETY_IOUT_FAULT_MIN_DA,
                              M45_SAFETY_IOUT_FAULT_MAX_DA);
    cJSON_Delete(json);

    if (!ok || config.hostname[0] == '\0' || config.pool_host[0] == '\0' ||
        config.backup_pool_host[0] == '\0' || config.pool_user[0] == '\0' ||
        (config.fan_override_percent != 0 &&
         (config.fan_override_percent < 35 || config.fan_override_percent > 100)) ||
        config.fan_target_temp_c < 35 || config.fan_target_temp_c > 66 ||
        !safety_settings_valid_for_tune(&config)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid settings\"}");
    }

    const m45_config_t old_config = *m45_config_get();
    if (wifi_credentials_changed(&old_config, &config) &&
        !wifi_test_result_matches(config.wifi_ssid, config.wifi_password)) {
        httpd_resp_set_status(req, "409 Conflict");
        return httpd_resp_sendstr(req, "{\"error\":\"test Wi-Fi connection before saving\"}");
    }

    bool wifi_reconnect = false;
    bool pool_reconnect = false;
    runtime_reconnect_flags(&old_config, &config, &wifi_reconnect, &pool_reconnect);

    esp_err_t err = apply_hardware_settings(&old_config, &config);
    if (err != ESP_OK) {
        esp_err_t revert_err = apply_hardware_settings(&config, &old_config);
        if (revert_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to restore hardware settings after apply error: %s",
                     esp_err_to_name(revert_err));
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    uint64_t started_us = http_now_us();
    err = m45_config_save(&config);
    log_http_handler_delay("settings NVS save", started_us);
    if (err != ESP_OK) {
        esp_err_t revert_err = apply_hardware_settings(&config, &old_config);
        if (revert_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to restore hardware settings after save error: %s",
                     esp_err_to_name(revert_err));
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    apply_runtime_state(m45_config_get());
    started_us = http_now_us();
    apply_runtime_reconnects(wifi_reconnect, pool_reconnect);
    log_http_handler_delay("settings reconnect apply", started_us);

    char response[96];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"restart\":false,\"wifi_reconnect\":%s,\"pool_reconnect\":%s}",
             wifi_reconnect ? "true" : "false", pool_reconnect ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static bool json_get_tune_u16(cJSON *root, const char *name, const char *alt_name,
                              uint16_t *dst, int min, int max)
{
    cJSON *item = cJSON_GetObjectItem(root, name);
    if (item == NULL && alt_name != NULL) {
        item = cJSON_GetObjectItem(root, alt_name);
    }
    if (item == NULL) {
        return true;
    }
    if (!cJSON_IsNumber(item) || item->valuedouble < min || item->valuedouble > max) {
        return false;
    }
    *dst = (uint16_t)item->valueint;
    return true;
}

static esp_err_t runtime_tune_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid size\"}");
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    }

    m45_config_t runtime = *m45_config_get();
    const bool ok =
        json_get_optional_bool(json, "overclock_enabled", &runtime.overclock_enabled) &&
        json_get_tune_u16(json, "frequency_mhz", "asic_frequency_mhz",
                          &runtime.asic_frequency_mhz, M45_ASIC_FREQUENCY_MIN_MHZ,
                          M45_ASIC_FREQUENCY_MAX_MHZ) &&
        json_get_tune_u16(json, "voltage_mv", "asic_voltage_mv",
                          &runtime.asic_voltage_mv, 500, 1370) &&
        json_get_optional_bool(json, "asic_voltage_temp_compensation_enabled",
                               &runtime.asic_voltage_temp_compensation_enabled) &&
        json_get_optional_bool(json, "fan_override_enabled", &runtime.fan_override_enabled) &&
        json_get_optional_u16(json, "fan_override_percent", &runtime.fan_override_percent,
                              0, 100) &&
        json_get_optional_bool(json, "fan_auto_off_allowed",
                               &runtime.fan_auto_off_allowed) &&
        json_get_optional_bool(json, "fan_target_override_enabled",
                               &runtime.fan_target_override_enabled) &&
        json_get_optional_u16(json, "fan_target_temp_c", &runtime.fan_target_temp_c,
                              35, 66);
    cJSON_Delete(json);

    if (!ok || (runtime.fan_override_percent != 0 &&
                (runtime.fan_override_percent < 35 || runtime.fan_override_percent > 100))) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid tune\"}");
    }

    const m45_config_t old_config = *m45_config_get();
    esp_err_t err = m45_config_set_runtime(&runtime);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    bool wifi_reconnect = false;
    bool pool_reconnect = false;
    uint64_t started_us = http_now_us();
    err = apply_runtime_settings(&old_config, m45_config_get(), &wifi_reconnect, &pool_reconnect);
    log_http_handler_delay("runtime tune apply", started_us);
    if (err != ESP_OK) {
        esp_err_t revert_err = apply_hardware_settings(m45_config_get(), &old_config);
        if (revert_err != ESP_OK) {
            ESP_LOGW(TAG, "failed to restore runtime hardware settings after apply error: %s",
                     esp_err_to_name(revert_err));
        }
        m45_config_set_runtime(&old_config);
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"runtime\":true}");
}

static esp_err_t asic_power_handler(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 128) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid size\"}");
    }

    char *body = calloc(1, req->content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    int received = 0;
    while (received < req->content_len) {
        const int ret = httpd_req_recv(req, body + received, req->content_len - received);
        if (ret <= 0) {
            free(body);
            return ESP_FAIL;
        }
        received += ret;
    }
    body[received] = '\0';

    cJSON *json = cJSON_Parse(body);
    free(body);
    if (json == NULL) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"bad json\"}");
    }

    bool enabled = false;
    bool manage_fan = true;
    const bool ok = json_get_bool(json, "enabled", &enabled) &&
                    json_get_optional_bool(json, "manage_fan", &manage_fan);
    cJSON_Delete(json);
    if (!ok) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid power state\"}");
    }

    const uint64_t power_started_us = http_now_us();
    esp_err_t err = bitaxe_gamma602_set_asic_power(g_state, enabled, manage_fan);
    log_http_handler_delay("ASIC power apply", power_started_us);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_STATE) {
            httpd_resp_set_status(req, "409 Conflict");
            return httpd_resp_sendstr(req, "{\"error\":\"hardware fault\"}");
        }
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    char response[96];
    snprintf(response, sizeof(response),
             "{\"ok\":true,\"asic_power_enabled\":%s,\"asic_ready\":%s}",
             bitaxe_gamma602_asic_power_enabled() ? "true" : "false",
             g_state->ASIC_initalized ? "true" : "false");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t settings_factory_reset_handler(httpd_req_t *req)
{
    const uint64_t reset_started_us = http_now_us();
    esp_err_t err = m45_config_factory_reset();
    log_http_handler_delay("factory reset NVS erase", reset_started_us);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    if (xTaskCreate(factory_reset_reboot_task, "factory_reboot", 2048, NULL,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"reboot task failed\"}");
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(
        req, "{\"ok\":true,\"restart\":true,\"rebooting\":true,"
             "\"wifi_reconnect\":false,\"pool_reconnect\":false}");
}

static esp_err_t best_diff_reset_handler(httpd_req_t *req)
{
    const uint64_t reset_started_us = http_now_us();
    esp_err_t err = stratum_minimal_reset_best_diff();
    log_http_handler_delay("best diff reset", reset_started_us);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        char error_body[80];
        snprintf(error_body, sizeof(error_body), "{\"error\":\"%s\"}", esp_err_to_name(err));
        return httpd_resp_send(req, error_body, HTTPD_RESP_USE_STRLEN);
    }

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"best_diff\":0}");
}

static esp_err_t block_alert_dismiss_handler(httpd_req_t *req)
{
    stratum_minimal_dismiss_block_alert();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"block_alert_active\":false}");
}

static void reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

static void factory_reset_reboot_task(void *arg)
{
    (void)arg;
    m45_oled_show_factory_reset();
    esp_err_t err = esp_wifi_restore();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "failed to clear Wi-Fi driver settings during factory reset: %s",
                 esp_err_to_name(err));
    }
    esp_restart();
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    xTaskCreate(reboot_task, "web_reboot", 2048, NULL, tskIDLE_PRIORITY + 1, NULL);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
}

static esp_err_t logs_handler(httpd_req_t *req)
{
    if (request_has_bad_page_token(req)) {
        return send_page_token_reload(req);
    }
    m45_log_buffer_keep_active(LOG_CAPTURE_TIMEOUT_MS);

    char *body = malloc(M45_LOG_BUFFER_SIZE + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "out of memory");
    }

    uint64_t next_seq = 0;
    bool truncated = false;
    size_t body_len = m45_log_buffer_copy_since(logs_since_from_query(req), body,
                                                M45_LOG_BUFFER_SIZE + 1, &next_seq,
                                                &truncated);
    if (!logs_verbose_from_query(req)) {
        body_len = filter_normal_logs(body, body_len);
    }

    char next_header[24];
    snprintf(next_header, sizeof(next_header), "%" PRIu64, next_seq);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    set_no_store_headers(req);
    httpd_resp_set_hdr(req, "X-Log-Next", next_header);
    httpd_resp_set_hdr(req, "X-Log-Truncated", truncated ? "1" : "0");
    const esp_err_t err = httpd_resp_send(req, body, body_len);
    free(body);
    return err;
}

static esp_err_t health_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, "ok");
}

static esp_err_t start_http_server(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.stack_size = 8192;
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = HTTP_URI_HANDLER_SLOTS;
    config.max_open_sockets = 4;
    config.lru_purge_enable = true;

    httpd_handle_t server = NULL;
    ESP_RETURN_ON_ERROR(httpd_start(&server, &config), TAG, "HTTP server start failed");

    static const timed_http_route_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/wifi", .method = HTTP_GET, .handler = setup_page_handler},
        {.uri = "/settings", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/overclock", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/calibration", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/logs", .method = HTTP_GET, .handler = root_handler},
        {.uri = "/styles.css", .method = HTTP_GET, .handler = styles_css_handler},
        {.uri = "/api/status", .method = HTTP_GET, .handler = status_handler},
        {.uri = "/api/setup", .method = HTTP_GET, .handler = setup_get_handler},
        {.uri = "/api/setup", .method = HTTP_POST, .handler = setup_post_handler},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_handler},
        {.uri = "/api/wifi-test", .method = HTTP_GET, .handler = wifi_test_status_handler},
        {.uri = "/api/wifi-test", .method = HTTP_POST, .handler = wifi_test_handler},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = settings_get_handler},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = settings_post_handler},
        {.uri = "/api/runtime-tune", .method = HTTP_POST, .handler = runtime_tune_handler},
        {.uri = "/api/asic-power", .method = HTTP_POST, .handler = asic_power_handler},
        {.uri = "/api/settings/factory-reset", .method = HTTP_POST,
         .handler = settings_factory_reset_handler},
        {.uri = "/api/best-diff/reset", .method = HTTP_POST,
         .handler = best_diff_reset_handler},
        {.uri = "/api/block-alert/dismiss", .method = HTTP_POST,
         .handler = block_alert_dismiss_handler},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = reboot_handler},
        {.uri = "/api/logs", .method = HTTP_GET, .handler = logs_handler},
        {.uri = "/health", .method = HTTP_GET, .handler = health_handler},
        {.uri = "/generate_204", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/gen_204", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/hotspot-detect.html", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/library/test/success.html", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/connecttest.txt", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/redirect", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/ncsi.txt", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/canonical.html", .method = HTTP_GET,
         .handler = captive_portal_redirect_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = redirect_handler},
    };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); ++i) {
        const httpd_uri_t handler = {
            .uri = routes[i].uri,
            .method = routes[i].method,
            .handler = timed_http_handler,
            .user_ctx = (void *)&routes[i],
        };
        ESP_RETURN_ON_ERROR(httpd_register_uri_handler(server, &handler), TAG,
                            "HTTP route register failed: %s", routes[i].uri);
    }
    ESP_LOGI(TAG, "HTTP server started");
    return ESP_OK;
}

esp_err_t wifi_http_start(GlobalState *state)
{
    const m45_config_t *config = m45_config_get();
    const bool setup_mode = config->wifi_ssid[0] == '\0';
    g_state = state;
    g_wifi_events = xEventGroupCreate();
    if (g_wifi_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    g_wifi_test_mutex = xSemaphoreCreateMutex();
    if (g_wifi_test_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }
    g_wifi_scan_mutex = xSemaphoreCreateMutex();
    if (g_wifi_scan_mutex == NULL) {
        return ESP_ERR_NO_MEM;
    }

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop failed");
    init_setup_identity();
    g_netif = esp_netif_create_default_wifi_sta();
    if (g_netif != NULL) {
        esp_netif_set_hostname(g_netif, config->hostname);
    }
    if (setup_mode) {
        ESP_RETURN_ON_ERROR(configure_setup_ap_netif(), TAG, "setup AP netif failed");
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "Wi-Fi init failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "Wi-Fi storage failed");
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler, NULL, NULL));

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(setup_mode ? WIFI_MODE_APSTA : WIFI_MODE_STA), TAG,
                        "Wi-Fi mode failed");
    if (setup_mode) {
        ESP_RETURN_ON_ERROR(configure_setup_ap_wifi(), TAG, "setup AP config failed");
        g_setup_ap_active = true;
        ESP_LOGW(TAG, "setup AP active: SSID %s, IP %s", g_setup_ssid, g_setup_ip);
    } else {
        ESP_RETURN_ON_ERROR(set_sta_config(config->wifi_ssid, config->wifi_password), TAG,
                            "Wi-Fi config failed");
        g_setup_ap_active = false;
    }
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "Wi-Fi start failed");

    init_page_token();
    ESP_RETURN_ON_ERROR(start_http_server(), TAG, "HTTP server failed");

    if (setup_mode) {
        start_setup_dns();
        ESP_LOGW(TAG, "Wi-Fi SSID is empty; join setup AP %s and open http://%s/",
                 g_setup_ssid, g_setup_ip);
        return ESP_OK;
    }

    xEventGroupWaitBits(g_wifi_events, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                        pdMS_TO_TICKS(15000));
    return ESP_OK;
}

bool wifi_http_connected(void)
{
    return g_connected;
}

const char *wifi_http_ip(void)
{
    return g_ip;
}

bool wifi_http_setup_active(void)
{
    return g_setup_ap_active;
}

const char *wifi_http_setup_ssid(void)
{
    return g_setup_ssid;
}

const char *wifi_http_setup_ip(void)
{
    return g_setup_ip;
}
