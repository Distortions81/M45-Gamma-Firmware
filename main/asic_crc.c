#include "asic_crc.h"

#define CRC16_CCITT_POLY 0x1021u
#define CRC5_BM_INIT 0x1fu

uint8_t crc5(const uint8_t *data, size_t len)
{
    if (data == NULL) {
        return 0;
    }

    uint8_t crc = CRC5_BM_INIT;
    for (size_t byte_index = 0; byte_index < len; ++byte_index) {
        uint8_t byte = data[byte_index];
        for (uint8_t bit_index = 0; bit_index < 8; ++bit_index) {
            const uint8_t input_bit = (uint8_t)((byte >> 7) & 1u);
            byte <<= 1;

            const uint8_t next_bit = (uint8_t)(((crc >> 4) ^ input_bit) & 1u);
            crc = (uint8_t)(((crc << 1) | next_bit) ^ (next_bit << 2));
            crc &= 0x1fu;
        }
    }

    return crc;
}

static uint16_t crc16_ccitt(const uint8_t *data, size_t len, uint16_t initial_crc)
{
    if (data == NULL) {
        return initial_crc;
    }

    uint16_t crc = initial_crc;
    for (size_t byte_index = 0; byte_index < len; ++byte_index) {
        crc ^= (uint16_t)data[byte_index] << 8;
        for (uint8_t bit_index = 0; bit_index < 8; ++bit_index) {
            if ((crc & 0x8000u) != 0) {
                crc = (uint16_t)((crc << 1) ^ CRC16_CCITT_POLY);
            } else {
                crc <<= 1;
            }
        }
    }

    return crc;
}

uint16_t crc16(const uint8_t *data, size_t len)
{
    return crc16_ccitt(data, len, 0x0000u);
}

uint16_t crc16_false(const uint8_t *data, size_t len)
{
    return crc16_ccitt(data, len, 0xffffu);
}
