#pragma once

#include <stddef.h>
#include <stdint.h>

#define STRATUM_DEFAULT_VERSION_MASK 0x1fffe000

size_t bin2hex(const uint8_t *buf, size_t buflen, char *hex, size_t hexlen);
size_t hex2bin(const char *hex, uint8_t *bin, size_t bin_len);
void double_sha256_bin(const uint8_t *data, size_t data_len, uint8_t dest[32]);
void midstate_sha256_bin(const uint8_t *data, size_t data_len, uint8_t dest[32]);
void reverse_32bit_words(const uint8_t src[32], uint8_t dest[32]);
void reverse_endianness_per_word(uint8_t data[32]);
double le256todouble(const void *target);
void prettyHex(unsigned char *buf, int len);
