#include "wifi_http_status.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bitaxe_fan.h"
#include "bitaxe_hw.h"
#include "build_info.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "m45_config.h"
#include "stratum_minimal.h"

#define STATUS_JSON_BUFFER_SIZE (32 * 1024)
#define M45_DEVICE_NAME "M45-Firmware"

typedef struct {
    stratum_minimal_stats_t stats;
    bitaxe_gamma602_power_snapshot_t power;
    bitaxe_gamma602_safety_limits_t limits;
    bitaxe_gamma602_auto_clock_status_t auto_clock;
    char wifi_ssid[80];
    char pool_host[160];
    char hardware_fault_msg[96];
    char imported_board_version[32];
    char auto_clock_hold_reason[128];
    char tps546_model[24];
    char domain_hashrates_json[512];
    char pool_statuses_json[4096];
    stratum_share_event_t share_events[STRATUM_SHARE_EVENT_MAX];
    char share_events_json[2048];
#ifdef M45_ASIC_LOSS_METRICS
    char asic_loss_json[1024];
#endif
} status_handler_scratch_t;

static bool format_share_events_json(status_handler_scratch_t *scratch)
{
    const size_t count = stratum_minimal_get_share_events(
        scratch->share_events, STRATUM_SHARE_EVENT_MAX);
    const uint64_t now_us = (uint64_t)esp_timer_get_time();
    size_t used = 0;
    scratch->share_events_json[used++] = '[';
    for (size_t i = 0; i < count; ++i) {
        const stratum_share_event_t *event = &scratch->share_events[i];
        const uint64_t age_ms = now_us > event->timestamp_us
                                    ? (now_us - event->timestamp_us) / 1000ULL
                                    : 0;
        const int written = snprintf(
            scratch->share_events_json + used,
            sizeof(scratch->share_events_json) - used,
            "%s[%" PRIu32 ",%" PRIu64 ",%.6g]",
            i == 0 ? "" : ",", event->sequence, age_ms, event->difficulty);
        if (written < 0 ||
            written >= (int)(sizeof(scratch->share_events_json) - used)) {
            return false;
        }
        used += (size_t)written;
    }
    if (used + 2 > sizeof(scratch->share_events_json)) {
        return false;
    }
    scratch->share_events_json[used++] = ']';
    scratch->share_events_json[used] = '\0';
    return true;
}

static void set_no_store_headers(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "Expires", "0");
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

