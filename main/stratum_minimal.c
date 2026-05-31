#include "stratum_minimal.h"

#include <errno.h>
#include <arpa/inet.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include "bitaxe_hw.h"
#include "bm1370.h"
#include "build_info.h"
#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_transport.h"
#include "esp_transport_tcp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "m45_config.h"
#include "m45_log_buffer.h"
#include "mining.h"
#include "utils.h"
#include "wifi_http.h"

#ifndef M45_STRATUM_FAST_PATHS
#define M45_STRATUM_FAST_PATHS 1
#endif

#define WORK_QUEUE_DEPTH 4
#define MAX_EXTRANONCE2_LEN 32
#define MAX_EXTRANONCE2_STR ((MAX_EXTRANONCE2_LEN * 2) + 1)
#define STRATUM_RECONNECT_MS 5000
#define STRATUM_PRIMARY_PROBE_INTERVAL_MS 60000
#define STRATUM_DNS_PREFETCH_INTERVAL_MS 60000
#define STRATUM_IDLE_TIMEOUT_MS 120000
#define TRANSPORT_TIMEOUT_MS 5000
#define STRATUM_BUFFER_SIZE 1024
#define STRATUM_MAX_LINE_SIZE 16384
#define STRATUM_MAX_COINBASE_BYTES 4096
#define SHARE_ID_SLOTS 16
#define RESPONSE_ID_SLOTS 16
#define HASHRATE_WINDOW_US 60000000ULL
#define HASHRATE_SAMPLE_US 2000000ULL
#define ASIC_HASHRATE_POLL_MS 15000
#define ASIC_HASHRATE_SETTLE_MS 100
#define ASIC_HASHRATE_STALE_US 45000000ULL
#define ASIC_REGISTER_AFTER_JOB_WINDOW_US 10000ULL
#define ASIC_REGISTER_AFTER_JOB_WAIT_MS 100
#define ASIC_HASH_DOMAIN_COUNT STRATUM_HASH_DOMAIN_COUNT
#define ASIC_DOMAIN_HASHRATE_SAMPLE_COUNT 5
#define HASH_COUNTER_LSB 4294967296.0
#define ASIC_JOB_DISPATCH_PERCENT 0.80f
#define ASIC_JOB_MIN_INTERVAL_MS 20
#define STRATUM_PAYOUT_MIN_PERCENT_X100 9700U
#define STRATUM_BASE58_DECODE_MAX 64
#define STRATUM_BECH32_DECODE_MAX 90
#define STRATUM_MINER_MODEL "M45-Bitaxe"
#define STRATUM_MINER_VERSION_CHARS 7
#define STRATUM_RX_TASK_PRIORITY 17
#define STRATUM_JOB_TASK_PRIORITY 18
#define STRATUM_RESULT_TASK_PRIORITY 16
#define STRATUM_HASHRATE_TASK_PRIORITY 6

#define STRATUM_ID_CONFIGURE 1
#define STRATUM_ID_SUBSCRIBE 2
#define STRATUM_ID_SUGGEST_DIFFICULTY 3
#define STRATUM_ID_AUTHORIZE 4
#define STRATUM_ID_EXTRANONCE_SUBSCRIBE 5
#define STRATUM_SUBMIT_NO_TRANSPORT (-2)

static const char *TAG = "stratum_min";

#define STRATUM_LOGI(...) m45_log_buffer_append_verbose(TAG, __VA_ARGS__)

static GlobalState *g_state;

typedef struct {
    mining_notify *work;
    uint32_t epoch;
} queued_work_t;

typedef struct {
    uint32_t value;
    uint64_t timestamp_us;
    double hashrate_ghs;
    bool valid;
} asic_hash_counter_t;

static QueueHandle_t g_work_queue;
static SemaphoreHandle_t g_transport_lock;
static SemaphoreHandle_t g_work_reset_lock;
static esp_transport_handle_t g_transport;
static portMUX_TYPE g_uid_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_share_id_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_response_id_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_pool_diff_mux = portMUX_INITIALIZER_UNLOCKED;
static portMUX_TYPE g_pool_endpoint_mux = portMUX_INITIALIZER_UNLOCKED;
static int g_next_uid = 10;
static int g_pending_share_ids[SHARE_ID_SLOTS];
static size_t g_pending_share_id_next;
static int g_pending_response_ids[RESPONSE_ID_SLOTS];
static uint64_t g_pending_response_us[RESPONSE_ID_SLOTS];
static size_t g_pending_response_id_next;
static char *g_rx_buffer;
static size_t g_rx_buffer_size;
static size_t g_rx_buffer_len;
static atomic_bool g_connected;
static atomic_bool g_work_paused;
static atomic_bool g_next_using_backup_pool;
static atomic_bool g_switch_to_primary_requested;
static atomic_bool g_primary_probe_in_progress;
static atomic_ullong g_last_primary_probe_us;
static atomic_ullong g_last_dns_prefetch_us;
static atomic_uint g_work_epoch;
static atomic_uint g_work_received;
static atomic_uint g_submitted;
static atomic_uint g_accepted;
static atomic_uint g_rejected;
static atomic_uint g_valid_nonces;
static atomic_uint g_nonce_errors;
static atomic_uint g_job_sent;
static atomic_uint g_response_time_ms;
static atomic_ullong g_share_submit_us;
static atomic_ullong g_share_submit_max_us;
static atomic_ullong g_share_write_us;
static atomic_ullong g_share_write_max_us;
static atomic_uint g_payout_status;
static atomic_uint g_payout_percent_x100;
static atomic_ullong g_connected_since_us;
static atomic_ullong g_last_job_sent_us;
static atomic_uint g_current_block_seq;
static atomic_bool g_block_alert_active;
static portMUX_TYPE g_best_diff_mux = portMUX_INITIALIZER_UNLOCKED;
static double g_best_diff;
static portMUX_TYPE g_block_alert_mux = portMUX_INITIALIZER_UNLOCKED;
static double g_block_alert_diff;
#ifdef M45_ASIC_LOSS_METRICS
static atomic_ullong g_metric_job_sent;
static atomic_ullong g_metric_job_send_skipped;
static atomic_ullong g_metric_job_alloc_failed;
static atomic_ullong g_metric_job_build_total_us;
static atomic_ullong g_metric_job_build_max_us;
static atomic_ullong g_metric_job_send_total_us;
static atomic_ullong g_metric_job_send_max_us;
static atomic_ullong g_metric_dispatch_late_count;
static atomic_ullong g_metric_dispatch_late_total_us;
static atomic_ullong g_metric_dispatch_late_max_us;
static atomic_ullong g_metric_dispatch_missed_slots;
static atomic_ullong g_metric_rx_calls;
static atomic_ullong g_metric_rx_null;
static atomic_ullong g_metric_rx_timeouts;
static atomic_ullong g_metric_rx_wait_total_us;
static atomic_ullong g_metric_rx_wait_max_us;
static atomic_ullong g_metric_rx_nonce_results;
static atomic_ullong g_metric_rx_register_results;
static atomic_ullong g_metric_invalid_job_nonces;
#endif
static portMUX_TYPE g_hashrate_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t g_hashrate_started_us;
static uint64_t g_hashrate_updated_us;
static uint64_t g_hashrate_last_work_us;
static double g_hashrate_work;
static double g_hashrate_cached_ghs;
static double g_hashrate_cached_nominal_ghs;
static double g_hashrate_cached_error_rate_percent;
static uint32_t g_hashrate_start_valid_nonces;
static uint32_t g_hashrate_start_nonce_errors;
static portMUX_TYPE g_asic_hashrate_mux = portMUX_INITIALIZER_UNLOCKED;
static asic_hash_counter_t *g_asic_total_counters;
static asic_hash_counter_t *g_asic_error_counters;
static asic_hash_counter_t *g_asic_domain_counters;
static size_t g_asic_hashrate_counter_count;
static double g_asic_hashrate_total_ghs;
static double g_asic_hashrate_error_ghs;
static double g_asic_error_rate_percent;
static uint64_t g_asic_hashrate_updated_us;
static double g_domain_hashrate_samples[ASIC_DOMAIN_HASHRATE_SAMPLE_COUNT];
static double g_domain_hashrate_sample_sum;
static size_t g_domain_hashrate_sample_next;
static size_t g_domain_hashrate_sample_count;
static double g_domain_hashrate_ghs;
static uint64_t g_domain_hashrate_updated_us;
static char g_current_block_hash[80];
static char g_active_pool_host[M45_POOL_HOST_MAX + 1];
static uint16_t g_active_pool_port;
static bool g_active_using_backup_pool;
static float g_job_interval_frequency_mhz;
static int g_job_interval_ms;
static uint8_t *g_extranonce_bin;
static size_t g_extranonce_len;

typedef struct {
    char host[M45_POOL_HOST_MAX + 1];
    char cached_ip[M45_POOL_IP_MAX + 1];
    uint16_t port;
    bool using_backup;
} stratum_endpoint_t;

typedef struct {
    const uint8_t *bytes;
    size_t len;
    size_t offset;
} coinbase_reader_t;

static bool stratum_runtime_ready(void);
static uint32_t reset_work_state(bool queue_marker);
static void set_payout_status(uint8_t status, uint16_t percent_x100);
static void stratum_enable_tcp_nodelay(esp_transport_handle_t transport);

static double nominal_hashrate_hs(void)
{
    if (g_state == NULL || !g_state->ASIC_initalized) {
        return 0.0;
    }

    const double frequency_hz =
        (double)g_state->POWER_MANAGEMENT_MODULE.actual_frequency * 1000000.0;
    return frequency_hz * (double)g_state->DEVICE_CONFIG.family.asic.small_core_count *
           (double)g_state->DEVICE_CONFIG.family.asic_count;
}

static uint16_t configured_suggested_difficulty(void)
{
    const m45_config_t *config = m45_config_get();
    if (g_state == NULL) {
        return config->pool_difficulty;
    }
    return m45_config_effective_pool_difficulty(
        config, g_state->DEVICE_CONFIG.family.asic.small_core_count,
        g_state->DEVICE_CONFIG.family.asic_count);
}

static double current_pool_difficulty(void)
{
    double difficulty = 0.0;
    taskENTER_CRITICAL(&g_pool_diff_mux);
    if (g_state != NULL) {
        difficulty = g_state->pool_difficulty;
    }
    taskEXIT_CRITICAL(&g_pool_diff_mux);
    return difficulty;
}

static bool set_pool_difficulty(double difficulty)
{
    bool raised = false;

    taskENTER_CRITICAL(&g_pool_diff_mux);
    if (g_state != NULL) {
        raised = difficulty > g_state->pool_difficulty;
        g_state->pool_difficulty = difficulty;
    }
    taskEXIT_CRITICAL(&g_pool_diff_mux);

    return raised;
}

static void copy_pool_host(char *dest, size_t dest_len, const char *host)
{
    if (dest_len == 0) {
        return;
    }
    size_t i = 0;
    if (host != NULL) {
        while (i + 1 < dest_len && host[i] != '\0') {
            dest[i] = host[i];
            ++i;
        }
    }
    dest[i] = '\0';
}

static void copy_pool_ip(char *dest, size_t dest_len, const char *ip)
{
    copy_pool_host(dest, dest_len, ip);
}

static bool backup_pool_configured(const m45_config_t *config)
{
    return config != NULL && config->backup_pool_host[0] != '\0' &&
           config->backup_pool_port > 0;
}

static void set_active_pool_endpoint(const stratum_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    taskENTER_CRITICAL(&g_pool_endpoint_mux);
    copy_pool_host(g_active_pool_host, sizeof(g_active_pool_host), endpoint->host);
    g_active_pool_port = endpoint->port;
    g_active_using_backup_pool = endpoint->using_backup;
    if (g_state != NULL) {
        g_state->SYSTEM_MODULE.pool_url = g_active_pool_host;
        g_state->SYSTEM_MODULE.pool_port = g_active_pool_port;
    }
    taskEXIT_CRITICAL(&g_pool_endpoint_mux);
}

static void set_active_primary_pool_endpoint(void)
{
    const m45_config_t *config = m45_config_get();
    const stratum_endpoint_t endpoint = {
        .port = config->pool_port,
        .using_backup = false,
    };
    stratum_endpoint_t copy = endpoint;
    copy_pool_host(copy.host, sizeof(copy.host), config->pool_host);
    set_active_pool_endpoint(&copy);
}

static void get_active_pool_endpoint(stratum_endpoint_t *endpoint)
{
    if (endpoint == NULL) {
        return;
    }

    taskENTER_CRITICAL(&g_pool_endpoint_mux);
    copy_pool_host(endpoint->host, sizeof(endpoint->host), g_active_pool_host);
    endpoint->cached_ip[0] = '\0';
    endpoint->port = g_active_pool_port;
    endpoint->using_backup = g_active_using_backup_pool;
    taskEXIT_CRITICAL(&g_pool_endpoint_mux);
}

static void select_next_pool_endpoint(stratum_endpoint_t *endpoint)
{
    const m45_config_t *config = m45_config_get();
    const bool using_backup = atomic_load(&g_next_using_backup_pool) &&
                              backup_pool_configured(config);

    copy_pool_host(endpoint->host, sizeof(endpoint->host),
                   using_backup ? config->backup_pool_host : config->pool_host);
    copy_pool_ip(endpoint->cached_ip, sizeof(endpoint->cached_ip),
                 using_backup ? config->backup_pool_ip : config->pool_ip);
    endpoint->port = using_backup ? config->backup_pool_port : config->pool_port;
    endpoint->using_backup = using_backup;
}

static bool stratum_host_is_ip4(const char *host, char *ip, size_t ip_size)
{
    struct in_addr addr;
    if (host == NULL || inet_pton(AF_INET, host, &addr) != 1) {
        return false;
    }
    copy_pool_ip(ip, ip_size, host);
    return true;
}

static bool stratum_resolve_endpoint_ip4(const stratum_endpoint_t *endpoint,
                                         char *ip, size_t ip_size)
{
    if (endpoint == NULL || endpoint->host[0] == '\0' || ip == NULL || ip_size == 0) {
        return false;
    }

    if (stratum_host_is_ip4(endpoint->host, ip, ip_size)) {
        (void)m45_config_set_pool_ip_cache(endpoint->using_backup, endpoint->host, ip);
        return true;
    }

    char port_text[8];
    struct addrinfo hints = {0};
    struct addrinfo *result = NULL;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(port_text, sizeof(port_text), "%u", endpoint->port);

    const int gai = getaddrinfo(endpoint->host, port_text, &hints, &result);
    if (gai != 0 || result == NULL) {
        STRATUM_LOGI("DNS lookup failed for %s: %d", endpoint->host, gai);
        if (result != NULL) {
            freeaddrinfo(result);
        }
        return false;
    }

    bool resolved = false;
    for (struct addrinfo *entry = result; entry != NULL; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET || entry->ai_addr == NULL) {
            continue;
        }
        const struct sockaddr_in *addr = (const struct sockaddr_in *)entry->ai_addr;
        if (inet_ntop(AF_INET, &addr->sin_addr, ip, ip_size) != NULL) {
            resolved = true;
            break;
        }
    }
    freeaddrinfo(result);

    if (resolved) {
        esp_err_t err = m45_config_set_pool_ip_cache(endpoint->using_backup,
                                                     endpoint->host, ip);
        if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(TAG, "failed to save %s pool IP %s: %s",
                     endpoint->using_backup ? "backup" : "primary", ip,
                     esp_err_to_name(err));
        } else {
            STRATUM_LOGI("DNS %s -> %s", endpoint->host, ip);
        }
    }
    return resolved;
}

static void stratum_connect_host_for_endpoint(const stratum_endpoint_t *endpoint,
                                              char *host, size_t host_size)
{
    char resolved_ip[M45_POOL_IP_MAX + 1];
    if (stratum_resolve_endpoint_ip4(endpoint, resolved_ip, sizeof(resolved_ip))) {
        copy_pool_ip(host, host_size, resolved_ip);
        return;
    }

    if (endpoint != NULL && endpoint->cached_ip[0] != '\0') {
        ESP_LOGW(TAG, "DNS failed for %s; using cached IP %s", endpoint->host,
                 endpoint->cached_ip);
        copy_pool_ip(host, host_size, endpoint->cached_ip);
        return;
    }

    copy_pool_host(host, host_size, endpoint != NULL ? endpoint->host : "");
}

