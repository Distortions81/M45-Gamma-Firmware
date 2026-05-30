#include "tps546.h"

#include <math.h>
#include <stdbool.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bitaxe.h"

#define PMBUS_OPERATION              0x01
#define PMBUS_ON_OFF_CONFIG          0x02
#define PMBUS_CLEAR_FAULTS           0x03
#define PMBUS_PHASE                  0x04
#define PMBUS_VOUT_MODE              0x20
#define PMBUS_VOUT_COMMAND           0x21
#define PMBUS_VOUT_MAX               0x24
#define PMBUS_VOUT_SCALE_LOOP        0x29
#define PMBUS_VOUT_MIN               0x2B
#define PMBUS_FREQUENCY_SWITCH       0x33
#define PMBUS_VIN_ON                 0x35
#define PMBUS_VIN_OFF                0x36
#define PMBUS_VOUT_OV_FAULT_LIMIT    0x40
#define PMBUS_VOUT_OV_WARN_LIMIT     0x42
#define PMBUS_VOUT_UV_WARN_LIMIT     0x43
#define PMBUS_VOUT_UV_FAULT_LIMIT    0x44
#define PMBUS_IOUT_OC_FAULT_LIMIT    0x46
#define PMBUS_IOUT_OC_FAULT_RESPONSE 0x47
#define PMBUS_IOUT_OC_WARN_LIMIT     0x4A
#define PMBUS_OT_FAULT_LIMIT         0x4F
#define PMBUS_OT_FAULT_RESPONSE      0x50
#define PMBUS_OT_WARN_LIMIT          0x51
#define PMBUS_VIN_OV_FAULT_LIMIT     0x55
#define PMBUS_VIN_OV_FAULT_RESPONSE  0x56
#define PMBUS_VIN_UV_WARN_LIMIT      0x58
#define PMBUS_TON_DELAY              0x60
#define PMBUS_TON_RISE               0x61
#define PMBUS_TON_MAX_FAULT_LIMIT    0x62
#define PMBUS_TON_MAX_FAULT_RESPONSE 0x63
#define PMBUS_TOFF_DELAY             0x64
#define PMBUS_TOFF_FALL              0x65
#define PMBUS_STATUS_WORD            0x79
#define PMBUS_STATUS_VOUT            0x7A
#define PMBUS_STATUS_IOUT            0x7B
#define PMBUS_STATUS_INPUT           0x7C
#define PMBUS_STATUS_TEMPERATURE     0x7D
#define PMBUS_STATUS_CML             0x7E
#define PMBUS_STATUS_OTHER           0x7F
#define PMBUS_STATUS_MFR_SPECIFIC    0x80
#define PMBUS_READ_VIN               0x88
#define PMBUS_READ_VOUT              0x8B
#define PMBUS_READ_IOUT              0x8C
#define PMBUS_READ_TEMPERATURE_1     0x8D
#define PMBUS_IC_DEVICE_ID           0xAD
#define PMBUS_SYNC_CONFIG            0xE4
#define PMBUS_STACK_CONFIG           0xEC
#define PMBUS_PIN_DETECT_OVERRIDE    0xEE

#define ON_OFF_CONFIG_PU       0x10
#define ON_OFF_CONFIG_CMD      0x08
#define ON_OFF_CONFIG_POLARITY 0x02
#define ON_OFF_CONFIG_DELAY    0x01

#define TPS546_ON_OFF_CONFIG_VALUE \
    (ON_OFF_CONFIG_DELAY | ON_OFF_CONFIG_POLARITY | ON_OFF_CONFIG_CMD | ON_OFF_CONFIG_PU)

