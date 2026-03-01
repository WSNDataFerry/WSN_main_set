#include "ble_gatt_service.h"
#include "logger.h"
#include "sensor_config.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatts_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ble_gatt";

// Custom service UUID: 12340000-1234-1234-1234-123456789abc
static const uint8_t SERVICE_UUID[16] = {0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                                         0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                                         0x00, 0x00, 0x34, 0x12};

// Characteristics:
// - Time Sync (write): 12340001-...
// - Data Request (read): 12340002-...
// - Node Info (read): 12340003-...

static const uint8_t CHAR_TIME_UUID[16] = {0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                                           0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                                           0x01, 0x00, 0x34, 0x12};

static const uint8_t CHAR_DATA_UUID[16] = {0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                                           0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                                           0x02, 0x00, 0x34, 0x12};

static const uint8_t CHAR_INFO_UUID[16] = {0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                                           0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                                           0x03, 0x00, 0x34, 0x12};

// Config Update (write): 12340004-...
static const uint8_t CHAR_CONFIG_UUID[16] = {0xBC, 0x9A, 0x78, 0x56, 0x34, 0x12,
                                             0x34, 0x12, 0x34, 0x12, 0x34, 0x12,
                                             0x04, 0x00, 0x34, 0x12};

static bool s_service_started = false;
static uint16_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_conn_id = 0xFFFF;
static uint16_t s_service_handle = 0;
static uint16_t s_char_time_handle = 0;
static uint16_t s_char_data_handle = 0;
static uint16_t s_char_info_handle = 0;
static uint16_t s_char_config_handle = 0;

// Simple authentication: require specific passkey (for demo)
#define AUTH_PASSKEY 123456

// Shared config parser: "key=value" -> update NVS and runtime (used by BLE and serial)
esp_err_t ble_gatt_service_apply_config(const char *key_value) {
  if (key_value == NULL) return ESP_ERR_INVALID_ARG;
  char cmd[128];
  size_t len = strlen(key_value);
  if (len >= sizeof(cmd)) return ESP_ERR_INVALID_ARG;
  memcpy(cmd, key_value, len + 1);

  char *eq = strchr(cmd, '=');
  if (!eq) {
    ESP_LOGW(TAG, "Invalid config format (expected key=value)");
    return ESP_ERR_INVALID_ARG;
  }
  *eq = '\0';
  const char *key = cmd;
  const char *value = eq + 1;

  sensor_config_t cfg;
  sensor_config_get(&cfg);

  if (strcmp(key, "audio_interval_ms") == 0) {
    cfg.audio_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "env_sensor_interval_ms") == 0) {
    cfg.env_sensor_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "gas_sensor_interval_ms") == 0) {
    cfg.gas_sensor_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "mag_sensor_interval_ms") == 0) {
    cfg.mag_sensor_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "power_sensor_interval_ms") == 0) {
    cfg.power_sensor_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "inmp441_enabled") == 0) {
    cfg.inmp441_enabled = (atoi(value) != 0);
  } else if (strcmp(key, "bme280_enabled") == 0) {
    cfg.bme280_enabled = (atoi(value) != 0);
  } else if (strcmp(key, "ens160_enabled") == 0) {
    cfg.ens160_enabled = (atoi(value) != 0);
  } else if (strcmp(key, "gy271_enabled") == 0) {
    cfg.gy271_enabled = (atoi(value) != 0);
  } else if (strcmp(key, "audio_sample_rate") == 0) {
    cfg.audio_sample_rate = (uint32_t)atoi(value);
  } else if (strcmp(key, "audio_duration_ms") == 0) {
    cfg.audio_duration_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "beacon_interval_ms") == 0) {
    cfg.beacon_interval_ms = (uint32_t)atoi(value);
  } else if (strcmp(key, "beacon_offset_ms") == 0) {
    cfg.beacon_offset_ms = (uint32_t)atoi(value);
  } else {
    ESP_LOGW(TAG, "Unknown config key: %s", key);
    return ESP_ERR_NOT_FOUND;
  }

  sensor_config_update(&cfg);
  sensor_config_save(&cfg);
  ESP_LOGI(TAG, "Config updated: %s=%s", key, value);
  return ESP_OK;
}

