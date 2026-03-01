#include "ina219_sensor.h"
#include "i2c_bus.h"
#include "sensors.h"

#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "sensors";

/* INA219 registers */
#define REG_CONFIG       0x00
#define REG_SHUNT_V      0x01
#define REG_BUS_V        0x02
#define REG_POWER        0x03
#define REG_CURRENT      0x04
#define REG_CALIB        0x05

static bool ina_inited = false;

static esp_err_t read_u16_be(uint8_t reg, uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t buf[2] = {0};
    esp_err_t ret = ms_i2c_read(ADDR_INA219, reg, buf, 2);
    if (ret != ESP_OK) return ret;

    *out = ((uint16_t)buf[0] << 8) | buf[1];
    return ESP_OK;
}

static esp_err_t write_u16_be(uint8_t reg, uint16_t val)
{
    uint8_t buf[2];
    buf[0] = (uint8_t)(val >> 8);
    buf[1] = (uint8_t)(val & 0xFF);
    return ms_i2c_write(ADDR_INA219, reg, buf, 2);
}

esp_err_t ina219_raw_check(void)
{
    uint16_t cfg = 0;
    esp_err_t ret = read_u16_be(REG_CONFIG, &cfg);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "INA219 cfg: 0x%04X", cfg);
    } else {
        ESP_LOGE(TAG, "INA219 raw check failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ina219_init_basic(void)
{
    if (ina_inited) return ESP_OK;

    uint16_t cfg = 0;
    esp_err_t ret = read_u16_be(REG_CONFIG, &cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "INA219 not responding on 0x%02X", ADDR_INA219);
        return ESP_ERR_NOT_FOUND;
    }

    // Force a known safe default config (also removes unused warning cleanly)
    const uint16_t desired = 0x399F;
    if (cfg != desired) {
        esp_err_t w = write_u16_be(REG_CONFIG, desired);
        if (w == ESP_OK) cfg = desired;
    }

    ina_inited = true;
    ESP_LOGI(TAG, "INA219 init OK (cfg=0x%04X)", cfg);
    return ESP_OK;
}

esp_err_t ina219_read_basic(ina219_basic_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!ina_inited) {
        esp_err_t r = ina219_init_basic();
        if (r != ESP_OK) return r;
    }

    uint16_t shunt_u16 = 0;
    uint16_t bus_u16 = 0;

    esp_err_t ret = read_u16_be(REG_SHUNT_V, &shunt_u16);
    if (ret != ESP_OK) return ret;

    ret = read_u16_be(REG_BUS_V, &bus_u16);
    if (ret != ESP_OK) return ret;

    // Shunt voltage is signed. LSB = 10uV = 0.01mV
    int16_t shunt_s16 = (int16_t)shunt_u16;
    out->shunt_voltage_mv = (float)shunt_s16 * 0.01f;
    out->current_ma = out->shunt_voltage_mv / INA219_SHUNT_OHMS; // mV / ohms -> mA

    // Bus voltage: bits [15:13] reserved, [12:3] bus voltage data
    // LSB = 4mV
    uint16_t bus_mv = (uint16_t)((bus_u16 >> 3) * 4);
    out->bus_voltage_v = (float)bus_mv / 1000.0f;

    return ESP_OK;
}