#define TPS546_FREQUENCY_KHZ              650
#define TPS546_VIN_OV_FAULT_RESPONSE      0xB7
#define TPS546_IOUT_OC_FAULT_RESPONSE     0xC0
#define TPS546_OT_FAULT_LIMIT_C           145
#define TPS546_OT_FAULT_RESPONSE          0xFF
#define TPS546_TON_DELAY_MS               0
#define TPS546_TON_RISE_MS                2
#define TPS546_TON_MAX_FAULT_LIMIT_MS     0
#define TPS546_TON_MAX_FAULT_RESPONSE     0x3B
#define TPS546_TOFF_DELAY_MS              0
#define TPS546_TOFF_FALL_MS               0
#define TPS546_PIN_DETECT_OVERRIDE_VALUE  0xFFFF
#define TPS546_VOUT_OV_FAULT_LIMIT_RATIO 1.25f
#define TPS546_VOUT_OV_WARN_LIMIT_RATIO  1.16f
#define TPS546_VOUT_UV_WARN_LIMIT_RATIO  0.90f
#define TPS546_VOUT_UV_FAULT_LIMIT_RATIO 0.75f
#define TPS546_READY_DELAY_MS             15
#define TPS546_ID_RETRIES                 6
#define TPS546_ID_RETRY_DELAY_MS          3

static const char *TAG = "tps546";
static const uint8_t DEVICE_ID_TPS546D24A[] = {0x54, 0x49, 0x54, 0x6D, 0x24, 0x41};
static const uint8_t DEVICE_ID_TPS546D24S[] = {0x54, 0x49, 0x54, 0x6D, 0x24, 0x62};

static i2c_master_dev_handle_t s_tps546_i2c_handle;
static TPS546_CONFIG s_config;
static int s_vout_exponent;
static bool s_vout_mode_valid;
static const char *s_tps546_model = "";

static int32_t sign_extend(uint32_t value, unsigned bits)
{
    const uint32_t sign_bit = 1u << (bits - 1u);
    return (int32_t)((value ^ sign_bit) - sign_bit);
}

static float linear11_to_float(uint16_t value)
{
    const int32_t exponent = sign_extend(value >> 11, 5);
    const int32_t mantissa = sign_extend(value & 0x07FF, 11);
    return ldexpf((float)mantissa, exponent);
}

static int linear11_to_int(uint16_t value)
{
    return (int)lroundf(linear11_to_float(value));
}

static uint16_t linear11_from_float(float value)
{
    if (!isfinite(value) || value == 0.0f) {
        return 0;
    }

    for (int exponent = -16; exponent <= 15; ++exponent) {
        const float scaled = ldexpf(value, -exponent);
        const int32_t mantissa = (int32_t)lroundf(scaled);
        if (mantissa >= -1024 && mantissa <= 1023) {
            return (uint16_t)(((uint16_t)exponent & 0x1F) << 11) |
                   ((uint16_t)mantissa & 0x07FF);
        }
    }

    return 0;
}

static float ulinear16_to_float(uint16_t value)
{
    return ldexpf((float)value, s_vout_exponent);
}

static uint16_t ulinear16_from_float(float value)
{
    if (!isfinite(value) || value <= 0.0f || !s_vout_mode_valid) {
        return 0;
    }

    const float scaled = ldexpf(value, -s_vout_exponent);
    if (scaled >= 65535.0f) {
        return UINT16_MAX;
    }
    return (uint16_t)lroundf(scaled);
}

static esp_err_t tps_read_byte(uint8_t command, uint8_t *data)
{
    return i2c_bitaxe_register_read(s_tps546_i2c_handle, command, data, 1);
}

static esp_err_t tps_write_byte(uint8_t command, uint8_t data)
{
    return i2c_bitaxe_register_write_byte(s_tps546_i2c_handle, command, data);
}

static esp_err_t tps_read_word(uint8_t command, uint16_t *result)
{
    uint8_t data[2] = {0};
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_read(s_tps546_i2c_handle, command, data, 2), TAG,
                        "PMBus read 0x%02x failed", command);
    *result = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return ESP_OK;
}

static esp_err_t tps_write_word(uint8_t command, uint16_t data)
{
    return i2c_bitaxe_register_write_word(s_tps546_i2c_handle, command, data);
}

static esp_err_t tps_write_addr(uint8_t command)
{
    return i2c_bitaxe_register_write_addr(s_tps546_i2c_handle, command);
}

