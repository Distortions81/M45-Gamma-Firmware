#include "asic_serial.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "utils.h"

#define ASIC_UART_PORT UART_NUM_1
#define ASIC_UART_TX_GPIO 17
#define ASIC_UART_RX_GPIO 18
#define ASIC_UART_BUFFER_SIZE 1024
#define ASIC_UART_BAUD_CHANGE_TIMEOUT_MS 1000

static const char *TAG = "asic_serial";

esp_err_t SERIAL_init(void)
{
    ESP_LOGI(TAG, "initializing ASIC UART");
    const uart_config_t uart_config = {
        .baud_rate = UART_FREQ,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .rx_flow_ctrl_thresh = 122,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_RETURN_ON_ERROR(uart_param_config(ASIC_UART_PORT, &uart_config), TAG,
                        "ASIC UART config failed");
    ESP_RETURN_ON_ERROR(uart_set_pin(ASIC_UART_PORT, ASIC_UART_TX_GPIO, ASIC_UART_RX_GPIO,
                                     UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE),
                        TAG, "ASIC UART pin config failed");
    if (uart_is_driver_installed(ASIC_UART_PORT)) {
        return ESP_OK;
    }

    return uart_driver_install(ASIC_UART_PORT, ASIC_UART_BUFFER_SIZE * 2,
                               ASIC_UART_BUFFER_SIZE * 2, 0, NULL, 0);
}

bool SERIAL_is_initialized(void)
{
    return uart_is_driver_installed(ASIC_UART_PORT);
}

esp_err_t SERIAL_set_baud(int baud)
{
    ESP_LOGI(TAG, "changing ASIC UART baud to %d", baud);
    esp_err_t err = uart_wait_tx_done(ASIC_UART_PORT,
                                      pdMS_TO_TICKS(ASIC_UART_BAUD_CHANGE_TIMEOUT_MS));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ASIC UART TX drain before baud change failed: %s",
                 esp_err_to_name(err));
    }
    return uart_set_baudrate(ASIC_UART_PORT, baud);
}

int SERIAL_send(uint8_t *data, int len, bool debug)
{
    if (data == NULL || len <= 0) {
        return 0;
    }

    if (debug) {
        printf("tx: ");
        prettyHex((unsigned char *)data, len);
        printf("\n");
    }

    return uart_write_bytes(ASIC_UART_PORT, (const char *)data, len);
}

int16_t SERIAL_rx(uint8_t *buf, uint16_t size, uint16_t timeout_ms)
{
    if (buf == NULL || size == 0) {
        return 0;
    }

    const int bytes_read =
        uart_read_bytes(ASIC_UART_PORT, buf, size, pdMS_TO_TICKS(timeout_ms));
    if (bytes_read > INT16_MAX) {
        return INT16_MAX;
    }
    return (int16_t)bytes_read;
}

void SERIAL_debug_rx(void)
{
    uint8_t buf[100] = {0};
    const int16_t ret = SERIAL_rx(buf, sizeof(buf), 20);
    if (ret < 0) {
        fprintf(stderr, "unable to read ASIC UART data\n");
        return;
    }
    memset(buf, 0, sizeof(buf));
}

void SERIAL_clear_buffer(void)
{
    uart_flush(ASIC_UART_PORT);
}
