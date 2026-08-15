#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    STRATUM_PAYOUT_STATUS_UNCHECKED = 0,
    STRATUM_PAYOUT_STATUS_OK,
    STRATUM_PAYOUT_STATUS_LOW,
    STRATUM_PAYOUT_STATUS_MISSING,
    STRATUM_PAYOUT_STATUS_UNSUPPORTED_WALLET,
    STRATUM_PAYOUT_STATUS_PARSE_ERROR,
};

typedef struct {
    uint8_t status;
    uint16_t percent_x100;
} stratum_payout_result_t;

stratum_payout_result_t stratum_payout_evaluate(const char *wallet,
                                                 const uint8_t *coinbase,
                                                 size_t coinbase_len);