static uint16_t suggested_pool_difficulty_for_config(const m45_config_t *config,
                                                     const GlobalState *state)
{
    if (config == NULL || state == NULL) {
        return config != NULL ? config->pool_difficulty : 1;
    }
    return m45_config_effective_pool_difficulty(
        config, state->DEVICE_CONFIG.family.asic.small_core_count,
        state->DEVICE_CONFIG.family.asic_count);
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

esp_err_t wifi_http_status_send(httpd_req_t *req,
                                const wifi_http_status_context_t *context)
{
    status_handler_scratch_t *scratch = calloc(1, sizeof(*scratch));
    if (scratch == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    const m45_config_t *config = m45_config_get();
    stratum_minimal_stats_t *stats = &scratch->stats;
    bitaxe_gamma602_power_snapshot_t *power = &scratch->power;
    bitaxe_gamma602_safety_limits_t *limits = &scratch->limits;
    bitaxe_gamma602_auto_clock_status_t *auto_clock = &scratch->auto_clock;
    stratum_minimal_get_stats(stats);
    const bool have_power = bitaxe_gamma602_power_snapshot(power);
    const float asic_power_watts = have_power ? power->read_vout * power->read_iout : 0.0f;
    const double asic_efficiency_j_per_th =
        asic_power_watts > 0.0f && stats->measured_hashrate_ghs > 0.0
            ? ((double)asic_power_watts * 1000.0) / stats->measured_hashrate_ghs
            : 0.0;
    bitaxe_gamma602_safety_limits(limits);
    bitaxe_gamma602_auto_clock_status(auto_clock);
    wifi_ap_record_t ap_info = {0};
    const int wifi_rssi = context->wifi_connected && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK
                              ? ap_info.rssi
                              : 0;
    const uint8_t chip_count = bitaxe_gamma602_chip_count();
    const float active_frequency_mhz = context->state->POWER_MANAGEMENT_MODULE.actual_frequency > 0.0f
                                           ? context->state->POWER_MANAGEMENT_MODULE.actual_frequency
                                           : (float)m45_config_effective_asic_frequency_mhz(config);
    const bool ota_supported = context->ota_supported;
    const size_t retained_axeos_count = context->retained_axeos_count;
    const bool ota_preserve_axeos_possible = context->ota_preserve_axeos_possible;
    const bool unsupported_board = !m45_config_hardware_identity_allowed();
    const bool axeos_return_available = context->axeos_return_available;
    const uint8_t expected_chip_count =
        chip_count > 0 ? chip_count : context->state->DEVICE_CONFIG.family.asic_count;
    const double expected_hashrate_ghs =
        (double)active_frequency_mhz *
        (double)context->state->DEVICE_CONFIG.family.asic.small_core_count *
        (double)expected_chip_count / 1000.0;
    const uint16_t suggested_pool_difficulty =
        suggested_pool_difficulty_for_config(config, context->state);
    const char *hardware_status = bitaxe_gamma602_status();
    const bool asic_power_enabled = bitaxe_gamma602_asic_power_enabled();
    const bool asic_power_off = !asic_power_enabled && strcmp(hardware_status, "asic off") == 0;
    const bool booting = !asic_power_off && !context->state->ASIC_initalized &&
                         !context->state->SYSTEM_MODULE.hardware_fault &&
                         strcmp(hardware_status, "ready") != 0;
    const bool fan_auto = !config->fan_override_enabled;
    const uint16_t asic_temp_target_c =
        fan_auto ? m45_config_effective_fan_target_temp_c(config) : M45_FAN_TARGET_DEFAULT_C;
    const float asic_temp_c = context->state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
    const uint16_t voltage_base_mv = m45_config_effective_asic_voltage_mv(config);
    const int16_t voltage_compensation_mv =
        m45_config_asic_voltage_temp_compensation_mv(config, asic_temp_c);
    const uint16_t voltage_target_mv =
        m45_config_effective_asic_voltage_mv_for_temp(config, asic_temp_c);

    json_escape(scratch->wifi_ssid, sizeof(scratch->wifi_ssid), config->wifi_ssid);
    json_escape(scratch->pool_host, sizeof(scratch->pool_host),
                stats->pool_host[0] != '\0' ? stats->pool_host : config->pool_host);
    json_escape(scratch->hardware_fault_msg, sizeof(scratch->hardware_fault_msg),
                context->state->SYSTEM_MODULE.hardware_fault ? context->state->SYSTEM_MODULE.hardware_fault_msg
                                                       : "");
    json_escape(scratch->imported_board_version,
                sizeof(scratch->imported_board_version),
                m45_config_imported_board_version());
    json_escape(scratch->auto_clock_hold_reason, sizeof(scratch->auto_clock_hold_reason),
                auto_clock->hold_reason);
    json_escape(scratch->tps546_model, sizeof(scratch->tps546_model),
                bitaxe_gamma602_tps_model());
    format_domain_hashrates_json(stats, expected_chip_count,
                                  scratch->domain_hashrates_json,
                                  sizeof(scratch->domain_hashrates_json));
    if (!format_share_events_json(scratch)) {
        free(scratch);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
    }
    size_t pool_status_offset = 0;
    scratch->pool_statuses_json[pool_status_offset++] = '[';
    bool first_pool_status = true;
    for (size_t i = 0; i < stats->pool_status_count; ++i) {
        const stratum_pool_status_t *pool = &stats->pool_statuses[i];
        if (!pool->configured) {
            continue;
        }
        char status_host[160];
        char status_note[160];
        char status_label[16];
        json_escape(status_host, sizeof(status_host), pool->pool_host);
        json_escape(status_note, sizeof(status_note), pool->note);
        if (pool->auxiliary) {
            snprintf(status_label, sizeof(status_label), "Pool %u",
                     (unsigned)pool->pool_id);
        } else {
            strlcpy(status_label, "Pool", sizeof(status_label));
        }
        const char *role = "pool";
        const int written = snprintf(
            scratch->pool_statuses_json + pool_status_offset,
            sizeof(scratch->pool_statuses_json) - pool_status_offset,
            "%s{\"pool_id\":%u,\"label\":\"%s\",\"role\":\"%s\","
            "\"host\":\"%s\",\"port\":%u,\"connected\":%s,\"using_backup\":%s,"
            "\"disabled\":%s,\"tls\":%s,\"tls_invalid\":%s,\"note\":\"%s\","
            "\"weight\":%u,\"share_percent\":%u,"
            "\"connected_seconds\":%lu,\"response_ms\":%lu,"
            "\"pool_difficulty\":%.2f,"
            "\"work_received\":%lu,\"submitted\":%lu,\"accepted\":%lu,\"rejected\":%lu,"
            "\"payout_status\":\"%s\",\"payout_percent_x100\":%u}",
            first_pool_status ? "" : ",", (unsigned)pool->pool_id, status_label, role,
            status_host,
            pool->pool_port, pool->connected ? "true" : "false",
            pool->using_backup_pool ? "true" : "false",
            pool->disabled ? "true" : "false", pool->tls ? "true" : "false",
            pool->tls_invalid ? "true" : "false", status_note, pool->weight,
            pool->share_percent,
            (unsigned long)pool->connected_seconds,
            (unsigned long)pool->response_time_ms, pool->pool_diff,
            (unsigned long)pool->work_received, (unsigned long)pool->submitted,
            (unsigned long)pool->accepted, (unsigned long)pool->rejected,
            payout_status_name(pool->payout_status), pool->payout_percent_x100);
        if (written < 0 ||
            written >= (int)(sizeof(scratch->pool_statuses_json) - pool_status_offset)) {
            free(scratch);
            httpd_resp_set_status(req, "500 Internal Server Error");
            return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
        }
        pool_status_offset += (size_t)written;
        first_pool_status = false;
    }
    if (pool_status_offset + 2 > sizeof(scratch->pool_statuses_json)) {
        free(scratch);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
    }
    scratch->pool_statuses_json[pool_status_offset++] = ']';
    scratch->pool_statuses_json[pool_status_offset] = '\0';
#ifdef M45_ASIC_LOSS_METRICS
    snprintf(scratch->asic_loss_json, sizeof(scratch->asic_loss_json),
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
             stats->asic_loss.job_sent, stats->asic_loss.job_send_skipped,
             stats->asic_loss.job_alloc_failed, stats->asic_loss.job_build_total_us,
             stats->asic_loss.job_build_max_us, stats->asic_loss.job_send_total_us,
             stats->asic_loss.job_send_max_us, stats->asic_loss.dispatch_late_count,
             stats->asic_loss.dispatch_late_total_us, stats->asic_loss.dispatch_late_max_us,
             stats->asic_loss.dispatch_missed_slots, stats->asic_loss.rx_calls,
             stats->asic_loss.rx_null, stats->asic_loss.rx_timeouts,
             stats->asic_loss.rx_wait_total_us, stats->asic_loss.rx_wait_max_us,
             stats->asic_loss.rx_nonce_results, stats->asic_loss.rx_register_results,
             stats->asic_loss.invalid_job_nonces);
#endif

    char *body = malloc(STATUS_JSON_BUFFER_SIZE);
    if (body == NULL) {
        free(scratch);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"out of memory\"}");
    }

    const int body_len =
        snprintf(body, STATUS_JSON_BUFFER_SIZE,
                 "{"
                 "\"page_token\":\"%s\","
                 "\"device_name\":\"%s\","
                 "\"version\":\"%s\","
                 "\"build_id\":\"%s\","
                 "\"build_time\":\"%s\","
                 "\"ota_supported\":%s,"
                 "\"retained_axeos_present\":%s,"
                 "\"retained_axeos_count\":%u,"
                 "\"ota_preserve_axeos_possible\":%s,"
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
                 "\"voltage_temp_compensation_mv\":%d,"
                 "\"overclock_enabled\":%s,"
                 "\"auto_clock_enabled\":%s,"
                 "\"auto_domain_reboot_enabled\":%s,"
                 "\"safety_limits_unrestricted\":%s,"
                 "\"auto_clock_target_temp_c\":%u,"
                 "\"auto_clock_max_watts_enabled\":%s,"
                 "\"auto_clock_max_watts\":%u,"
                 "\"auto_clock_active\":%s,"
                 "\"auto_clock_input_voltage_limited\":%s,"
                 "\"auto_clock_output_current_limited\":%s,"
                 "\"auto_clock_vr_temp_limited\":%s,"
                 "\"auto_clock_power_limited\":%s,"
                 "\"auto_clock_temperature_limited\":%s,"
                 "\"auto_clock_hold_reason\":\"%s\","
                 "\"auto_clock_target_frequency_mhz\":%u,"
                 "\"auto_clock_target_voltage_mv\":%u,"
                 "\"auto_clock_next_up_frequency_mhz\":%u,"
                 "\"auto_clock_power_now_w\":%.2f,"
                 "\"auto_clock_power_target_w\":%.2f,"
                 "\"auto_clock_next_up_power_w\":%.2f,"
                 "\"auto_clock_thermal_resistance_c_per_w\":%.2f,"
                 "\"auto_clock_output_current_ceiling_a\":%.2f,"
                 "\"auto_clock_next_up_output_current_a\":%.2f,"
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
                 "\"asic_efficiency_j_per_th\":%.2f,"
                 "\"power_fault\":%u,"
                 "\"hardware_fault\":%s,"
                 "\"hardware_fault_msg\":\"%s\","
                 "\"unsupported_board\":%s,"
                 "\"imported_board_version\":\"%s\","
                 "\"axeos_return_available\":%s,"
                 "\"pool\":\"%s\","
                 "\"pool_port\":%u,"
                 "\"pool_using_backup\":%s,"
                 "\"multi_pool_enabled\":%s,"
                 "\"pool_statuses\":%s,"
                 "\"stratum_connected\":%s,"
                 "\"stratum_connected_seconds\":%lu,"
                 "\"stratum_response_ms\":%lu,"
                 "\"stratum_share_submit_us\":%" PRIu64 ","
                 "\"stratum_share_submit_max_us\":%" PRIu64 ","
                 "\"stratum_share_write_us\":%" PRIu64 ","
                 "\"stratum_share_write_max_us\":%" PRIu64 ","
                 "\"stratum_line_handle_us\":%" PRIu64 ","
                 "\"stratum_line_handle_max_us\":%" PRIu64 ","
                 "\"stratum_job_queue_wait_us\":%" PRIu64 ","
                 "\"stratum_job_queue_wait_max_us\":%" PRIu64 ","
                 "\"stratum_job_dispatch_us\":%" PRIu64 ","
                 "\"stratum_job_dispatch_max_us\":%" PRIu64 ","
                 "\"work_received\":%lu,"
                 "\"shares_accepted\":%lu,"
                 "\"shares_rejected\":%lu,"
                 "\"share_events\":%s,"
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
                 context->page_token, M45_DEVICE_NAME, APP_BUILD_VERSION, APP_BUILD_ID,
                 APP_BUILD_TIME_UTC,
                 ota_supported ? "true" : "false",
                 retained_axeos_count > 0 ? "true" : "false",
                 (unsigned)retained_axeos_count,
                 ota_preserve_axeos_possible ? "true" : "false",
                 context->wifi_connected ? "true" : "false", context->ip, wifi_rssi,
                 hardware_status, booting ? "true" : "false",
                 context->setup_active ? "true" : "false", context->setup_ssid, context->setup_ip,
                 context->state->ASIC_initalized ? "true" : "false",
                 asic_power_enabled ? "true" : "false",
                 context->state->DEVICE_CONFIG.family.name, context->state->DEVICE_CONFIG.board_version,
                 context->state->DEVICE_CONFIG.family.asic.name, chip_count,
                 context->state->POWER_MANAGEMENT_MODULE.actual_frequency,
                 stats->measured_hashrate_ghs, stats->nominal_hashrate_ghs,
                 stats->domain_hashrate_ghs, scratch->domain_hashrates_json,
                 stats->asic_error_rate_percent,
                 expected_hashrate_ghs, scratch->wifi_ssid,
                 voltage_target_mv, voltage_base_mv,
                 config->asic_voltage_temp_compensation_enabled ? "true" : "false",
                 voltage_compensation_mv,
                 config->overclock_enabled ? "true" : "false",
                 config->auto_clock_enabled ? "true" : "false",
                 config->auto_domain_reboot_enabled ? "true" : "false",
                 config->safety_limits_unrestricted ? "true" : "false",
                 m45_config_effective_auto_clock_target_temp_c(config),
                 config->auto_clock_max_watts_enabled ? "true" : "false",
                 m45_config_effective_auto_clock_max_watts(config),
                 auto_clock->active ? "true" : "false",
                 auto_clock->input_voltage_limited ? "true" : "false",
                 auto_clock->output_current_limited ? "true" : "false",
                 auto_clock->vr_temp_limited ? "true" : "false",
                 auto_clock->power_limited ? "true" : "false",
                 auto_clock->temperature_limited ? "true" : "false",
                 scratch->auto_clock_hold_reason,
                 auto_clock->target_frequency_mhz, auto_clock->target_voltage_mv,
                 auto_clock->next_up_frequency_mhz,
                 auto_clock->power_now_w, auto_clock->power_target_w,
                 auto_clock->next_up_power_w,
                 auto_clock->thermal_resistance_c_per_w,
                 auto_clock->output_current_ceiling_a,
                 auto_clock->next_up_output_current_a,
                 asic_temp_c,
                 context->state->POWER_MANAGEMENT_MODULE.fan_perc,
                 context->state->POWER_MANAGEMENT_MODULE.fan_rpm,
                 fan_auto ? "true" : "false",
                 config->fan_auto_off_allowed ? "true" : "false", asic_temp_target_c,
                 have_power ? "true" : "false", have_power ? power->read_vout : 0.0f,
                 have_power ? power->read_vin : 0.0f, have_power ? power->read_iout : 0.0f,
                 have_power ? power->read_temp_c : 0, scratch->tps546_model,
                 asic_power_watts,
                 asic_efficiency_j_per_th, context->state->SYSTEM_MODULE.power_fault,
                 context->state->SYSTEM_MODULE.hardware_fault ? "true" : "false",
                 scratch->hardware_fault_msg,
                 unsupported_board ? "true" : "false",
                 scratch->imported_board_version,
                 axeos_return_available ? "true" : "false",
                 scratch->pool_host,
                 stats->pool_port > 0 ? stats->pool_port : config->pool_port,
                 stats->using_backup_pool ? "true" : "false",
                 config->multi_pool_enabled ? "true" : "false",
                 scratch->pool_statuses_json,
                 stats->connected ? "true" : "false",
                 (unsigned long)stats->connected_seconds,
                 (unsigned long)stats->response_time_ms,
                 stats->share_submit_us, stats->share_submit_max_us,
                 stats->share_write_us, stats->share_write_max_us,
                 stats->line_handle_us, stats->line_handle_max_us,
                 stats->job_queue_wait_us, stats->job_queue_wait_max_us,
                 stats->job_dispatch_us, stats->job_dispatch_max_us,
                 (unsigned long)stats->work_received,
                 (unsigned long)stats->accepted, (unsigned long)stats->rejected,
                 scratch->share_events_json,
                 (unsigned long)stats->valid_nonces, (unsigned long)stats->nonce_errors,
                 stats->best_diff, stats->pool_diff,
                 config->pool_difficulty_auto ? "true" : "false",
                 suggested_pool_difficulty, payout_status_name(stats->payout_status),
                 stats->payout_percent_x100,
                 stats->block_alert_active ? "true" : "false", stats->block_alert_diff,
#ifdef M45_ASIC_LOSS_METRICS
                 scratch->asic_loss_json,
#endif
                 limits->input_voltage_min_v,
                 limits->input_voltage_expected_min_v, limits->input_voltage_expected_max_v,
                 limits->input_voltage_max_v, limits->asic_voltage_min_v,
                 limits->asic_voltage_expected_min_v, limits->asic_voltage_expected_max_v,
                 limits->asic_voltage_max_v, limits->asic_voltage_target_v,
                 limits->asic_temp_expected_max_c, limits->asic_temp_max_c,
                 limits->tps546_temp_expected_max_c, limits->tps546_temp_max_c,
                 limits->iout_warn_a, limits->iout_fault_a, limits->power_warn_w,
                 limits->power_fault_w, limits->fan_expected_percent);
    if (body_len < 0 || body_len >= STATUS_JSON_BUFFER_SIZE) {
        free(body);
        free(scratch);
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "{\"error\":\"status too large\"}");
    }

    httpd_resp_set_type(req, "application/json");
    set_no_store_headers(req);
    const esp_err_t err = httpd_resp_send(req, body, body_len);
    free(body);
    free(scratch);
    return err;
}
