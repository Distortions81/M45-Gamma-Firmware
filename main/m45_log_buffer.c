#include "m45_log_buffer.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#define M45_LOG_LINE_MAX 512

static char *g_log_buffer;
static uint64_t g_log_seq;
static int64_t g_log_capture_until_us;
static portMUX_TYPE g_log_mux = portMUX_INITIALIZER_UNLOCKED;
static esp_timer_handle_t g_log_release_timer;
static vprintf_like_t g_previous_vprintf;
static bool g_log_buffer_ready;

static void release_log_buffer(void *arg)
{
    (void)arg;

    const int64_t now_us = esp_timer_get_time();
    char *old_buffer = NULL;

    portENTER_CRITICAL(&g_log_mux);
    if (g_log_capture_until_us > now_us) {
        const uint64_t delay_us = (uint64_t)(g_log_capture_until_us - now_us);
        portEXIT_CRITICAL(&g_log_mux);
        if (g_log_release_timer != NULL) {
            (void)esp_timer_start_once(g_log_release_timer, delay_us);
        }
        return;
    }

    old_buffer = g_log_buffer;
    g_log_buffer = NULL;
    g_log_seq = 0;
    g_log_capture_until_us = 0;
    portEXIT_CRITICAL(&g_log_mux);

    free(old_buffer);
}

static bool log_capture_active(void)
{
    char *buffer = NULL;
    int64_t until_us = 0;

    portENTER_CRITICAL(&g_log_mux);
    buffer = g_log_buffer;
    until_us = g_log_capture_until_us;
    portEXIT_CRITICAL(&g_log_mux);

    return buffer != NULL && until_us > esp_timer_get_time();
}

static void append_log_bytes(const char *data, size_t len)
{
    if (data == NULL || len == 0) {
        return;
    }

    if (len > M45_LOG_BUFFER_SIZE) {
        data += len - M45_LOG_BUFFER_SIZE;
        len = M45_LOG_BUFFER_SIZE;
    }

    portENTER_CRITICAL(&g_log_mux);
    if (g_log_buffer == NULL) {
        portEXIT_CRITICAL(&g_log_mux);
        return;
    }

    const size_t offset = (size_t)(g_log_seq % M45_LOG_BUFFER_SIZE);
    const size_t first = len < (M45_LOG_BUFFER_SIZE - offset)
                             ? len
                             : (M45_LOG_BUFFER_SIZE - offset);
    memcpy(&g_log_buffer[offset], data, first);
    if (len > first) {
        memcpy(g_log_buffer, data + first, len - first);
    }
    g_log_seq += len;
    portEXIT_CRITICAL(&g_log_mux);
}

static int m45_log_vprintf(const char *fmt, va_list args)
{
    if (!log_capture_active()) {
        return g_previous_vprintf != NULL ? g_previous_vprintf(fmt, args)
                                          : vprintf(fmt, args);
    }

    va_list copy;
    va_copy(copy, args);
    const int written = g_previous_vprintf != NULL ? g_previous_vprintf(fmt, args)
                                                   : vprintf(fmt, args);

    char line[M45_LOG_LINE_MAX];
    const int len = vsnprintf(line, sizeof(line), fmt, copy);
    va_end(copy);
    if (len > 0) {
        append_log_bytes(line, len < (int)sizeof(line) ? (size_t)len : sizeof(line) - 1);
    }

    return written;
}

void m45_log_buffer_init(void)
{
    if (g_log_buffer_ready) {
        return;
    }

    const esp_timer_create_args_t timer_args = {
        .callback = release_log_buffer,
        .name = "log_release",
    };
    (void)esp_timer_create(&timer_args, &g_log_release_timer);

    g_previous_vprintf = esp_log_set_vprintf(m45_log_vprintf);
    g_log_buffer_ready = true;
}