static void stratum_maybe_prefetch_backup_pool_dns(const stratum_endpoint_t *current)
{
    const m45_config_t *config = m45_config_get();
    if (!backup_pool_configured(config) || config->backup_pool_ip[0] != '\0' ||
        (current != NULL && current->using_backup)) {
        return;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t last_us = atomic_load(&g_last_dns_prefetch_us);
    if (last_us != 0 &&
        now_us - last_us < (uint64_t)STRATUM_DNS_PREFETCH_INTERVAL_MS * 1000ULL) {
        return;
    }
    atomic_store(&g_last_dns_prefetch_us, now_us);

    stratum_endpoint_t backup = {
        .port = config->backup_pool_port,
        .using_backup = true,
    };
    copy_pool_host(backup.host, sizeof(backup.host), config->backup_pool_host);
    copy_pool_ip(backup.cached_ip, sizeof(backup.cached_ip), config->backup_pool_ip);

    char resolved_ip[M45_POOL_IP_MAX + 1];
    (void)stratum_resolve_endpoint_ip4(&backup, resolved_ip, sizeof(resolved_ip));
}

static void rotate_next_pool_endpoint(void)
{
    const m45_config_t *config = m45_config_get();
    if (!backup_pool_configured(config)) {
        atomic_store(&g_next_using_backup_pool, false);
        return;
    }
    atomic_store(&g_next_using_backup_pool, !atomic_load(&g_next_using_backup_pool));
}

static void request_stratum_transport_close(bool clear_work)
{
    if (clear_work) {
        reset_work_state(true);
    }

    if (g_transport_lock == NULL) {
        return;
    }

    xSemaphoreTake(g_transport_lock, portMAX_DELAY);
    esp_transport_handle_t transport = g_transport;
    if (transport != NULL) {
        esp_transport_close(transport);
    }
    xSemaphoreGive(g_transport_lock);
}

static double error_rate_percent_from_counts(uint32_t valid, uint32_t errors)
{
    const double total = (double)valid + (double)errors;
    return total > 0.0 ? ((double)errors / total) * 100.0 : 0.0;
}

static void set_hashrate_window_counter_start_locked(void)
{
    g_hashrate_start_valid_nonces = atomic_load(&g_valid_nonces);
    g_hashrate_start_nonce_errors = atomic_load(&g_nonce_errors);
}

static double current_nonce_error_rate_percent_locked(void)
{
    const uint32_t valid = atomic_load(&g_valid_nonces) - g_hashrate_start_valid_nonces;
    const uint32_t errors = atomic_load(&g_nonce_errors) - g_hashrate_start_nonce_errors;
    return error_rate_percent_from_counts(valid, errors);
}

static void update_assigned_hashrate_estimate(uint64_t now_us)
{
    if (g_hashrate_started_us == 0) {
        g_hashrate_cached_ghs = 0.0;
        g_hashrate_cached_nominal_ghs = 0.0;
        g_hashrate_cached_error_rate_percent = 0.0;
        return;
    }
    if (now_us <= g_hashrate_started_us) {
        return;
    }

    const uint64_t elapsed_us = now_us - g_hashrate_started_us;
    if (elapsed_us > HASHRATE_WINDOW_US) {
        return;
    }
    if (elapsed_us < HASHRATE_SAMPLE_US) {
        return;
    }
    if (g_hashrate_updated_us != 0 && now_us - g_hashrate_updated_us < HASHRATE_SAMPLE_US) {
        return;
    }

    g_hashrate_cached_nominal_ghs =
        g_hashrate_work / ((double)elapsed_us / 1000000.0) / 1000000000.0;
    g_hashrate_cached_error_rate_percent = current_nonce_error_rate_percent_locked();
    const double error_scale = 1.0 - (g_hashrate_cached_error_rate_percent / 100.0);
    g_hashrate_cached_ghs =
        g_hashrate_cached_nominal_ghs * (error_scale > 0.0 ? error_scale : 0.0);
    g_hashrate_updated_us = now_us;
}

static void reset_hashrate_window_locked(void)
{
    g_hashrate_started_us = 0;
    g_hashrate_updated_us = 0;
    g_hashrate_last_work_us = 0;
    g_hashrate_work = 0.0;
    g_hashrate_cached_ghs = 0.0;
    g_hashrate_cached_nominal_ghs = 0.0;
    g_hashrate_cached_error_rate_percent = 0.0;
    set_hashrate_window_counter_start_locked();
}

static void reset_hashrate_window(void)
{
    taskENTER_CRITICAL(&g_hashrate_mux);
    reset_hashrate_window_locked();
    taskEXIT_CRITICAL(&g_hashrate_mux);
}

static void record_assigned_work(int interval_ms)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();

    taskENTER_CRITICAL(&g_hashrate_mux);
    if (g_hashrate_started_us == 0 ||
        now_us - g_hashrate_started_us > HASHRATE_WINDOW_US) {
        g_hashrate_started_us = now_us;
        g_hashrate_updated_us = now_us;
        g_hashrate_work = 0.0;
        g_hashrate_cached_ghs = 0.0;
        g_hashrate_cached_nominal_ghs = 0.0;
        g_hashrate_cached_error_rate_percent = 0.0;
        set_hashrate_window_counter_start_locked();
    }
    g_hashrate_last_work_us = now_us;
    g_hashrate_work += nominal_hashrate_hs() * ((double)interval_ms / 1000.0);
    update_assigned_hashrate_estimate(now_us);
    taskEXIT_CRITICAL(&g_hashrate_mux);
}

static void assigned_hashrate_snapshot(double *effective_ghs, double *nominal_ghs,
                                       double *error_rate_percent)
{
    taskENTER_CRITICAL(&g_hashrate_mux);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (g_hashrate_last_work_us != 0 && now_us - g_hashrate_last_work_us > HASHRATE_WINDOW_US) {
        reset_hashrate_window_locked();
    }
    if (effective_ghs != NULL) {
        *effective_ghs = g_hashrate_cached_ghs;
    }
    if (nominal_ghs != NULL) {
        *nominal_ghs = g_hashrate_cached_nominal_ghs;
    }
    if (error_rate_percent != NULL) {
        *error_rate_percent = g_hashrate_cached_error_rate_percent;
    }
    taskEXIT_CRITICAL(&g_hashrate_mux);
}

static double hash_counter_to_ghs(uint64_t duration_us, uint32_t counter)
{
    if (duration_us == 0) {
        return 0.0;
    }
    const double seconds = (double)duration_us / 1000000.0;
    return ((double)counter * HASH_COUNTER_LSB) / seconds / 1000000000.0;
}

static void update_asic_hash_counter(asic_hash_counter_t *counter, uint32_t value,
                                     uint64_t timestamp_us)
{
    if (counter == NULL) {
        return;
    }
    if (counter->timestamp_us != 0 && timestamp_us > counter->timestamp_us) {
        counter->hashrate_ghs =
            hash_counter_to_ghs(timestamp_us - counter->timestamp_us, value - counter->value);
        counter->valid = true;
    }
    counter->value = value;
    counter->timestamp_us = timestamp_us;
}

static void update_asic_hashrate_totals_locked(uint64_t timestamp_us)
{
    double total_ghs = 0.0;
    double error_ghs = 0.0;

    for (size_t i = 0; i < g_asic_hashrate_counter_count; ++i) {
        if (g_asic_total_counters[i].valid) {
            total_ghs += g_asic_total_counters[i].hashrate_ghs;
        }
        if (g_asic_error_counters[i].valid) {
            error_ghs += g_asic_error_counters[i].hashrate_ghs;
        }
    }

    g_asic_hashrate_total_ghs = total_ghs;
    g_asic_hashrate_error_ghs = error_ghs > total_ghs ? total_ghs : error_ghs;
    g_asic_error_rate_percent =
        total_ghs > 0.0 ? (g_asic_hashrate_error_ghs / total_ghs) * 100.0 : 0.0;
    if (total_ghs > 0.0) {
        g_asic_hashrate_updated_us = timestamp_us;
    }
}

static int domain_index_for_register(register_type_t register_type)
{
    switch (register_type) {
    case REGISTER_DOMAIN_0_COUNT:
        return 0;
    case REGISTER_DOMAIN_1_COUNT:
        return 1;
    case REGISTER_DOMAIN_2_COUNT:
        return 2;
    case REGISTER_DOMAIN_3_COUNT:
        return 3;
    default:
        return -1;
    }
}

static void record_asic_register_read(register_type_t register_type, uint8_t asic_nr,
                                      uint32_t value, uint64_t timestamp_us)
{
    const int domain_index = domain_index_for_register(register_type);
    if (register_type != REGISTER_TOTAL_COUNT && register_type != REGISTER_ERROR_COUNT &&
        domain_index < 0) {
        return;
    }
    if (g_asic_total_counters == NULL || g_asic_error_counters == NULL ||
        g_asic_domain_counters == NULL ||
        asic_nr >= g_asic_hashrate_counter_count) {
        return;
    }
    if (timestamp_us == 0) {
        timestamp_us = (uint64_t)esp_timer_get_time();
    }

    taskENTER_CRITICAL(&g_asic_hashrate_mux);
    if (register_type == REGISTER_TOTAL_COUNT) {
        update_asic_hash_counter(&g_asic_total_counters[asic_nr], value, timestamp_us);
        update_asic_hashrate_totals_locked(timestamp_us);
    } else if (register_type == REGISTER_ERROR_COUNT) {
        update_asic_hash_counter(&g_asic_error_counters[asic_nr], value, timestamp_us);
        update_asic_hashrate_totals_locked(timestamp_us);
    } else {
        update_asic_hash_counter(
            &g_asic_domain_counters[(asic_nr * ASIC_HASH_DOMAIN_COUNT) + domain_index], value,
            timestamp_us);
    }
    taskEXIT_CRITICAL(&g_asic_hashrate_mux);
}

static void record_domain_hashrate_sample(uint64_t min_timestamp_us)
{
    double domain_total_ghs = 0.0;
    size_t valid_domains = 0;

    taskENTER_CRITICAL(&g_asic_hashrate_mux);
    for (size_t asic = 0; asic < g_asic_hashrate_counter_count; ++asic) {
        for (size_t domain = 0; domain < ASIC_HASH_DOMAIN_COUNT; ++domain) {
            const asic_hash_counter_t *counter =
                &g_asic_domain_counters[(asic * ASIC_HASH_DOMAIN_COUNT) + domain];
            if (counter->valid && counter->timestamp_us >= min_timestamp_us) {
                domain_total_ghs += counter->hashrate_ghs;
                ++valid_domains;
            }
        }
    }

    const size_t expected_domains = g_asic_hashrate_counter_count * ASIC_HASH_DOMAIN_COUNT;
    if (expected_domains > 0 && valid_domains == expected_domains) {
        const size_t index = g_domain_hashrate_sample_next;
        if (g_domain_hashrate_sample_count == ASIC_DOMAIN_HASHRATE_SAMPLE_COUNT) {
            g_domain_hashrate_sample_sum -= g_domain_hashrate_samples[index];
        } else {
            ++g_domain_hashrate_sample_count;
        }
        g_domain_hashrate_samples[index] = domain_total_ghs;
        g_domain_hashrate_sample_sum += domain_total_ghs;
        g_domain_hashrate_sample_next =
            (g_domain_hashrate_sample_next + 1) % ASIC_DOMAIN_HASHRATE_SAMPLE_COUNT;
        g_domain_hashrate_ghs =
            g_domain_hashrate_sample_sum / (double)g_domain_hashrate_sample_count;
        g_domain_hashrate_updated_us = (uint64_t)esp_timer_get_time();
    }
    taskEXIT_CRITICAL(&g_asic_hashrate_mux);
}

