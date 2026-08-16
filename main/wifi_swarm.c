#include "wifi_swarm.h"
#include "wifi_http_limits.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netdb.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "sdkconfig.h"

#define SWARM_MAX_DEVICES 32
#define SWARM_SCAN_BATCH 2
#define SWARM_CONNECT_TIMEOUT_MS 180
#define SWARM_FETCH_TIMEOUT_MS 1000
#define SWARM_INFO_RESPONSE_MAX (24U * 1024U)
#define SWARM_POST_BODY_MAX 256
#define SWARM_TASK_STACK 6144

_Static_assert(SWARM_SCAN_BATCH + M45_HTTP_MAX_OPEN_SOCKETS +
                       M45_HTTP_LISTEN_SOCKET_RESERVE +
                       M45_STRATUM_SOCKET_RESERVE <=
                   CONFIG_LWIP_MAX_SOCKETS,
               "swarm scan batch exhausts configured lwIP sockets");

typedef struct {
    char ip[16];
    char hostname[65];
    char version[40];
    char board_version[24];
    char asic_model[24];
    char device_model[24];
    char swarm_color[16];
    double hashrate_ghs;
    double power_w;
    double temp_c;
    double vr_temp_c;
    double best_diff;
    double pool_difficulty;
    uint32_t shares_accepted;
    uint32_t shares_rejected;
    uint32_t uptime_seconds;
    bool mining_paused;
    bool manual;
    uint64_t last_seen_us;
} swarm_device_t;

typedef struct {
    char address[65];
} swarm_add_args_t;

static SemaphoreHandle_t g_swarm_mutex;
static const char *TAG = "wifi_swarm";
static swarm_device_t g_devices[SWARM_MAX_DEVICES];
static size_t g_device_count;
static bool g_scan_running;
static uint16_t g_scanned_hosts;
static uint64_t g_last_scan_us;

static uint32_t open_hosts_in_batch(uint32_t network, uint16_t first_host,
                                    uint16_t host_count)
{
    int sockets[SWARM_SCAN_BATCH];
    fd_set write_set;
    FD_ZERO(&write_set);
    int max_socket = -1;
    uint32_t open_mask = 0;
    for (uint16_t i = 0; i < host_count; ++i) {
        sockets[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (sockets[i] < 0) {
            continue;
        }
        const int flags = fcntl(sockets[i], F_GETFL, 0);
        if (flags < 0 || fcntl(sockets[i], F_SETFL, flags | O_NONBLOCK) < 0) {
            close(sockets[i]);
            sockets[i] = -1;
            continue;
        }
        struct sockaddr_in address = {
            .sin_family = AF_INET,
            .sin_port = htons(80),
            .sin_addr = {.s_addr = htonl(network | (first_host + i))},
        };
        const int result = connect(sockets[i], (struct sockaddr *)&address,
                                   sizeof(address));
        if (result == 0) {
            open_mask |= 1UL << i;
        } else if (errno == EINPROGRESS) {
            FD_SET(sockets[i], &write_set);
            if (sockets[i] > max_socket) {
                max_socket = sockets[i];
            }
        } else {
            close(sockets[i]);
            sockets[i] = -1;
        }
    }

    if (max_socket >= 0) {
        struct timeval timeout = {
            .tv_sec = SWARM_CONNECT_TIMEOUT_MS / 1000,
            .tv_usec = (SWARM_CONNECT_TIMEOUT_MS % 1000) * 1000,
        };
        if (select(max_socket + 1, NULL, &write_set, NULL, &timeout) > 0) {
            for (uint16_t i = 0; i < host_count; ++i) {
                if (sockets[i] < 0 || !FD_ISSET(sockets[i], &write_set)) {
                    continue;
                }
                int socket_error = 0;
                socklen_t error_len = sizeof(socket_error);
                if (getsockopt(sockets[i], SOL_SOCKET, SO_ERROR, &socket_error,
                               &error_len) == 0 && socket_error == 0) {
                    open_mask |= 1UL << i;
                }
            }
        }
    }
    for (uint16_t i = 0; i < host_count; ++i) {
        if (sockets[i] >= 0) {
            close(sockets[i]);
        }
    }
    return open_mask;
}

static void copy_json_string(cJSON *root, const char *name, char *dest, size_t dest_len)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        strlcpy(dest, item->valuestring, dest_len);
    }
}

