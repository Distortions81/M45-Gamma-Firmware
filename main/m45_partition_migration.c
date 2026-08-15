#include "m45_partition_migration.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_flash.h"
#include "esp_flash_encrypt.h"
#include "esp_flash_internal.h"
#include "esp_flash_partitions.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "migration_partition_table.h"

#define M45_FLASH_SIZE (16U * 1024U * 1024U)
#define M45_FLASH_SECTOR_SIZE 0x1000U
#define M45_PARTITION_TABLE_OFFSET 0x8000U
#define M45_CANONICAL_FACTORY_OFFSET 0x10000U
#define M45_CANONICAL_FACTORY_SIZE 0x400000U
#define M45_CANONICAL_WWW_OFFSET 0x410000U
#define M45_CANONICAL_WWW_SIZE 0x300000U
#define M45_CANONICAL_OTA0_OFFSET 0x710000U
#define M45_CANONICAL_OTA1_OFFSET 0xb10000U
#define M45_CANONICAL_OTA_SIZE 0x400000U
#define M45_CANONICAL_OTADATA_OFFSET 0xf10000U
#define M45_CANONICAL_OTADATA_SIZE 0x2000U
#define M45_LEGACY_FACTORY_OFFSET 0x20000U
#define M45_LEGACY_OTA0_OFFSET 0x320000U
#define M45_LEGACY_OTA1_OFFSET 0x620000U
#define M45_LEGACY_APP_SIZE 0x300000U
#define M45_COPY_CHUNK_SIZE 0x4000U

static const char *TAG = "partition_migration";

static bool partition_matches(esp_partition_type_t type,
                              esp_partition_subtype_t subtype,
                              const char *label, uint32_t address,
                              uint32_t size)
{
    const esp_partition_t *partition =
        esp_partition_find_first(type, subtype, label);
    return partition != NULL && partition->address == address &&
           partition->size == size;
}

static bool canonical_layout_active(void)
{
    return partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory",
                             M45_CANONICAL_FACTORY_OFFSET,
                             M45_CANONICAL_FACTORY_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_DATA,
                             ESP_PARTITION_SUBTYPE_DATA_SPIFFS, "www",
                             M45_CANONICAL_WWW_OFFSET, M45_CANONICAL_WWW_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0",
                             M45_CANONICAL_OTA0_OFFSET, M45_CANONICAL_OTA_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1",
                             M45_CANONICAL_OTA1_OFFSET, M45_CANONICAL_OTA_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_DATA,
                             ESP_PARTITION_SUBTYPE_DATA_OTA, "otadata",
                             M45_CANONICAL_OTADATA_OFFSET,
                             M45_CANONICAL_OTADATA_SIZE);
}

static bool legacy_layout_active(void)
{
    return partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_FACTORY, "factory",
                             M45_LEGACY_FACTORY_OFFSET, M45_LEGACY_APP_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_0, "ota_0",
                             M45_LEGACY_OTA0_OFFSET, M45_LEGACY_APP_SIZE) &&
           partition_matches(ESP_PARTITION_TYPE_APP,
                             ESP_PARTITION_SUBTYPE_APP_OTA_1, "ota_1",
                             M45_LEGACY_OTA1_OFFSET, M45_LEGACY_APP_SIZE) &&
           esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                    ESP_PARTITION_SUBTYPE_DATA_SPIFFS,
                                    "www") == NULL;
}

static bool ranges_overlap(uint32_t first_start, uint32_t first_size,
                           uint32_t second_start, uint32_t second_size)
{
    return first_start < second_start + second_size &&
           second_start < first_start + first_size;
}

static esp_err_t verify_image_at(uint32_t address, uint32_t size,
                                 esp_image_metadata_t *metadata)
{
    const esp_partition_pos_t position = {
        .offset = address,
        .size = size,
    };
    memset(metadata, 0, sizeof(*metadata));
    metadata->start_addr = address;
    return esp_image_verify(ESP_IMAGE_VERIFY, &position, metadata);
}

