#pragma once
#include "esp_err.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float bus_voltage_v;
    float shunt_voltage_mv;
    float current_ma;     // Derived using configured shunt ohms
} ina219_basic_t;

// Adjust to your actual shunt resistor value (ohms). Default assumes 0.1 ohm.
#ifndef INA219_SHUNT_OHMS
#define INA219_SHUNT_OHMS 0.1f
#endif

// Crude sanity check (used by sensors_raw_sanity_check)
esp_err_t ina219_raw_check(void);

// Basic init for early testing (non-fatal usage)
esp_err_t ina219_init_basic(void);

// Read bus + shunt only (no calibration needed)
esp_err_t ina219_read_basic(ina219_basic_t *out);

#ifdef __cplusplus
}
#endif