static double suffix_number(const char *text)
{
    if (text == NULL || text[0] == '\0') {
        return 0.0;
    }
    char *end = NULL;
    double value = strtod(text, &end);
    if (end == text || !isfinite(value)) {
        return 0.0;
    }
    switch (*end) {
    case 'k':
    case 'K':
        value *= 1e3;
        break;
    case 'm':
    case 'M':
        value *= 1e6;
        break;
    case 'g':
    case 'G':
        value *= 1e9;
        break;
    case 't':
    case 'T':
        value *= 1e12;
        break;
    case 'p':
    case 'P':
        value *= 1e15;
        break;
    default:
        break;
    }
    return value;
}

static double json_number(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (cJSON_IsNumber(item)) {
        return item->valuedouble;
    }
    if (cJSON_IsString(item)) {
        return suffix_number(item->valuestring);
    }
    return 0.0;
}

static bool json_bool(cJSON *root, const char *name)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
    return cJSON_IsTrue(item) || (cJSON_IsNumber(item) && item->valueint != 0);
}

static int connect_ipv4(const char *ip, int timeout_ms)
{
    const int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return -1;
    }

    const int flags = fcntl(sock, F_GETFL, 0);
    if (flags < 0 || fcntl(sock, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(sock);
        return -1;
    }

    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons(80),
    };
    if (inet_pton(AF_INET, ip, &address.sin_addr) != 1) {
        close(sock);
        return -1;
    }

    int result = connect(sock, (struct sockaddr *)&address, sizeof(address));
    if (result < 0 && errno != EINPROGRESS) {
        close(sock);
        return -1;
    }
    if (result < 0) {
        fd_set write_set;
        FD_ZERO(&write_set);
        FD_SET(sock, &write_set);
        struct timeval timeout = {
            .tv_sec = timeout_ms / 1000,
            .tv_usec = (timeout_ms % 1000) * 1000,
        };
        result = select(sock + 1, NULL, &write_set, NULL, &timeout);
        int socket_error = 0;
        socklen_t error_len = sizeof(socket_error);
        if (result <= 0 || !FD_ISSET(sock, &write_set) ||
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &socket_error, &error_len) < 0 ||
            socket_error != 0) {
            close(sock);
            return -1;
        }
    }

    (void)fcntl(sock, F_SETFL, flags);
    struct timeval io_timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
    return sock;
}

