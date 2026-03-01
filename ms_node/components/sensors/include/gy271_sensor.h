#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int16_t x_raw;
    int16_t y_raw;
    int16_t z_raw;

    // Rough converted values (see note in .c)
    float x_uT;
    float y_uT;
    float z_uT;

    uint8_t status;
} gy271_reading_t;

// Crude sanity check (used by sensors_raw_sanity_check)
esp_err_t gy271_raw_check(void);

// Proper init for QMC5883L
esp_err_t gy271_init(void);

// Read raw + rough converted XYZ
esp_err_t gy271_read(gy271_reading_t *out);

#ifdef __cplusplus
}
#endif