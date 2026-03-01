#pragma once

#include "esp_err.h"
#include "sensor_config.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Known addresses from your scans
#define ADDR_BME280   0x76
#define ADDR_AHT21    0x38
#define ADDR_ENS160   0x53
#define ADDR_GY271    0x0D
#define ADDR_INA219   0x40

typedef struct {
    bool bme280_ok;
    bool aht21_ok;
    bool ens160_ok;
    bool gy271_ok;
    bool ina219_ok;
    bool inmp441_ok;
} sensors_presence_t;

esp_err_t sensors_init(sensors_presence_t *out_presence);

// Your current raw sanity test, now component-owned
esp_err_t sensors_raw_sanity_check(void);

// Trust filtering: validate sensor readings are within expected ranges
bool sensors_validate_temperature(float temp_c);
bool sensors_validate_humidity(float humidity_pct);
bool sensors_validate_pressure(float pressure_hpa);
bool sensors_validate_voc(uint16_t tvoc_ppb);
bool sensors_validate_co2(uint16_t eco2_ppm);

#ifdef __cplusplus
}
#endif