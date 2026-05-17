#include "mining.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "utils.h"

#define BM_JOB_POOL_SIZE 32

static bm_job g_bm_job_pool[BM_JOB_POOL_SIZE];
static bool g_bm_job_pool_used[BM_JOB_POOL_SIZE];
static portMUX_TYPE g_bm_job_pool_mux = portMUX_INITIALIZER_UNLOCKED;

static bool bm_job_from_pool(const bm_job *job, size_t *index)
{
    for (size_t i = 0; i < BM_JOB_POOL_SIZE; ++i) {
        if (job == &g_bm_job_pool[i]) {
            if (index != NULL) {
                *index = i;
            }
            return true;
        }
    }
    return false;
}

void free_mining_notify(mining_notify *params)
{
    if (params == NULL) {
        return;
    }
    free(params->job_id);
    free(params->prev_block_hash);
    free(params->coinbase_1);
    free(params->coinbase_2);
    free(params->coinbase_1_bin);
    free(params->coinbase_2_bin);
    free(params->merkle_branches);
    free(params);
}

bm_job *alloc_bm_job(void)
{
    bm_job *job = NULL;
    taskENTER_CRITICAL(&g_bm_job_pool_mux);
    for (size_t i = 0; i < BM_JOB_POOL_SIZE; ++i) {
        if (!g_bm_job_pool_used[i]) {
            g_bm_job_pool_used[i] = true;
            job = &g_bm_job_pool[i];
            break;
        }
    }
    taskEXIT_CRITICAL(&g_bm_job_pool_mux);

    if (job != NULL) {
        memset(job, 0, sizeof(*job));
        return job;
    }

    return calloc(1, sizeof(bm_job));
}

void free_bm_job(bm_job *job)
{
    if (job == NULL) {
        return;
    }
    if (job->owns_jobid) {
        free(job->jobid);
    }
    if (job->owns_extranonce2) {
        free(job->extranonce2);
    }

    size_t pool_index = 0;
    if (bm_job_from_pool(job, &pool_index)) {
        memset(job, 0, sizeof(*job));
        taskENTER_CRITICAL(&g_bm_job_pool_mux);
        g_bm_job_pool_used[pool_index] = false;
        taskEXIT_CRITICAL(&g_bm_job_pool_mux);
        return;
    }

    free(job);
}

static bool bm_job_set_string(char **dest, bool *owns, char *inline_buf, size_t inline_len,
                              const char *value)
{
    if (dest == NULL || owns == NULL || inline_buf == NULL || inline_len == 0 ||
        value == NULL) {
        return false;
    }

    const size_t len = strlen(value);
    if (len < inline_len) {
        memcpy(inline_buf, value, len + 1);
        *dest = inline_buf;
        *owns = false;
        return true;
    }

    char *copy = strdup(value);
    if (copy == NULL) {
        return false;
    }
    *dest = copy;
    *owns = true;
    return true;
}

bool bm_job_set_ids(bm_job *job, const char *jobid, const char *extranonce2)
{
    if (job == NULL) {
        return false;
    }

    return bm_job_set_string(&job->jobid, &job->owns_jobid, job->jobid_inline,
                             sizeof(job->jobid_inline), jobid) &&
           bm_job_set_string(&job->extranonce2, &job->owns_extranonce2,
                             job->extranonce2_inline, sizeof(job->extranonce2_inline),
                             extranonce2);
}

void calculate_coinbase_tx_hash(const char *coinbase_1, const char *coinbase_2,
                                const char *extranonce, const char *extranonce_2,
                                uint8_t dest[32])
{
    const size_t len1 = strlen(coinbase_1);
    const size_t len2 = strlen(extranonce);
    const size_t len3 = strlen(extranonce_2);
    const size_t len4 = strlen(coinbase_2);
    const size_t tx_len = (len1 + len2 + len3 + len4) / 2;
    uint8_t tx[tx_len];

    size_t offset = 0;
    offset += hex2bin(coinbase_1, tx + offset, tx_len - offset);
    offset += hex2bin(extranonce, tx + offset, tx_len - offset);
    offset += hex2bin(extranonce_2, tx + offset, tx_len - offset);
    hex2bin(coinbase_2, tx + offset, tx_len - offset);

    double_sha256_bin(tx, tx_len, dest);
}

void calculate_coinbase_tx_hash_bin(const uint8_t *coinbase_1, size_t coinbase_1_len,
                                    const uint8_t *coinbase_2, size_t coinbase_2_len,
                                    const char *extranonce, const char *extranonce_2,
                                    uint8_t dest[32])
{
    const size_t extranonce_len = strlen(extranonce) / 2;
    const size_t extranonce_2_len = strlen(extranonce_2) / 2;
    uint8_t extranonce_bin[extranonce_len > 0 ? extranonce_len : 1];
    uint8_t extranonce_2_bin[extranonce_2_len > 0 ? extranonce_2_len : 1];

    if (extranonce_len > 0) {
        hex2bin(extranonce, extranonce_bin, extranonce_len);
    }
    if (extranonce_2_len > 0) {
        hex2bin(extranonce_2, extranonce_2_bin, extranonce_2_len);
    }

    calculate_coinbase_tx_hash_parts(coinbase_1, coinbase_1_len, extranonce_bin,
                                     extranonce_len, extranonce_2_bin, extranonce_2_len,
                                     coinbase_2, coinbase_2_len, dest);
}

