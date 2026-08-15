#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define M45_FAULT_LOG_CAPACITY 8
#define M45_FAULT_MESSAGE_MAX 95

typedef struct {
    uint32_t id;
    uint64_t epoch_seconds;
    uint32_t uptime_seconds;
    char message[M45_FAULT_MESSAGE_MAX + 1];
} m45_fault_entry_t;

esp_err_t m45_fault_log_init(void);
esp_err_t m45_fault_log_record(const char *message);
size_t m45_fault_log_snapshot(m45_fault_entry_t *entries, size_t capacity);
esp_err_t m45_fault_log_remove(uint32_t id);
esp_err_t m45_fault_log_clear(void);
