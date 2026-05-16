#ifndef M45_BITMAIN_ASIC_H_
#define M45_BITMAIN_ASIC_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

static const double NONCE_SPACE = 4294967296.0;

typedef enum {
    REGISTER_INVALID = 0,
    REGISTER_HASHRATE,
    REGISTER_TOTAL_COUNT,
    REGISTER_DOMAIN_0_COUNT,
    REGISTER_DOMAIN_1_COUNT,
    REGISTER_DOMAIN_2_COUNT,
    REGISTER_DOMAIN_3_COUNT,
    REGISTER_ERROR_COUNT,
    REGISTER_PLL_PARAM,
} register_type_t;

typedef struct {
    uint8_t job_id;
    uint32_t nonce;
    uint32_t rolled_version;
    register_type_t register_type;
    uint8_t asic_nr;
    uint32_t value;
    uint8_t core_id;
    uint8_t small_core_id;
    uint64_t timestamp_us;
} task_result;

unsigned char _reverse_bits(unsigned char num);
int _largest_power_of_two(int num);
int _next_power_of_two(int num);
int count_asic_chips(uint16_t asic_count, uint16_t chip_id, int chip_id_response_length);
esp_err_t receive_work(uint8_t *buffer, int buffer_size, uint64_t *out_timestamp_us);
void get_difficulty_mask(double difficulty, uint8_t *job_difficulty_mask);
double calculate_bm_timeout_ms(float frequency_mhz, size_t asic_count, size_t small_cores,
                               size_t cores, size_t version_size, float timeout_percent,
                               double default_time_ms);

#endif