static bool fetch_device(const char *ip, swarm_device_t *device)
{
    int sock = connect_ipv4(ip, SWARM_FETCH_TIMEOUT_MS);
    if (sock < 0) {
        return false;
    }

    char request[192];
    const int request_len = snprintf(
        request, sizeof(request),
        "GET /api/system/info HTTP/1.0\r\nHost: %s\r\nAccept: application/json\r\nConnection: close\r\n\r\n",
        ip);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        close(sock);
        return false;
    }
    size_t sent = 0;
    while (sent < (size_t)request_len) {
        const int written = send(sock, request + sent, (size_t)request_len - sent, 0);
        if (written <= 0) {
            close(sock);
            return false;
        }
        sent += (size_t)written;
    }

    char *response = malloc(SWARM_INFO_RESPONSE_MAX + 1);
    if (response == NULL) {
        close(sock);
        return false;
    }
    size_t used = 0;
    while (used < SWARM_INFO_RESPONSE_MAX) {
        const int received =
            recv(sock, response + used, SWARM_INFO_RESPONSE_MAX - used, 0);
        if (received <= 0) {
            break;
        }
        used += (size_t)received;
    }
    close(sock);
    if (used == SWARM_INFO_RESPONSE_MAX) {
        ESP_LOGW(TAG, "peer response from %s reached the %u-byte limit", ip,
                 (unsigned)SWARM_INFO_RESPONSE_MAX);
        free(response);
        return false;
    }
    response[used] = '\0';

    char *body = strstr(response, "\r\n\r\n");
    if (body == NULL ||
        (strncmp(response, "HTTP/1.0 200", 12) != 0 &&
         strncmp(response, "HTTP/1.1 200", 12) != 0)) {
        free(response);
        return false;
    }
    body += 4;
    cJSON *root = cJSON_Parse(body);
    if (!cJSON_IsObject(root) || cJSON_GetObjectItem(root, "hashRate") == NULL) {
        cJSON_Delete(root);
        free(response);
        return false;
    }

    memset(device, 0, sizeof(*device));
    strlcpy(device->ip, ip, sizeof(device->ip));
    copy_json_string(root, "hostname", device->hostname, sizeof(device->hostname));
    copy_json_string(root, "version", device->version, sizeof(device->version));
    copy_json_string(root, "boardVersion", device->board_version,
                     sizeof(device->board_version));
    copy_json_string(root, "ASICModel", device->asic_model, sizeof(device->asic_model));
    copy_json_string(root, "deviceModel", device->device_model,
                     sizeof(device->device_model));
    copy_json_string(root, "swarmColor", device->swarm_color,
                     sizeof(device->swarm_color));
    if (device->hostname[0] == '\0') {
        strlcpy(device->hostname, ip, sizeof(device->hostname));
    }
    if (device->device_model[0] == '\0') {
        strlcpy(device->device_model,
                strcmp(device->asic_model, "BM1370") == 0 ? "Gamma" : "Bitaxe",
                sizeof(device->device_model));
    }
    if (device->swarm_color[0] == '\0') {
        strlcpy(device->swarm_color,
                strcmp(device->asic_model, "BM1370") == 0 ? "blue" : "gray",
                sizeof(device->swarm_color));
    }

    device->hashrate_ghs = json_number(root, "hashRate");
    device->power_w = json_number(root, "power");
    device->temp_c = json_number(root, "temp");
    device->vr_temp_c = json_number(root, "vrTemp");
    device->best_diff = json_number(root, "bestDiff");
    device->pool_difficulty = json_number(root, "poolDifficulty");
    device->shares_accepted = (uint32_t)json_number(root, "sharesAccepted");
    device->shares_rejected = (uint32_t)json_number(root, "sharesRejected");
    device->uptime_seconds = (uint32_t)json_number(root, "uptimeSeconds");
    device->mining_paused = json_bool(root, "miningPaused");
    device->last_seen_us = (uint64_t)esp_timer_get_time();

    cJSON_Delete(root);
    free(response);
    return true;
}

static void store_device(const swarm_device_t *device)
{
    if (g_swarm_mutex == NULL ||
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return;
    }
    size_t index = 0;
    while (index < g_device_count && strcmp(g_devices[index].ip, device->ip) != 0) {
        ++index;
    }
    if (index < g_device_count) {
        const bool manual = g_devices[index].manual || device->manual;
        g_devices[index] = *device;
        g_devices[index].manual = manual;
    } else if (g_device_count < SWARM_MAX_DEVICES) {
        g_devices[g_device_count++] = *device;
    }
    xSemaphoreGive(g_swarm_mutex);
}

