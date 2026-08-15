#include "m45_fault_log.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#include "esp_timer.h"
#include "nvs.h"

#define FAULT_NAMESPACE "m45_faults"
#define FAULT_KEY "history"
#define FAULT_MAGIC 0x4d343546U
#define VALID_WALL_TIME_EPOCH 1704067200ULL

typedef struct {
    uint32_t magic;
    uint32_t next_id;
    uint8_t count;
    uint8_t reserved[3];
    m45_fault_entry_t entries[M45_FAULT_LOG_CAPACITY];
} fault_store_t;

static pthread_mutex_t g_fault_lock = PTHREAD_MUTEX_INITIALIZER;
static fault_store_t g_faults;

static void fault_store_reset(void)
{
    memset(&g_faults, 0, sizeof(g_faults));
    g_faults.magic = FAULT_MAGIC;
    g_faults.next_id = 1;
}

static esp_err_t fault_store_save_locked(void)
{
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READWRITE, &nvs);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(nvs, FAULT_KEY, &g_faults, sizeof(g_faults));
    if (err == ESP_OK) {
        err = nvs_commit(nvs);
    }
    nvs_close(nvs);
    return err;
}

esp_err_t m45_fault_log_init(void)
{
    pthread_mutex_lock(&g_fault_lock);
    fault_store_reset();
    nvs_handle_t nvs;
    esp_err_t err = nvs_open(FAULT_NAMESPACE, NVS_READONLY, &nvs);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        pthread_mutex_unlock(&g_fault_lock);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        size_t size = sizeof(g_faults);
        err = nvs_get_blob(nvs, FAULT_KEY, &g_faults, &size);
        nvs_close(nvs);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            err = ESP_OK;
        }
        if (err != ESP_OK || size != sizeof(g_faults) || g_faults.magic != FAULT_MAGIC ||
            g_faults.count > M45_FAULT_LOG_CAPACITY || g_faults.next_id == 0) {
            fault_store_reset();
            err = ESP_OK;
        }
    }
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

esp_err_t m45_fault_log_record(const char *message)
{
    if (message == NULL || message[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&g_fault_lock);
    const uint32_t uptime_seconds = (uint32_t)(esp_timer_get_time() / 1000000ULL);
    if (g_faults.count > 0) {
        const m45_fault_entry_t *latest = &g_faults.entries[g_faults.count - 1U];
        if (strcmp(latest->message, message) == 0 &&
            uptime_seconds >= latest->uptime_seconds &&
            uptime_seconds - latest->uptime_seconds <= 5U) {
            pthread_mutex_unlock(&g_fault_lock);
            return ESP_OK;
        }
    }
    if (g_faults.count == M45_FAULT_LOG_CAPACITY) {
        memmove(&g_faults.entries[0], &g_faults.entries[1],
                sizeof(g_faults.entries[0]) * (M45_FAULT_LOG_CAPACITY - 1));
        --g_faults.count;
    }
    m45_fault_entry_t *entry = &g_faults.entries[g_faults.count++];
    memset(entry, 0, sizeof(*entry));
    entry->id = g_faults.next_id++;
    if (g_faults.next_id == 0) {
        g_faults.next_id = 1;
    }
    const time_t now = time(NULL);
    entry->epoch_seconds = now >= (time_t)VALID_WALL_TIME_EPOCH ? (uint64_t)now : 0;
    entry->uptime_seconds = uptime_seconds;
    strlcpy(entry->message, message, sizeof(entry->message));
    const esp_err_t err = fault_store_save_locked();
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

size_t m45_fault_log_snapshot(m45_fault_entry_t *entries, size_t capacity)
{
    if (entries == NULL || capacity == 0) {
        return 0;
    }
    pthread_mutex_lock(&g_fault_lock);
    const size_t count = g_faults.count < capacity ? g_faults.count : capacity;
    for (size_t i = 0; i < count; ++i) {
        entries[i] = g_faults.entries[g_faults.count - 1U - i];
    }
    pthread_mutex_unlock(&g_fault_lock);
    return count;
}

esp_err_t m45_fault_log_remove(uint32_t id)
{
    pthread_mutex_lock(&g_fault_lock);
    size_t index = g_faults.count;
    for (size_t i = 0; i < g_faults.count; ++i) {
        if (g_faults.entries[i].id == id) {
            index = i;
            break;
        }
    }
    if (index == g_faults.count) {
        pthread_mutex_unlock(&g_fault_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (index + 1U < g_faults.count) {
        memmove(&g_faults.entries[index], &g_faults.entries[index + 1U],
                sizeof(g_faults.entries[0]) * (g_faults.count - index - 1U));
    }
    memset(&g_faults.entries[g_faults.count - 1U], 0, sizeof(g_faults.entries[0]));
    --g_faults.count;
    const esp_err_t err = fault_store_save_locked();
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}

esp_err_t m45_fault_log_clear(void)
{
    pthread_mutex_lock(&g_fault_lock);
    const uint32_t next_id = g_faults.next_id;
    fault_store_reset();
    g_faults.next_id = next_id == 0 ? 1 : next_id;
    const esp_err_t err = fault_store_save_locked();
    pthread_mutex_unlock(&g_fault_lock);
    return err;
}
