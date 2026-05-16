#ifndef M45_ASIC_CRC_H_
#define M45_ASIC_CRC_H_

#include <stddef.h>
#include <stdint.h>

uint8_t crc5(const uint8_t *data, size_t len);
uint16_t crc16(const uint8_t *data, size_t len);
uint16_t crc16_false(const uint8_t *data, size_t len);

#endif
