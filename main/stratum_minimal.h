#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "global_state.h"
#include "m45_config.h"

#define STRATUM_HASHRATE_MAX_ASICS 2
#define STRATUM_HASH_DOMAIN_COUNT 4

#ifdef M45_ASIC_LOSS_METRICS
typedef struct {
    uint64_t job_sent;
    uint64_t job_send_skipped;
    uint64_t job_alloc_failed;
    uint64_t job_build_total_us;
    uint64_t job_build_max_us;
    uint64_t job_send_total_us;
    uint64_t job_send_max_us;
    uint64_t dispatch_late_count;
    uint64_t dispatch_late_total_us;
    uint64_t dispatch_late_max_us;
    uint64_t dispatch_missed_slots;
    uint64_t rx_calls;
    uint64_t rx_null;
    uint64_t rx_timeouts;
    uint64_t rx_wait_total_us;
    uint64_t rx_wait_max_us;
    uint64_t rx_nonce_results;
    uint64_t rx_register_results;
    uint64_t invalid_job_nonces;
} stratum_asic_loss_metrics_t;
#endif

typedef struct {
    bool connected;
    uint32_t work_received;
    uint32_t submitted;
    uint32_t accepted;
    uint32_t rejected;
    uint32_t valid_nonces;
    uint32_t nonce_errors;
    double measured_hashrate_ghs;
    double nominal_hashrate_ghs;
    double domain_hashrate_ghs;
    uint8_t domain_asic_count;
    uint8_t domain_count;
    double domain_hashrates_ghs[STRATUM_HASHRATE_MAX_ASICS][STRATUM_HASH_DOMAIN_COUNT];
    double asic_error_rate_percent;
    double best_diff;
    double pool_diff;
    uint32_t response_time_ms;
    uint64_t share_submit_us;
    uint64_t share_submit_max_us;
    uint64_t share_write_us;
    uint64_t share_write_max_us;
    char pool_host[M45_POOL_HOST_MAX + 1];
    uint16_t pool_port;
    bool using_backup_pool;
    uint16_t payout_percent_x100;
    uint8_t payout_status;
    uint32_t connected_seconds;
    uint32_t current_block_seq;
    bool block_alert_active;
    double block_alert_diff;
#ifdef M45_ASIC_LOSS_METRICS
    stratum_asic_loss_metrics_t asic_loss;
#endif
} stratum_minimal_stats_t;

enum {
    STRATUM_PAYOUT_STATUS_UNCHECKED = 0,
    STRATUM_PAYOUT_STATUS_OK,
    STRATUM_PAYOUT_STATUS_LOW,
    STRATUM_PAYOUT_STATUS_MISSING,
    STRATUM_PAYOUT_STATUS_UNSUPPORTED_WALLET,
    STRATUM_PAYOUT_STATUS_PARSE_ERROR,
};

esp_err_t stratum_minimal_start(GlobalState *state);
void stratum_minimal_reconnect(void);
void stratum_minimal_pause_work(void);
void stratum_minimal_resume_work(void);
bool stratum_minimal_work_paused(void);
uint32_t stratum_minimal_job_sent_count(void);
esp_err_t stratum_minimal_reset_best_diff(void);
void stratum_minimal_dismiss_block_alert(void);
void stratum_minimal_get_stats(stratum_minimal_stats_t *out);
