#include "aht21_sensor.h"
#include "sensors.h"
#include "i2c_bus.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "driver/i2c.h"

static const char *TAG = "sensors";

// This should match your i2c_bus implementation.
// Your logs show you're using bus on SDA=8/SCL=9 and likely I2C_NUM_0.
#define AHT21_I2C_PORT I2C_NUM_0

// AHT2x family common commands
#define CMD_INIT               0xBE
#define CMD_TRIGGER_MEASURE    0xAC
#define CMD_SOFT_RESET         0xBA

static bool aht_inited = false;

static esp_err_t aht21_send_cmd(const uint8_t *cmd, size_t len)
{
    if (!cmd || len == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = i2c_master_write_to_device(
        AHT21_I2C_PORT,
        ADDR_AHT21,
        cmd,
        len,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT21 I2C write failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

static esp_err_t aht21_read_bytes(uint8_t *buf, size_t len)
{
    if (!buf || len == 0) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = i2c_master_read_from_device(
        AHT21_I2C_PORT,
        ADDR_AHT21,
        buf,
        len,
        pdMS_TO_TICKS(100)
    );

    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT21 I2C read failed: %s", esp_err_to_name(ret));
    }

    return ret;
}

esp_err_t aht21_init(void)
{
    if (aht_inited) return ESP_OK;

    // Init/calibration command sequence used by AHT20/AHT21 modules
    // 0xBE 0x08 0x00
    uint8_t cmd[3] = { CMD_INIT, 0x08, 0x00 };

    esp_err_t ret = aht21_send_cmd(cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT21 init command failed");
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    aht_inited = true;
    return ESP_OK;
}

esp_err_t aht21_read_raw(uint8_t raw[AHT21_RAW_LEN])
{
    if (!raw) return ESP_ERR_INVALID_ARG;

    // Ensure init is attempted (non-fatal)
    esp_err_t ret = aht21_init();
    if (ret != ESP_OK) {
        return ret;
    }

    // Trigger measurement: 0xAC 0x33 0x00
    uint8_t cmd[3] = { CMD_TRIGGER_MEASURE, 0x33, 0x00 };

    ret = aht21_send_cmd(cmd, sizeof(cmd));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AHT21 trigger measure failed");
        return ret;
    }

    // Typical wait ~80 ms for AHT2x
    vTaskDelay(pdMS_TO_TICKS(80));

    ret = aht21_read_bytes(raw, AHT21_RAW_LEN);
    if (ret != ESP_OK) {
        return ret;
    }

    // raw[0] contains status/busy bits; we keep it simple here.
    return ESP_OK;
}

static void aht21_convert(const uint8_t raw[AHT21_RAW_LEN], aht21_reading_t *out)
{
    // Data format (AHT2x family):
    // humidity_raw: 20 bits from raw[1..3]
    // temperature_raw: 20 bits from raw[3..5]
    uint32_t hum_raw = ((uint32_t)raw[1] << 16) | ((uint32_t)raw[2] << 8) | raw[3];
    hum_raw >>= 4;

    uint32_t temp_raw = ((uint32_t)(raw[3] & 0x0F) << 16) | ((uint32_t)raw[4] << 8) | raw[5];

    out->humidity_pct = ((float)hum_raw * 100.0f) / 1048576.0f; // 2^20
    out->temperature_c = (((float)temp_raw * 200.0f) / 1048576.0f) - 50.0f;
}

esp_err_t aht21_read(aht21_reading_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    uint8_t raw[AHT21_RAW_LEN] = {0};
    esp_err_t ret = aht21_read_raw(raw);
    if (ret != ESP_OK) return ret;

    aht21_convert(raw, out);
    return ESP_OK;
}

esp_err_t aht21_read_with_raw(aht21_reading_t *out, uint8_t raw[AHT21_RAW_LEN])
{
    if (!out || !raw) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = aht21_read_raw(raw);
    if (ret != ESP_OK) return ret;

    aht21_convert(raw, out);
    return ESP_OK;
}

esp_err_t aht21_raw_check(void)
{
    uint8_t raw[AHT21_RAW_LEN] = {0};
    esp_err_t ret = aht21_read_raw(raw);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "AHT21 raw: %02X %02X %02X %02X %02X %02X",
                 raw[0], raw[1], raw[2], raw[3], raw[4], raw[5]);
    } else {
        ESP_LOGW(TAG, "AHT21 raw check failed: %s", esp_err_to_name(ret));
    }

    return ret;
}