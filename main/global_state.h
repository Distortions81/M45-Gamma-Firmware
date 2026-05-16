#pragma once

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "esp_transport.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "mining.h"

typedef struct {
    const char *name;
    uint16_t difficulty;
    uint16_t core_count;
    uint16_t small_core_count;
    uint16_t default_asic_timeout;
} AsicConfig;

typedef struct {
    const char *name;
    AsicConfig asic;
    uint8_t asic_count;
} FamilyConfig;

typedef struct {
    const char *board_version;
    FamilyConfig family;
    bool EMC2101 : 1;
    bool TPS546 : 1;
    uint8_t emc_ideality_factor;
    uint8_t emc_beta_compensation;
    int8_t temp_offset;
    uint16_t power_consumption_target;
} DeviceConfig;

typedef struct {
    float fan_perc;
    uint16_t fan_rpm;
    float chip_temp_avg;
    float vr_temp;
    float voltage;
    float frequency_value;
    float actual_frequency;
    float power;
    float current;
    float core_voltage;
} PowerManagementModule;

typedef struct {
    uint64_t shares_accepted;
    uint64_t shares_rejected;
    uint64_t work_received;
    uint64_t best_session_nonce_diff;
    char *pool_url;
    uint16_t pool_port;
    char *pool_user;
    char *pool_pass;
    uint16_t power_fault;
    bool hardware_fault;
    char hardware_fault_msg[64];
} SystemModule;

typedef struct {
    bm_job **active_jobs;
    bm_job *current_job;
    SemaphoreHandle_t semaphore;
} AsicTaskModule;

typedef struct {
    DeviceConfig DEVICE_CONFIG;
    PowerManagementModule POWER_MANAGEMENT_MODULE;
    SystemModule SYSTEM_MODULE;
    AsicTaskModule ASIC_TASK_MODULE;

    char *extranonce_str;
    int extranonce_2_len;

    uint8_t *valid_jobs;
    pthread_mutex_t valid_jobs_lock;

    double pool_difficulty;
    uint32_t version_mask;

    esp_transport_handle_t transport;
    portMUX_TYPE stratum_mux;
    int send_uid;

    bool ASIC_initalized;
} GlobalState;

static const AsicConfig ASIC_BM1370 = {
    .name = "BM1370",
    .difficulty = 256,
    .core_count = 128,
    .small_core_count = 2040,
    .default_asic_timeout = 500,
};

static const FamilyConfig FAMILY_GAMMA = {
    .name = "Gamma",
    .asic = ASIC_BM1370,
    .asic_count = 1,
};