static void wait_for_recent_job_send(void)
{
    const uint64_t wait_until_us =
        (uint64_t)esp_timer_get_time() + ((uint64_t)ASIC_REGISTER_AFTER_JOB_WAIT_MS * 1000ULL);

    while (true) {
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        const uint64_t last_job_us = atomic_load(&g_last_job_sent_us);
        if (last_job_us != 0 && now_us >= last_job_us &&
            now_us - last_job_us <= ASIC_REGISTER_AFTER_JOB_WINDOW_US) {
            return;
        }
        if (now_us >= wait_until_us) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

static bool asic_hashrate_snapshot(double *effective_ghs, double *nominal_ghs,
                                   double *error_rate_percent)
{
    double total_ghs = 0.0;
    double error_ghs = 0.0;
    double error_percent = 0.0;
    uint64_t updated_us = 0;

    taskENTER_CRITICAL(&g_asic_hashrate_mux);
    total_ghs = g_asic_hashrate_total_ghs;
    error_ghs = g_asic_hashrate_error_ghs;
    error_percent = g_asic_error_rate_percent;
    updated_us = g_asic_hashrate_updated_us;
    taskEXIT_CRITICAL(&g_asic_hashrate_mux);

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (updated_us == 0 || now_us < updated_us || now_us - updated_us > ASIC_HASHRATE_STALE_US) {
        return false;
    }

    if (effective_ghs != NULL) {
        *effective_ghs = total_ghs > error_ghs ? total_ghs - error_ghs : 0.0;
    }
    if (nominal_ghs != NULL) {
        *nominal_ghs = total_ghs;
    }
    if (error_rate_percent != NULL) {
        *error_rate_percent = error_percent;
    }
    return true;
}

static bool domain_hashrate_snapshot(double *domain_ghs)
{
    double result = 0.0;
    uint64_t updated_us = 0;

    taskENTER_CRITICAL(&g_asic_hashrate_mux);
    result = g_domain_hashrate_ghs;
    updated_us = g_domain_hashrate_updated_us;
    taskEXIT_CRITICAL(&g_asic_hashrate_mux);

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    if (updated_us == 0 || now_us < updated_us || now_us - updated_us > ASIC_HASHRATE_STALE_US) {
        return false;
    }
    if (domain_ghs != NULL) {
        *domain_ghs = result;
    }
    return true;
}

static void domain_hashrate_values_snapshot(stratum_minimal_stats_t *out)
{
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    size_t asic_count = 0;

    taskENTER_CRITICAL(&g_asic_hashrate_mux);
    asic_count = g_asic_hashrate_counter_count;
    if (g_asic_domain_counters == NULL) {
        asic_count = 0;
    }
    if (asic_count > STRATUM_HASHRATE_MAX_ASICS) {
        asic_count = STRATUM_HASHRATE_MAX_ASICS;
    }
    out->domain_asic_count = (uint8_t)asic_count;
    out->domain_count = ASIC_HASH_DOMAIN_COUNT;
    for (size_t asic = 0; asic < asic_count; ++asic) {
        for (size_t domain = 0; domain < ASIC_HASH_DOMAIN_COUNT; ++domain) {
            const asic_hash_counter_t *counter =
                &g_asic_domain_counters[(asic * ASIC_HASH_DOMAIN_COUNT) + domain];
            if (counter->valid && now_us >= counter->timestamp_us &&
                now_us - counter->timestamp_us <= ASIC_HASHRATE_STALE_US) {
                out->domain_hashrates_ghs[asic][domain] = counter->hashrate_ghs;
            }
        }
    }
    taskEXIT_CRITICAL(&g_asic_hashrate_mux);
}

static esp_err_t init_asic_hashrate_monitor(size_t asic_count)
{
    if (g_asic_total_counters != NULL && g_asic_error_counters != NULL &&
        g_asic_domain_counters != NULL) {
        return ESP_OK;
    }
    if (asic_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    g_asic_total_counters = calloc(asic_count, sizeof(*g_asic_total_counters));
    g_asic_error_counters = calloc(asic_count, sizeof(*g_asic_error_counters));
    g_asic_domain_counters =
        calloc(asic_count * ASIC_HASH_DOMAIN_COUNT, sizeof(*g_asic_domain_counters));
    if (g_asic_total_counters == NULL || g_asic_error_counters == NULL ||
        g_asic_domain_counters == NULL) {
        free(g_asic_total_counters);
        free(g_asic_error_counters);
        free(g_asic_domain_counters);
        g_asic_total_counters = NULL;
        g_asic_error_counters = NULL;
        g_asic_domain_counters = NULL;
        g_asic_hashrate_counter_count = 0;
        return ESP_ERR_NO_MEM;
    }
    g_asic_hashrate_counter_count = asic_count;
    memset(g_domain_hashrate_samples, 0, sizeof(g_domain_hashrate_samples));
    g_domain_hashrate_sample_sum = 0.0;
    g_domain_hashrate_sample_next = 0;
    g_domain_hashrate_sample_count = 0;
    g_domain_hashrate_ghs = 0.0;
    g_domain_hashrate_updated_us = 0;
    return ESP_OK;
}

static void asic_hashrate_monitor_task(void *arg)
{
    (void)arg;

    while (true) {
        if (g_state != NULL && g_state->ASIC_initalized &&
            !g_state->SYSTEM_MODULE.hardware_fault && !atomic_load(&g_work_paused)) {
            wait_for_recent_job_send();
            const uint64_t read_started_us = (uint64_t)esp_timer_get_time();
            BM1370_read_registers();
            vTaskDelay(pdMS_TO_TICKS(ASIC_HASHRATE_SETTLE_MS));
            record_domain_hashrate_sample(read_started_us);
        }
        vTaskDelay(pdMS_TO_TICKS(ASIC_HASHRATE_POLL_MS));
    }
}

static void record_best_diff(double diff)
{
    bool new_best = false;

    taskENTER_CRITICAL(&g_best_diff_mux);
    if (diff > g_best_diff) {
        g_best_diff = diff;
        if (g_state != NULL) {
            g_state->SYSTEM_MODULE.best_session_nonce_diff = (uint64_t)diff;
        }
        new_best = true;
    }
    taskEXIT_CRITICAL(&g_best_diff_mux);

    if (new_best) {
        esp_err_t err = m45_config_set_best_diff(diff);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "failed to persist best diff %.2f: %s", diff, esp_err_to_name(err));
        }
    }
}

static double current_best_diff(void)
{
    double result = 0.0;

    taskENTER_CRITICAL(&g_best_diff_mux);
    result = g_best_diff;
    taskEXIT_CRITICAL(&g_best_diff_mux);

    return result;
}

static void record_block_alert(double diff)
{
    taskENTER_CRITICAL(&g_block_alert_mux);
    if (diff > g_block_alert_diff) {
        g_block_alert_diff = diff;
    }
    taskEXIT_CRITICAL(&g_block_alert_mux);
    atomic_store(&g_block_alert_active, true);
}

static double current_block_alert_diff(void)
{
    double result = 0.0;
    taskENTER_CRITICAL(&g_block_alert_mux);
    result = g_block_alert_diff;
    taskEXIT_CRITICAL(&g_block_alert_mux);
    return result;
}

static int bm1370_job_interval_ms(void);

#ifdef M45_ASIC_LOSS_METRICS
static void metric_inc(atomic_ullong *counter)
{
    atomic_fetch_add(counter, 1ULL);
}

static void metric_add(atomic_ullong *counter, uint64_t value)
{
    atomic_fetch_add(counter, (unsigned long long)value);
}

static void metric_max(atomic_ullong *counter, uint64_t value)
{
    unsigned long long current = atomic_load(counter);
    while (value > current &&
           !atomic_compare_exchange_weak(counter, &current, (unsigned long long)value)) {
    }
}

static void metric_record_dispatch_late(uint64_t late_us, uint64_t interval_us)
{
    if (late_us == 0) {
        return;
    }
    metric_inc(&g_metric_dispatch_late_count);
    metric_add(&g_metric_dispatch_late_total_us, late_us);
    metric_max(&g_metric_dispatch_late_max_us, late_us);
    if (interval_us > 0) {
        metric_add(&g_metric_dispatch_missed_slots, late_us / interval_us);
    }
}

static void metric_record_job_build(uint64_t duration_us)
{
    metric_add(&g_metric_job_build_total_us, duration_us);
    metric_max(&g_metric_job_build_max_us, duration_us);
}

static void metric_record_job_send(uint64_t duration_us)
{
    metric_inc(&g_metric_job_sent);
    metric_add(&g_metric_job_send_total_us, duration_us);
    metric_max(&g_metric_job_send_max_us, duration_us);
}

static void metric_record_rx_wait(uint64_t duration_us, bool timeout_like)
{
    metric_inc(&g_metric_rx_calls);
    metric_add(&g_metric_rx_wait_total_us, duration_us);
    metric_max(&g_metric_rx_wait_max_us, duration_us);
    if (timeout_like) {
        metric_inc(&g_metric_rx_timeouts);
    }
}
#else
#define metric_inc(counter) ((void)0)
#define metric_record_dispatch_late(late_us, interval_us) ((void)0)
#define metric_record_job_build(duration_us) ((void)0)
#define metric_record_job_send(duration_us) ((void)0)
#define metric_record_rx_wait(duration_us, timeout_like) ((void)0)
#endif

static void atomic_max_u64(atomic_ullong *target, uint64_t value)
{
    unsigned long long current = atomic_load(target);
    while (value > current &&
           !atomic_compare_exchange_weak(target, &current, (unsigned long long)value)) {
    }
}

static void record_share_submit_timing(uint64_t submit_us, uint64_t write_us)
{
    atomic_store(&g_share_submit_us, (unsigned long long)submit_us);
    atomic_store(&g_share_write_us, (unsigned long long)write_us);
    atomic_max_u64(&g_share_submit_max_us, submit_us);
    atomic_max_u64(&g_share_write_max_us, write_us);
}

static int next_uid(void)
{
    taskENTER_CRITICAL(&g_uid_mux);
    const int uid = g_next_uid++;
    taskEXIT_CRITICAL(&g_uid_mux);
    return uid;
}

static void mark_share_request(int id)
{
    taskENTER_CRITICAL(&g_share_id_mux);
    g_pending_share_ids[g_pending_share_id_next++ % SHARE_ID_SLOTS] = id;
    taskEXIT_CRITICAL(&g_share_id_mux);
}

static void mark_response_request(int id)
{
    if (id < 0) {
        return;
    }

    taskENTER_CRITICAL(&g_response_id_mux);
    const size_t slot = g_pending_response_id_next++ % RESPONSE_ID_SLOTS;
    g_pending_response_ids[slot] = id;
    g_pending_response_us[slot] = (uint64_t)esp_timer_get_time();
    taskEXIT_CRITICAL(&g_response_id_mux);
}

static bool take_response_request_ms(int id, uint32_t *elapsed_ms)
{
    bool found = false;
    uint64_t sent_us = 0;

    taskENTER_CRITICAL(&g_response_id_mux);
    for (size_t i = 0; i < RESPONSE_ID_SLOTS; ++i) {
        if (g_pending_response_ids[i] == id) {
            sent_us = g_pending_response_us[i];
            g_pending_response_ids[i] = 0;
            g_pending_response_us[i] = 0;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_response_id_mux);

    if (!found || sent_us == 0) {
        return false;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t delta_ms = now_us > sent_us ? (now_us - sent_us) / 1000ULL : 0;
    if (elapsed_ms != NULL) {
        *elapsed_ms = delta_ms > UINT32_MAX ? UINT32_MAX : (uint32_t)delta_ms;
    }
    return true;
}

static bool take_share_request(int id)
{
    bool found = false;

    taskENTER_CRITICAL(&g_share_id_mux);
    for (size_t i = 0; i < SHARE_ID_SLOTS; ++i) {
        if (g_pending_share_ids[i] == id) {
            g_pending_share_ids[i] = 0;
            found = true;
            break;
        }
    }
    taskEXIT_CRITICAL(&g_share_id_mux);

    return found;
}

static void set_transport(esp_transport_handle_t transport)
{
    if (g_transport_lock == NULL) {
        g_transport = transport;
        return;
    }

    xSemaphoreTake(g_transport_lock, portMAX_DELAY);
    g_transport = transport;
    xSemaphoreGive(g_transport_lock);
}

static bool stratum_runtime_ready(void)
{
    return g_work_queue != NULL && g_transport_lock != NULL && g_work_reset_lock != NULL;
}

static void drain_work_queue(void)
{
    if (g_work_queue == NULL) {
        return;
    }

    queued_work_t old = {0};
    while (xQueueReceive(g_work_queue, &old, 0) == pdPASS) {
        free_mining_notify(old.work);
    }
}

static uint32_t reset_work_state(bool queue_marker)
{
    if (g_work_reset_lock != NULL) {
        xSemaphoreTake(g_work_reset_lock, portMAX_DELAY);
    }

    const uint32_t epoch = atomic_fetch_add(&g_work_epoch, 1) + 1;
    drain_work_queue();
    if (g_state != NULL) {
        bitaxe_gamma602_clear_jobs(g_state);
    }

    if (queue_marker && g_work_queue != NULL) {
        queued_work_t marker = {
            .work = NULL,
            .epoch = epoch,
        };
        if (xQueueSend(g_work_queue, &marker, 0) != pdPASS) {
            ESP_LOGW(TAG, "failed to queue work reset marker");
        }
    }

    if (g_work_reset_lock != NULL) {
        xSemaphoreGive(g_work_reset_lock);
    }

    return epoch;
}

static void stratum_set_disconnected(bool clear_work)
{
    const bool was_connected = atomic_exchange(&g_connected, false);
    atomic_store(&g_connected_since_us, 0);
    set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);
    if (g_state != NULL) {
        g_state->extranonce_2_len = 0;
    }
    if (clear_work && was_connected) {
        reset_work_state(true);
    }
}

static bool stratum_note_current_block(const char *prev_block_hash)
{
    if (prev_block_hash == NULL || prev_block_hash[0] == '\0') {
        return false;
    }
    if (strcmp(prev_block_hash, g_current_block_hash) == 0) {
        return false;
    }

    const bool had_block = g_current_block_hash[0] != '\0';
    strlcpy(g_current_block_hash, prev_block_hash, sizeof(g_current_block_hash));
    atomic_fetch_add(&g_current_block_seq, 1);
    if (had_block) {
        STRATUM_LOGI("new block %s", g_current_block_hash);
    }
    return had_block;
}

void stratum_minimal_reconnect(void)
{
    if (!stratum_runtime_ready()) {
        ESP_LOGW(TAG, "stratum reconnect ignored before runtime start");
        return;
    }

    atomic_store(&g_next_using_backup_pool, false);
    atomic_store(&g_switch_to_primary_requested, false);
    set_active_primary_pool_endpoint();
    reset_work_state(true);
    ESP_LOGI(TAG, "cleared queued work for reconnect");

    request_stratum_transport_close(false);
}

void stratum_minimal_pause_work(void)
{
    if (!stratum_runtime_ready()) {
        return;
    }

    atomic_store(&g_work_paused, true);
    reset_hashrate_window();
    if (g_work_reset_lock != NULL) {
        xSemaphoreTake(g_work_reset_lock, portMAX_DELAY);
    }
    if (g_state != NULL) {
        bitaxe_gamma602_clear_jobs(g_state);
    }
    if (g_work_reset_lock != NULL) {
        xSemaphoreGive(g_work_reset_lock);
    }
}

void stratum_minimal_resume_work(void)
{
    if (!stratum_runtime_ready()) {
        return;
    }

    if (g_work_reset_lock != NULL) {
        xSemaphoreTake(g_work_reset_lock, portMAX_DELAY);
    }
    if (g_state != NULL) {
        bitaxe_gamma602_clear_jobs(g_state);
    }
    if (g_work_reset_lock != NULL) {
        xSemaphoreGive(g_work_reset_lock);
    }
    atomic_store(&g_work_paused, false);
}

bool stratum_minimal_work_paused(void)
{
    return atomic_load(&g_work_paused);
}

bool stratum_minimal_started(void)
{
    return stratum_runtime_ready();
}

bool stratum_minimal_connected(void)
{
    return atomic_load(&g_connected);
}

uint32_t stratum_minimal_job_sent_count(void)
{
    return atomic_load(&g_job_sent);
}

uint32_t stratum_minimal_valid_nonce_count(void)
{
    return atomic_load(&g_valid_nonces);
}

static stratum_endpoint_t g_primary_probe_args;

static void stratum_primary_probe_task(void *arg)
{
    stratum_endpoint_t probe = {0};
    memcpy(&probe, arg, sizeof(probe));

    bool reachable = false;
    esp_transport_handle_t transport = esp_transport_tcp_init();
    if (transport != NULL) {
        char connect_host[M45_POOL_HOST_MAX + 1];
        stratum_connect_host_for_endpoint(&probe, connect_host, sizeof(connect_host));
        if (esp_transport_connect(transport, connect_host, probe.port,
                                  TRANSPORT_TIMEOUT_MS) >= 0) {
            stratum_enable_tcp_nodelay(transport);
            reachable = true;
            esp_transport_close(transport);
        }
        esp_transport_destroy(transport);
    }

    if (reachable) {
        stratum_endpoint_t active = {0};
        const m45_config_t *config = m45_config_get();
        get_active_pool_endpoint(&active);
        if (wifi_http_connected() && active.using_backup &&
            strcmp(config->pool_host, probe.host) == 0 && config->pool_port == probe.port) {
            atomic_store(&g_switch_to_primary_requested, true);
            atomic_store(&g_next_using_backup_pool, false);
            request_stratum_transport_close(true);
            ESP_LOGI(TAG, "primary pool is reachable again; switching from backup");
        }
    }

    atomic_store(&g_primary_probe_in_progress, false);
    vTaskDelete(NULL);
}

static void stratum_maybe_probe_primary_pool(void)
{
    stratum_endpoint_t active = {0};
    get_active_pool_endpoint(&active);
    if (!wifi_http_connected() || !atomic_load(&g_connected) || !active.using_backup ||
        atomic_load(&g_primary_probe_in_progress)) {
        return;
    }

    const m45_config_t *config = m45_config_get();
    if (config->pool_host[0] == '\0' || config->pool_port == 0) {
        return;
    }

    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    const uint64_t last_probe_us = atomic_load(&g_last_primary_probe_us);
    if (last_probe_us != 0 &&
        now_us - last_probe_us < (uint64_t)STRATUM_PRIMARY_PROBE_INTERVAL_MS * 1000ULL) {
        return;
    }

    atomic_store(&g_last_primary_probe_us, now_us);
    copy_pool_host(g_primary_probe_args.host, sizeof(g_primary_probe_args.host),
                   config->pool_host);
    copy_pool_ip(g_primary_probe_args.cached_ip, sizeof(g_primary_probe_args.cached_ip),
                 config->pool_ip);
    g_primary_probe_args.port = config->pool_port;
    g_primary_probe_args.using_backup = false;
    atomic_store(&g_primary_probe_in_progress, true);
    if (xTaskCreate(stratum_primary_probe_task, "stratum_probe", 3072,
                    &g_primary_probe_args, 4, NULL) != pdPASS) {
        atomic_store(&g_primary_probe_in_progress, false);
        ESP_LOGW(TAG, "failed to start primary pool probe");
    }
}

static void stratum_enable_tcp_nodelay(esp_transport_handle_t transport)
{
    const int sock = esp_transport_get_socket(transport);
    if (sock < 0) {
        ESP_LOGW(TAG, "TCP_NODELAY skipped; no socket");
        return;
    }

    const int enabled = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
        ESP_LOGW(TAG, "TCP_NODELAY failed errno=%d", errno);
    }
}

static int stratum_writef(esp_transport_handle_t transport, const char *fmt, ...)
{
    char msg[STRATUM_BUFFER_SIZE];
    va_list args;

    va_start(args, fmt);
    const int len = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (len < 0 || len >= (int)sizeof(msg)) {
        ESP_LOGE(TAG, "stratum message format overflow");
        return -1;
    }

    char *newline = strchr(msg, '\n');
    if (newline != NULL) {
        STRATUM_LOGI("tx: %.*s", (int)(newline - msg), msg);
    } else {
        STRATUM_LOGI("tx: %s", msg);
    }

    return esp_transport_write(transport, msg, len, TRANSPORT_TIMEOUT_MS);
}

static int stratum_write_message(esp_transport_handle_t transport, const char *msg)
{
    const size_t len = msg != NULL ? strlen(msg) : 0;
    if (len == 0 || len >= STRATUM_BUFFER_SIZE) {
        ESP_LOGE(TAG, "stratum message invalid length");
        return -1;
    }

    char *newline = strchr(msg, '\n');
    if (newline != NULL) {
        STRATUM_LOGI("tx: %.*s", (int)(newline - msg), msg);
    } else {
        STRATUM_LOGI("tx: %s", msg);
    }

    return esp_transport_write(transport, msg, (int)len, TRANSPORT_TIMEOUT_MS);
}

static bool stratum_append_text(char *dest, size_t dest_size, size_t *offset,
                                const char *text)
{
    if (dest_size == 0 || offset == NULL || *offset >= dest_size) {
        return false;
    }
    if (text == NULL) {
        text = "";
    }

    const size_t len = strlen(text);
    if (len >= dest_size - *offset) {
        return false;
    }
    memcpy(dest + *offset, text, len);
    *offset += len;
    dest[*offset] = '\0';
    return true;
}

static bool stratum_append_raw(char *dest, size_t dest_size, size_t *offset,
                               const char *text, size_t len)
{
    if (dest_size == 0 || offset == NULL || *offset >= dest_size || text == NULL) {
        return false;
    }
    if (len >= dest_size - *offset) {
        return false;
    }
    memcpy(dest + *offset, text, len);
    *offset += len;
    dest[*offset] = '\0';
    return true;
}

static bool stratum_append_int(char *dest, size_t dest_size, size_t *offset, int value)
{
    char text[16];
    snprintf(text, sizeof(text), "%d", value);
    return stratum_append_text(dest, dest_size, offset, text);
}

static bool stratum_append_json_string(char *dest, size_t dest_size, size_t *offset,
                                       const char *text);

#if M45_STRATUM_FAST_PATHS
static bool stratum_append_json_string_fast(char *dest, size_t dest_size, size_t *offset,
                                            const char *text)
{
    if (text == NULL) {
        text = "";
    }

    const char *cursor = text;
    while (*cursor != '\0') {
        const unsigned char ch = (unsigned char)*cursor++;
        if (ch < 0x20 || ch == '"' || ch == '\\') {
            return stratum_append_json_string(dest, dest_size, offset, text);
        }
    }

    const size_t len = strlen(text);
    return stratum_append_raw(dest, dest_size, offset, "\"", 1) &&
           stratum_append_raw(dest, dest_size, offset, text, len) &&
           stratum_append_raw(dest, dest_size, offset, "\"", 1);
}
#else
#define stratum_append_json_string_fast stratum_append_json_string
#endif

static bool stratum_append_json_string(char *dest, size_t dest_size, size_t *offset,
                                       const char *text)
{
    static const char hex[] = "0123456789abcdef";
    if (!stratum_append_text(dest, dest_size, offset, "\"")) {
        return false;
    }
    if (text == NULL) {
        text = "";
    }

    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0';
         ++cursor) {
        char escape[7] = {0};
        switch (*cursor) {
        case '"':
            if (!stratum_append_text(dest, dest_size, offset, "\\\"")) {
                return false;
            }
            break;
        case '\\':
            if (!stratum_append_text(dest, dest_size, offset, "\\\\")) {
                return false;
            }
            break;
        case '\b':
            if (!stratum_append_text(dest, dest_size, offset, "\\b")) {
                return false;
            }
            break;
        case '\f':
            if (!stratum_append_text(dest, dest_size, offset, "\\f")) {
                return false;
            }
            break;
        case '\n':
            if (!stratum_append_text(dest, dest_size, offset, "\\n")) {
                return false;
            }
            break;
        case '\r':
            if (!stratum_append_text(dest, dest_size, offset, "\\r")) {
                return false;
            }
            break;
        case '\t':
            if (!stratum_append_text(dest, dest_size, offset, "\\t")) {
                return false;
            }
            break;
        default:
            if (*cursor < 0x20) {
                escape[0] = '\\';
                escape[1] = 'u';
                escape[2] = '0';
                escape[3] = '0';
                escape[4] = hex[*cursor >> 4];
                escape[5] = hex[*cursor & 0x0f];
                if (!stratum_append_text(dest, dest_size, offset, escape)) {
                    return false;
                }
            } else {
                if (*offset + 1 >= dest_size) {
                    return false;
                }
                dest[(*offset)++] = (char)*cursor;
                dest[*offset] = '\0';
            }
            break;
        }
    }
    return stratum_append_text(dest, dest_size, offset, "\"");
}

