#include "sensor_config.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>

static const char *TAG = "sensor_config";
static const char *NVS_NAMESPACE = "sensor_cfg";

static sensor_config_t s_config = {0};
static bool s_initialized = false;

sensor_config_t sensor_config_get_default(void) {
  return (sensor_config_t){
      .bme280_enabled = true,
      .aht21_enabled = true,
      .ens160_enabled = true,
      .gy271_enabled = true,
      .ina219_enabled = true,
      .inmp441_enabled = false, // Disabled by default (high power)

      .env_sensor_interval_ms = 60000,   // 1 minute (slow-changing)
      .gas_sensor_interval_ms = 120000,  // 2 minutes (warmup consideration)
      .mag_sensor_interval_ms = 60000,   // 1 minute
      .power_sensor_interval_ms = 10000, // 10 seconds (monitor battery closely)
      .audio_interval_ms = 300000,       // 5 minutes (high power consumption)

      .audio_sample_rate = 16000,
      .audio_duration_ms = 1000, // 1 second clips

      .beacon_interval_ms = 1000, // 1 second base interval
      .beacon_offset_ms = 0,      // Auto-calculated from MAC in ble_beacon_init

      // Reasonable environmental ranges for trust filtering
      .temp_min_c = -40.0f,
      .temp_max_c = 85.0f,
      .humidity_min_pct = 0.0f,
      .humidity_max_pct = 100.0f,
      .pressure_min_hpa = 300.0f,
      .pressure_max_hpa = 1100.0f,
  };
}

esp_err_t sensor_config_load(sensor_config_t *config) {
  if (!config)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);

  if (ret != ESP_OK) {
    if (ret == ESP_ERR_NVS_NOT_FOUND) {
      ESP_LOGI(TAG, "No saved config found, using defaults (first boot)");
    } else {
      ESP_LOGW(TAG, "NVS open failed, using defaults: %s",
               esp_err_to_name(ret));
    }
    *config = sensor_config_get_default();
    return ESP_OK;
  }

  // Load configuration (use defaults for missing keys)
  sensor_config_t defaults = sensor_config_get_default();

  uint8_t u8;
  uint32_t u32;

#define LOAD_BOOL(key, field)                                                  \
  if (nvs_get_u8(handle, key, &u8) == ESP_OK)                                  \
    config->field = u8;                                                        \
  else                                                                         \
    config->field = defaults.field

#define LOAD_U32(key, field)                                                   \
  if (nvs_get_u32(handle, key, &u32) == ESP_OK)                                \
    config->field = u32;                                                       \
  else                                                                         \
    config->field = defaults.field

  LOAD_BOOL("bme280_en", bme280_enabled);
  LOAD_BOOL("aht21_en", aht21_enabled);
  LOAD_BOOL("ens160_en", ens160_enabled);
  LOAD_BOOL("gy271_en", gy271_enabled);
  LOAD_BOOL("ina219_en", ina219_enabled);
  LOAD_BOOL("inmp441_en", inmp441_enabled);

  LOAD_U32("env_int", env_sensor_interval_ms);
  LOAD_U32("gas_int", gas_sensor_interval_ms);
  LOAD_U32("mag_int", mag_sensor_interval_ms);
  LOAD_U32("pwr_int", power_sensor_interval_ms);
  LOAD_U32("aud_int", audio_interval_ms);
  LOAD_U32("aud_rate", audio_sample_rate);
  LOAD_U32("aud_dur", audio_duration_ms);
  LOAD_U32("bcn_int", beacon_interval_ms);
  LOAD_U32("bcn_off", beacon_offset_ms);

  nvs_close(handle);

  ESP_LOGI(TAG, "Configuration loaded from NVS");
  return ESP_OK;
}

esp_err_t sensor_config_save(const sensor_config_t *config) {
  if (!config)
    return ESP_ERR_INVALID_ARG;

  nvs_handle_t handle;
  esp_err_t ret = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);

  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "NVS open for write failed: %s", esp_err_to_name(ret));
    return ret;
  }

#define SAVE_BOOL(key, field) nvs_set_u8(handle, key, config->field)
#define SAVE_U32(key, field) nvs_set_u32(handle, key, config->field)

  SAVE_BOOL("bme280_en", bme280_enabled);
  SAVE_BOOL("aht21_en", aht21_enabled);
  SAVE_BOOL("ens160_en", ens160_enabled);
  SAVE_BOOL("gy271_en", gy271_enabled);
  SAVE_BOOL("ina219_en", ina219_enabled);
  SAVE_BOOL("inmp441_en", inmp441_enabled);

  SAVE_U32("env_int", env_sensor_interval_ms);
  SAVE_U32("gas_int", gas_sensor_interval_ms);
  SAVE_U32("mag_int", mag_sensor_interval_ms);
  SAVE_U32("pwr_int", power_sensor_interval_ms);
  SAVE_U32("aud_int", audio_interval_ms);
  SAVE_U32("aud_rate", audio_sample_rate);
  SAVE_U32("aud_dur", audio_duration_ms);
  SAVE_U32("bcn_int", beacon_interval_ms);
  SAVE_U32("bcn_off", beacon_offset_ms);

  ret = nvs_commit(handle);
  nvs_close(handle);

  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "Configuration saved to NVS");
  } else {
    ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(ret));
  }

  return ret;
}

esp_err_t sensor_config_update(const sensor_config_t *config) {
  if (!config)
    return ESP_ERR_INVALID_ARG;

  memcpy(&s_config, config, sizeof(sensor_config_t));
  s_initialized = true;

  ESP_LOGI(TAG, "Active configuration updated");
  return ESP_OK;
}

esp_err_t sensor_config_get(sensor_config_t *config) {
  if (!config)
    return ESP_ERR_INVALID_ARG;

  if (!s_initialized) {
    s_config = sensor_config_get_default();
    s_initialized = true;
  }

  memcpy(config, &s_config, sizeof(sensor_config_t));
  return ESP_OK;
}

esp_err_t sensor_enable(const char *sensor_name, bool enable) {
  if (!sensor_name)
    return ESP_ERR_INVALID_ARG;

  if (!s_initialized) {
    s_config = sensor_config_get_default();
    s_initialized = true;
  }

  bool *field = NULL;

  if (strcmp(sensor_name, "bme280") == 0)
    field = &s_config.bme280_enabled;
  else if (strcmp(sensor_name, "aht21") == 0)
    field = &s_config.aht21_enabled;
  else if (strcmp(sensor_name, "ens160") == 0)
    field = &s_config.ens160_enabled;
  else if (strcmp(sensor_name, "gy271") == 0)
    field = &s_config.gy271_enabled;
  else if (strcmp(sensor_name, "ina219") == 0)
    field = &s_config.ina219_enabled;
  else if (strcmp(sensor_name, "inmp441") == 0)
    field = &s_config.inmp441_enabled;
  else
    return ESP_ERR_NOT_FOUND;

  *field = enable;
  ESP_LOGI(TAG, "Sensor %s: %s", sensor_name, enable ? "enabled" : "disabled");

  return ESP_OK;
}