static void scan_task(void *arg)
{
    const uint64_t scan_started_us = (uint64_t)esp_timer_get_time();
    char local_ip[16];
    strlcpy(local_ip, (const char *)arg, sizeof(local_ip));
    free(arg);

    struct in_addr local_address;
    if (inet_pton(AF_INET, local_ip, &local_address) != 1) {
        goto done;
    }
    const uint32_t host_order_ip = ntohl(local_address.s_addr);
    const uint32_t network = host_order_ip & 0xffffff00UL;
    ESP_LOGI(TAG, "scanning local /24 from %s", local_ip);

    for (uint16_t first_host = 1; first_host < 255;
         first_host += SWARM_SCAN_BATCH) {
        const uint16_t host_count =
            first_host + SWARM_SCAN_BATCH < 255 ? SWARM_SCAN_BATCH
                                                : (uint16_t)(255 - first_host);
        const uint32_t open_mask =
            open_hosts_in_batch(network, first_host, host_count);
        for (uint16_t i = 0; i < host_count; ++i) {
            if ((open_mask & (1UL << i)) == 0) {
                continue;
            }
            struct in_addr candidate = {
                .s_addr = htonl(network | (first_host + i)),
            };
            char ip[16];
            if (inet_ntop(AF_INET, &candidate, ip, sizeof(ip)) == NULL) {
                continue;
            }
            swarm_device_t device;
            if (fetch_device(ip, &device)) {
                store_device(&device);
            }
        }
        if (g_swarm_mutex != NULL &&
            xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_scanned_hosts = first_host + host_count - 1;
            xSemaphoreGive(g_swarm_mutex);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    char manual_ips[SWARM_MAX_DEVICES][16];
    size_t manual_count = 0;
    if (g_swarm_mutex != NULL &&
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        for (size_t i = 0; i < g_device_count && manual_count < SWARM_MAX_DEVICES; ++i) {
            if (g_devices[i].manual) {
                strlcpy(manual_ips[manual_count++], g_devices[i].ip,
                        sizeof(manual_ips[0]));
            }
        }
        xSemaphoreGive(g_swarm_mutex);
    }
    for (size_t i = 0; i < manual_count; ++i) {
        swarm_device_t device;
        if (fetch_device(manual_ips[i], &device)) {
            device.manual = true;
            store_device(&device);
        }
    }

    if (g_swarm_mutex != NULL &&
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        size_t write_index = 0;
        for (size_t i = 0; i < g_device_count; ++i) {
            if (g_devices[i].manual || g_devices[i].last_seen_us >= scan_started_us) {
                if (write_index != i) {
                    g_devices[write_index] = g_devices[i];
                }
                ++write_index;
            }
        }
        g_device_count = write_index;
        xSemaphoreGive(g_swarm_mutex);
    }

done:
    size_t found_count = 0;
    if (g_swarm_mutex != NULL &&
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        g_scan_running = false;
        g_scanned_hosts = 254;
        g_last_scan_us = (uint64_t)esp_timer_get_time();
        found_count = g_device_count;
        xSemaphoreGive(g_swarm_mutex);
    }
    ESP_LOGI(TAG, "swarm scan complete: %u device(s)", (unsigned)found_count);
    vTaskDelete(NULL);
}

static void add_task(void *arg)
{
    swarm_add_args_t *args = arg;
    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *results = NULL;
    if (getaddrinfo(args->address, NULL, &hints, &results) == 0 && results != NULL) {
        const struct sockaddr_in *address = (const struct sockaddr_in *)results->ai_addr;
        char ip[16];
        if (inet_ntop(AF_INET, &address->sin_addr, ip, sizeof(ip)) != NULL) {
            swarm_device_t device;
            if (fetch_device(ip, &device)) {
                device.manual = true;
                store_device(&device);
                ESP_LOGI(TAG, "added swarm device %s (%s)", device.hostname, ip);
            }
        }
        freeaddrinfo(results);
    }
    free(args);
    vTaskDelete(NULL);
}

static bool valid_manual_address(const char *address)
{
    if (address == NULL || address[0] == '\0' || strlen(address) >= 65) {
        return false;
    }
    for (const char *cursor = address; *cursor != '\0'; ++cursor) {
        const char ch = *cursor;
        if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
              (ch >= '0' && ch <= '9') || ch == '-' || ch == '.')) {
            return false;
        }
    }
    return true;
}

static bool known_device_ip(const char *address, char ip[16])
{
    bool found = false;
    if (g_swarm_mutex == NULL || address == NULL ||
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return false;
    }
    for (size_t i = 0; i < g_device_count; ++i) {
        if (strcmp(address, g_devices[i].ip) == 0) {
            strlcpy(ip, g_devices[i].ip, 16);
            found = true;
            break;
        }
    }
    xSemaphoreGive(g_swarm_mutex);
    return found;
}

