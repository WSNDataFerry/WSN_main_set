#pragma once

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Sensor configuration and power management

typedef struct {
    bool bme280_enabled;
    bool aht21_enabled;
    bool ens160_enabled;
    bool gy271_enabled;
    bool ina219_enabled;
    bool inmp441_enabled;
    
    // Sampling intervals (milliseconds)
    uint32_t env_sensor_interval_ms;   // BME280, AHT21
    uint32_t gas_sensor_interval_ms;   // ENS160
    uint32_t mag_sensor_interval_ms;   // GY-271
    uint32_t power_sensor_interval_ms; // INA219
    uint32_t audio_interval_ms;        // INMP441
    
    // Audio settings
    uint32_t audio_sample_rate;
    uint32_t audio_duration_ms;        // Recording duration per capture
    
    // BLE beacon settings (multi-node coordination)
    uint32_t beacon_interval_ms;       // Base beacon interval
    uint32_t beacon_offset_ms;         // Node-specific offset for collision avoidance
    
    // Trust filtering thresholds
    float temp_min_c;
    float temp_max_c;
    float humidity_min_pct;
    float humidity_max_pct;
    float pressure_min_hpa;
    float pressure_max_hpa;
} sensor_config_t;

// Get default configuration
sensor_config_t sensor_config_get_default(void);

// Load configuration from NVS
esp_err_t sensor_config_load(sensor_config_t *config);

// Save configuration to NVS
esp_err_t sensor_config_save(const sensor_config_t *config);

// Update configuration at runtime
esp_err_t sensor_config_update(const sensor_config_t *config);

// Get current active configuration
esp_err_t sensor_config_get(sensor_config_t *config);

// Enable/disable individual sensors
esp_err_t sensor_enable(const char *sensor_name, bool enable);

#ifdef __cplusplus
}
#endif