void calculate_coinbase_tx_hash_parts(const uint8_t *coinbase_1, size_t coinbase_1_len,
                                      const uint8_t *extranonce, size_t extranonce_len,
                                      const uint8_t *extranonce_2, size_t extranonce_2_len,
                                      const uint8_t *coinbase_2, size_t coinbase_2_len,
                                      uint8_t dest[32])
{
    const size_t tx_len = coinbase_1_len + extranonce_len + extranonce_2_len + coinbase_2_len;
    uint8_t tx[tx_len > 0 ? tx_len : 1];

    size_t offset = 0;
    if (coinbase_1_len > 0) {
        memcpy(tx + offset, coinbase_1, coinbase_1_len);
        offset += coinbase_1_len;
    }
    if (extranonce_len > 0) {
        memcpy(tx + offset, extranonce, extranonce_len);
        offset += extranonce_len;
    }
    if (extranonce_2_len > 0) {
        memcpy(tx + offset, extranonce_2, extranonce_2_len);
        offset += extranonce_2_len;
    }
    if (coinbase_2_len > 0) {
        memcpy(tx + offset, coinbase_2, coinbase_2_len);
    }

    double_sha256_bin(tx, tx_len, dest);
}

void calculate_merkle_root_hash(const uint8_t coinbase_tx_hash[32],
                                const uint8_t merkle_branches[][32],
                                int num_merkle_branches, uint8_t dest[32])
{
    uint8_t pair[64];
    memcpy(pair, coinbase_tx_hash, 32);

    for (int i = 0; i < num_merkle_branches; ++i) {
        memcpy(pair + 32, merkle_branches[i], 32);
        double_sha256_bin(pair, sizeof(pair), pair);
    }

    memcpy(dest, pair, 32);
}

void construct_bm_job(const mining_notify *params, const uint8_t merkle_root[32],
                      uint32_t version_mask, double difficulty, bm_job *new_job)
{
    new_job->version = params->version;
    new_job->target = params->target;
    new_job->ntime = params->ntime;
    new_job->starting_nonce = 0;
    new_job->pool_diff = difficulty;
    reverse_32bit_words(merkle_root, new_job->merkle_root);

    uint8_t prev_block_hash[32];
    memcpy(prev_block_hash, params->prev_block_hash_bin, sizeof(prev_block_hash));
    reverse_endianness_per_word(prev_block_hash);
    reverse_32bit_words(prev_block_hash, new_job->prev_block_hash);

    uint8_t midstate_data[64];
    memcpy(midstate_data, &new_job->version, 4);
    memcpy(midstate_data + 4, prev_block_hash, 32);
    memcpy(midstate_data + 36, merkle_root, 28);

    uint8_t midstate[32];
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, new_job->midstate);

    if (version_mask == 0) {
        new_job->num_midstates = 1;
        return;
    }

    uint32_t rolled_version = increment_bitmask(new_job->version, version_mask);
    memcpy(midstate_data, &rolled_version, 4);
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, new_job->midstate1);

    rolled_version = increment_bitmask(rolled_version, version_mask);
    memcpy(midstate_data, &rolled_version, 4);
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, new_job->midstate2);

    rolled_version = increment_bitmask(rolled_version, version_mask);
    memcpy(midstate_data, &rolled_version, 4);
    midstate_sha256_bin(midstate_data, sizeof(midstate_data), midstate);
    reverse_32bit_words(midstate, new_job->midstate3);
    new_job->num_midstates = 4;
}

double test_nonce_value(const bm_job *job, uint32_t nonce, uint32_t rolled_version)
{
    static const double truediffone =
        26959535291011309493156476344723991336010898738574164086137773096960.0;

    uint8_t header[80];
    memcpy(header, &rolled_version, 4);
    reverse_32bit_words(job->prev_block_hash, header + 4);
    reverse_32bit_words(job->merkle_root, header + 36);
    memcpy(header + 68, &job->ntime, 4);
    memcpy(header + 72, &job->target, 4);
    memcpy(header + 76, &nonce, 4);

    uint8_t hash_result[32];
    double_sha256_bin(header, sizeof(header), hash_result);

    return truediffone / le256todouble(hash_result);
}

double block_target_difficulty(uint32_t compact_target)
{
    static const double truediffone =
        26959535291011309493156476344723991336010898738574164086137773096960.0;
    const uint32_t exponent = compact_target >> 24;
    const uint32_t mantissa = compact_target & 0x007fffff;
    if (exponent == 0 || mantissa == 0) {
        return 0.0;
    }

    const double target = ldexp((double)mantissa, 8 * ((int)exponent - 3));
    return target > 0.0 ? truediffone / target : 0.0;
}

void extranonce_2_generate(uint64_t extranonce_2, uint32_t length,
                           char dest[static length * 2 + 1])
{
    uint8_t bytes[length];
    extranonce_2_generate_bin(extranonce_2, length, bytes);
    bin2hex(bytes, length, dest, length * 2 + 1);
}

void extranonce_2_generate_bin(uint64_t extranonce_2, uint32_t length,
                               uint8_t dest[static length])
{
    memset(dest, 0, length);

    const size_t copy_len = length < sizeof(extranonce_2) ? length : sizeof(extranonce_2);
    memcpy(dest, &extranonce_2, copy_len);
}

uint32_t increment_bitmask(uint32_t value, uint32_t mask)
{
    if (mask == 0) {
        return value;
    }

    const uint32_t carry = (value & mask) + (mask & -mask);
    const uint32_t overflow = carry & ~mask;
    uint32_t new_value = (value & ~mask) | (carry & mask);

    if (overflow > 0) {
        new_value = increment_bitmask(new_value, overflow << 1);
    }

    return new_value;
}