static esp_err_t tps_read_block(uint8_t command, uint8_t *data, uint8_t len)
{
    if (len > 32) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t buffer[33] = {0};
    ESP_RETURN_ON_ERROR(i2c_bitaxe_register_read(s_tps546_i2c_handle, command, buffer, len + 1),
                        TAG, "PMBus block read 0x%02x failed", command);
    ESP_RETURN_ON_FALSE(buffer[0] >= len, ESP_ERR_INVALID_RESPONSE, TAG,
                        "PMBus block 0x%02x returned %u bytes, expected %u", command, buffer[0],
                        len);
    memcpy(data, &buffer[1], len);
    return ESP_OK;
}

static esp_err_t tps_write_linear11(uint8_t command, float value, const char *name)
{
    ESP_RETURN_ON_ERROR(tps_write_word(command, linear11_from_float(value)), TAG,
                        "write %s %.3f failed", name, value);
    return ESP_OK;
}

static esp_err_t tps_write_ulinear16(uint8_t command, float value, const char *name)
{
    ESP_RETURN_ON_ERROR(tps_write_word(command, ulinear16_from_float(value)), TAG,
                        "write %s %.3f failed", name, value);
    return ESP_OK;
}

static esp_err_t read_vout_mode(void)
{
    uint8_t mode = 0;
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_VOUT_MODE, &mode), TAG, "read VOUT_MODE failed");

    s_vout_exponent = (int)sign_extend(mode & 0x1F, 5);
    s_vout_mode_valid = true;
    ESP_LOGI(TAG, "VOUT_MODE 0x%02x, exponent %d", mode, s_vout_exponent);
    return ESP_OK;
}

static const char *model_from_device_id(const uint8_t id[6])
{
    if (memcmp(id, DEVICE_ID_TPS546D24A, sizeof(DEVICE_ID_TPS546D24A)) == 0) {
        return "TPS546D24A";
    }
    if (memcmp(id, DEVICE_ID_TPS546D24S, sizeof(DEVICE_ID_TPS546D24S)) == 0) {
        return "TPS546D24S";
    }
    return NULL;
}

static esp_err_t read_device_id(uint8_t id[6])
{
    esp_err_t last_err = ESP_FAIL;

    for (int attempt = 0; attempt < TPS546_ID_RETRIES; ++attempt) {
        last_err = tps_read_block(PMBUS_IC_DEVICE_ID, id, 6);
        const char *model = last_err == ESP_OK ? model_from_device_id(id) : NULL;
        if (model != NULL) {
            s_tps546_model = model;
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(TPS546_ID_RETRY_DELAY_MS));
    }

    ESP_LOGE(TAG, "unexpected TPS546 device ID %02x %02x %02x %02x %02x %02x", id[0], id[1],
             id[2], id[3], id[4], id[5]);
    return last_err == ESP_OK ? ESP_ERR_NOT_FOUND : last_err;
}

