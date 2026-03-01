#include "i2c_bus.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "i2c_bus";

esp_err_t ms_i2c_init(void) {
  static bool initialized = false;
  if (initialized)
    return ESP_OK;

  i2c_config_t conf = {
      .mode = I2C_MODE_MASTER,
      .sda_io_num = MS_I2C_SDA_GPIO,
      .scl_io_num = MS_I2C_SCL_GPIO,
      .sda_pullup_en = GPIO_PULLUP_ENABLE,
      .scl_pullup_en = GPIO_PULLUP_ENABLE,
      .master.clk_speed = MS_I2C_FREQ_HZ,
      .clk_flags = 0,
  };

  ESP_LOGI(TAG, "Initializing I2C on SDA=%d SCL=%d", MS_I2C_SDA_GPIO,
           MS_I2C_SCL_GPIO);

  ESP_ERROR_CHECK(i2c_param_config(MS_I2C_PORT, &conf));
  esp_err_t ret = i2c_driver_install(MS_I2C_PORT, conf.mode, 0, 0, 0);

  if (ret == ESP_OK) {
    // Set bus timeout to prevent WDT crashes if sensors hang
    // ~13ms at 80MHz APB (2^20 - 1 cycles)
    i2c_set_timeout(MS_I2C_PORT, 1048575);
    initialized = true;
  }
  return ret;
}

esp_err_t ms_i2c_read_u8(uint8_t addr, uint8_t reg, uint8_t *out) {
  return ms_i2c_read(addr, reg, out, 1);
}

esp_err_t ms_i2c_read(uint8_t addr, uint8_t reg, uint8_t *buf, uint8_t len) {
  if (!buf || len == 0)
    return ESP_ERR_INVALID_ARG;

  return i2c_master_write_read_device(MS_I2C_PORT, addr, &reg, 1, buf, len,
                                      pdMS_TO_TICKS(100));
}

esp_err_t ms_i2c_write_u8(uint8_t addr, uint8_t reg, uint8_t val) {
  uint8_t data[2] = {reg, val};
  return i2c_master_write_to_device(MS_I2C_PORT, addr, data, sizeof(data),
                                    pdMS_TO_TICKS(100));
}

esp_err_t ms_i2c_write(uint8_t addr, uint8_t reg, const uint8_t *buf,
                       uint8_t len) {
  if (!buf || len == 0)
    return ESP_ERR_INVALID_ARG;

  // len is tiny for your use cases (INA219 uses 2 bytes)
  uint8_t data[1 + 16];
  if (len > 16)
    return ESP_ERR_INVALID_ARG;

  data[0] = reg;
  for (uint8_t i = 0; i < len; i++) {
    data[1 + i] = buf[i];
  }

  return i2c_master_write_to_device(MS_I2C_PORT, addr, data, (size_t)(1 + len),
                                    pdMS_TO_TICKS(100));
}