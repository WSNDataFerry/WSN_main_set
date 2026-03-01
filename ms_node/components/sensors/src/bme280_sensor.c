#include "bme280_sensor.h"
#include "i2c_bus.h"
#include "sensors.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"

static const char *TAG = "sensors";

/* BME280 registers */
#define REG_ID          0xD0
#define REG_RESET       0xE0
#define REG_STATUS      0xF3
#define REG_CTRL_HUM    0xF2
#define REG_CTRL_MEAS   0xF4
#define REG_CONFIG      0xF5
#define REG_PRESS_MSB   0xF7  // start of data block

/* Calibration registers */
#define REG_CALIB_00    0x88
#define REG_CALIB_26    0xE1
#define REG_CALIB_H1    0xA1

/* Expected chip id */
#define BME280_CHIP_ID  0x60

typedef struct {
    uint16_t dig_T1;
    int16_t  dig_T2;
    int16_t  dig_T3;

    uint16_t dig_P1;
    int16_t  dig_P2;
    int16_t  dig_P3;
    int16_t  dig_P4;
    int16_t  dig_P5;
    int16_t  dig_P6;
    int16_t  dig_P7;
    int16_t  dig_P8;
    int16_t  dig_P9;

    uint8_t  dig_H1;
    int16_t  dig_H2;
    uint8_t  dig_H3;
    int16_t  dig_H4;
    int16_t  dig_H5;
    int8_t   dig_H6;
} bme280_calib_t;

static bme280_calib_t calib;
static int32_t t_fine = 0;
static bool bme_inited = false;

static uint16_t u16_le(const uint8_t *p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }
static int16_t  s16_le(const uint8_t *p) { return (int16_t)u16_le(p); }

static esp_err_t read_calibration(void)
{
    uint8_t buf1[24] = {0};
    uint8_t h1 = 0;
    uint8_t buf2[7] = {0};

    esp_err_t ret = ms_i2c_read(ADDR_BME280, REG_CALIB_00, buf1, sizeof(buf1));
    if (ret != ESP_OK) return ret;

    ret = ms_i2c_read_u8(ADDR_BME280, REG_CALIB_H1, &h1);
    if (ret != ESP_OK) return ret;

    ret = ms_i2c_read(ADDR_BME280, REG_CALIB_26, buf2, sizeof(buf2));
    if (ret != ESP_OK) return ret;

    calib.dig_T1 = u16_le(&buf1[0]);
    calib.dig_T2 = s16_le(&buf1[2]);
    calib.dig_T3 = s16_le(&buf1[4]);

    calib.dig_P1 = u16_le(&buf1[6]);
    calib.dig_P2 = s16_le(&buf1[8]);
    calib.dig_P3 = s16_le(&buf1[10]);
    calib.dig_P4 = s16_le(&buf1[12]);
    calib.dig_P5 = s16_le(&buf1[14]);
    calib.dig_P6 = s16_le(&buf1[16]);
    calib.dig_P7 = s16_le(&buf1[18]);
    calib.dig_P8 = s16_le(&buf1[20]);
    calib.dig_P9 = s16_le(&buf1[22]);

    calib.dig_H1 = h1;
    calib.dig_H2 = s16_le(&buf2[0]);
    calib.dig_H3 = buf2[2];

    // H4 and H5 are packed oddly
    calib.dig_H4 = (int16_t)((buf2[3] << 4) | (buf2[4] & 0x0F));
    calib.dig_H5 = (int16_t)((buf2[5] << 4) | (buf2[4] >> 4));
    calib.dig_H6 = (int8_t)buf2[6];

    return ESP_OK;
}

static int32_t compensate_T(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)calib.dig_T1 << 1))) * ((int32_t)calib.dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - ((int32_t)calib.dig_T1)) * ((adc_T >> 4) - ((int32_t)calib.dig_T1))) >> 12) *
            ((int32_t)calib.dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8; // temperature in 0.01 °C
    return T;
}

static uint32_t compensate_P(int32_t adc_P)
{
    int64_t var1, var2, p;

    var1 = ((int64_t)t_fine) - 128000;
    var2 = var1 * var1 * (int64_t)calib.dig_P6;
    var2 = var2 + ((var1 * (int64_t)calib.dig_P5) << 17);
    var2 = var2 + (((int64_t)calib.dig_P4) << 35);
    var1 = ((var1 * var1 * (int64_t)calib.dig_P3) >> 8) + ((var1 * (int64_t)calib.dig_P2) << 12);
    var1 = (((((int64_t)1) << 47) + var1) * ((int64_t)calib.dig_P1)) >> 33;

    if (var1 == 0) return 0;

    p = 1048576 - adc_P;
    p = (((p << 31) - var2) * 3125) / var1;
    var1 = (((int64_t)calib.dig_P9) * (p >> 13) * (p >> 13)) >> 25;
    var2 = (((int64_t)calib.dig_P8) * p) >> 19;
    p = ((p + var1 + var2) >> 8) + (((int64_t)calib.dig_P7) << 4);

    return (uint32_t)p; // pressure in Q24.8 Pa
}