static esp_err_t write_boot_config(void)
{
    ESP_LOGI(TAG,
             "limits: VIN %.2f-%.2f V, VOUT %.3f-%.3f V, IOUT %.1f/%.1f A, TON_RISE %d ms",
             s_config.TPS546_INIT_VIN_OFF, s_config.TPS546_INIT_VIN_OV_FAULT_LIMIT,
             s_config.TPS546_INIT_VOUT_MIN, s_config.TPS546_INIT_VOUT_MAX,
             s_config.TPS546_INIT_IOUT_OC_WARN_LIMIT, s_config.TPS546_INIT_IOUT_OC_FAULT_LIMIT,
             TPS546_TON_RISE_MS);

    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_ON_OFF_CONFIG, TPS546_ON_OFF_CONFIG_VALUE), TAG,
                        "write ON_OFF_CONFIG failed");
    ESP_RETURN_ON_ERROR(tps_write_word(PMBUS_STACK_CONFIG, s_config.TPS546_INIT_STACK_CONFIG),
                        TAG, "write STACK_CONFIG failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_SYNC_CONFIG, s_config.TPS546_INIT_SYNC_CONFIG),
                        TAG, "write SYNC_CONFIG failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_PHASE, s_config.TPS546_INIT_PHASE), TAG,
                        "write PHASE failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_FREQUENCY_SWITCH, TPS546_FREQUENCY_KHZ,
                                           "FREQUENCY_SWITCH"),
                        TAG, "write FREQUENCY_SWITCH failed");

    if (s_config.TPS546_INIT_VIN_UV_WARN_LIMIT > 0.0f) {
        ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_UV_WARN_LIMIT,
                                               s_config.TPS546_INIT_VIN_UV_WARN_LIMIT,
                                               "VIN_UV_WARN_LIMIT"),
                            TAG, "write VIN_UV_WARN_LIMIT failed");
    }
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_ON, s_config.TPS546_INIT_VIN_ON,
                                           "VIN_ON"),
                        TAG, "write VIN_ON failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_OFF, s_config.TPS546_INIT_VIN_OFF,
                                           "VIN_OFF"),
                        TAG, "write VIN_OFF failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_OV_FAULT_LIMIT,
                                           s_config.TPS546_INIT_VIN_OV_FAULT_LIMIT,
                                           "VIN_OV_FAULT_LIMIT"),
                        TAG, "write VIN_OV_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_VIN_OV_FAULT_RESPONSE,
                                       TPS546_VIN_OV_FAULT_RESPONSE),
                        TAG, "write VIN_OV_FAULT_RESPONSE failed");

    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VOUT_SCALE_LOOP,
                                           s_config.TPS546_INIT_SCALE_LOOP,
                                           "VOUT_SCALE_LOOP"),
                        TAG, "write VOUT_SCALE_LOOP failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_MIN, s_config.TPS546_INIT_VOUT_MIN,
                                            "VOUT_MIN"),
                        TAG, "write VOUT_MIN failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_MAX, s_config.TPS546_INIT_VOUT_MAX,
                                            "VOUT_MAX"),
                        TAG, "write VOUT_MAX failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_COMMAND,
                                            s_config.TPS546_INIT_VOUT_COMMAND,
                                            "VOUT_COMMAND"),
                        TAG, "write VOUT_COMMAND failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_OV_FAULT_LIMIT,
                                            TPS546_VOUT_OV_FAULT_LIMIT_RATIO,
                                            "VOUT_OV_FAULT_LIMIT"),
                        TAG, "write VOUT_OV_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_OV_WARN_LIMIT,
                                            TPS546_VOUT_OV_WARN_LIMIT_RATIO,
                                            "VOUT_OV_WARN_LIMIT"),
                        TAG, "write VOUT_OV_WARN_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_UV_WARN_LIMIT,
                                            TPS546_VOUT_UV_WARN_LIMIT_RATIO,
                                            "VOUT_UV_WARN_LIMIT"),
                        TAG, "write VOUT_UV_WARN_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_UV_FAULT_LIMIT,
                                            TPS546_VOUT_UV_FAULT_LIMIT_RATIO,
                                            "VOUT_UV_FAULT_LIMIT"),
                        TAG, "write VOUT_UV_FAULT_LIMIT failed");

    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_IOUT_OC_WARN_LIMIT,
                                           s_config.TPS546_INIT_IOUT_OC_WARN_LIMIT,
                                           "IOUT_OC_WARN_LIMIT"),
                        TAG, "write IOUT_OC_WARN_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_IOUT_OC_FAULT_LIMIT,
                                           s_config.TPS546_INIT_IOUT_OC_FAULT_LIMIT,
                                           "IOUT_OC_FAULT_LIMIT"),
                        TAG, "write IOUT_OC_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_IOUT_OC_FAULT_RESPONSE,
                                       TPS546_IOUT_OC_FAULT_RESPONSE),
                        TAG, "write IOUT_OC_FAULT_RESPONSE failed");

    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_OT_WARN_LIMIT, TPS546_INIT_OT_WARN_LIMIT,
                                           "OT_WARN_LIMIT"),
                        TAG, "write OT_WARN_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_OT_FAULT_LIMIT, TPS546_OT_FAULT_LIMIT_C,
                                           "OT_FAULT_LIMIT"),
                        TAG, "write OT_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_OT_FAULT_RESPONSE, TPS546_OT_FAULT_RESPONSE), TAG,
                        "write OT_FAULT_RESPONSE failed");

    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_TON_DELAY, TPS546_TON_DELAY_MS, "TON_DELAY"),
                        TAG, "write TON_DELAY failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_TON_RISE, TPS546_TON_RISE_MS, "TON_RISE"),
                        TAG, "write TON_RISE failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_TON_MAX_FAULT_LIMIT,
                                           TPS546_TON_MAX_FAULT_LIMIT_MS,
                                           "TON_MAX_FAULT_LIMIT"),
                        TAG, "write TON_MAX_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_TON_MAX_FAULT_RESPONSE,
                                       TPS546_TON_MAX_FAULT_RESPONSE),
                        TAG, "write TON_MAX_FAULT_RESPONSE failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_TOFF_DELAY, TPS546_TOFF_DELAY_MS,
                                           "TOFF_DELAY"),
                        TAG, "write TOFF_DELAY failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_TOFF_FALL, TPS546_TOFF_FALL_MS, "TOFF_FALL"),
                        TAG, "write TOFF_FALL failed");
    ESP_RETURN_ON_ERROR(tps_write_word(PMBUS_PIN_DETECT_OVERRIDE,
                                       TPS546_PIN_DETECT_OVERRIDE_VALUE),
                        TAG, "write PIN_DETECT_OVERRIDE failed");

    return ESP_OK;
}

