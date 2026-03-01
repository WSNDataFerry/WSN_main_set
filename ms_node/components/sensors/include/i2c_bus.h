#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Your proven working pins
#define MS_I2C_SDA_GPIO  8
#define MS_I2C_SCL_GPIO  9

// Keep the bus simple for now
#define MS_I2C_PORT      0
#define MS_I2C_FREQ_HZ   100000

esp_err_t ms_i2c_init(void);

// Simple helpers for raw register access
esp_err_t ms_i2c_read_u8(uint8_t addr, uint8_t reg, uint8_t *out);
esp_err_t ms_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len);

esp_err_t ms_i2c_write_u8(uint8_t addr, uint8_t reg, uint8_t val);
// NEW: multi-byte write helper (reg + payload)
esp_err_t ms_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf, uint8_t len);

#ifdef __cplusplus
}
#endif