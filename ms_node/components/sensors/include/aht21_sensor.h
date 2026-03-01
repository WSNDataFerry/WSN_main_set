#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AHT21_RAW_LEN 6

typedef struct {
    float temperature_c;
    float humidity_pct;
} aht21_reading_t;

// Quick presence-ish check (now measurement-based, not dummy register read)
esp_err_t aht21_raw_check(void);

// Optional explicit init (safe to call multiple times)
esp_err_t aht21_init(void);

// Read only the 6 raw bytes from a fresh measurement
esp_err_t aht21_read_raw(uint8_t raw[AHT21_RAW_LEN]);

// Read converted values (internally triggers measurement)
esp_err_t aht21_read(aht21_reading_t *out);

// Read raw + converted in one call
esp_err_t aht21_read_with_raw(aht21_reading_t *out, uint8_t raw[AHT21_RAW_LEN]);

#ifdef __cplusplus
}
#endif