static esp_err_t write_limit_config(void)
{
    ESP_LOGI(TAG, "runtime limits: VIN %.2f-%.2f V, VOUT %.3f-%.3f V, IOUT %.1f/%.1f A",
             s_config.TPS546_INIT_VIN_OFF, s_config.TPS546_INIT_VIN_OV_FAULT_LIMIT,
             s_config.TPS546_INIT_VOUT_MIN, s_config.TPS546_INIT_VOUT_MAX,
             s_config.TPS546_INIT_IOUT_OC_WARN_LIMIT, s_config.TPS546_INIT_IOUT_OC_FAULT_LIMIT);

    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_ON, s_config.TPS546_INIT_VIN_ON,
                                           "VIN_ON"),
                        TAG, "write VIN_ON failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_OFF, s_config.TPS546_INIT_VIN_OFF,
                                           "VIN_OFF"),
                        TAG, "write VIN_OFF failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_VIN_OV_FAULT_LIMIT,
                                           s_config.TPS546_INIT_VIN_OV_FAULT_LIMIT,
                                           "VIN_OV_FAULT_LIMIT"),
                        TAG, "write VIN_OV_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_MIN, s_config.TPS546_INIT_VOUT_MIN,
                                            "VOUT_MIN"),
                        TAG, "write VOUT_MIN failed");
    ESP_RETURN_ON_ERROR(tps_write_ulinear16(PMBUS_VOUT_MAX, s_config.TPS546_INIT_VOUT_MAX,
                                            "VOUT_MAX"),
                        TAG, "write VOUT_MAX failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_IOUT_OC_WARN_LIMIT,
                                           s_config.TPS546_INIT_IOUT_OC_WARN_LIMIT,
                                           "IOUT_OC_WARN_LIMIT"),
                        TAG, "write IOUT_OC_WARN_LIMIT failed");
    ESP_RETURN_ON_ERROR(tps_write_linear11(PMBUS_IOUT_OC_FAULT_LIMIT,
                                           s_config.TPS546_INIT_IOUT_OC_FAULT_LIMIT,
                                           "IOUT_OC_FAULT_LIMIT"),
                        TAG, "write IOUT_OC_FAULT_LIMIT failed");
    ESP_RETURN_ON_ERROR(TPS546_clear_faults(), TAG, "clear faults failed");

    return ESP_OK;
}

esp_err_t TPS546_clear_faults(void)
{
    return tps_write_addr(PMBUS_CLEAR_FAULTS);
}

