#pragma once
#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Crude sanity check (used by sensors_raw_sanity_check)
esp_err_t ens160_raw_check(void);

// Simple single-byte read for early testing.
// We read the low byte of PART_ID at 0x00.
esp_err_t ens160_read_basic_u8(uint8_t *out);

// Proper early-stage init
esp_err_t ens160_init(void);

// Provide ambient T/H to improve accuracy (optional but recommended)
esp_err_t ens160_set_env(float temp_c, float rh_pct);

// IAQ outputs
typedef struct {
    uint8_t  status;
    uint8_t  aqi_uba;
    uint16_t tvoc_ppb;
    uint16_t eco2_ppm;
} ens160_reading_t;

esp_err_t ens160_read_iaq(ens160_reading_t *out);

// Optional helper if you want to log the full part id
esp_err_t ens160_get_part_id(uint16_t *out);

#ifdef __cplusplus
}
#endif