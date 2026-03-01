#include "ens160_sensor.h"
#include "i2c_bus.h"
#include "sensors.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "sensors";

/*
 * ENS160 register map (common across ENS16x family)
 * PART_ID:       0x00 (2 bytes, little-endian, expected 0x0160)
 * OPMODE:        0x10 (1 byte)
 * COMMAND:       0x12 (1 byte)  // not used yet
 * TEMP_IN:       0x13 (2 bytes) // Kelvin * 64
 * RH_IN:         0x15 (2 bytes) // %RH * 512
 * DATA_STATUS:   0x20 (1 byte)
 * DATA_AQI:      0x21 (1 byte)
 * DATA_TVOC:     0x22 (2 bytes)
 * DATA_ECO2:     0x24 (2 bytes)
 */
#define REG_PART_ID         0x00
#define REG_OPMODE          0x10
#define REG_COMMAND         0x12
#define REG_TEMP_IN         0x13
#define REG_RH_IN           0x15
#define REG_DATA_STATUS     0x20
#define REG_DATA_AQI        0x21
#define REG_DATA_TVOC       0x22
#define REG_DATA_ECO2       0x24

#define ENS160_PART_ID_EXPECTED  0x0160

#define ENS160_OPMODE_SLEEP      0x00
#define ENS160_OPMODE_IDLE       0x01
#define ENS160_OPMODE_STANDARD   0x02

static bool ens_inited = false;
static uint16_t cached_part_id = 0;

static esp_err_t read_u16_le(uint8_t reg, uint16_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t buf[2] = {0};
    esp_err_t ret = ms_i2c_read(ADDR_ENS160, reg, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    *out = (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
    return ESP_OK;
}

static esp_err_t write_u16_le(uint8_t reg, uint16_t v)
{
    // Use two single-byte writes to rely only on known helpers
    esp_err_t ret = ms_i2c_write_u8(ADDR_ENS160, reg,     (uint8_t)(v & 0xFF));
    if (ret != ESP_OK) return ret;

    ret = ms_i2c_write_u8(ADDR_ENS160, reg + 1, (uint8_t)((v >> 8) & 0xFF));
    return ret;
}

esp_err_t ens160_read_basic_u8(uint8_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t v = 0;
    esp_err_t ret = ms_i2c_read_u8(ADDR_ENS160, REG_PART_ID, &v);
    if (ret != ESP_OK) return ret;

    *out = v;
    return ESP_OK;
}

esp_err_t ens160_raw_check(void)
{
    uint8_t raw0 = 0;
    esp_err_t ret = ens160_read_basic_u8(&raw0);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "ENS160 raw[0]: 0x%02X", raw0);
    } else {
        ESP_LOGE(TAG, "ENS160 raw check failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t ens160_get_part_id(uint16_t *out)
{
    uint16_t pid = 0;
    esp_err_t ret = read_u16_le(REG_PART_ID, &pid);
    if (ret != ESP_OK) return ret;

    if (out) *out = pid;
    return ESP_OK;
}

esp_err_t ens160_init(void)
{
    if (ens_inited) return ESP_OK;

    uint16_t pid = 0;
    esp_err_t ret = read_u16_le(REG_PART_ID, &pid);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 not responding for PART_ID read: %s", esp_err_to_name(ret));
        return ESP_ERR_NOT_FOUND;
    }

    cached_part_id = pid;

    if (pid != ENS160_PART_ID_EXPECTED) {
        ESP_LOGW(TAG, "Unexpected ENS160 PART_ID: 0x%04X", pid);
        return ESP_ERR_INVALID_RESPONSE;
    }

    // Set STANDARD mode
    ret = ms_i2c_write_u8(ADDR_ENS160, REG_OPMODE, ENS160_OPMODE_STANDARD);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "ENS160 opmode set failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Give the sensor a short settle time after mode change
    vTaskDelay(pdMS_TO_TICKS(50));

    ens_inited = true;
    ESP_LOGI(TAG, "ENS160 init OK (PART_ID=0x%04X)", pid);
    return ESP_OK;
}

esp_err_t ens160_set_env(float temp_c, float rh_pct)
{
    esp_err_t ret = ens160_init();
    if (ret != ESP_OK) return ret;

    // Clamp reasonable values
    if (rh_pct < 0.0f) rh_pct = 0.0f;
    if (rh_pct > 100.0f) rh_pct = 100.0f;

    // TEMP_IN format: Kelvin * 64
    float k = temp_c + 273.15f;
    if (k < 0.0f) k = 0.0f;

    uint16_t temp_in = (uint16_t)(k * 64.0f + 0.5f);

    // RH_IN format: %RH * 512
    uint16_t rh_in = (uint16_t)(rh_pct * 512.0f + 0.5f);

    ret = write_u16_le(REG_TEMP_IN, temp_in);
    if (ret != ESP_OK) return ret;

    ret = write_u16_le(REG_RH_IN, rh_in);
    return ret;
}

esp_err_t ens160_read_iaq(ens160_reading_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = ens160_init();
    if (ret != ESP_OK) return ret;

    uint8_t status = 0;
    uint8_t aqi = 0;
    uint16_t tvoc = 0;
    uint16_t eco2 = 0;

    ret = ms_i2c_read_u8(ADDR_ENS160, REG_DATA_STATUS, &status);
    if (ret != ESP_OK) return ret;

    ret = ms_i2c_read_u8(ADDR_ENS160, REG_DATA_AQI, &aqi);
    if (ret != ESP_OK) return ret;

    ret = read_u16_le(REG_DATA_TVOC, &tvoc);
    if (ret != ESP_OK) return ret;

    ret = read_u16_le(REG_DATA_ECO2, &eco2);
    if (ret != ESP_OK) return ret;

    out->status = status;
    out->aqi_uba = aqi;
    out->tvoc_ppb = tvoc;
    out->eco2_ppm = eco2;

    return ESP_OK;
}