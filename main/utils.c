#include "utils.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

static const char HEX_TABLE[] = "0123456789abcdef";

static const uint8_t HEX_VALUES[256] = {
    ['0'] = 0,  ['1'] = 1,  ['2'] = 2,  ['3'] = 3,  ['4'] = 4,
    ['5'] = 5,  ['6'] = 6,  ['7'] = 7,  ['8'] = 8,  ['9'] = 9,
    ['a'] = 10, ['b'] = 11, ['c'] = 12, ['d'] = 13, ['e'] = 14, ['f'] = 15,
    ['A'] = 10, ['B'] = 11, ['C'] = 12, ['D'] = 13, ['E'] = 14, ['F'] = 15,
};

size_t bin2hex(const uint8_t *buf, size_t buflen, char *hex, size_t hexlen)
{
    if (hexlen <= buflen * 2) {
        return 0;
    }

    for (size_t i = 0; i < buflen; ++i) {
        hex[2 * i] = HEX_TABLE[buf[i] >> 4];
        hex[2 * i + 1] = HEX_TABLE[buf[i] & 0x0f];
    }
    hex[2 * buflen] = '\0';
    return 2 * buflen;
}

size_t hex2bin(const char *hex, uint8_t *bin, size_t bin_len)
{
    size_t len = 0;
    while (len < bin_len && hex[0] != '\0') {
        if (hex[1] == '\0') {
            bin[len++] = HEX_VALUES[(unsigned char)hex[0]] << 4;
            break;
        }
        bin[len++] = (HEX_VALUES[(unsigned char)hex[0]] << 4) |
                     HEX_VALUES[(unsigned char)hex[1]];
        hex += 2;
    }
    return len;
}

void double_sha256_bin(const uint8_t *data, size_t data_len, uint8_t dest[32])
{
    uint8_t first_hash[32];
    mbedtls_sha256(data, data_len, first_hash, 0);
    mbedtls_sha256(first_hash, sizeof(first_hash), dest, 0);
}

void midstate_sha256_bin(const uint8_t *data, size_t data_len, uint8_t dest[32])
{
    (void)data_len;

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, 64);
    memcpy(dest, ctx.state, 32);
    mbedtls_sha256_free(&ctx);
}

void reverse_32bit_words(const uint8_t src[32], uint8_t dest[32])
{
    const uint32_t *s = (const uint32_t *)src;
    uint32_t *d = (uint32_t *)dest;

    d[0] = s[7];
    d[1] = s[6];
    d[2] = s[5];
    d[3] = s[4];
    d[4] = s[3];
    d[5] = s[2];
    d[6] = s[1];
    d[7] = s[0];
}

void reverse_endianness_per_word(uint8_t data[32])
{
    uint32_t *d = (uint32_t *)data;

    for (size_t i = 0; i < 8; ++i) {
        d[i] = __builtin_bswap32(d[i]);
    }
}

double le256todouble(const void *target)
{
    static const double bits192 = 6277101735386680763835789423207666416102355444464034512896.0;
    static const double bits128 = 340282366920938463463374607431768211456.0;
    static const double bits64 = 18446744073709551616.0;

    const uint8_t *bytes = (const uint8_t *)target;
    double result = 0.0;

    result += *(const uint64_t *)(bytes + 24) * bits192;
    result += *(const uint64_t *)(bytes + 16) * bits128;
    result += *(const uint64_t *)(bytes + 8) * bits64;
    result += *(const uint64_t *)bytes;
    return result;
}

void prettyHex(unsigned char *buf, int len)
{
    if (buf == NULL || len <= 0) {
        printf("[]");
        return;
    }

    printf("[");
    for (int i = 0; i < len - 1; ++i) {
        printf("%02X ", buf[i]);
    }
    printf("%02X]", buf[len - 1]);
}