static void stratum_build_miner_info(char *out, size_t out_size)
{
    snprintf(out, out_size, "%s/%.*s%s", STRATUM_MINER_MODEL,
             STRATUM_MINER_VERSION_CHARS, APP_BUILD_GIT_SHA,
             APP_BUILD_DIRTY ? "-dirty" : "");
}

static int stratum_send_authorize(esp_transport_handle_t transport)
{
    char msg[STRATUM_BUFFER_SIZE];
    size_t offset = 0;
    msg[0] = '\0';

    if (!stratum_append_text(msg, sizeof(msg), &offset, "{\"id\":") ||
        !stratum_append_int(msg, sizeof(msg), &offset, STRATUM_ID_AUTHORIZE) ||
        !stratum_append_text(msg, sizeof(msg), &offset,
                             ",\"method\":\"mining.authorize\",\"params\":[") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset,
                                         g_state->SYSTEM_MODULE.pool_user) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset,
                                         g_state->SYSTEM_MODULE.pool_pass) ||
        !stratum_append_text(msg, sizeof(msg), &offset, "]}\n")) {
        ESP_LOGE(TAG, "stratum authorize format overflow");
        return -1;
    }

    return stratum_write_message(transport, msg);
}

static esp_err_t rx_buffer_reserve(size_t needed)
{
    if (needed > STRATUM_MAX_LINE_SIZE + 1U) {
        return ESP_ERR_INVALID_SIZE;
    }

    if (needed <= g_rx_buffer_size) {
        return ESP_OK;
    }

    size_t new_size = g_rx_buffer_size == 0 ? STRATUM_BUFFER_SIZE : g_rx_buffer_size;
    while (new_size < needed) {
        new_size += STRATUM_BUFFER_SIZE;
    }

    char *new_buffer = realloc(g_rx_buffer, new_size);
    if (new_buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    g_rx_buffer = new_buffer;
    g_rx_buffer_size = new_size;
    return ESP_OK;
}

static char *receive_jsonrpc_line(esp_transport_handle_t transport, size_t *consumed_len)
{
    if (rx_buffer_reserve(STRATUM_BUFFER_SIZE) != ESP_OK) {
        return NULL;
    }
    if (consumed_len != NULL) {
        *consumed_len = 0;
    }

    uint64_t last_activity_us = (uint64_t)esp_timer_get_time();
    while (memchr(g_rx_buffer, '\n', g_rx_buffer_len) == NULL) {
        char recv_buffer[STRATUM_BUFFER_SIZE];
        const int nbytes = esp_transport_read(transport, recv_buffer, sizeof(recv_buffer),
                                              TRANSPORT_TIMEOUT_MS);
        if (nbytes < 0) {
            ESP_LOGE(TAG, "stratum read failed: %d", nbytes);
            g_rx_buffer_len = 0;
            return NULL;
        }
        if (nbytes == 0) {
            stratum_maybe_probe_primary_pool();
            if (atomic_load(&g_switch_to_primary_requested) || !wifi_http_connected()) {
                return NULL;
            }
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            if (now_us - last_activity_us >
                (uint64_t)STRATUM_IDLE_TIMEOUT_MS * 1000ULL) {
                ESP_LOGW(TAG, "stratum idle timeout");
                g_rx_buffer_len = 0;
                return NULL;
            }
            continue;
        }
        last_activity_us = (uint64_t)esp_timer_get_time();

        esp_err_t reserve_err = rx_buffer_reserve(g_rx_buffer_len + (size_t)nbytes + 1);
        if (reserve_err != ESP_OK) {
            ESP_LOGE(TAG, "stratum rx buffer failed: %s", esp_err_to_name(reserve_err));
            g_rx_buffer_len = 0;
            return NULL;
        }

        memcpy(g_rx_buffer + g_rx_buffer_len, recv_buffer, (size_t)nbytes);
        g_rx_buffer_len += (size_t)nbytes;
        g_rx_buffer[g_rx_buffer_len] = '\0';
    }

    char *newline = memchr(g_rx_buffer, '\n', g_rx_buffer_len);
    size_t line_len = (size_t)(newline - g_rx_buffer);
    const size_t consumed = line_len + 1U;
    if (line_len > 0 && g_rx_buffer[line_len - 1] == '\r') {
        --line_len;
    }

    g_rx_buffer[line_len] = '\0';
    if (consumed_len != NULL) {
        *consumed_len = consumed;
    }
    return g_rx_buffer;
}

static void consume_jsonrpc_line(size_t consumed)
{
    if (g_rx_buffer == NULL || consumed == 0) {
        return;
    }
    if (consumed > g_rx_buffer_len) {
        g_rx_buffer_len = 0;
        g_rx_buffer[0] = '\0';
        return;
    }

    const size_t remaining = g_rx_buffer_len - consumed;
    memmove(g_rx_buffer, g_rx_buffer + consumed, remaining);
    g_rx_buffer_len = remaining;
    g_rx_buffer[g_rx_buffer_len] = '\0';
}

static bool payout_read_bytes(coinbase_reader_t *reader, size_t len, const uint8_t **out)
{
    if (reader->offset + len > reader->len) {
        return false;
    }
    *out = reader->bytes + reader->offset;
    reader->offset += len;
    return true;
}

static bool payout_skip_bytes(coinbase_reader_t *reader, size_t len)
{
    const uint8_t *ignored = NULL;
    return payout_read_bytes(reader, len, &ignored);
}

static bool payout_read_u64_le(coinbase_reader_t *reader, uint64_t *out)
{
    const uint8_t *bytes = NULL;
    if (!payout_read_bytes(reader, 8, &bytes)) {
        return false;
    }

    uint64_t value = 0;
    for (int i = 7; i >= 0; --i) {
        value = (value << 8) | bytes[i];
    }
    *out = value;
    return true;
}

static bool payout_read_varint(coinbase_reader_t *reader, uint64_t *out)
{
    const uint8_t *first = NULL;
    if (!payout_read_bytes(reader, 1, &first)) {
        return false;
    }
    if (*first < 0xfd) {
        *out = *first;
        return true;
    }

    size_t byte_count = *first == 0xfd ? 2 : (*first == 0xfe ? 4 : 8);
    const uint8_t *bytes = NULL;
    if (!payout_read_bytes(reader, byte_count, &bytes)) {
        return false;
    }

    uint64_t value = 0;
    for (size_t i = 0; i < byte_count; ++i) {
        value |= (uint64_t)bytes[i] << (8U * i);
    }
    *out = value;
    return true;
}

static int payout_base58_value(char ch)
{
    static const char *alphabet =
        "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz";
    const char *found = strchr(alphabet, ch);
    return found == NULL ? -1 : (int)(found - alphabet);
}

static bool payout_base58check_decode(const char *text, uint8_t *out, size_t *out_len)
{
    uint8_t number[STRATUM_BASE58_DECODE_MAX] = {0};
    size_t number_len = 1;
    size_t leading_zeroes = 0;

    while (text[leading_zeroes] == '1') {
        ++leading_zeroes;
    }
    for (const char *cursor = text; *cursor != '\0'; ++cursor) {
        const int digit = payout_base58_value(*cursor);
        if (digit < 0) {
            return false;
        }
        uint32_t carry = (uint32_t)digit;
        for (size_t i = 0; i < number_len; ++i) {
            const size_t index = number_len - 1U - i;
            carry += (uint32_t)number[index] * 58U;
            number[index] = (uint8_t)(carry & 0xffU);
            carry >>= 8;
        }
        while (carry > 0) {
            if (number_len >= sizeof(number)) {
                return false;
            }
            memmove(number + 1, number, number_len);
            number[0] = (uint8_t)(carry & 0xffU);
            ++number_len;
            carry >>= 8;
        }
    }

    size_t first_nonzero = 0;
    while (first_nonzero < number_len && number[first_nonzero] == 0) {
        ++first_nonzero;
    }
    const size_t decoded_len = leading_zeroes + number_len - first_nonzero;
    if (decoded_len < 5 || decoded_len > STRATUM_BASE58_DECODE_MAX) {
        return false;
    }

    memset(out, 0, leading_zeroes);
    memcpy(out + leading_zeroes, number + first_nonzero, number_len - first_nonzero);
    uint8_t checksum[32];
    double_sha256_bin(out, decoded_len - 4U, checksum);
    if (memcmp(checksum, out + decoded_len - 4U, 4) != 0) {
        return false;
    }
    *out_len = decoded_len - 4U;
    return true;
}

static int payout_bech32_value(char ch)
{
    static const char *charset = "qpzry9x8gf2tvdw0s3jn54khce6mua7l";
    const char *found = strchr(charset, ch);
    return found == NULL ? -1 : (int)(found - charset);
}

static uint32_t payout_bech32_polymod_step(uint32_t chk, uint8_t value)
{
    static const uint32_t generator[5] = {
        0x3b6a57b2UL, 0x26508e6dUL, 0x1ea119faUL, 0x3d4233ddUL, 0x2a1462b3UL,
    };
    const uint8_t top = (uint8_t)(chk >> 25);
    chk = ((chk & 0x1ffffffUL) << 5) ^ value;
    for (uint8_t i = 0; i < 5; ++i) {
        if (((top >> i) & 1U) != 0) {
            chk ^= generator[i];
        }
    }
    return chk;
}

static bool payout_bech32_convert_bits(const uint8_t *input, size_t input_len,
                                       uint8_t from_bits, uint8_t to_bits,
                                       bool pad, uint8_t *out, size_t *out_len)
{
    uint32_t accumulator = 0;
    uint8_t bits = 0;
    size_t offset = 0;
    const uint32_t max_value = (1U << to_bits) - 1U;

    for (size_t i = 0; i < input_len; ++i) {
        if ((input[i] >> from_bits) != 0) {
            return false;
        }
        accumulator = (accumulator << from_bits) | input[i];
        bits += from_bits;
        while (bits >= to_bits) {
            bits -= to_bits;
            if (offset >= *out_len) {
                return false;
            }
            out[offset++] = (uint8_t)((accumulator >> bits) & max_value);
        }
    }
    if (pad) {
        if (bits > 0) {
            if (offset >= *out_len) {
                return false;
            }
            out[offset++] = (uint8_t)((accumulator << (to_bits - bits)) & max_value);
        }
    } else if (bits >= from_bits || ((accumulator << (to_bits - bits)) & max_value) != 0) {
        return false;
    }
    *out_len = offset;
    return true;
}

static bool payout_bech32_decode_witness(const char *address, uint8_t *version,
                                         uint8_t *program, size_t *program_len)
{
    char lower[STRATUM_BECH32_DECODE_MAX + 1];
    const size_t len = strlen(address);
    if (len > STRATUM_BECH32_DECODE_MAX || len < 8) {
        return false;
    }

    bool has_lower = false;
    bool has_upper = false;
    size_t separator = SIZE_MAX;
    for (size_t i = 0; i < len; ++i) {
        const char ch = address[i];
        if (ch < 33 || ch > 126) {
            return false;
        }
        if (ch >= 'a' && ch <= 'z') {
            has_lower = true;
            lower[i] = ch;
        } else if (ch >= 'A' && ch <= 'Z') {
            has_upper = true;
            lower[i] = (char)(ch + ('a' - 'A'));
        } else {
            lower[i] = ch;
        }
        if (lower[i] == '1') {
            separator = i;
        }
    }
    lower[len] = '\0';
    if ((has_lower && has_upper) || separator == SIZE_MAX || separator < 1 ||
        separator + 7 > len) {
        return false;
    }
    if (!((separator == 2 && memcmp(lower, "bc", 2) == 0) ||
          (separator == 2 && memcmp(lower, "tb", 2) == 0))) {
        return false;
    }

    uint32_t polymod = 1;
    for (size_t i = 0; i < separator; ++i) {
        polymod = payout_bech32_polymod_step(polymod, (uint8_t)(lower[i] >> 5));
    }
    polymod = payout_bech32_polymod_step(polymod, 0);
    for (size_t i = 0; i < separator; ++i) {
        polymod = payout_bech32_polymod_step(polymod, (uint8_t)(lower[i] & 31));
    }

    uint8_t data[STRATUM_BECH32_DECODE_MAX];
    size_t data_len = 0;
    for (size_t i = separator + 1U; i < len; ++i) {
        const int value = payout_bech32_value(lower[i]);
        if (value < 0) {
            return false;
        }
        polymod = payout_bech32_polymod_step(polymod, (uint8_t)value);
        data[data_len++] = (uint8_t)value;
    }
    if (polymod != 1 && polymod != 0x2bc830a3UL) {
        return false;
    }
    if (data_len < 7 || data[0] > 16) {
        return false;
    }

    *version = data[0];
    *program_len = 40;
    if (!payout_bech32_convert_bits(data + 1, data_len - 7U, 5, 8,
                                    false, program, program_len)) {
        return false;
    }
    if (*program_len < 2 || *program_len > 40) {
        return false;
    }
    if (*version == 0 && polymod != 1) {
        return false;
    }
    if (*version > 0 && polymod != 0x2bc830a3UL) {
        return false;
    }
    return true;
}

static bool payout_wallet_script(const char *wallet, uint8_t *script, size_t *script_len)
{
    uint8_t decoded[STRATUM_BASE58_DECODE_MAX];
    size_t decoded_len = 0;

    if (payout_base58check_decode(wallet, decoded, &decoded_len) && decoded_len == 21) {
        if (decoded[0] == 0x00) {
            script[0] = 0x76;
            script[1] = 0xa9;
            script[2] = 0x14;
            memcpy(script + 3, decoded + 1, 20);
            script[23] = 0x88;
            script[24] = 0xac;
            *script_len = 25;
            return true;
        }
        if (decoded[0] == 0x05) {
            script[0] = 0xa9;
            script[1] = 0x14;
            memcpy(script + 2, decoded + 1, 20);
            script[22] = 0x87;
            *script_len = 23;
            return true;
        }
    }

    uint8_t witness_version = 0;
    uint8_t program[40];
    size_t program_len = sizeof(program);
    if (payout_bech32_decode_witness(wallet, &witness_version, program, &program_len)) {
        script[0] = witness_version == 0 ? 0x00 : (uint8_t)(0x50U + witness_version);
        script[1] = (uint8_t)program_len;
        memcpy(script + 2, program, program_len);
        *script_len = program_len + 2U;
        return true;
    }
    return false;
}

static bool payout_wallet_text_script(const char *wallet, uint8_t *script, size_t *script_len)
{
    if (wallet == NULL || wallet[0] == '\0') {
        return false;
    }
    if (payout_wallet_script(wallet, script, script_len)) {
        return true;
    }

    const char *worker = strchr(wallet, '.');
    if (worker == NULL || worker == wallet) {
        return false;
    }

    char address[M45_WALLET_ADDRESS_MAX + 1];
    const size_t address_len = (size_t)(worker - wallet);
    if (address_len >= sizeof(address)) {
        return false;
    }
    memcpy(address, wallet, address_len);
    address[address_len] = '\0';
    return payout_wallet_script(address, script, script_len);
}

static bool payout_scan_coinbase_outputs(const uint8_t *coinbase, size_t coinbase_len,
                                         const uint8_t *wallet_script,
                                         size_t wallet_script_len,
                                         uint64_t *total_value, uint64_t *wallet_value)
{
    coinbase_reader_t reader = {
        .bytes = coinbase,
        .len = coinbase_len,
        .offset = 0,
    };
    *total_value = 0;
    *wallet_value = 0;

    if (!payout_skip_bytes(&reader, 4)) {
        return false;
    }

    uint64_t input_count = 0;
    if (!payout_read_varint(&reader, &input_count)) {
        return false;
    }
    if (input_count == 0 && reader.offset + 1 <= reader.len &&
        coinbase[reader.offset] == 0x01) {
        ++reader.offset;
        if (!payout_read_varint(&reader, &input_count)) {
            return false;
        }
    }
    if (input_count == 0 || input_count > 8) {
        return false;
    }
    for (uint64_t input = 0; input < input_count; ++input) {
        uint64_t script_len = 0;
        if (!payout_skip_bytes(&reader, 36) ||
            !payout_read_varint(&reader, &script_len) ||
            script_len > coinbase_len ||
            !payout_skip_bytes(&reader, (size_t)script_len) ||
            !payout_skip_bytes(&reader, 4)) {
            return false;
        }
    }

    uint64_t output_count = 0;
    if (!payout_read_varint(&reader, &output_count) || output_count == 0 ||
        output_count > 64) {
        return false;
    }
    for (uint64_t output = 0; output < output_count; ++output) {
        uint64_t value = 0;
        uint64_t script_len = 0;
        const uint8_t *script = NULL;
        if (!payout_read_u64_le(&reader, &value) ||
            !payout_read_varint(&reader, &script_len) ||
            script_len > coinbase_len ||
            !payout_read_bytes(&reader, (size_t)script_len, &script)) {
            return false;
        }
        *total_value += value;
        if (script_len == wallet_script_len &&
            memcmp(script, wallet_script, wallet_script_len) == 0) {
            *wallet_value += value;
        }
    }
    return *total_value > 0;
}

static void pool_user_wallet(char *wallet, size_t wallet_size)
{
    if (wallet_size == 0) {
        return;
    }

    const char *pool_user = g_state != NULL && g_state->SYSTEM_MODULE.pool_user != NULL
                                ? g_state->SYSTEM_MODULE.pool_user
                                : "";
    size_t len = 0;
    while (pool_user[len] != '\0' && pool_user[len] != '.' && len + 1 < wallet_size) {
        wallet[len] = pool_user[len];
        ++len;
    }
    wallet[len] = '\0';
}

static void set_payout_status(uint8_t status, uint16_t percent_x100)
{
    atomic_store(&g_payout_status, status);
    atomic_store(&g_payout_percent_x100, percent_x100);
}

static void stratum_update_payout_from_coinbase(const uint8_t *coinbase, size_t coinbase_len)
{
    char wallet[M45_POOL_USER_MAX + 1];
    uint8_t wallet_script[42];
    size_t wallet_script_len = 0;
    pool_user_wallet(wallet, sizeof(wallet));
    set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);

    if (!payout_wallet_text_script(wallet, wallet_script, &wallet_script_len)) {
        set_payout_status(STRATUM_PAYOUT_STATUS_UNSUPPORTED_WALLET, 0);
        return;
    }

    uint64_t total_value = 0;
    uint64_t wallet_value = 0;
    if (!payout_scan_coinbase_outputs(coinbase, coinbase_len, wallet_script,
                                      wallet_script_len, &total_value, &wallet_value)) {
        set_payout_status(STRATUM_PAYOUT_STATUS_PARSE_ERROR, 0);
        return;
    }
    if (wallet_value == 0) {
        set_payout_status(STRATUM_PAYOUT_STATUS_MISSING, 0);
        return;
    }

    const uint64_t percent_x100 =
        ((wallet_value * 10000ULL) + (total_value / 2ULL)) / total_value;
    const uint16_t clamped =
        percent_x100 > UINT16_MAX ? UINT16_MAX : (uint16_t)percent_x100;
    set_payout_status(clamped < STRATUM_PAYOUT_MIN_PERCENT_X100
                          ? STRATUM_PAYOUT_STATUS_LOW
                          : STRATUM_PAYOUT_STATUS_OK,
                      clamped);
}

