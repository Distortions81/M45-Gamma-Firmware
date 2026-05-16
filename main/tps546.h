#ifndef M45_TPS546_H_
#define M45_TPS546_H_

#include <stdint.h>

#include "esp_err.h"

#define TPS546_I2CADDR 0x24

#define OPERATION_OFF 0x00
#define OPERATION_ON  0x80

#define TPS546_INIT_PHASE_SINGLE 0x00
#define TPS546_INIT_OT_WARN_LIMIT 105

typedef struct {
    uint16_t status_word;
    uint8_t st_vout;
    uint8_t st_input;
    uint8_t st_iout;
    uint8_t st_temp;
    uint8_t st_cml;
    uint8_t st_mfr;
    uint8_t st_other;
    uint8_t operation;
    uint8_t on_off_config;
    float read_vout;
    float read_vin;
    float read_iout;
    int read_temp1;
    float vout_command;
} TPS546_StatusSnapshot;

typedef struct {
    uint8_t TPS546_INIT_PHASE;
    float TPS546_INIT_VIN_ON;
    float TPS546_INIT_VIN_OFF;
    float TPS546_INIT_VIN_UV_WARN_LIMIT;
    float TPS546_INIT_VIN_OV_FAULT_LIMIT;
    float TPS546_INIT_SCALE_LOOP;
    float TPS546_INIT_VOUT_MIN;
    float TPS546_INIT_VOUT_MAX;
    float TPS546_INIT_VOUT_COMMAND;
    float TPS546_INIT_IOUT_OC_WARN_LIMIT;
    float TPS546_INIT_IOUT_OC_FAULT_LIMIT;
    uint16_t TPS546_INIT_STACK_CONFIG;
    uint8_t TPS546_INIT_SYNC_CONFIG;
} TPS546_CONFIG;

#define TPS546_STATUS_VOUT    0x8000
#define TPS546_STATUS_IOUT    0x4000
#define TPS546_STATUS_INPUT   0x2000
#define TPS546_STATUS_MFR     0x1000
#define TPS546_STATUS_PGOOD   0x0800
#define TPS546_STATUS_OTHER   0x0200
#define TPS546_STATUS_BUSY    0x0080
#define TPS546_STATUS_OFF     0x0040
#define TPS546_STATUS_VOUT_OV 0x0020
#define TPS546_STATUS_IOUT_OC 0x0010
#define TPS546_STATUS_VIN_UV  0x0008
#define TPS546_STATUS_TEMP    0x0004
#define TPS546_STATUS_CML     0x0002
#define TPS546_STATUS_NONE    0x0001

#define TPS546_STATUS_VOUT_OVF     0x80
#define TPS546_STATUS_VOUT_OVW     0x40
#define TPS546_STATUS_VOUT_UVW     0x20
#define TPS546_STATUS_VOUT_UVF     0x10
#define TPS546_STATUS_VOUT_MIN_MAX 0x08
#define TPS546_STATUS_VOUT_TON_MAX 0x04

#define TPS546_STATUS_IOUT_OCF 0x80
#define TPS546_STATUS_IOUT_OCW 0x20

#define TPS546_STATUS_VIN_OVF     0x80
#define TPS546_STATUS_VIN_UVW     0x20
#define TPS546_STATUS_VIN_LOW_VIN 0x08

#define TPS546_STATUS_TEMP_OTF 0x80
#define TPS546_STATUS_TEMP_OTW 0x40

#define TPS546_STATUS_CML_IVC  0x80
#define TPS546_STATUS_CML_IVD  0x40
#define TPS546_STATUS_CML_PEC  0x20
#define TPS546_STATUS_CML_MEM  0x10
#define TPS546_STATUS_CML_PROC 0x08
#define TPS546_STATUS_CML_COMM 0x02

#define TPS546_STATUS_OTHER_FIRST 0x01

#define TPS546_STATUS_MFR_POR   0x80
#define TPS546_STATUS_MFR_SELF  0x40
#define TPS546_STATUS_MFR_RESET 0x08
#define TPS546_STATUS_MFR_BCX   0x04
#define TPS546_STATUS_MFR_SYNC  0x02

esp_err_t TPS546_init(TPS546_CONFIG config);
esp_err_t TPS546_set_vout(float volts);
esp_err_t TPS546_clear_faults(void);
esp_err_t TPS546_snapshot_status(TPS546_StatusSnapshot *snapshot);
const char *TPS546_model(void);
void TPS546_log_snapshot(const TPS546_StatusSnapshot *snapshot);

#endif