static esp_err_t post_device_action(const char *ip, const char *action)
{
    int sock = connect_ipv4(ip, SWARM_FETCH_TIMEOUT_MS);
    if (sock < 0) {
        return ESP_ERR_TIMEOUT;
    }
    char request[256];
    const int request_len = snprintf(
        request, sizeof(request),
        "POST /api/system/%s HTTP/1.0\r\nHost: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n",
        action, ip);
    if (request_len <= 0 || request_len >= (int)sizeof(request)) {
        close(sock);
        return ESP_ERR_INVALID_SIZE;
    }
    size_t sent = 0;
    while (sent < (size_t)request_len) {
        const int written = send(sock, request + sent, (size_t)request_len - sent, 0);
        if (written <= 0) {
            close(sock);
            return ESP_FAIL;
        }
        sent += (size_t)written;
    }
    char response[96] = {0};
    const int received = recv(sock, response, sizeof(response) - 1, 0);
    close(sock);
    if (received <= 0 ||
        (strncmp(response, "HTTP/1.0 200", 12) != 0 &&
         strncmp(response, "HTTP/1.1 200", 12) != 0)) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t start_scan(const char *local_ip)
{
    if (g_swarm_mutex == NULL || local_ip == NULL || local_ip[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    if (g_scan_running) {
        xSemaphoreGive(g_swarm_mutex);
        return ESP_ERR_NOT_FINISHED;
    }
    char *task_ip = strdup(local_ip);
    if (task_ip == NULL) {
        xSemaphoreGive(g_swarm_mutex);
        return ESP_ERR_NO_MEM;
    }
    g_scan_running = true;
    g_scanned_hosts = 0;
    xSemaphoreGive(g_swarm_mutex);

    if (xTaskCreate(scan_task, "swarm_scan", SWARM_TASK_STACK, task_ip,
                    tskIDLE_PRIORITY + 1, NULL) != pdPASS) {
        free(task_ip);
        xSemaphoreTake(g_swarm_mutex, portMAX_DELAY);
        g_scan_running = false;
        xSemaphoreGive(g_swarm_mutex);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t wifi_swarm_init(void)
{
    if (g_swarm_mutex == NULL) {
        g_swarm_mutex = xSemaphoreCreateMutex();
    }
    return g_swarm_mutex != NULL ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t wifi_swarm_get_handler(httpd_req_t *req)
{
    swarm_device_t *devices = NULL;
    size_t count = 0;
    bool scanning = false;
    uint16_t scanned_hosts = 0;
    uint64_t last_scan_us = 0;
    bool snapshot_alloc_failed = false;
    if (g_swarm_mutex != NULL &&
        xSemaphoreTake(g_swarm_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        count = g_device_count;
        if (count > 0) {
            devices = heap_caps_malloc(count * sizeof(*devices),
                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (devices == NULL) {
                devices = heap_caps_malloc(count * sizeof(*devices),
                                           MALLOC_CAP_8BIT);
            }
            if (devices != NULL) {
                memcpy(devices, g_devices, count * sizeof(*devices));
            } else {
                snapshot_alloc_failed = true;
            }
        }
        scanning = g_scan_running;
        scanned_hosts = g_scanned_hosts;
        last_scan_us = g_last_scan_us;
        xSemaphoreGive(g_swarm_mutex);
    }

    if (snapshot_alloc_failed) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    cJSON *root = cJSON_CreateObject();
    if (root == NULL) {
        heap_caps_free(devices);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }
    cJSON *array = cJSON_AddArrayToObject(root, "devices");
    if (array == NULL) {
        heap_caps_free(devices);
        cJSON_Delete(root);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }
    cJSON_AddBoolToObject(root, "scanning", scanning);
    cJSON_AddNumberToObject(root, "scanned_hosts", scanned_hosts);
    cJSON_AddNumberToObject(root, "total_hosts", 254);
    cJSON_AddNumberToObject(root, "last_scan_seconds",
                            last_scan_us > 0
                                ? (double)(((uint64_t)esp_timer_get_time() - last_scan_us) /
                                           1000000ULL)
                                : 0);

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    for (size_t i = 0; i < count; ++i) {
        cJSON *item = cJSON_CreateObject();
        if (item == NULL) {
            continue;
        }
        cJSON_AddStringToObject(item, "ip", devices[i].ip);
        cJSON_AddStringToObject(item, "hostname", devices[i].hostname);
        cJSON_AddStringToObject(item, "version", devices[i].version);
        cJSON_AddStringToObject(item, "board_version", devices[i].board_version);
        cJSON_AddStringToObject(item, "asic_model", devices[i].asic_model);
        cJSON_AddStringToObject(item, "device_model", devices[i].device_model);
        cJSON_AddStringToObject(item, "swarm_color", devices[i].swarm_color);
        cJSON_AddNumberToObject(item, "hashrate_ghs", devices[i].hashrate_ghs);
        cJSON_AddNumberToObject(item, "power_w", devices[i].power_w);
        cJSON_AddNumberToObject(item, "temp_c", devices[i].temp_c);
        cJSON_AddNumberToObject(item, "vr_temp_c", devices[i].vr_temp_c);
        cJSON_AddNumberToObject(item, "best_diff", devices[i].best_diff);
        cJSON_AddNumberToObject(item, "pool_difficulty", devices[i].pool_difficulty);
        cJSON_AddNumberToObject(item, "shares_accepted", devices[i].shares_accepted);
        cJSON_AddNumberToObject(item, "shares_rejected", devices[i].shares_rejected);
        cJSON_AddNumberToObject(item, "uptime_seconds", devices[i].uptime_seconds);
        cJSON_AddBoolToObject(item, "mining_paused", devices[i].mining_paused);
        cJSON_AddBoolToObject(item, "manual", devices[i].manual);
        cJSON_AddBoolToObject(item, "online",
                              now_us >= devices[i].last_seen_us &&
                                  now_us - devices[i].last_seen_us < 60000000ULL);
        cJSON_AddNumberToObject(item, "last_seen_seconds",
                                now_us >= devices[i].last_seen_us
                                    ? (double)((now_us - devices[i].last_seen_us) /
                                               1000000ULL)
                                    : 0);
        cJSON_AddItemToArray(array, item);
    }
    heap_caps_free(devices);

    char *body = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    const esp_err_t err = httpd_resp_sendstr(req, body);
    free(body);
    return err;
}

esp_err_t wifi_swarm_post_handler(httpd_req_t *req, const char *local_ip)
{
    if (req->content_len <= 0 || req->content_len > SWARM_POST_BODY_MAX) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "{\"error\":\"invalid request\"}");
    }
    char body[SWARM_POST_BODY_MAX + 1] = {0};
    int received = 0;
    while (received < req->content_len) {
        const int result = httpd_req_recv(req, body + received,
                                          req->content_len - received);
        if (result <= 0) {
            return ESP_FAIL;
        }
        received += result;
    }
    cJSON *json = cJSON_Parse(body);
    cJSON *command = cJSON_GetObjectItemCaseSensitive(json, "command");
    esp_err_t result = ESP_ERR_INVALID_ARG;
    if (cJSON_IsString(command) && strcmp(command->valuestring, "scan") == 0) {
        result = start_scan(local_ip);
    } else if (cJSON_IsString(command) && strcmp(command->valuestring, "add") == 0) {
        cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");
        if (cJSON_IsString(address) && valid_manual_address(address->valuestring)) {
            swarm_add_args_t *args = calloc(1, sizeof(*args));
            if (args != NULL) {
                strlcpy(args->address, address->valuestring, sizeof(args->address));
                if (xTaskCreate(add_task, "swarm_add", SWARM_TASK_STACK, args,
                                tskIDLE_PRIORITY + 1, NULL) == pdPASS) {
                    result = ESP_OK;
                } else {
                    free(args);
                    result = ESP_ERR_NO_MEM;
                }
            } else {
                result = ESP_ERR_NO_MEM;
            }
        }
    } else if (cJSON_IsString(command) &&
               strcmp(command->valuestring, "action") == 0) {
        cJSON *address = cJSON_GetObjectItemCaseSensitive(json, "address");
        cJSON *action = cJSON_GetObjectItemCaseSensitive(json, "action");
        const bool allowed_action =
            cJSON_IsString(action) &&
            (strcmp(action->valuestring, "pause") == 0 ||
             strcmp(action->valuestring, "resume") == 0 ||
             strcmp(action->valuestring, "restart") == 0);
        char ip[16];
        if (cJSON_IsString(address) && allowed_action &&
            known_device_ip(address->valuestring, ip)) {
            result = post_device_action(ip, action->valuestring);
        }
    }
    cJSON_Delete(json);

    if (result != ESP_OK && result != ESP_ERR_NOT_FINISHED) {
        httpd_resp_set_status(req, result == ESP_ERR_INVALID_ARG
                                      ? "400 Bad Request"
                                      : "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"swarm request failed\"}");
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, result == ESP_OK
                                      ? "{\"ok\":true}"
                                      : "{\"ok\":true,\"already_scanning\":true}");
}