static void stratum_update_payout_from_notify(const mining_notify *work)
{
    if (work == NULL || g_state == NULL || g_state->extranonce_str == NULL ||
        (g_extranonce_len > 0 && g_extranonce_bin == NULL) ||
        g_state->extranonce_2_len <= 0 || g_state->extranonce_2_len > MAX_EXTRANONCE2_LEN) {
        set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);
        return;
    }

    const size_t coinbase_len = work->coinbase_1_len + g_extranonce_len +
                                (size_t)g_state->extranonce_2_len + work->coinbase_2_len;
    if (coinbase_len > STRATUM_MAX_COINBASE_BYTES) {
        set_payout_status(STRATUM_PAYOUT_STATUS_PARSE_ERROR, 0);
        return;
    }

    uint8_t *coinbase = malloc(coinbase_len);
    if (coinbase == NULL) {
        set_payout_status(STRATUM_PAYOUT_STATUS_PARSE_ERROR, 0);
        return;
    }

    size_t offset = 0;
    if (work->coinbase_1_len > 0) {
        memcpy(coinbase + offset, work->coinbase_1_bin, work->coinbase_1_len);
        offset += work->coinbase_1_len;
    }
    if (g_extranonce_len > 0) {
        memcpy(coinbase + offset, g_extranonce_bin, g_extranonce_len);
        offset += g_extranonce_len;
    }
    memset(coinbase + offset, 0, (size_t)g_state->extranonce_2_len);
    offset += (size_t)g_state->extranonce_2_len;
    if (work->coinbase_2_len > 0) {
        memcpy(coinbase + offset, work->coinbase_2_bin, work->coinbase_2_len);
    }

    stratum_update_payout_from_coinbase(coinbase, coinbase_len);
    free(coinbase);
}

static bool array_string(cJSON *array, int index, const char **out)
{
    cJSON *item = cJSON_GetArrayItem(array, index);
    if (!cJSON_IsString(item) || item->valuestring == NULL) {
        return false;
    }
    *out = item->valuestring;
    return true;
}

