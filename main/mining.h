#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HASH_SIZE 32
#define MAX_MERKLE_BRANCHES 32
#define BM_JOB_ID_INLINE_LEN 128
#define BM_JOB_EXTRANONCE2_INLINE_LEN 65

typedef struct {
    char *job_id;
    char *prev_block_hash;
    uint8_t prev_block_hash_bin[HASH_SIZE];
    uint8_t *coinbase_1_bin;
    size_t coinbase_1_len;
    uint8_t *coinbase_2_bin;
    size_t coinbase_2_len;
    uint8_t *merkle_branches;
    size_t n_merkle_branches;
    uint32_t version;
    uint32_t target;
    uint32_t ntime;
    bool clean_jobs;
} mining_notify;

typedef struct {
    uint32_t version;
    uint32_t version_mask;
    uint8_t prev_block_hash[32];
    uint8_t merkle_root[32];
    uint32_t ntime;
    uint32_t target;
    uint32_t starting_nonce;
    uint8_t header_prefix[76];
    char ntime_hex[9];
    double block_diff;

    uint8_t num_midstates;
    uint8_t midstate[32];
    uint8_t midstate1[32];
    uint8_t midstate2[32];
    uint8_t midstate3[32];
    double pool_diff;
    char *jobid;
    char *extranonce2;
    bool owns_jobid;
    bool owns_extranonce2;
    char jobid_inline[BM_JOB_ID_INLINE_LEN];
    char extranonce2_inline[BM_JOB_EXTRANONCE2_INLINE_LEN];
} bm_job;

void free_mining_notify(mining_notify *params);
bm_job *alloc_bm_job(void);
void free_bm_job(bm_job *job);
bool bm_job_set_ids(bm_job *job, const char *jobid, const char *extranonce2);

void calculate_coinbase_tx_hash(const char *coinbase_1, const char *coinbase_2,
                                const char *extranonce, const char *extranonce_2,
                                uint8_t dest[32]);
void calculate_coinbase_tx_hash_bin(const uint8_t *coinbase_1, size_t coinbase_1_len,
                                    const uint8_t *coinbase_2, size_t coinbase_2_len,
                                    const char *extranonce, const char *extranonce_2,
                                    uint8_t dest[32]);
void calculate_coinbase_tx_hash_parts(const uint8_t *coinbase_1, size_t coinbase_1_len,
                                      const uint8_t *extranonce, size_t extranonce_len,
                                      const uint8_t *extranonce_2, size_t extranonce_2_len,
                                      const uint8_t *coinbase_2, size_t coinbase_2_len,
                                      uint8_t dest[32]);
void calculate_merkle_root_hash(const uint8_t coinbase_tx_hash[32],
                                const uint8_t merkle_branches[][32],
                                int num_merkle_branches, uint8_t dest[32]);
void construct_bm_job(const mining_notify *params, const uint8_t merkle_root[32],
                      uint32_t version_mask, double difficulty, bm_job *new_job);
double test_nonce_value(const bm_job *job, uint32_t nonce, uint32_t rolled_version);
double block_target_difficulty(uint32_t compact_target);
void extranonce_2_generate(uint64_t extranonce_2, uint32_t length,
                           char dest[static length * 2 + 1]);
void extranonce_2_generate_bin(uint64_t extranonce_2, uint32_t length,
                               uint8_t dest[static length]);
uint32_t increment_bitmask(uint32_t value, uint32_t mask);
