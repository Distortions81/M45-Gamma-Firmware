#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define M45_LOG_BUFFER_SIZE 16384

void m45_log_buffer_init(void);
void m45_log_buffer_keep_active(uint32_t timeout_ms);
void m45_log_buffer_append_verbose(const char *tag, const char *fmt, ...);
size_t m45_log_buffer_copy_since(uint64_t since_seq, char *dst, size_t dst_size,
                                 uint64_t *next_seq, bool *truncated);