static int hex_value_checked(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static bool decode_hex_bytes(const char *hex, size_t hex_len, uint8_t *out,
                             size_t out_len)
{
    if (hex == NULL || (out_len > 0 && out == NULL) || hex_len != out_len * 2U) {
        return false;
    }

    for (size_t i = 0; i < out_len; ++i) {
        const int hi = hex_value_checked(hex[i * 2U]);
        const int lo = hex_value_checked(hex[(i * 2U) + 1U]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

static bool parse_hex_u32_text(const char *hex, size_t hex_len, uint32_t *out)
{
    if (hex == NULL || out == NULL || hex_len == 0 || hex_len > 8) {
        return false;
    }

    uint32_t value = 0;
    for (size_t i = 0; i < hex_len; ++i) {
        const int digit = hex_value_checked(hex[i]);
        if (digit < 0) {
            return false;
        }
        value = (value << 4) | (uint32_t)digit;
    }
    *out = value;
    return true;
}

static bool decode_hex_alloc_limited_span(const char *hex, size_t hex_len,
                                          size_t max_bytes, uint8_t **out,
                                          size_t *out_len)
{
    if (hex == NULL || out == NULL || out_len == NULL) {
        return false;
    }
    if ((hex_len % 2U) != 0) {
        return false;
    }

    const size_t bin_len = hex_len / 2U;
    if (bin_len > max_bytes) {
        return false;
    }

    uint8_t *bin = NULL;
    if (bin_len > 0) {
        bin = malloc(bin_len);
        if (bin == NULL) {
            return false;
        }
        if (!decode_hex_bytes(hex, hex_len, bin, bin_len)) {
            free(bin);
            return false;
        }
    }

    *out = bin;
    *out_len = bin_len;
    return true;
}

static bool decode_hex_alloc_limited(const char *hex, size_t max_bytes, uint8_t **out,
                                     size_t *out_len)
{
    return decode_hex_alloc_limited_span(hex, strlen(hex), max_bytes, out, out_len);
}

static mining_notify *parse_mining_notify(cJSON *params)
{
    if (!cJSON_IsArray(params) || cJSON_GetArraySize(params) < 8) {
        ESP_LOGE(TAG, "invalid mining.notify params");
        return NULL;
    }

    cJSON *merkle_branch = cJSON_GetArrayItem(params, 4);
    if (!cJSON_IsArray(merkle_branch)) {
        ESP_LOGE(TAG, "invalid mining.notify merkle branch");
        return NULL;
    }

    const int branch_count = cJSON_GetArraySize(merkle_branch);
    if (branch_count < 0 || branch_count > MAX_MERKLE_BRANCHES) {
        ESP_LOGE(TAG, "too many merkle branches: %d", branch_count);
        return NULL;
    }

    const char *job_id = NULL;
    const char *prev_block_hash = NULL;
    const char *coinbase_1 = NULL;
    const char *coinbase_2 = NULL;
    if (!array_string(params, 0, &job_id) ||
        !array_string(params, 1, &prev_block_hash) ||
        !array_string(params, 2, &coinbase_1) ||
        !array_string(params, 3, &coinbase_2)) {
        return NULL;
    }

    mining_notify *work = calloc(1, sizeof(*work));
    if (work == NULL) {
        return NULL;
    }

    work->job_id = strdup(job_id);
    work->prev_block_hash = strdup(prev_block_hash);
    if (work->job_id == NULL || work->prev_block_hash == NULL) {
        free_mining_notify(work);
        return NULL;
    }
    if (!decode_hex_bytes(prev_block_hash, strlen(prev_block_hash),
                          work->prev_block_hash_bin,
                          sizeof(work->prev_block_hash_bin)) ||
        !decode_hex_alloc_limited(coinbase_1, STRATUM_MAX_COINBASE_BYTES,
                                  &work->coinbase_1_bin, &work->coinbase_1_len) ||
        !decode_hex_alloc_limited(coinbase_2, STRATUM_MAX_COINBASE_BYTES,
                                  &work->coinbase_2_bin, &work->coinbase_2_len) ||
        work->coinbase_1_len + work->coinbase_2_len >
            STRATUM_MAX_COINBASE_BYTES - (MAX_EXTRANONCE2_LEN * 2U)) {
        ESP_LOGE(TAG, "invalid mining.notify hex fields");
        free_mining_notify(work);
        return NULL;
    }

    work->n_merkle_branches = (size_t)branch_count;
    if (work->n_merkle_branches > 0) {
        work->merkle_branches = calloc(work->n_merkle_branches, HASH_SIZE);
        if (work->merkle_branches == NULL) {
            free_mining_notify(work);
            return NULL;
        }
    }

    for (int i = 0; i < branch_count; ++i) {
        cJSON *branch = cJSON_GetArrayItem(merkle_branch, i);
        if (!cJSON_IsString(branch) || branch->valuestring == NULL) {
            ESP_LOGE(TAG, "invalid merkle branch item");
            free_mining_notify(work);
            return NULL;
        }
        if (!decode_hex_bytes(branch->valuestring, strlen(branch->valuestring),
                              work->merkle_branches + (i * HASH_SIZE), HASH_SIZE)) {
            ESP_LOGE(TAG, "invalid merkle branch hex");
            free_mining_notify(work);
            return NULL;
        }
    }

    const char *version = NULL;
    const char *nbits = NULL;
    const char *ntime = NULL;
    if (!array_string(params, 5, &version) || !array_string(params, 6, &nbits) ||
        !array_string(params, 7, &ntime)) {
        free_mining_notify(work);
        return NULL;
    }

    if (!parse_hex_u32_text(version, strlen(version), &work->version) ||
        !parse_hex_u32_text(nbits, strlen(nbits), &work->target) ||
        !parse_hex_u32_text(ntime, strlen(ntime), &work->ntime)) {
        free_mining_notify(work);
        return NULL;
    }
    work->clean_jobs = cJSON_IsTrue(cJSON_GetArrayItem(params, cJSON_GetArraySize(params) - 1));
    return work;
}

static void update_extranonce_values(const char *extranonce, int extranonce_2_len)
{
    if (extranonce == NULL || extranonce_2_len <= 0 || extranonce_2_len > MAX_EXTRANONCE2_LEN) {
        ESP_LOGW(TAG, "ignoring invalid extranonce setup");
        return;
    }

    uint8_t *bin = NULL;
    size_t bin_len = 0;
    if (!decode_hex_alloc_limited(extranonce, MAX_EXTRANONCE2_LEN, &bin, &bin_len)) {
        ESP_LOGW(TAG, "ignoring invalid extranonce hex");
        return;
    }

    char *copy = strdup(extranonce);
    if (copy == NULL) {
        free(bin);
        ESP_LOGE(TAG, "extranonce alloc failed");
        return;
    }

    free(g_state->extranonce_str);
    free(g_extranonce_bin);
    g_state->extranonce_str = copy;
    g_extranonce_bin = bin;
    g_extranonce_len = bin_len;
    g_state->extranonce_2_len = extranonce_2_len;
    STRATUM_LOGI("extranonce ready: len=%d", g_state->extranonce_2_len);
}

static void update_version_mask(uint32_t mask)
{
    if (mask == 0) {
        return;
    }
    g_state->version_mask = mask;
    if (g_state->ASIC_initalized) {
        BM1370_set_version_mask(mask);
    }
    STRATUM_LOGI("version mask 0x%08" PRIx32, mask);
}

static void update_asic_job_difficulty(double difficulty)
{
    if (!(difficulty > 0.0) || g_state == NULL || !g_state->ASIC_initalized) {
        return;
    }

    BM1370_set_job_difficulty(difficulty);
    STRATUM_LOGI("asic ticket difficulty %.2f", difficulty);
}

static void enqueue_work(mining_notify *work)
{
    if (work == NULL) {
        return;
    }

    uint32_t epoch = atomic_load(&g_work_epoch);

    queued_work_t queued = {
        .work = work,
        .epoch = epoch,
    };
    if (xQueueSend(g_work_queue, &queued, 0) != pdPASS) {
        queued_work_t old = {0};
        if (xQueueReceive(g_work_queue, &old, 0) == pdPASS) {
            free_mining_notify(old.work);
        }
        if (xQueueSend(g_work_queue, &queued, 0) != pdPASS) {
            free_mining_notify(work);
            ESP_LOGW(TAG, "dropping work; queue full");
            return;
        }
    }

    atomic_fetch_add(&g_work_received, 1);
    g_state->SYSTEM_MODULE.work_received++;
    STRATUM_LOGI("queued job %s clean=%d", work->job_id, work->clean_jobs ? 1 : 0);
}

static bool response_is_success(cJSON *json)
{
    cJSON *result = cJSON_GetObjectItem(json, "result");
    cJSON *error = cJSON_GetObjectItem(json, "error");

    if (error != NULL && !cJSON_IsNull(error)) {
        return false;
    }
    if (result == NULL || cJSON_IsNull(result)) {
        return false;
    }
    if (cJSON_IsBool(result)) {
        return cJSON_IsTrue(result);
    }
    return true;
}

static void log_rejected_share(cJSON *json)
{
    const char *reason = "unknown";
    cJSON *reject_reason = cJSON_GetObjectItem(json, "reject-reason");
    cJSON *error = cJSON_GetObjectItem(json, "error");

    if (cJSON_IsString(reject_reason) && reject_reason->valuestring != NULL) {
        reason = reject_reason->valuestring;
    } else if (cJSON_IsString(error) && error->valuestring != NULL) {
        reason = error->valuestring;
    } else if (cJSON_IsArray(error) && cJSON_GetArraySize(error) >= 2) {
        cJSON *message = cJSON_GetArrayItem(error, 1);
        if (cJSON_IsString(message) && message->valuestring != NULL) {
            reason = message->valuestring;
        }
    }

    if (strstr(reason, "low difficulty share") != NULL) {
        STRATUM_LOGI("share rejected: %s", reason);
    } else {
        ESP_LOGW(TAG, "share rejected: %s", reason);
    }
}

static void handle_subscribe_response(cJSON *json)
{
    cJSON *result = cJSON_GetObjectItem(json, "result");
    if (!cJSON_IsArray(result)) {
        ESP_LOGW(TAG, "subscribe result was not an array");
        return;
    }

    cJSON *extranonce = cJSON_GetArrayItem(result, 1);
    cJSON *extranonce_2_len = cJSON_GetArrayItem(result, 2);
    if (!cJSON_IsString(extranonce) || !cJSON_IsNumber(extranonce_2_len)) {
        ESP_LOGW(TAG, "subscribe result missing extranonce");
        return;
    }

    update_extranonce_values(extranonce->valuestring, extranonce_2_len->valueint);
}

static void handle_configure_response(cJSON *json)
{
    cJSON *result = cJSON_GetObjectItem(json, "result");
    cJSON *mask = cJSON_GetObjectItem(result, "version-rolling.mask");
    if (cJSON_IsString(mask) && mask->valuestring != NULL) {
        update_version_mask(strtoul(mask->valuestring, NULL, 16));
    }
}

static void handle_response(cJSON *json, int message_id)
{
    const bool success = response_is_success(json);
    uint32_t response_ms = 0;
    if (take_response_request_ms(message_id, &response_ms)) {
        atomic_store(&g_response_time_ms, response_ms);
    }

    if (message_id == STRATUM_ID_SUBSCRIBE) {
        if (success) {
            handle_subscribe_response(json);
        } else {
            ESP_LOGW(TAG, "subscribe failed");
        }
        return;
    }

    if (message_id == STRATUM_ID_CONFIGURE) {
        if (success) {
            handle_configure_response(json);
        } else {
            ESP_LOGW(TAG, "version rolling configure failed");
        }
        return;
    }

    if (take_share_request(message_id)) {
        if (success) {
            atomic_fetch_add(&g_accepted, 1);
            g_state->SYSTEM_MODULE.shares_accepted++;
            STRATUM_LOGI("share accepted");
        } else {
            atomic_fetch_add(&g_rejected, 1);
            g_state->SYSTEM_MODULE.shares_rejected++;
            log_rejected_share(json);
        }
        return;
    }

    STRATUM_LOGI("setup response id=%d ok=%d", message_id, success ? 1 : 0);
}

static void handle_set_extranonce(cJSON *params)
{
    if (!cJSON_IsArray(params)) {
        return;
    }

    cJSON *extranonce = cJSON_GetArrayItem(params, 0);
    cJSON *extranonce_2_len = cJSON_GetArrayItem(params, 1);
    if (cJSON_IsString(extranonce) && cJSON_IsNumber(extranonce_2_len)) {
        reset_work_state(true);
        update_extranonce_values(extranonce->valuestring, extranonce_2_len->valueint);
    }
}

static void handle_mining_notify_work(mining_notify *work)
{
    if (work == NULL) {
        return;
    }

    const bool new_block = stratum_note_current_block(work->prev_block_hash);
    if (new_block) {
        reset_work_state(true);
    }
    if (new_block || atomic_load(&g_payout_status) == STRATUM_PAYOUT_STATUS_UNCHECKED) {
        stratum_update_payout_from_notify(work);
    }
    enqueue_work(work);
}

#if M45_STRATUM_FAST_PATHS
typedef struct {
    const char *ptr;
    size_t len;
} json_span_t;

static void fast_json_skip_ws(const char **cursor)
{
    while (**cursor == ' ' || **cursor == '\t' || **cursor == '\r' ||
           **cursor == '\n') {
        ++(*cursor);
    }
}

static bool fast_json_consume_char(const char **cursor, char expected)
{
    fast_json_skip_ws(cursor);
    if (**cursor != expected) {
        return false;
    }
    ++(*cursor);
    return true;
}

static bool fast_json_parse_string_span(const char **cursor, json_span_t *out)
{
    fast_json_skip_ws(cursor);
    if (**cursor != '"') {
        return false;
    }
    const char *start = ++(*cursor);
    while (**cursor != '\0') {
        const unsigned char ch = (unsigned char)**cursor;
        if (ch == '"') {
            if (out != NULL) {
                out->ptr = start;
                out->len = (size_t)(*cursor - start);
            }
            ++(*cursor);
            return true;
        }
        if (ch == '\\' || ch < 0x20) {
            return false;
        }
        ++(*cursor);
    }
    return false;
}

static bool fast_json_parse_bool(const char **cursor, bool *out)
{
    fast_json_skip_ws(cursor);
    if (strncmp(*cursor, "true", 4) == 0) {
        *cursor += 4;
        if (out != NULL) {
            *out = true;
        }
        return true;
    }
    if (strncmp(*cursor, "false", 5) == 0) {
        *cursor += 5;
        if (out != NULL) {
            *out = false;
        }
        return true;
    }
    return false;
}

static const char *fast_json_field_value(const char *line, const char *quoted_name)
{
    const char *field = strstr(line, quoted_name);
    if (field == NULL) {
        return NULL;
    }

    const char *cursor = field + strlen(quoted_name);
    fast_json_skip_ws(&cursor);
    if (*cursor != ':') {
        return NULL;
    }
    ++cursor;
    fast_json_skip_ws(&cursor);
    return cursor;
}

static bool fast_json_method_is(const char *line, const char *method)
{
    const char *cursor = fast_json_field_value(line, "\"method\"");
    json_span_t span = {0};
    return cursor != NULL && fast_json_parse_string_span(&cursor, &span) &&
           span.len == strlen(method) && memcmp(span.ptr, method, span.len) == 0;
}

static bool fast_json_parse_id(const char *line, int *out)
{
    const char *cursor = fast_json_field_value(line, "\"id\"");
    if (cursor == NULL) {
        return false;
    }

    char *end = NULL;
    const long value = strtol(cursor, &end, 10);
    if (end == cursor || value < INT_MIN || value > INT_MAX) {
        return false;
    }
    if (out != NULL) {
        *out = (int)value;
    }
    return true;
}

static char *fast_json_dup_span(json_span_t span)
{
    char *copy = malloc(span.len + 1U);
    if (copy == NULL) {
        return NULL;
    }
    memcpy(copy, span.ptr, span.len);
    copy[span.len] = '\0';
    return copy;
}

static bool fast_span_contains(json_span_t span, const char *needle)
{
    const size_t needle_len = strlen(needle);
    if (needle_len == 0 || needle_len > span.len) {
        return false;
    }
    for (size_t i = 0; i + needle_len <= span.len; ++i) {
        if (memcmp(span.ptr + i, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static bool fast_parse_response_success(const char *line, bool *success)
{
    const char *error = fast_json_field_value(line, "\"error\"");
    if (error != NULL && strncmp(error, "null", 4) != 0) {
        if (success != NULL) {
            *success = false;
        }
        return true;
    }

    const char *result = fast_json_field_value(line, "\"result\"");
    if (result == NULL) {
        return false;
    }
    if (strncmp(result, "true", 4) == 0) {
        if (success != NULL) {
            *success = true;
        }
        return true;
    }
    if (strncmp(result, "false", 5) == 0 || strncmp(result, "null", 4) == 0) {
        if (success != NULL) {
            *success = false;
        }
        return true;
    }
    return false;
}

static bool fast_reject_reason(const char *line, json_span_t *reason)
{
    const char *cursor = fast_json_field_value(line, "\"reject-reason\"");
    if (cursor != NULL && fast_json_parse_string_span(&cursor, reason)) {
        return true;
    }

    cursor = fast_json_field_value(line, "\"error\"");
    if (cursor == NULL || !fast_json_consume_char(&cursor, '[')) {
        return false;
    }

    while (*cursor != '\0' && *cursor != ',') {
        ++cursor;
    }
    if (*cursor != ',' || !fast_json_consume_char(&cursor, ',')) {
        return false;
    }
    return fast_json_parse_string_span(&cursor, reason);
}

static void fast_log_rejected_share(const char *line)
{
    json_span_t reason = {
        .ptr = "unknown",
        .len = 7,
    };
    (void)fast_reject_reason(line, &reason);

    const int len = reason.len > 160U ? 160 : (int)reason.len;
    if (fast_span_contains(reason, "low difficulty share")) {
        STRATUM_LOGI("share rejected: %.*s", len, reason.ptr);
    } else {
        ESP_LOGW(TAG, "share rejected: %.*s", len, reason.ptr);
    }
}

static bool fast_handle_share_response(const char *line)
{
    if (fast_json_field_value(line, "\"method\"") != NULL) {
        return false;
    }

    int message_id = -1;
    bool success = false;
    if (!fast_json_parse_id(line, &message_id) ||
        message_id <= STRATUM_ID_EXTRANONCE_SUBSCRIBE ||
        !fast_parse_response_success(line, &success) ||
        !take_share_request(message_id)) {
        return false;
    }

    uint32_t response_ms = 0;
    if (take_response_request_ms(message_id, &response_ms)) {
        atomic_store(&g_response_time_ms, response_ms);
    }
    if (success) {
        atomic_fetch_add(&g_accepted, 1);
        g_state->SYSTEM_MODULE.shares_accepted++;
        STRATUM_LOGI("share accepted");
    } else {
        atomic_fetch_add(&g_rejected, 1);
        g_state->SYSTEM_MODULE.shares_rejected++;
        fast_log_rejected_share(line);
    }
    return true;
}

static mining_notify *parse_mining_notify_fast(const char *line)
{
    const char *cursor = fast_json_field_value(line, "\"params\"");
    if (cursor == NULL || !fast_json_consume_char(&cursor, '[')) {
        return NULL;
    }

    json_span_t job_id = {0};
    json_span_t prev_block_hash = {0};
    json_span_t coinbase_1 = {0};
    json_span_t coinbase_2 = {0};
    json_span_t branches[MAX_MERKLE_BRANCHES] = {0};
    json_span_t version = {0};
    json_span_t nbits = {0};
    json_span_t ntime = {0};
    size_t branch_count = 0;
    bool clean_jobs = false;

    if (!fast_json_parse_string_span(&cursor, &job_id) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &prev_block_hash) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &coinbase_1) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &coinbase_2) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_consume_char(&cursor, '[')) {
        return NULL;
    }

    fast_json_skip_ws(&cursor);
    if (*cursor == ']') {
        ++cursor;
    } else {
        while (true) {
            if (branch_count >= MAX_MERKLE_BRANCHES ||
                !fast_json_parse_string_span(&cursor, &branches[branch_count])) {
                return NULL;
            }
            ++branch_count;
            fast_json_skip_ws(&cursor);
            if (*cursor == ',') {
                ++cursor;
                continue;
            }
            if (*cursor == ']') {
                ++cursor;
                break;
            }
            return NULL;
        }
    }

    if (!fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &version) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &nbits) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_string_span(&cursor, &ntime) ||
        !fast_json_consume_char(&cursor, ',') ||
        !fast_json_parse_bool(&cursor, &clean_jobs)) {
        return NULL;
    }

    mining_notify *work = calloc(1, sizeof(*work));
    if (work == NULL) {
        return NULL;
    }
    work->job_id = fast_json_dup_span(job_id);
    work->prev_block_hash = fast_json_dup_span(prev_block_hash);
    if (work->job_id == NULL || work->prev_block_hash == NULL ||
        !decode_hex_bytes(prev_block_hash.ptr, prev_block_hash.len,
                          work->prev_block_hash_bin,
                          sizeof(work->prev_block_hash_bin)) ||
        !decode_hex_alloc_limited_span(coinbase_1.ptr, coinbase_1.len,
                                       STRATUM_MAX_COINBASE_BYTES,
                                       &work->coinbase_1_bin, &work->coinbase_1_len) ||
        !decode_hex_alloc_limited_span(coinbase_2.ptr, coinbase_2.len,
                                       STRATUM_MAX_COINBASE_BYTES,
                                       &work->coinbase_2_bin, &work->coinbase_2_len) ||
        work->coinbase_1_len + work->coinbase_2_len >
            STRATUM_MAX_COINBASE_BYTES - (MAX_EXTRANONCE2_LEN * 2U) ||
        !parse_hex_u32_text(version.ptr, version.len, &work->version) ||
        !parse_hex_u32_text(nbits.ptr, nbits.len, &work->target) ||
        !parse_hex_u32_text(ntime.ptr, ntime.len, &work->ntime)) {
        free_mining_notify(work);
        return NULL;
    }

    work->n_merkle_branches = branch_count;
    if (branch_count > 0) {
        work->merkle_branches = calloc(branch_count, HASH_SIZE);
        if (work->merkle_branches == NULL) {
            free_mining_notify(work);
            return NULL;
        }
        for (size_t i = 0; i < branch_count; ++i) {
            if (!decode_hex_bytes(branches[i].ptr, branches[i].len,
                                  work->merkle_branches + (i * HASH_SIZE),
                                  HASH_SIZE)) {
                free_mining_notify(work);
                return NULL;
            }
        }
    }

    work->clean_jobs = clean_jobs;
    return work;
}

static bool fast_handle_mining_notify(const char *line)
{
    if (!fast_json_method_is(line, "mining.notify")) {
        return false;
    }

    mining_notify *work = parse_mining_notify_fast(line);
    if (work == NULL) {
        return false;
    }
    handle_mining_notify_work(work);
    return true;
}

static bool fast_handle_set_difficulty(const char *line)
{
    if (!fast_json_method_is(line, "mining.set_difficulty")) {
        return false;
    }

    const char *cursor = fast_json_field_value(line, "\"params\"");
    if (cursor == NULL || !fast_json_consume_char(&cursor, '[')) {
        return false;
    }

    char *end = NULL;
    const double pool_difficulty = strtod(cursor, &end);
    if (end == cursor) {
        return false;
    }

    const bool raised = set_pool_difficulty(pool_difficulty);
    if (raised) {
        reset_work_state(true);
        ESP_LOGI(TAG, "cleared queued work for raised difficulty %.2f",
                 current_pool_difficulty());
    }
    update_asic_job_difficulty(pool_difficulty);
    STRATUM_LOGI("pool difficulty %.2f", current_pool_difficulty());
    return true;
}

static bool fast_handle_stratum_line(const char *line)
{
    return fast_handle_share_response(line) ||
           fast_handle_mining_notify(line) ||
           fast_handle_set_difficulty(line);
}
#endif

static void handle_stratum_method(cJSON *json, const char *method, int message_id,
                                  esp_transport_handle_t transport)
{
    cJSON *params = cJSON_GetObjectItem(json, "params");

    if (strcmp(method, "mining.notify") == 0) {
        mining_notify *work = parse_mining_notify(params);
        if (work != NULL) {
            handle_mining_notify_work(work);
        }
    } else if (strcmp(method, "mining.set_difficulty") == 0) {
        cJSON *difficulty = cJSON_GetArrayItem(params, 0);
        if (cJSON_IsNumber(difficulty)) {
            const double pool_difficulty = difficulty->valuedouble;
            const bool raised = set_pool_difficulty(pool_difficulty);
            if (raised) {
                reset_work_state(true);
                ESP_LOGI(TAG, "cleared queued work for raised difficulty %.2f",
                         current_pool_difficulty());
            }
            update_asic_job_difficulty(pool_difficulty);
            STRATUM_LOGI("pool difficulty %.2f", current_pool_difficulty());
        }
    } else if (strcmp(method, "mining.set_version_mask") == 0) {
        const char *mask = NULL;
        if (array_string(params, 0, &mask)) {
            update_version_mask(strtoul(mask, NULL, 16));
        }
    } else if (strcmp(method, "mining.set_extranonce") == 0) {
        handle_set_extranonce(params);
    } else if (strcmp(method, "mining.ping") == 0) {
        stratum_writef(transport, "{\"id\":%d,\"method\":\"pong\",\"params\":[]}\n", message_id);
    } else if (strcmp(method, "client.reconnect") == 0) {
        ESP_LOGW(TAG, "pool requested reconnect");
        esp_transport_close(transport);
    } else if (strcmp(method, "client.show_message") == 0) {
        const char *pool_message = NULL;
        if (array_string(params, 0, &pool_message)) {
            STRATUM_LOGI("pool message: %.128s", pool_message);
        }
    } else {
        STRATUM_LOGI("unhandled stratum method: %s", method);
    }
}

static void handle_stratum_line(const char *line, esp_transport_handle_t transport)
{
    STRATUM_LOGI("rx: %s", line);

#if M45_STRATUM_FAST_PATHS
    if (fast_handle_stratum_line(line)) {
        return;
    }
#endif

    cJSON *json = cJSON_Parse(line);
    if (json == NULL) {
        ESP_LOGW(TAG, "invalid JSON from pool");
        return;
    }

    int message_id = -1;
    cJSON *id = cJSON_GetObjectItem(json, "id");
    if (cJSON_IsNumber(id)) {
        message_id = id->valueint;
    }

    cJSON *method = cJSON_GetObjectItem(json, "method");
    if (cJSON_IsString(method) && method->valuestring != NULL) {
        handle_stratum_method(json, method->valuestring, message_id, transport);
    } else {
        handle_response(json, message_id);
    }

    cJSON_Delete(json);
}

static esp_err_t send_setup_messages(esp_transport_handle_t transport)
{
    if (stratum_write_message(
            transport,
            "{\"id\":1,\"method\":\"mining.configure\",\"params\":[[\"version-rolling\"],{\"version-rolling.mask\":\"ffffffff\"}]}\n") < 0) {
        return ESP_FAIL;
    }
    mark_response_request(STRATUM_ID_CONFIGURE);
    char miner_info[64];
    char subscribe_msg[160];
    size_t subscribe_offset = 0;
    stratum_build_miner_info(miner_info, sizeof(miner_info));
    subscribe_msg[0] = '\0';
    if (!stratum_append_text(subscribe_msg, sizeof(subscribe_msg), &subscribe_offset,
                             "{\"id\":") ||
        !stratum_append_int(subscribe_msg, sizeof(subscribe_msg), &subscribe_offset,
                            STRATUM_ID_SUBSCRIBE) ||
        !stratum_append_text(subscribe_msg, sizeof(subscribe_msg), &subscribe_offset,
                             ",\"method\":\"mining.subscribe\",\"params\":[") ||
        !stratum_append_json_string_fast(subscribe_msg, sizeof(subscribe_msg),
                                         &subscribe_offset, miner_info) ||
        !stratum_append_text(subscribe_msg, sizeof(subscribe_msg), &subscribe_offset,
                             "]}\n")) {
        ESP_LOGE(TAG, "stratum subscribe format overflow");
        return ESP_FAIL;
    }
    if (stratum_write_message(transport, subscribe_msg) < 0) {
        return ESP_FAIL;
    }
    mark_response_request(STRATUM_ID_SUBSCRIBE);
    const uint16_t suggested_difficulty = configured_suggested_difficulty();
    set_pool_difficulty((double)suggested_difficulty);
    STRATUM_LOGI("suggest difficulty %u", suggested_difficulty);
    char difficulty_msg[96];
    size_t difficulty_offset = 0;
    difficulty_msg[0] = '\0';
    if (!stratum_append_text(difficulty_msg, sizeof(difficulty_msg), &difficulty_offset,
                             "{\"id\":3,\"method\":\"mining.suggest_difficulty\",\"params\":[") ||
        !stratum_append_int(difficulty_msg, sizeof(difficulty_msg), &difficulty_offset,
                            (int)suggested_difficulty) ||
        !stratum_append_text(difficulty_msg, sizeof(difficulty_msg), &difficulty_offset,
                             "]}\n") ||
        stratum_write_message(transport, difficulty_msg) < 0) {
        return ESP_FAIL;
    }
    mark_response_request(STRATUM_ID_SUGGEST_DIFFICULTY);
    if (stratum_send_authorize(transport) < 0) {
        return ESP_FAIL;
    }
    mark_response_request(STRATUM_ID_AUTHORIZE);
    if (stratum_write_message(
            transport,
            "{\"id\":5,\"method\":\"mining.extranonce.subscribe\",\"params\":[]}\n") < 0) {
        return ESP_FAIL;
    }
    mark_response_request(STRATUM_ID_EXTRANONCE_SUBSCRIBE);
    return ESP_OK;
}

static void stratum_rx_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!wifi_http_connected()) {
            stratum_set_disconnected(true);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        esp_transport_handle_t transport = esp_transport_tcp_init();
        if (transport == NULL) {
            vTaskDelay(pdMS_TO_TICKS(STRATUM_RECONNECT_MS));
            continue;
        }

        stratum_endpoint_t endpoint = {0};
        select_next_pool_endpoint(&endpoint);
        set_active_pool_endpoint(&endpoint);
        stratum_maybe_prefetch_backup_pool_dns(&endpoint);

        char connect_host[M45_POOL_HOST_MAX + 1];
        stratum_connect_host_for_endpoint(&endpoint, connect_host, sizeof(connect_host));

        ESP_LOGI(TAG, "connecting to %s:%d via %s%s", endpoint.host, endpoint.port,
                 connect_host,
                 endpoint.using_backup ? " (backup)" : "");
        bool rotate_endpoint = false;
        reset_work_state(true);
        g_state->extranonce_2_len = 0;
        const int ret = esp_transport_connect(transport, connect_host, endpoint.port, 10000);
        if (ret >= 0) {
            stratum_enable_tcp_nodelay(transport);
        }
        if (ret < 0 || send_setup_messages(transport) != ESP_OK) {
            ESP_LOGW(TAG, "stratum connect/setup failed");
            esp_transport_destroy(transport);
            set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);
            rotate_endpoint = true;
            if (atomic_exchange(&g_switch_to_primary_requested, false)) {
                atomic_store(&g_next_using_backup_pool, false);
            } else if (rotate_endpoint) {
                rotate_next_pool_endpoint();
            }
            vTaskDelay(pdMS_TO_TICKS(STRATUM_RECONNECT_MS));
            continue;
        }

        set_transport(transport);
        atomic_store(&g_connected, true);
        atomic_store(&g_connected_since_us, (uint64_t)esp_timer_get_time());
        atomic_store(&g_response_time_ms, 0);
        set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);
        g_state->transport = transport;
        ESP_LOGI(TAG, "stratum connected");

        while (wifi_http_connected()) {
            stratum_maybe_probe_primary_pool();
            if (atomic_load(&g_switch_to_primary_requested)) {
                rotate_endpoint = false;
                break;
            }

            size_t consumed_len = 0;
            char *line = receive_jsonrpc_line(transport, &consumed_len);
            if (line == NULL) {
                rotate_endpoint = true;
                break;
            }

            handle_stratum_line(line, transport);
            consume_jsonrpc_line(consumed_len);
        }

        set_transport(NULL);
        g_state->transport = NULL;
        stratum_set_disconnected(true);
        esp_transport_destroy(transport);
        ESP_LOGW(TAG, "stratum disconnected");
        if (atomic_exchange(&g_switch_to_primary_requested, false)) {
            atomic_store(&g_next_using_backup_pool, false);
        } else if (rotate_endpoint) {
            rotate_next_pool_endpoint();
        }
        vTaskDelay(pdMS_TO_TICKS(STRATUM_RECONNECT_MS));
    }
}