static esp_err_t stage_running_image(const esp_partition_t *running,
                                     const esp_image_metadata_t *source)
{
    if (source->image_len == 0 ||
        source->image_len > M45_CANONICAL_FACTORY_SIZE ||
        ranges_overlap(running->address, source->image_len,
                       M45_CANONICAL_FACTORY_OFFSET, source->image_len)) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t erase_size =
        (source->image_len + M45_FLASH_SECTOR_SIZE - 1) &
        ~(M45_FLASH_SECTOR_SIZE - 1);
    ESP_LOGI(TAG, "staging %lu-byte bridge image at 0x%lx",
             (unsigned long)source->image_len,
             (unsigned long)M45_CANONICAL_FACTORY_OFFSET);
    esp_err_t err = esp_flash_erase_region(NULL, M45_CANONICAL_FACTORY_OFFSET,
                                           erase_size);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t *buffer = heap_caps_malloc(M45_COPY_CHUNK_SIZE,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (buffer == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (uint32_t offset = 0; offset < source->image_len;) {
        const uint32_t remaining = source->image_len - offset;
        const uint32_t chunk = remaining < M45_COPY_CHUNK_SIZE
                                   ? remaining
                                   : M45_COPY_CHUNK_SIZE;
        err = esp_partition_read(running, offset, buffer, chunk);
        if (err == ESP_OK) {
            err = esp_flash_write(NULL, buffer,
                                  M45_CANONICAL_FACTORY_OFFSET + offset, chunk);
        }
        if (err != ESP_OK) {
            free(buffer);
            return err;
        }
        offset += chunk;
        vTaskDelay(1);
    }
    free(buffer);

    esp_image_metadata_t staged;
    err = verify_image_at(M45_CANONICAL_FACTORY_OFFSET,
                          M45_CANONICAL_FACTORY_SIZE, &staged);
    if (err != ESP_OK || staged.image_len != source->image_len ||
        memcmp(staged.image_digest, source->image_digest,
               sizeof(staged.image_digest)) != 0) {
        ESP_LOGE(TAG, "staged bridge image verification failed");
        return err == ESP_OK ? ESP_ERR_INVALID_CRC : err;
    }
    ESP_LOGI(TAG, "canonical factory image verified");
    return ESP_OK;
}

static esp_err_t write_canonical_partition_table(void)
{
    if (m45_canonical_partition_table_size > ESP_PARTITION_TABLE_MAX_LEN) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *sector = heap_caps_malloc(M45_FLASH_SECTOR_SIZE,
                                       MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    uint8_t *readback = heap_caps_malloc(M45_FLASH_SECTOR_SIZE,
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (sector == NULL || readback == NULL) {
        free(sector);
        free(readback);
        return ESP_ERR_NO_MEM;
    }
    memset(sector, 0xff, M45_FLASH_SECTOR_SIZE);
    memcpy(sector, m45_canonical_partition_table,
           m45_canonical_partition_table_size);

    int partition_count = 0;
    esp_err_t err = esp_partition_table_verify(
        (const esp_partition_info_t *)sector, true, &partition_count);
    if (err != ESP_OK || partition_count != 8) {
        free(sector);
        free(readback);
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    err = esp_flash_set_dangerous_write_protection(esp_flash_default_chip,
                                                    false);
    if (err != ESP_OK) {
        free(sector);
        free(readback);
        return err;
    }

    ESP_LOGW(TAG, "committing canonical partition table; do not remove power");
    err = esp_flash_erase_region(NULL, M45_PARTITION_TABLE_OFFSET,
                                 M45_FLASH_SECTOR_SIZE);
    if (err == ESP_OK) {
        err = esp_flash_write(NULL, sector, M45_PARTITION_TABLE_OFFSET,
                              M45_FLASH_SECTOR_SIZE);
    }
    if (err == ESP_OK) {
        err = esp_flash_read(NULL, readback, M45_PARTITION_TABLE_OFFSET,
                             M45_FLASH_SECTOR_SIZE);
    }
    if (err == ESP_OK &&
        memcmp(sector, readback, M45_FLASH_SECTOR_SIZE) != 0) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err == ESP_OK) {
        partition_count = 0;
        err = esp_partition_table_verify(
            (const esp_partition_info_t *)readback, true, &partition_count);
        if (err == ESP_OK && partition_count != 8) {
            err = ESP_ERR_INVALID_SIZE;
        }
    }
    const esp_err_t protect_err = esp_flash_set_dangerous_write_protection(
        esp_flash_default_chip, true);
    if (err == ESP_OK) {
        err = protect_err;
    }
    free(sector);
    free(readback);
    return err;
}

esp_err_t m45_partition_migrate_if_needed(void)
{
    if (canonical_layout_active()) {
        ESP_LOGI(TAG, "canonical partition layout active");
        return ESP_OK;
    }
    if (!legacy_layout_active()) {
        ESP_LOGE(TAG, "partition layout is neither v0.0.9 nor canonical");
        return ESP_ERR_INVALID_STATE;
    }
    if (esp_flash_encryption_enabled()) {
        ESP_LOGE(TAG, "automatic migration does not support flash encryption");
        return ESP_ERR_NOT_SUPPORTED;
    }

    uint32_t flash_size = 0;
    esp_err_t err = esp_flash_get_size(NULL, &flash_size);
    if (err != ESP_OK || flash_size < M45_FLASH_SIZE) {
        ESP_LOGE(TAG, "migration requires 16 MB flash");
        return err == ESP_OK ? ESP_ERR_INVALID_SIZE : err;
    }

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL || running->type != ESP_PARTITION_TYPE_APP ||
        (running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_0 &&
         running->subtype != ESP_PARTITION_SUBTYPE_APP_OTA_1)) {
        ESP_LOGE(TAG, "bridge release must run from a legacy OTA slot");
        return ESP_ERR_INVALID_STATE;
    }

    esp_image_metadata_t source;
    err = verify_image_at(running->address, running->size, &source);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "running bridge image verification failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = stage_running_image(running, &source);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bridge staging failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_flash_erase_region(NULL, M45_CANONICAL_OTADATA_OFFSET,
                                 M45_CANONICAL_OTADATA_SIZE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "canonical OTA metadata initialization failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = write_canonical_partition_table();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "partition table migration failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "partition migration complete; rebooting canonical layout");
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_restart();
    return ESP_OK;
}