esp_err_t TPS546_init(TPS546_CONFIG config)
{
    s_config = config;
    s_tps546_model = "";

    ESP_LOGI(TAG, "initializing TPS546 regulator at 0x%02x", TPS546_I2CADDR);
    if (s_tps546_i2c_handle == NULL) {
        ESP_RETURN_ON_ERROR(i2c_bitaxe_add_device(TPS546_I2CADDR, &s_tps546_i2c_handle, TAG),
                            TAG, "add TPS546 I2C device failed");
    }

    vTaskDelay(pdMS_TO_TICKS(TPS546_READY_DELAY_MS));

    uint8_t id[6] = {0};
    ESP_RETURN_ON_ERROR(read_device_id(id), TAG, "TPS546 device ID check failed");
    ESP_LOGI(TAG, "device ID %02x %02x %02x %02x %02x %02x", id[0], id[1], id[2], id[3],
             id[4], id[5]);

    ESP_RETURN_ON_ERROR(read_vout_mode(), TAG, "VOUT mode read failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_OPERATION, OPERATION_OFF), TAG,
                        "turn output off failed");
    ESP_RETURN_ON_ERROR(write_boot_config(), TAG, "TPS546 boot config failed");
    ESP_RETURN_ON_ERROR(TPS546_clear_faults(), TAG, "clear faults failed");

    return ESP_OK;
}

esp_err_t TPS546_apply_limits(TPS546_CONFIG config)
{
    ESP_RETURN_ON_FALSE(s_tps546_i2c_handle != NULL, ESP_ERR_INVALID_STATE, TAG,
                        "TPS546 is not initialized");
    const TPS546_CONFIG old_config = s_config;
    s_config = config;
    const esp_err_t err = write_limit_config();
    if (err != ESP_OK) {
        s_config = old_config;
    }
    return err;
}

const char *TPS546_model(void)
{
    return s_tps546_model;
}

esp_err_t TPS546_set_vout(float volts)
{
    if (volts <= 0.0f) {
        ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_OPERATION, OPERATION_OFF), TAG,
                            "turn output off failed");
        ESP_LOGI(TAG, "output off");
        return ESP_OK;
    }

    ESP_RETURN_ON_FALSE(s_vout_mode_valid, ESP_ERR_INVALID_STATE, TAG,
                        "TPS546 VOUT_MODE was not read");
    ESP_RETURN_ON_FALSE(volts >= s_config.TPS546_INIT_VOUT_MIN &&
                            volts <= s_config.TPS546_INIT_VOUT_MAX,
                        ESP_ERR_INVALID_ARG, TAG, "requested VOUT %.3f outside %.3f-%.3f V",
                        volts, s_config.TPS546_INIT_VOUT_MIN, s_config.TPS546_INIT_VOUT_MAX);

    ESP_RETURN_ON_ERROR(tps_write_word(PMBUS_VOUT_COMMAND, ulinear16_from_float(volts)), TAG,
                        "write VOUT_COMMAND failed");
    ESP_RETURN_ON_ERROR(tps_write_byte(PMBUS_OPERATION, OPERATION_ON), TAG,
                        "turn output on failed");

    uint8_t operation = 0;
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_OPERATION, &operation), TAG,
                        "read OPERATION failed");
    ESP_RETURN_ON_FALSE((operation & OPERATION_ON) != 0, ESP_FAIL, TAG,
                        "OPERATION did not latch ON: 0x%02x", operation);

    ESP_LOGI(TAG, "output on %.3f V", volts);
    return ESP_OK;
}

