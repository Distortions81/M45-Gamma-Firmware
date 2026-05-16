#ifndef M45_ASIC_SERIAL_H_
#define M45_ASIC_SERIAL_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    JOB_PACKET = 0,
    CMD_PACKET = 1,
} packet_type_t;

#define UART_FREQ 115200

esp_err_t SERIAL_init(void);
bool SERIAL_is_initialized(void);
esp_err_t SERIAL_set_baud(int baud);
int SERIAL_send(uint8_t *data, int len, bool debug);
int16_t SERIAL_rx(uint8_t *buf, uint16_t size, uint16_t timeout_ms);
void SERIAL_debug_rx(void);
void SERIAL_clear_buffer(void);

#endif