void m45_log_buffer_keep_active(uint32_t timeout_ms)
{
    char *new_buffer = NULL;

    portENTER_CRITICAL(&g_log_mux);
    const bool needs_buffer = g_log_buffer == NULL;
    portEXIT_CRITICAL(&g_log_mux);

    if (needs_buffer) {
        new_buffer = malloc(M45_LOG_BUFFER_SIZE);
        if (new_buffer == NULL) {
            return;
        }
    }

    const uint64_t timeout_us = (uint64_t)timeout_ms * 1000ULL;
    const int64_t active_until_us = esp_timer_get_time() + (int64_t)timeout_us;

    portENTER_CRITICAL(&g_log_mux);
    if (g_log_buffer == NULL) {
        g_log_buffer = new_buffer;
        new_buffer = NULL;
        g_log_seq = 0;
    }
    g_log_capture_until_us = active_until_us;
    portEXIT_CRITICAL(&g_log_mux);

    free(new_buffer);

    if (g_log_release_timer != NULL) {
        (void)esp_timer_stop(g_log_release_timer);
        (void)esp_timer_start_once(g_log_release_timer, timeout_us);
    }
}

void m45_log_buffer_append_verbose(const char *tag, const char *fmt, ...)
{
    if (!log_capture_active() || fmt == NULL) {
        return;
    }

    char line[M45_LOG_LINE_MAX];
    const int prefix_len = snprintf(line, sizeof(line), "V (%lu) %s: ",
                                    (unsigned long)(esp_timer_get_time() / 1000LL),
                                    tag != NULL ? tag : "app");
    if (prefix_len < 0 || prefix_len >= (int)sizeof(line)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    const int message_len = vsnprintf(line + prefix_len, sizeof(line) - (size_t)prefix_len,
                                      fmt, args);
    va_end(args);
    if (message_len < 0) {
        return;
    }

    size_t used = (size_t)prefix_len;
    const size_t message_space = sizeof(line) - used;
    used += (size_t)message_len < message_space ? (size_t)message_len : message_space - 1;
    if (used + 1 < sizeof(line)) {
        line[used++] = '\n';
    } else {
        line[sizeof(line) - 2] = '\n';
        used = sizeof(line) - 1;
    }

    append_log_bytes(line, used);
}

size_t m45_log_buffer_copy_since(uint64_t since_seq, char *dst, size_t dst_size,
                                 uint64_t *next_seq, bool *truncated)
{
    if (next_seq != NULL) {
        *next_seq = 0;
    }
    if (truncated != NULL) {
        *truncated = false;
    }
    if (dst == NULL || dst_size == 0) {
        return 0;
    }

    portENTER_CRITICAL(&g_log_mux);
    if (g_log_buffer == NULL) {
        portEXIT_CRITICAL(&g_log_mux);
        dst[0] = '\0';
        return 0;
    }

    const uint64_t end_seq = g_log_seq;
    uint64_t start_seq = since_seq;
    bool was_truncated = false;

    if (start_seq == 0 || start_seq + M45_LOG_BUFFER_SIZE < end_seq) {
        start_seq = end_seq > M45_LOG_BUFFER_SIZE ? end_seq - M45_LOG_BUFFER_SIZE : 0;
        was_truncated = since_seq != 0;
    } else if (start_seq > end_seq) {
        start_seq = end_seq;
    }

    size_t available = (size_t)(end_seq - start_seq);
    size_t copy_len = available < (dst_size - 1) ? available : (dst_size - 1);
    if (copy_len < available) {
        start_seq = end_seq - copy_len;
        was_truncated = true;
    }

    const size_t offset = (size_t)(start_seq % M45_LOG_BUFFER_SIZE);
    const size_t first = copy_len < (M45_LOG_BUFFER_SIZE - offset)
                             ? copy_len
                             : (M45_LOG_BUFFER_SIZE - offset);
    memcpy(dst, &g_log_buffer[offset], first);
    if (copy_len > first) {
        memcpy(dst + first, g_log_buffer, copy_len - first);
    }
    dst[copy_len] = '\0';
    portEXIT_CRITICAL(&g_log_mux);

    if (next_seq != NULL) {
        *next_seq = end_seq;
    }
    if (truncated != NULL) {
        *truncated = was_truncated;
    }
    return copy_len;
}