esp_err_t TPS546_snapshot_status(TPS546_StatusSnapshot *snapshot)
{
    ESP_RETURN_ON_FALSE(snapshot != NULL, ESP_ERR_INVALID_ARG, TAG, "snapshot is NULL");
    memset(snapshot, 0, sizeof(*snapshot));

    uint16_t word = 0;
    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_STATUS_WORD, &snapshot->status_word), TAG,
                        "read STATUS_WORD failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_VOUT, &snapshot->st_vout), TAG,
                        "read STATUS_VOUT failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_INPUT, &snapshot->st_input), TAG,
                        "read STATUS_INPUT failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_IOUT, &snapshot->st_iout), TAG,
                        "read STATUS_IOUT failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_TEMPERATURE, &snapshot->st_temp), TAG,
                        "read STATUS_TEMPERATURE failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_CML, &snapshot->st_cml), TAG,
                        "read STATUS_CML failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_MFR_SPECIFIC, &snapshot->st_mfr), TAG,
                        "read STATUS_MFR_SPECIFIC failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_STATUS_OTHER, &snapshot->st_other), TAG,
                        "read STATUS_OTHER failed");

    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_OPERATION, &snapshot->operation), TAG,
                        "read OPERATION failed");
    ESP_RETURN_ON_ERROR(tps_read_byte(PMBUS_ON_OFF_CONFIG, &snapshot->on_off_config), TAG,
                        "read ON_OFF_CONFIG failed");

    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_VOUT_COMMAND, &word), TAG,
                        "read VOUT_COMMAND failed");
    snapshot->vout_command = ulinear16_to_float(word);
    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_READ_VOUT, &word), TAG, "read READ_VOUT failed");
    snapshot->read_vout = ulinear16_to_float(word);
    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_READ_VIN, &word), TAG, "read READ_VIN failed");
    snapshot->read_vin = linear11_to_float(word);
    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_READ_IOUT, &word), TAG, "read READ_IOUT failed");
    snapshot->read_iout = linear11_to_float(word);
    ESP_RETURN_ON_ERROR(tps_read_word(PMBUS_READ_TEMPERATURE_1, &word), TAG,
                        "read READ_TEMPERATURE_1 failed");
    snapshot->read_temp1 = linear11_to_int(word);

    return ESP_OK;
}

void TPS546_log_snapshot(const TPS546_StatusSnapshot *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    ESP_LOGE(TAG,
             "snapshot: status=0x%04x op=0x%02x cfg=0x%02x cmd=%.3f V vout=%.3f V vin=%.3f V iout=%.3f A temp=%d C",
             snapshot->status_word, snapshot->operation, snapshot->on_off_config,
             snapshot->vout_command, snapshot->read_vout, snapshot->read_vin,
             snapshot->read_iout, snapshot->read_temp1);

    if (snapshot->status_word & TPS546_STATUS_BUSY) {
        ESP_LOGE(TAG, "  BUSY");
    }
    if (snapshot->status_word & TPS546_STATUS_OFF) {
        ESP_LOGE(TAG, "  OFF");
    }
    if (snapshot->status_word & TPS546_STATUS_VOUT_OV) {
        ESP_LOGE(TAG, "  VOUT_OV");
    }
    if (snapshot->status_word & TPS546_STATUS_IOUT_OC) {
        ESP_LOGE(TAG, "  IOUT_OC");
    }
    if (snapshot->status_word & TPS546_STATUS_VIN_UV) {
        ESP_LOGE(TAG, "  VIN_UV");
    }
    if (snapshot->status_word & TPS546_STATUS_TEMP) {
        ESP_LOGE(TAG, "  TEMP");
    }
    if (snapshot->status_word & TPS546_STATUS_CML) {
        ESP_LOGE(TAG, "  CML");
    }
    if (snapshot->status_word & TPS546_STATUS_PGOOD) {
        ESP_LOGE(TAG, "  PGOOD=not in regulation");
    }
    if (snapshot->status_word & TPS546_STATUS_VOUT) {
        ESP_LOGE(TAG, "  STATUS_VOUT=0x%02x", snapshot->st_vout);
    }
    if (snapshot->status_word & TPS546_STATUS_INPUT) {
        ESP_LOGE(TAG, "  STATUS_INPUT=0x%02x", snapshot->st_input);
    }
    if (snapshot->status_word & TPS546_STATUS_IOUT) {
        ESP_LOGE(TAG, "  STATUS_IOUT=0x%02x", snapshot->st_iout);
    }
    if (snapshot->status_word & TPS546_STATUS_TEMP) {
        ESP_LOGE(TAG, "  STATUS_TEMPERATURE=0x%02x", snapshot->st_temp);
    }
    if (snapshot->status_word & TPS546_STATUS_CML) {
        ESP_LOGE(TAG, "  STATUS_CML=0x%02x", snapshot->st_cml);
    }
    if (snapshot->status_word & TPS546_STATUS_MFR) {
        ESP_LOGE(TAG, "  STATUS_MFR=0x%02x", snapshot->st_mfr);
    }
    if (snapshot->status_word & TPS546_STATUS_OTHER) {
        ESP_LOGE(TAG, "  STATUS_OTHER=0x%02x", snapshot->st_other);
    }
}