static void generate_and_send_work(mining_notify *notification, uint64_t extranonce_2,
                                   uint32_t work_epoch, int interval_ms)
{
    if (atomic_load(&g_work_paused)) {
        metric_inc(&g_metric_job_send_skipped);
        return;
    }
    if (work_epoch != atomic_load(&g_work_epoch)) {
        metric_inc(&g_metric_job_send_skipped);
        return;
    }
    if (!g_state->ASIC_initalized || g_state->extranonce_str == NULL ||
        (g_extranonce_len > 0 && g_extranonce_bin == NULL) ||
        g_state->extranonce_2_len <= 0 || g_state->extranonce_2_len > MAX_EXTRANONCE2_LEN) {
        metric_inc(&g_metric_job_send_skipped);
        return;
    }

#ifdef M45_ASIC_LOSS_METRICS
    const uint64_t build_started_us = (uint64_t)esp_timer_get_time();
#endif
    uint8_t extranonce_2_bin[MAX_EXTRANONCE2_LEN];
    char extranonce_2_str[MAX_EXTRANONCE2_STR];
    const size_t extranonce_2_len = (size_t)g_state->extranonce_2_len;
    const size_t coinbase_len = notification->coinbase_1_len + g_extranonce_len +
                                extranonce_2_len + notification->coinbase_2_len;
    if (coinbase_len > STRATUM_MAX_COINBASE_BYTES) {
        metric_inc(&g_metric_job_send_skipped);
        ESP_LOGE(TAG, "dropping oversized coinbase work");
        return;
    }

    extranonce_2_generate_bin(extranonce_2, g_state->extranonce_2_len, extranonce_2_bin);
    bin2hex(extranonce_2_bin, extranonce_2_len, extranonce_2_str, sizeof(extranonce_2_str));

    uint8_t coinbase_tx_hash[32];
    calculate_coinbase_tx_hash_parts(notification->coinbase_1_bin, notification->coinbase_1_len,
                                     g_extranonce_bin, g_extranonce_len, extranonce_2_bin,
                                     extranonce_2_len, notification->coinbase_2_bin,
                                     notification->coinbase_2_len, coinbase_tx_hash);

    uint8_t merkle_root[32];
    calculate_merkle_root_hash(coinbase_tx_hash, (uint8_t(*)[32])notification->merkle_branches,
                               notification->n_merkle_branches, merkle_root);

    bm_job *job = alloc_bm_job();
    if (job == NULL) {
        metric_inc(&g_metric_job_alloc_failed);
        ESP_LOGE(TAG, "job alloc failed");
        return;
    }

    construct_bm_job(notification, merkle_root, g_state->version_mask, current_pool_difficulty(),
                     job);
    job->version_mask = g_state->version_mask;
    if (!bm_job_set_ids(job, notification->job_id, extranonce_2_str)) {
        metric_inc(&g_metric_job_alloc_failed);
        free_bm_job(job);
        return;
    }
#ifdef M45_ASIC_LOSS_METRICS
    metric_record_job_build((uint64_t)esp_timer_get_time() - build_started_us);
#endif

    bool sent = false;
    if (g_work_reset_lock != NULL) {
        xSemaphoreTake(g_work_reset_lock, portMAX_DELAY);
    }
    if (work_epoch == atomic_load(&g_work_epoch) && !atomic_load(&g_work_paused)) {
#ifdef M45_ASIC_LOSS_METRICS
        const uint64_t send_started_us = (uint64_t)esp_timer_get_time();
#endif
        BM1370_send_work(g_state, job);
#ifdef M45_ASIC_LOSS_METRICS
        metric_record_job_send((uint64_t)esp_timer_get_time() - send_started_us);
#endif
        sent = true;
    }
    if (g_work_reset_lock != NULL) {
        xSemaphoreGive(g_work_reset_lock);
    }
    if (!sent) {
        metric_inc(&g_metric_job_send_skipped);
        free_bm_job(job);
        return;
    }

    atomic_fetch_add(&g_job_sent, 1);
    atomic_store(&g_last_job_sent_us, (uint64_t)esp_timer_get_time());
    record_assigned_work(interval_ms);
}

static int bm1370_job_interval_ms(void)
{
    const float frequency_mhz = g_state->POWER_MANAGEMENT_MODULE.actual_frequency;
    if (g_job_interval_ms > 0 && g_job_interval_frequency_mhz == frequency_mhz) {
        return g_job_interval_ms;
    }

    const AsicConfig *asic = &g_state->DEVICE_CONFIG.family.asic;
    const int small_cores_up = _next_power_of_two(asic->small_core_count);
    const int cores_up = _next_power_of_two(asic->core_count);
    const size_t parallel_midstates =
        (cores_up > 0 && small_cores_up >= cores_up) ? (size_t)(small_cores_up / cores_up) : 1;
    const double timeout = calculate_bm_timeout_ms(
        frequency_mhz,
        g_state->DEVICE_CONFIG.family.asic_count,
        asic->small_core_count,
        asic->core_count,
        parallel_midstates,
        ASIC_JOB_DISPATCH_PERCENT,
        asic->default_asic_timeout);
    const int interval = (int)timeout;
    g_job_interval_frequency_mhz = frequency_mhz;
    g_job_interval_ms = interval >= ASIC_JOB_MIN_INTERVAL_MS ? interval : ASIC_JOB_MIN_INTERVAL_MS;
    return g_job_interval_ms;
}

