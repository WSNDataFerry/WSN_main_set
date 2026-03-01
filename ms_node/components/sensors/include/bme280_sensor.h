#pragma once

#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
} bme280_reading_t;

esp_err_t bme280_init(void);
esp_err_t bme280_read(bme280_reading_t *out);

// Keep your existing raw check if you want
esp_err_t bme280_raw_check(void);

#ifdef __cplusplus
}
#endif