static void gatts_event_handler(esp_gatts_cb_event_t event,
                                esp_gatt_if_t gatts_if,
                                esp_ble_gatts_cb_param_t *param) {
  switch (event) {
  case ESP_GATTS_REG_EVT:
    ESP_LOGI(TAG, "GATTS register app_id=%u status=%d", param->reg.app_id,
             param->reg.status);
    if (param->reg.status == ESP_GATT_OK) {
      s_gatts_if = gatts_if;

      // Create service with 128-bit UUID
      esp_gatt_srvc_id_t service_id = {
          .is_primary = true,
          .id = {.uuid = {.len = ESP_UUID_LEN_128, .uuid = {.uuid128 = {0}}},
                 .inst_id = 0}};
      memcpy(service_id.id.uuid.uuid.uuid128, SERVICE_UUID, 16);
      esp_ble_gatts_create_service(gatts_if, &service_id, 10);
    }
    break;

  case ESP_GATTS_CREATE_EVT:
    ESP_LOGI(TAG, "Service created: handle=%u status=%d",
             param->create.service_handle, param->create.status);
    s_service_handle = param->create.service_handle;
    esp_ble_gatts_start_service(s_service_handle);

    // Add time characteristic
    esp_bt_uuid_t time_uuid = {.len = ESP_UUID_LEN_128,
                               .uuid = {.uuid128 = {0}}};
    memcpy(time_uuid.uuid.uuid128, CHAR_TIME_UUID, 16);
    esp_ble_gatts_add_char(s_service_handle, &time_uuid, ESP_GATT_PERM_WRITE,
                           ESP_GATT_CHAR_PROP_BIT_WRITE, NULL, NULL);
    break;

  case ESP_GATTS_ADD_CHAR_EVT:
    ESP_LOGI(TAG, "Characteristic added: handle=%u",
             param->add_char.attr_handle);

    if (s_char_time_handle == 0) {
      s_char_time_handle = param->add_char.attr_handle;

      // Add data characteristic
      esp_bt_uuid_t data_uuid = {.len = ESP_UUID_LEN_128,
                                 .uuid = {.uuid128 = {0}}};
      memcpy(data_uuid.uuid.uuid128, CHAR_DATA_UUID, 16);
      esp_ble_gatts_add_char(s_service_handle, &data_uuid, ESP_GATT_PERM_READ,
                             ESP_GATT_CHAR_PROP_BIT_READ, NULL, NULL);
    } else if (s_char_data_handle == 0) {
      s_char_data_handle = param->add_char.attr_handle;

      // Add info characteristic
      esp_bt_uuid_t info_uuid = {.len = ESP_UUID_LEN_128,
                                 .uuid = {.uuid128 = {0}}};
      memcpy(info_uuid.uuid.uuid128, CHAR_INFO_UUID, 16);
      esp_ble_gatts_add_char(s_service_handle, &info_uuid, ESP_GATT_PERM_READ,
                             ESP_GATT_CHAR_PROP_BIT_READ, NULL, NULL);
    } else if (s_char_info_handle == 0) {
      s_char_info_handle = param->add_char.attr_handle;

      // Add config characteristic
      esp_bt_uuid_t config_uuid = {.len = ESP_UUID_LEN_128,
                                   .uuid = {.uuid128 = {0}}};
      memcpy(config_uuid.uuid.uuid128, CHAR_CONFIG_UUID, 16);
      esp_ble_gatts_add_char(s_service_handle, &config_uuid,
                             ESP_GATT_PERM_WRITE, ESP_GATT_CHAR_PROP_BIT_WRITE,
                             NULL, NULL);
    } else {
      s_char_config_handle = param->add_char.attr_handle;
    }
    break;

  case ESP_GATTS_CONNECT_EVT:
    ESP_LOGI(TAG, "Client connected: conn_id=%u", param->connect.conn_id);
    s_conn_id = param->connect.conn_id;

    // Set security requirements (pairing)
    esp_ble_set_encryption(param->connect.remote_bda, ESP_BLE_SEC_ENCRYPT);
    break;

  case ESP_GATTS_DISCONNECT_EVT:
    ESP_LOGI(TAG, "Client disconnected");
    s_conn_id = 0xFFFF;
    break;

  case ESP_GATTS_WRITE_EVT:
    if (param->write.handle == s_char_time_handle) {
      // Time sync characteristic written
      if (param->write.len == 4) {
        uint32_t timestamp;
        memcpy(&timestamp, param->write.value, 4);
        ESP_LOGI(TAG, "Time sync request: %" PRIu32, timestamp);
        logger_set_time(timestamp);

        esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                    param->write.trans_id, ESP_GATT_OK, NULL);
      } else {
        ESP_LOGW(TAG, "Invalid time sync length: %u", param->write.len);
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                    param->write.trans_id,
                                    ESP_GATT_INVALID_ATTR_LEN, NULL);
      }
    } else if (param->write.handle == s_char_config_handle) {
      // Config update: simple key=value format (uses shared apply_config)
      char cmd[128];
      if (param->write.len < sizeof(cmd)) {
        memcpy(cmd, param->write.value, param->write.len);
        cmd[param->write.len] = '\0';
        esp_err_t err = ble_gatt_service_apply_config(cmd);
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                    param->write.trans_id,
                                    (err == ESP_OK) ? ESP_GATT_OK : ESP_GATT_ERROR,
                                    NULL);
      } else {
        ESP_LOGW(TAG, "Config command too long");
        esp_ble_gatts_send_response(gatts_if, param->write.conn_id,
                                    param->write.trans_id,
                                    ESP_GATT_INVALID_ATTR_LEN, NULL);
      }
    }
    break;

  case ESP_GATTS_READ_EVT:
    if (param->read.handle == s_char_info_handle) {
      // Node info: return node ID + storage usage
      char info[64];
      char node_id[18];
      size_t used = 0, total = 0;

      logger_get_node_id(node_id, sizeof(node_id));
      logger_get_storage_usage(&used, &total);

      snprintf(info, sizeof(info), "ID:%s,Used:%u,Total:%u", node_id,
               (unsigned)used, (unsigned)total);

      esp_gatt_rsp_t rsp = {0};
      rsp.attr_value.len = strlen(info);
      memcpy(rsp.attr_value.value, info, rsp.attr_value.len);

      esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                  param->read.trans_id, ESP_GATT_OK, &rsp);
      ESP_LOGI(TAG, "Sent node info: %s", info);
    } else if (param->read.handle == s_char_data_handle) {
      // Data request: return file size for now (actual data transfer needs
      // streaming)
      char data[32];
      size_t file_size = logger_get_file_size();

      snprintf(data, sizeof(data), "FileSize:%u", (unsigned)file_size);

      esp_gatt_rsp_t rsp = {0};
      rsp.attr_value.len = strlen(data);
      memcpy(rsp.attr_value.value, data, rsp.attr_value.len);

      esp_ble_gatts_send_response(gatts_if, param->read.conn_id,
                                  param->read.trans_id, ESP_GATT_OK, &rsp);
      ESP_LOGI(TAG, "Sent data info: %s", data);
    }
    break;

  default:
    break;
  }
}

