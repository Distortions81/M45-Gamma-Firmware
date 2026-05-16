#include "i2c_bitaxe.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define I2C_MASTER_PORT 0
#define I2C_MASTER_TIMEOUT_MS 500
#define I2C_TRANSFER_RETRIES 3
#define I2C_RETRY_DELAY_MS 10
#define I2C_DEVICE_MAP_CAPACITY 10

typedef struct {
    i2c_master_dev_handle_t handle;
    uint8_t address;
    char tag[32];
} i2c_bitaxe_device_t;

static const char *TAG = "i2c_bitaxe";
static i2c_master_bus_handle_t s_i2c_bus;
static i2c_bitaxe_device_t s_devices[I2C_DEVICE_MAP_CAPACITY];
static size_t s_device_count;

static const i2c_bitaxe_device_t *find_device(i2c_master_dev_handle_t dev_handle)
{
    for (size_t i = 0; i < s_device_count; ++i) {
        if (s_devices[i].handle == dev_handle) {
            return &s_devices[i];
        }
    }
    return NULL;
}

static void log_transfer_failure(i2c_master_dev_handle_t dev_handle, esp_err_t err)
{
    const i2c_bitaxe_device_t *device = find_device(dev_handle);
    if (device != NULL) {
        ESP_LOGE(TAG, "%s at 0x%02x failed after %d attempts: %s", device->tag, device->address,
                 I2C_TRANSFER_RETRIES, esp_err_to_name(err));
        return;
    }

    ESP_LOGE(TAG, "unmapped I2C device failed after %d attempts: %s", I2C_TRANSFER_RETRIES,
             esp_err_to_name(err));
}

static esp_err_t transfer_with_retries(i2c_master_dev_handle_t dev_handle,
                                       const uint8_t *write_buf, size_t write_len,
                                       uint8_t *read_buf, size_t read_len)
{
    if (dev_handle == NULL || write_buf == NULL || write_len == 0 ||
        (read_len > 0 && read_buf == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_FAIL;
    for (int attempt = 0; attempt < I2C_TRANSFER_RETRIES; ++attempt) {
        if (read_len > 0) {
            err = i2c_master_transmit_receive(dev_handle, write_buf, write_len, read_buf, read_len,
                                              I2C_MASTER_TIMEOUT_MS);
        } else {
            err = i2c_master_transmit(dev_handle, write_buf, write_len, I2C_MASTER_TIMEOUT_MS);
        }

        if (err == ESP_OK) {
            return ESP_OK;
        }

        vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
    }

    log_transfer_failure(dev_handle, err);
    return err;
}

esp_err_t i2c_bitaxe_init(void)
{
    if (s_i2c_bus != NULL) {
        return ESP_OK;
    }

    const i2c_master_bus_config_t bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_PORT,
        .scl_io_num = CONFIG_GPIO_I2C_SCL,
        .sda_io_num = CONFIG_GPIO_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&bus_config, &s_i2c_bus);
}

esp_err_t i2c_bitaxe_add_device(uint8_t device_address, i2c_master_dev_handle_t *dev_handle,
                                const char *device_tag)
{
    ESP_RETURN_ON_FALSE(dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "missing device handle");
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");
    ESP_RETURN_ON_FALSE(s_device_count < I2C_DEVICE_MAP_CAPACITY, ESP_ERR_NO_MEM, TAG,
                        "I2C device map full");

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = I2C_BUS_SPEED_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(s_i2c_bus, &device_config, dev_handle), TAG,
                        "add I2C device 0x%02x failed", device_address);

    i2c_bitaxe_device_t *device = &s_devices[s_device_count++];
    device->handle = *dev_handle;
    device->address = device_address;
    strlcpy(device->tag, device_tag != NULL ? device_tag : "i2c-device", sizeof(device->tag));
    return ESP_OK;
}

esp_err_t i2c_bitaxe_get_master_bus_handle(i2c_master_bus_handle_t *dev_handle)
{
    ESP_RETURN_ON_FALSE(dev_handle != NULL, ESP_ERR_INVALID_ARG, TAG, "missing bus handle");
    ESP_RETURN_ON_FALSE(s_i2c_bus != NULL, ESP_ERR_INVALID_STATE, TAG, "I2C bus not initialized");

    *dev_handle = s_i2c_bus;
    return ESP_OK;
}

esp_err_t i2c_bitaxe_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr,
                                   uint8_t *read_buf, size_t len)
{
    return transfer_with_retries(dev_handle, &reg_addr, 1, read_buf, len);
}

esp_err_t i2c_bitaxe_register_write_addr(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr)
{
    return transfer_with_retries(dev_handle, &reg_addr, 1, NULL, 0);
}

esp_err_t i2c_bitaxe_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr,
                                         uint8_t data)
{
    const uint8_t write_buf[] = {reg_addr, data};
    return transfer_with_retries(dev_handle, write_buf, sizeof(write_buf), NULL, 0);
}

esp_err_t i2c_bitaxe_register_write_bytes(i2c_master_dev_handle_t dev_handle, const uint8_t *data,
                                          size_t len)
{
    return transfer_with_retries(dev_handle, data, len, NULL, 0);
}

esp_err_t i2c_bitaxe_register_write_word(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr,
                                         uint16_t data)
{
    const uint8_t write_buf[] = {
        reg_addr,
        (uint8_t)(data & 0x00ff),
        (uint8_t)(data >> 8),
    };
    return transfer_with_retries(dev_handle, write_buf, sizeof(write_buf), NULL, 0);
}