static void job_task(void *arg)
{
    (void)arg;
    mining_notify *current = NULL;
    uint32_t current_epoch = atomic_load(&g_work_epoch);
    uint64_t extranonce_2 = 0;
    uint64_t next_dispatch_us = 0;

    while (true) {
        int timeout_ms = 500;
        if (atomic_load(&g_work_paused)) {
            timeout_ms = 50;
            next_dispatch_us = 0;
        } else if (current != NULL) {
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            if (next_dispatch_us == 0 || now_us >= next_dispatch_us) {
                timeout_ms = 0;
            } else {
                const uint64_t wait_us = next_dispatch_us - now_us;
                timeout_ms = (int)((wait_us + 999ULL) / 1000ULL);
                if (timeout_ms < 1) {
                    timeout_ms = 1;
                }
            }
        }

        queued_work_t incoming = {0};
        const BaseType_t received =
            xQueueReceive(g_work_queue, &incoming, pdMS_TO_TICKS(timeout_ms));
        const uint32_t latest_epoch = atomic_load(&g_work_epoch);

        if (latest_epoch != current_epoch) {
            if (current != NULL) {
                free_mining_notify(current);
                current = NULL;
            }
            extranonce_2 = 0;
            next_dispatch_us = 0;
            current_epoch = latest_epoch;
        }

        if (received == pdPASS) {
            if (incoming.epoch != current_epoch) {
                free_mining_notify(incoming.work);
            } else if (incoming.work == NULL) {
                if (current != NULL) {
                    free_mining_notify(current);
                    current = NULL;
                }
                extranonce_2 = 0;
                next_dispatch_us = 0;
            } else {
                if (current != NULL) {
                    free_mining_notify(current);
                }
                current = incoming.work;
                extranonce_2 = 0;
                next_dispatch_us = 0;
            }
        }

        if (atomic_load(&g_work_epoch) != current_epoch) {
            if (current != NULL) {
                free_mining_notify(current);
                current = NULL;
            }
            current_epoch = atomic_load(&g_work_epoch);
            extranonce_2 = 0;
            next_dispatch_us = 0;
        }
        if (current != NULL && !atomic_load(&g_work_paused)) {
            const uint64_t now_us = (uint64_t)esp_timer_get_time();
            if (next_dispatch_us == 0 || now_us >= next_dispatch_us) {
                const uint64_t scheduled_us = next_dispatch_us;
                const int interval_ms = bm1370_job_interval_ms();
                const uint64_t interval_us = (uint64_t)interval_ms * 1000ULL;
                if (scheduled_us != 0 && now_us > scheduled_us) {
                    metric_record_dispatch_late(now_us - scheduled_us, interval_us);
                }
                generate_and_send_work(current, extranonce_2++, current_epoch, interval_ms);

                if (scheduled_us == 0 || now_us > scheduled_us + interval_us) {
                    next_dispatch_us = now_us + interval_us;
                } else {
                    next_dispatch_us = scheduled_us + interval_us;
                }

                const uint64_t after_send_us = (uint64_t)esp_timer_get_time();
                if (next_dispatch_us <= after_send_us) {
                    next_dispatch_us = after_send_us + 1000ULL;
                }
            }
        }
    }
}

static int submit_share(esp_transport_handle_t transport, int request_id, const bm_job *job,
                        uint32_t nonce, uint32_t version_bits, uint64_t *write_us)
{
    char msg[STRATUM_BUFFER_SIZE];
    char nonce_hex[9];
    char version_bits_hex[9];
    size_t offset = 0;

    uint32_to_hex8(nonce, nonce_hex);
    uint32_to_hex8(version_bits, version_bits_hex);
    msg[0] = '\0';

    if (!stratum_append_text(msg, sizeof(msg), &offset, "{\"id\":") ||
        !stratum_append_int(msg, sizeof(msg), &offset, request_id) ||
        !stratum_append_text(msg, sizeof(msg), &offset,
                             ",\"method\":\"mining.submit\",\"params\":[") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset,
                                         g_state->SYSTEM_MODULE.pool_user) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset, job->jobid) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset, job->extranonce2) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset, job->ntime_hex) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset, nonce_hex) ||
        !stratum_append_text(msg, sizeof(msg), &offset, ",") ||
        !stratum_append_json_string_fast(msg, sizeof(msg), &offset, version_bits_hex) ||
        !stratum_append_text(msg, sizeof(msg), &offset, "]}\n")) {
        ESP_LOGE(TAG, "stratum submit format overflow");
        return -1;
    }

    const uint64_t started_us = (uint64_t)esp_timer_get_time();
    const int ret = stratum_write_message(transport, msg);
    const uint64_t finished_us = (uint64_t)esp_timer_get_time();
    if (write_us != NULL) {
        *write_us = finished_us > started_us ? finished_us - started_us : 0;
    }
    return ret;
}

static int submit_share_current_transport(int request_id, const bm_job *job, uint32_t nonce,
                                          uint32_t version_bits, uint64_t *write_us)
{
    if (g_transport_lock == NULL) {
        return STRATUM_SUBMIT_NO_TRANSPORT;
    }

    xSemaphoreTake(g_transport_lock, portMAX_DELAY);
    esp_transport_handle_t transport = g_transport;
    int ret = STRATUM_SUBMIT_NO_TRANSPORT;
    if (transport != NULL) {
        ret = submit_share(transport, request_id, job, nonce, version_bits, write_us);
    }
    xSemaphoreGive(g_transport_lock);
    return ret;
}

static void result_task(void *arg)
{
    (void)arg;

    while (true) {
        if (!g_state->ASIC_initalized || atomic_load(&g_work_paused)) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

#ifdef M45_ASIC_LOSS_METRICS
        const uint64_t rx_started_us = (uint64_t)esp_timer_get_time();
#endif
        task_result *result = BM1370_process_work(g_state);
#ifdef M45_ASIC_LOSS_METRICS
        const uint64_t rx_duration_us = (uint64_t)esp_timer_get_time() - rx_started_us;
#endif
        if (result == NULL) {
#ifdef M45_ASIC_LOSS_METRICS
            metric_record_rx_wait(rx_duration_us, rx_duration_us >= 9000000ULL);
            metric_inc(&g_metric_rx_null);
#endif
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }
        metric_record_rx_wait(rx_duration_us, false);
        if (result->register_type != REGISTER_INVALID) {
#ifdef M45_ASIC_LOSS_METRICS
            metric_inc(&g_metric_rx_register_results);
#endif
            record_asic_register_read(result->register_type, result->asic_nr, result->value,
                                      result->timestamp_us);
            continue;
        }
#ifdef M45_ASIC_LOSS_METRICS
        metric_inc(&g_metric_rx_nonce_results);
#endif
        const uint64_t nonce_result_us =
            result->timestamp_us != 0 ? result->timestamp_us
                                      : (uint64_t)esp_timer_get_time();

        const uint8_t job_id = result->job_id;
        bm_job job_snapshot = {0};
        char jobid_snapshot[STRATUM_BUFFER_SIZE];
        char extranonce2_snapshot[MAX_EXTRANONCE2_STR];
        bool job_valid = false;

        pthread_mutex_lock(&g_state->valid_jobs_lock);
        if (job_id < 128 && g_state->valid_jobs[job_id] != 0) {
            const bm_job *active_job = g_state->ASIC_TASK_MODULE.active_jobs[job_id];
            if (active_job != NULL && active_job->jobid != NULL &&
                active_job->extranonce2 != NULL) {
                job_snapshot = *active_job;
                if (strlcpy(jobid_snapshot, active_job->jobid, sizeof(jobid_snapshot)) <
                        sizeof(jobid_snapshot) &&
                    strlcpy(extranonce2_snapshot, active_job->extranonce2,
                            sizeof(extranonce2_snapshot)) < sizeof(extranonce2_snapshot)) {
                    job_snapshot.jobid = jobid_snapshot;
                    job_snapshot.extranonce2 = extranonce2_snapshot;
                    job_valid = true;
                }
            }
        }
        pthread_mutex_unlock(&g_state->valid_jobs_lock);

        if (!job_valid) {
#ifdef M45_ASIC_LOSS_METRICS
            metric_inc(&g_metric_invalid_job_nonces);
#endif
            STRATUM_LOGI("invalid nonce job id 0x%02x", job_id);
            atomic_fetch_add(&g_nonce_errors, 1);
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        const double diff = test_nonce_value(&job_snapshot, result->nonce, result->rolled_version);
        if (diff <= 0.0) {
            atomic_fetch_add(&g_nonce_errors, 1);
            continue;
        }

        if (atomic_load(&g_work_paused)) {
            continue;
        }

        atomic_fetch_add(&g_valid_nonces, 1);
        record_best_diff(diff);
        const double block_diff = job_snapshot.block_diff;
        if (block_diff > 0.0 && diff >= block_diff) {
            record_block_alert(diff);
            ESP_LOGW(TAG, "BLOCK FOUND candidate diff=%.2f target=%.2f job=%s", diff,
                     block_diff, job_snapshot.jobid);
        }

        const double current_diff = current_pool_difficulty();
        const double required_diff =
            current_diff > job_snapshot.pool_diff ? current_diff : job_snapshot.pool_diff;

        if (diff < required_diff) {
            continue;
        }

        STRATUM_LOGI("share job=%s asic=%u core=%u/%u diff=%.2f target=%.2f current=%.2f",
                     job_snapshot.jobid, result->asic_nr, result->core_id,
                     result->small_core_id, diff, job_snapshot.pool_diff, current_diff);

        const uint32_t version_bits = result->rolled_version ^ job_snapshot.version;
        const int request_id = next_uid();
        uint64_t write_us = 0;
        const int ret = submit_share_current_transport(request_id, &job_snapshot, result->nonce,
                                                       version_bits, &write_us);
        if (ret == STRATUM_SUBMIT_NO_TRANSPORT) {
            ESP_LOGW(TAG, "dropping share; no stratum transport");
        } else if (ret < 0) {
            ESP_LOGW(TAG, "share write failed errno=%d", errno);
        } else {
            const uint64_t finished_us = (uint64_t)esp_timer_get_time();
            const uint64_t submit_us =
                finished_us > nonce_result_us ? finished_us - nonce_result_us : write_us;
            record_share_submit_timing(submit_us, write_us);
            mark_share_request(request_id);
            mark_response_request(request_id);
            atomic_fetch_add(&g_submitted, 1);
        }
    }
}

esp_err_t stratum_minimal_start(GlobalState *state)
{
    g_state = state;
    atomic_store(&g_next_using_backup_pool, false);
    atomic_store(&g_switch_to_primary_requested, false);
    atomic_store(&g_primary_probe_in_progress, false);
    atomic_store(&g_last_primary_probe_us, 0);
    atomic_store(&g_last_dns_prefetch_us, 0);
    atomic_store(&g_last_job_sent_us, 0);
    atomic_store(&g_job_sent, 0);
    atomic_store(&g_work_paused, false);
    atomic_store(&g_response_time_ms, 0);
    atomic_store(&g_share_submit_us, 0);
    atomic_store(&g_share_submit_max_us, 0);
    atomic_store(&g_share_write_us, 0);
    atomic_store(&g_share_write_max_us, 0);
    set_payout_status(STRATUM_PAYOUT_STATUS_UNCHECKED, 0);
    reset_hashrate_window();
    set_active_primary_pool_endpoint();
    taskENTER_CRITICAL(&g_best_diff_mux);
    g_best_diff = m45_config_get()->best_diff;
    g_state->SYSTEM_MODULE.best_session_nonce_diff = (uint64_t)g_best_diff;
    taskEXIT_CRITICAL(&g_best_diff_mux);
    g_work_queue = xQueueCreate(WORK_QUEUE_DEPTH, sizeof(queued_work_t));
    g_transport_lock = xSemaphoreCreateMutex();
    g_work_reset_lock = xSemaphoreCreateMutex();
    if (g_work_queue == NULL || g_transport_lock == NULL || g_work_reset_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t monitor_err = init_asic_hashrate_monitor(g_state->DEVICE_CONFIG.family.asic_count);
    if (monitor_err != ESP_OK) {
        ESP_LOGE(TAG, "hashrate monitor init failed: %s", esp_err_to_name(monitor_err));
        return monitor_err;
    }

    if (xTaskCreate(stratum_rx_task, "stratum_rx", 8192, NULL,
                    STRATUM_RX_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(job_task, "asic_jobs", 8192, NULL,
                    STRATUM_JOB_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(result_task, "asic_result", 8192, NULL,
                    STRATUM_RESULT_TASK_PRIORITY, NULL) != pdPASS ||
        xTaskCreate(asic_hashrate_monitor_task, "asic_hashrate", 4096, NULL,
                    STRATUM_HASHRATE_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t stratum_minimal_reset_best_diff(void)
{
    esp_err_t err = m45_config_reset_best_diff();
    if (err != ESP_OK) {
        return err;
    }

    taskENTER_CRITICAL(&g_best_diff_mux);
    g_best_diff = 0.0;
    if (g_state != NULL) {
        g_state->SYSTEM_MODULE.best_session_nonce_diff = 0;
    }
    taskEXIT_CRITICAL(&g_best_diff_mux);
    return ESP_OK;
}

void stratum_minimal_dismiss_block_alert(void)
{
    atomic_store(&g_block_alert_active, false);
}

void stratum_minimal_get_stats(stratum_minimal_stats_t *out)
{
    memset(out, 0, sizeof(*out));
    stratum_endpoint_t endpoint = {0};
    get_active_pool_endpoint(&endpoint);
    out->connected = atomic_load(&g_connected);
    out->work_received = atomic_load(&g_work_received);
    out->submitted = atomic_load(&g_submitted);
    out->accepted = atomic_load(&g_accepted);
    out->rejected = atomic_load(&g_rejected);
    out->valid_nonces = atomic_load(&g_valid_nonces);
    out->nonce_errors = atomic_load(&g_nonce_errors);
    assigned_hashrate_snapshot(&out->measured_hashrate_ghs, &out->nominal_hashrate_ghs,
                               &out->asic_error_rate_percent);
    asic_hashrate_snapshot(&out->measured_hashrate_ghs, &out->nominal_hashrate_ghs,
                           &out->asic_error_rate_percent);
    domain_hashrate_snapshot(&out->domain_hashrate_ghs);
    domain_hashrate_values_snapshot(out);
    out->best_diff = current_best_diff();
    out->pool_diff = current_pool_difficulty();
    out->response_time_ms = atomic_load(&g_response_time_ms);
    out->share_submit_us = atomic_load(&g_share_submit_us);
    out->share_submit_max_us = atomic_load(&g_share_submit_max_us);
    out->share_write_us = atomic_load(&g_share_write_us);
    out->share_write_max_us = atomic_load(&g_share_write_max_us);
    copy_pool_host(out->pool_host, sizeof(out->pool_host), endpoint.host);
    out->pool_port = endpoint.port;
    out->using_backup_pool = endpoint.using_backup;
    out->payout_percent_x100 = (uint16_t)atomic_load(&g_payout_percent_x100);
    out->payout_status = (uint8_t)atomic_load(&g_payout_status);
    out->current_block_seq = atomic_load(&g_current_block_seq);
    out->block_alert_active = atomic_load(&g_block_alert_active);
    out->block_alert_diff = current_block_alert_diff();
    const uint64_t connected_since_us = atomic_load(&g_connected_since_us);
    if (out->connected && connected_since_us > 0) {
        const uint64_t now_us = (uint64_t)esp_timer_get_time();
        out->connected_seconds = now_us > connected_since_us
                                     ? (uint32_t)((now_us - connected_since_us) / 1000000ULL)
                                     : 0;
    }
#ifdef M45_ASIC_LOSS_METRICS
    out->asic_loss.job_sent = atomic_load(&g_metric_job_sent);
    out->asic_loss.job_send_skipped = atomic_load(&g_metric_job_send_skipped);
    out->asic_loss.job_alloc_failed = atomic_load(&g_metric_job_alloc_failed);
    out->asic_loss.job_build_total_us = atomic_load(&g_metric_job_build_total_us);
    out->asic_loss.job_build_max_us = atomic_load(&g_metric_job_build_max_us);
    out->asic_loss.job_send_total_us = atomic_load(&g_metric_job_send_total_us);
    out->asic_loss.job_send_max_us = atomic_load(&g_metric_job_send_max_us);
    out->asic_loss.dispatch_late_count = atomic_load(&g_metric_dispatch_late_count);
    out->asic_loss.dispatch_late_total_us = atomic_load(&g_metric_dispatch_late_total_us);
    out->asic_loss.dispatch_late_max_us = atomic_load(&g_metric_dispatch_late_max_us);
    out->asic_loss.dispatch_missed_slots = atomic_load(&g_metric_dispatch_missed_slots);
    out->asic_loss.rx_calls = atomic_load(&g_metric_rx_calls);
    out->asic_loss.rx_null = atomic_load(&g_metric_rx_null);
    out->asic_loss.rx_timeouts = atomic_load(&g_metric_rx_timeouts);
    out->asic_loss.rx_wait_total_us = atomic_load(&g_metric_rx_wait_total_us);
    out->asic_loss.rx_wait_max_us = atomic_load(&g_metric_rx_wait_max_us);
    out->asic_loss.rx_nonce_results = atomic_load(&g_metric_rx_nonce_results);
    out->asic_loss.rx_register_results = atomic_load(&g_metric_rx_register_results);
    out->asic_loss.invalid_job_nonces = atomic_load(&g_metric_invalid_job_nonces);
#endif
}
