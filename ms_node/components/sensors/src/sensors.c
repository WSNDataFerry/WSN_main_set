#include "sensors.h"
#include "i2c_bus.h"
#include "inmp441_sensor.h"
#include "esp_log.h"

// Forward declarations of per-sensor raw checks
esp_err_t bme280_raw_check(void);
esp_err_t aht21_raw_check(void);
esp_err_t ens160_raw_check(void);
esp_err_t gy271_raw_check(void);
esp_err_t ina219_raw_check(void);
esp_err_t inmp441_raw_check(void);

static const char *TAG = "sensors";

static bool probe_addr(uint8_t addr)
{
    // Quick ACK probe using a harmless read attempt of 0 bytes isn't possible here,
    // so we do a simple 1-byte read from reg 0x00 and treat OK as presence.
    uint8_t dummy = 0;
    return (ms_i2c_read_u8(addr, 0x00, &dummy) == ESP_OK);
}

esp_err_t sensors_init(sensors_presence_t *out_presence)
{
    ESP_ERROR_CHECK(ms_i2c_init());

    sensors_presence_t p = {0};

    // Presence checks based on your known addresses
    p.bme280_ok = probe_addr(ADDR_BME280);
    p.aht21_ok  = probe_addr(ADDR_AHT21);
    p.ens160_ok = probe_addr(ADDR_ENS160);
    p.gy271_ok  = probe_addr(ADDR_GY271);
    p.ina219_ok = probe_addr(ADDR_INA219);
    
    // Check INMP441 I2S microphone (not I2C)
    // Don't call inmp441_get_level() - it does blocking I2S read that conflicts with ISRs
    // Just check if inmp441_raw_check succeeds
    p.inmp441_ok = (inmp441_raw_check() == ESP_OK);

    if (out_presence) *out_presence = p;

    ESP_LOGI(TAG, "Presence -> BME280:%d AHT21:%d ENS160:%d GY-271:%d INA219:%d INMP441:%d",
             p.bme280_ok, p.aht21_ok, p.ens160_ok, p.gy271_ok, p.ina219_ok, p.inmp441_ok);

    return ESP_OK;
}

esp_err_t sensors_raw_sanity_check(void)
{
    ESP_LOGI(TAG, "Running sensors_raw_sanity_check...");

    sensors_presence_t p;
    sensors_init(&p);

    if (p.bme280_ok)  ESP_ERROR_CHECK(bme280_raw_check());
    if (p.aht21_ok)   ESP_ERROR_CHECK(aht21_raw_check());
    if (p.ens160_ok)  ESP_ERROR_CHECK(ens160_raw_check());
    if (p.gy271_ok)   ESP_ERROR_CHECK(gy271_raw_check());
    if (p.ina219_ok)  ESP_ERROR_CHECK(ina219_raw_check());
    if (p.inmp441_ok) ESP_ERROR_CHECK(inmp441_raw_check());

    ESP_LOGI(TAG, "sensors_raw_sanity_check done.");
    return ESP_OK;
}

// Trust filtering functions
bool sensors_validate_temperature(float temp_c)
{
    sensor_config_t cfg;
    sensor_config_get(&cfg);
    
    if (temp_c < cfg.temp_min_c || temp_c > cfg.temp_max_c) {
        ESP_LOGW(TAG, "Temperature %.2f°C out of range [%.1f, %.1f]", 
                 temp_c, cfg.temp_min_c, cfg.temp_max_c);
        return false;
    }
    return true;
}

bool sensors_validate_humidity(float humidity_pct)
{
    sensor_config_t cfg;
    sensor_config_get(&cfg);
    
    if (humidity_pct < cfg.humidity_min_pct || humidity_pct > cfg.humidity_max_pct) {
        ESP_LOGW(TAG, "Humidity %.2f%% out of range [%.1f, %.1f]", 
                 humidity_pct, cfg.humidity_min_pct, cfg.humidity_max_pct);
        return false;
    }
    return true;
}

bool sensors_validate_pressure(float pressure_hpa)
{
    sensor_config_t cfg;
    sensor_config_get(&cfg);
    
    if (pressure_hpa < cfg.pressure_min_hpa || pressure_hpa > cfg.pressure_max_hpa) {
        ESP_LOGW(TAG, "Pressure %.2f hPa out of range [%.1f, %.1f]", 
                 pressure_hpa, cfg.pressure_min_hpa, cfg.pressure_max_hpa);
        return false;
    }
    return true;
}

bool sensors_validate_voc(uint16_t tvoc_ppb)
{
    // TVOC typical range: 0-60000 ppb
    if (tvoc_ppb > 60000) {
        ESP_LOGW(TAG, "TVOC %u ppb suspiciously high", tvoc_ppb);
        return false;
    }
    return true;
}

bool sensors_validate_co2(uint16_t eco2_ppm)
{
    // eCO2 typical range: 400-60000 ppm
    if (eco2_ppm < 100 || eco2_ppm > 65000) {
        ESP_LOGW(TAG, "eCO2 %u ppm out of range", eco2_ppm);
        return false;
    }
    return true;
}