static uint32_t compensate_H(int32_t adc_H)
{
    int32_t v_x1;

    v_x1 = (t_fine - ((int32_t)76800));
    v_x1 = (((((adc_H << 14) - (((int32_t)calib.dig_H4) << 20) -
               (((int32_t)calib.dig_H5) * v_x1)) + ((int32_t)16384)) >> 15) *
            (((((((v_x1 * ((int32_t)calib.dig_H6)) >> 10) *
                 (((v_x1 * ((int32_t)calib.dig_H3)) >> 11) + ((int32_t)32768))) >> 10) +
               ((int32_t)2097152)) * ((int32_t)calib.dig_H2) + 8192) >> 14));

    v_x1 = (v_x1 - (((((v_x1 >> 15) * (v_x1 >> 15)) >> 7) * ((int32_t)calib.dig_H1)) >> 4));

    if (v_x1 < 0) v_x1 = 0;
    if (v_x1 > 419430400) v_x1 = 419430400;

    return (uint32_t)(v_x1 >> 12); // humidity in Q22.10 (%RH * 1024)
}

esp_err_t bme280_raw_check(void)
{
    uint8_t id = 0;
    esp_err_t ret = ms_i2c_read_u8(ADDR_BME280, REG_ID, &id);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "BME280 chip id: 0x%02X", id);
    } else {
        ESP_LOGE(TAG, "BME280 raw check failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t bme280_init(void)
{
    uint8_t id = 0;

    esp_err_t ret = ms_i2c_read_u8(ADDR_BME280, REG_ID, &id);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "BME280 not responding on 0x%02X", ADDR_BME280);
        return ESP_ERR_NOT_FOUND;
    }

    if (id != BME280_CHIP_ID) {
        ESP_LOGW(TAG, "Unexpected BME280 chip id: 0x%02X", id);
        return ESP_ERR_INVALID_RESPONSE;
    }

    ret = read_calibration();
    if (ret != ESP_OK) return ret;

    // Humidity oversampling x1
    ret = ms_i2c_write_u8(ADDR_BME280, REG_CTRL_HUM, 0x01);
    if (ret != ESP_OK) return ret;

    // Temp oversampling x1, Pressure oversampling x1, mode sleep
    ret = ms_i2c_write_u8(ADDR_BME280, REG_CTRL_MEAS, 0x24);
    if (ret != ESP_OK) return ret;

    // config: filter off, standby default
    ret = ms_i2c_write_u8(ADDR_BME280, REG_CONFIG, 0x00);
    if (ret != ESP_OK) return ret;

    bme_inited = true;
    ESP_LOGI(TAG, "BME280 init OK (id=0x%02X)", id);
    return ESP_OK;
}

esp_err_t bme280_read(bme280_reading_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    if (!bme_inited) {
        esp_err_t r = bme280_init();
        if (r != ESP_OK) return r;
    }

    // Trigger forced measurement: same osrs settings + mode=01
    esp_err_t ret = ms_i2c_write_u8(ADDR_BME280, REG_CTRL_MEAS, 0x25);
    if (ret != ESP_OK) return ret;

    // Wait for measurement to finish
    for (int i = 0; i < 20; i++) {
        uint8_t status = 0;
        ret = ms_i2c_read_u8(ADDR_BME280, REG_STATUS, &status);
        if (ret != ESP_OK) return ret;

        if ((status & 0x08) == 0) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    uint8_t data[8] = {0};
    ret = ms_i2c_read(ADDR_BME280, REG_PRESS_MSB, data, sizeof(data));
    if (ret != ESP_OK) return ret;

    int32_t adc_P = (int32_t)((data[0] << 12) | (data[1] << 4) | (data[2] >> 4));
    int32_t adc_T = (int32_t)((data[3] << 12) | (data[4] << 4) | (data[5] >> 4));
    int32_t adc_H = (int32_t)((data[6] << 8)  |  data[7]);

    int32_t t_x100 = compensate_T(adc_T);
    uint32_t p_q24_8 = compensate_P(adc_P);
    uint32_t h_q22_10 = compensate_H(adc_H);

    out->temperature_c = (float)t_x100 / 100.0f;

    float pressure_pa = (float)p_q24_8 / 256.0f;
    out->pressure_hpa = pressure_pa / 100.0f;

    out->humidity_pct = (float)h_q22_10 / 1024.0f;

    return ESP_OK;
}