static void gap_event_handler(esp_gap_ble_cb_event_t event,
                              esp_ble_gap_cb_param_t *param) {
  switch (event) {
  case ESP_GAP_BLE_AUTH_CMPL_EVT:
    ESP_LOGI(TAG, "Authentication complete: success=%d",
             param->ble_security.auth_cmpl.success);
    break;

  case ESP_GAP_BLE_PASSKEY_NOTIF_EVT:
    ESP_LOGI(TAG, "Passkey notification: %" PRIu32,
             param->ble_security.key_notif.passkey);
    break;

  case ESP_GAP_BLE_PASSKEY_REQ_EVT:
    ESP_LOGI(TAG, "Passkey request");
    esp_ble_passkey_reply(param->ble_security.ble_req.bd_addr, true,
                          AUTH_PASSKEY);
    break;

  default:
    break;
  }
}

esp_err_t ble_gatt_service_init(void) {
  if (s_service_started)
    return ESP_OK;

  // Register GAP callback for security
  ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_event_handler));

  // Set security IO capability (display only for passkey)
  esp_ble_io_cap_t iocap = ESP_IO_CAP_NONE;
  esp_ble_gap_set_security_param(ESP_BLE_SM_IOCAP_MODE, &iocap,
                                 sizeof(uint8_t));

  // Register GATT server
  ESP_ERROR_CHECK(esp_ble_gatts_register_callback(gatts_event_handler));
  ESP_ERROR_CHECK(esp_ble_gatts_app_register(0));

  s_service_started = true;
  ESP_LOGI(TAG, "GATT service initialized");
  return ESP_OK;
}

esp_err_t ble_gatt_service_start(void) {
  if (!s_service_started)
    return ESP_ERR_INVALID_STATE;

  // Start advertising (this would typically set connectable advertising params)
  ESP_LOGI(
      TAG,
      "GATT service start requested (advertising handled by beacon module)");
  return ESP_OK;
}

esp_err_t ble_gatt_service_stop(void) {
  if (!s_service_started)
    return ESP_ERR_INVALID_STATE;

  ESP_LOGI(TAG, "GATT service stop requested");
  return ESP_OK;
}
