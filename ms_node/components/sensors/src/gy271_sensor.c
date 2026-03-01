#include "gy271_sensor.h"
#include "i2c_bus.h"
#include "sensors.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "sensors";

// QMC5883L register map (common for GY-271 @ 0x0D)
#define REG_DATA_X_LSB      0x00  // 0x00..0x05 = X/Y/Z LSB/MSB
#define REG_STATUS          0x06
#define REG_CTRL1           0x09
#define REG_CTRL2           0x0A
#define REG_SET_RESET       0x0B

// CTRL1 bits (typical QMC layout)
// [7:6] OSR, [5:4] RNG, [3:2] ODR, [1:0] MODE
#define MODE_STANDBY        0x00
#define MODE_CONTINUOUS     0x01

#define ODR_10HZ            (0x00 << 2)
#define ODR_50HZ            (0x01 << 2)
#define ODR_100HZ           (0x02 << 2)
#define ODR_200HZ           (0x03 << 2)

#define RNG_2G              (0x00 << 4)
#define RNG_8G              (0x01 << 4)

#define OSR_512             (0x00 << 6)
#define OSR_256             (0x01 << 6)
#define OSR_128             (0x02 << 6)
#define OSR_64              (0x03 << 6)

// CTRL2 soft reset bit is commonly documented as 0x80 in many libs
#define CTRL2_SOFT_RESET    0x80

// Rough scale assumption for 2G range.
// This is a practical approximation:
// assume full-scale ±2 Gauss maps to ±32768 counts.
// 1 Gauss = 100 uT
// => 1 LSB ≈ (2 / 32768) Gauss ≈ 0.000061035 Gauss ≈ 0.0061035 uT
// Use this for early testing; later you can refine/calibrate.
#define QMC_2G_UT_PER_LSB   0.0061035f

static bool gy_inited = false;

static int16_t s16_le(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

esp_err_t gy271_raw_check(void)
{
    // A simple "does data read work?" check
    uint8_t buf[6] = {0};
    uint8_t st = 0;

    esp_err_t ret = ms_i2c_read(ADDR_GY271, REG_DATA_X_LSB, buf, sizeof(buf));
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GY-271 raw check failed (data): %s", esp_err_to_name(ret));
        return ret;
    }

    ret = ms_i2c_read_u8(ADDR_GY271, REG_STATUS, &st);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GY-271 raw check failed (status): %s", esp_err_to_name(ret));
        return ret;
    }

    int16_t x = s16_le(&buf[0]);
    int16_t y = s16_le(&buf[2]);
    int16_t z = s16_le(&buf[4]);

    ESP_LOGI(TAG, "GY-271 raw XYZ: %d %d %d | status: 0x%02X", x, y, z, st);
    return ESP_OK;
}

esp_err_t gy271_init(void)
{
    // Soft reset
    esp_err_t ret = ms_i2c_write_u8(ADDR_GY271, REG_CTRL2, CTRL2_SOFT_RESET);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GY-271 reset failed: %s", esp_err_to_name(ret));
        return ret;
    }

    vTaskDelay(pdMS_TO_TICKS(10));

    // Set/Reset period (common recommended value)
    ret = ms_i2c_write_u8(ADDR_GY271, REG_SET_RESET, 0x01);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GY-271 set/reset write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // Continuous mode, ODR 50Hz, range 2G, OSR 512
    uint8_t ctrl1 = OSR_512 | RNG_2G | ODR_50HZ | MODE_CONTINUOUS;

    ret = ms_i2c_write_u8(ADDR_GY271, REG_CTRL1, ctrl1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "GY-271 ctrl1 write failed: %s", esp_err_to_name(ret));
        return ret;
    }

    gy_inited = true;
    ESP_LOGI(TAG, "GY-271 init OK (assumed QMC5883L @ 0x%02X)", ADDR_GY271);
    return ESP_OK;
}

esp_err_t gy271_read(gy271_reading_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!gy_inited) {
        esp_err_t r = gy271_init();
        if (r != ESP_OK) return r;
    }

    uint8_t st = 0;
    esp_err_t ret = ms_i2c_read_u8(ADDR_GY271, REG_STATUS, &st);
    if (ret != ESP_OK) return ret;

    // Read 6 bytes XYZ
    uint8_t buf[6] = {0};
    ret = ms_i2c_read(ADDR_GY271, REG_DATA_X_LSB, buf, sizeof(buf));
    if (ret != ESP_OK) return ret;

    int16_t x = s16_le(&buf[0]);
    int16_t y = s16_le(&buf[2]);
    int16_t z = s16_le(&buf[4]);

    out->status = st;
    out->x_raw = x;
    out->y_raw = y;
    out->z_raw = z;

    // Rough conversion for early testing
    out->x_uT = (float)x * QMC_2G_UT_PER_LSB;
    out->y_uT = (float)y * QMC_2G_UT_PER_LSB;
    out->z_uT = (float)z * QMC_2G_UT_PER_LSB;

    return ESP_OK;
}