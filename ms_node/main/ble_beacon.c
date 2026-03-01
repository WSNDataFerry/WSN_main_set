#include "ble_beacon.h"
#include "sensor_config.h"

#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include <inttypes.h>
#include <string.h>

static const char *TAG = "ble_beacon";

static bool s_ready = false;
static bool s_adv_pending_start = false;
static bool s_adv_data_ready = false;
static bool s_scan_rsp_ready = false;
static uint8_t s_last_batt = 255;
static pme_mode_t s_last_mode = PME_MODE_NORMAL;
static uint32_t s_beacon_base_interval_ms = 1000; // Configurable base interval
static uint32_t s_beacon_offset_ms = 0; // MAC-based collision avoidance

static esp_ble_adv_params_t s_adv_params = {
    .adv_int_min = 0x0640, // default ~1s
    .adv_int_max = 0x0640,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static void ble_beacon_start(void);

static void gap_cb(esp_gap_ble_cb_event_t event,
                   esp_ble_gap_cb_param_t *param) {
  switch (event) {
  case ESP_GAP_BLE_ADV_DATA_RAW_SET_COMPLETE_EVT:
    s_adv_data_ready = true;
    ESP_LOGI(TAG, "adv data set complete");
    break;
  case ESP_GAP_BLE_SCAN_RSP_DATA_RAW_SET_COMPLETE_EVT:
    s_scan_rsp_ready = true;
    ESP_LOGI(TAG, "scan rsp set complete");
    break;
  case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
    if (param->adv_start_cmpl.status != ESP_BT_STATUS_SUCCESS) {
      ESP_LOGE(TAG, "adv start failed: %d", param->adv_start_cmpl.status);
    } else {
      ESP_LOGI(TAG, "adv started (interval min=%u max=%u)",
               s_adv_params.adv_int_min, s_adv_params.adv_int_max);
    }
    break;
  default:
    break;
  }

  if (s_adv_pending_start && s_adv_data_ready && s_scan_rsp_ready) {
    s_adv_pending_start = false;
    ble_beacon_start();
  }
}

static esp_err_t ensure_bt_ready(void) {
  if (s_ready)
    return ESP_OK;

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
      err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  if (err != ESP_OK)
    return err;

  esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));
  ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
  ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));
  ESP_ERROR_CHECK(esp_bluedroid_init());
  ESP_ERROR_CHECK(esp_bluedroid_enable());
  ESP_ERROR_CHECK(esp_ble_gap_register_callback(gap_cb));

  s_ready = true;
  return ESP_OK;
}

static void mode_to_adv_interval(pme_mode_t mode, uint16_t *min,
                                 uint16_t *max) {
  // Base interval from config, multiplied by mode factor
  // Units: 0.625 ms. Apply mode scaling: Normal 1x, PowerSave 3x, Critical 10x.
  uint32_t base_ms = s_beacon_base_interval_ms;
  uint32_t offset_ms = s_beacon_offset_ms;

  uint32_t interval_ms;
  switch (mode) {
  case PME_MODE_NORMAL:
    interval_ms = base_ms; // 1x (e.g., 1000ms)
    break;
  case PME_MODE_POWER_SAVE:
    interval_ms = base_ms * 3; // 3x (e.g., 3000ms)
    break;
  case PME_MODE_CRITICAL:
  default:
    interval_ms = base_ms * 10; // 10x (e.g., 10000ms)
    break;
  }

  // Add node-specific offset for collision avoidance
  interval_ms += offset_ms;

  // Convert ms to BLE units (0.625ms per unit)
  // interval_ms * 1000us / 625us = interval_ms * 1.6
  uint16_t interval_units = (uint16_t)((interval_ms * 1000) / 625);

  *min = interval_units;
  *max = interval_units;
}

static const char *mode_to_tag(pme_mode_t mode) {
  switch (mode) {
  case PME_MODE_POWER_SAVE:
    return "PS";
  case PME_MODE_CRITICAL:
    return "CR";
  case PME_MODE_NORMAL:
  default:
    return "NM";
  }
}

static void ble_beacon_start(void) {
  esp_err_t err = esp_ble_gap_start_advertising(&s_adv_params);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "start adv failed: %s", esp_err_to_name(err));
  }
}

esp_err_t ble_beacon_init(void) {
  esp_err_t ret = ensure_bt_ready();
  if (ret != ESP_OK)
    return ret;

  // Load beacon configuration from NVS
  sensor_config_t cfg;
  if (sensor_config_get(&cfg) == ESP_OK) {
    s_beacon_base_interval_ms = cfg.beacon_interval_ms;
    s_beacon_offset_ms = cfg.beacon_offset_ms;
  }

  // If offset is 0, auto-calculate from MAC for collision avoidance
  if (s_beacon_offset_ms == 0) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    // Spread nodes across 0-990ms based on last byte of MAC
    s_beacon_offset_ms = (mac[5] * 10) % 1000;
    ESP_LOGI(TAG,
             "Auto-calculated beacon offset: %" PRIu32 "ms (MAC byte: 0x%02X)",
             s_beacon_offset_ms, mac[5]);
  }

  ESP_LOGI(TAG, "Beacon config: base=%" PRIu32 "ms, offset=%" PRIu32 "ms",
           s_beacon_base_interval_ms, s_beacon_offset_ms);

  // Start advertising immediately with default values (batt=0, mode=Normal)
  // Payload will update when first ble_beacon_update() is called
  ble_beacon_update(0, PME_MODE_NORMAL);

  return ESP_OK;
}

void ble_beacon_update(uint8_t batt_pct, pme_mode_t mode) {
  static bool first_update = true;

  if (batt_pct > 100)
    batt_pct = 100;
  if (ensure_bt_ready() != ESP_OK)
    return;

  // Check if anything actually changed
  bool mode_changed = (mode != s_last_mode);

  // Only update beacon when MODE changes (affects interval)
  // Battery % changes are ignored to keep advertising continuous
  if (!first_update && !mode_changed) {
    return; // Nothing important changed, keep advertising as-is
  }

  bool batt_changed = (batt_pct != s_last_batt);

  // Update state tracking
  s_last_batt = batt_pct;
  s_last_mode = mode;

  uint16_t min_int = 1600, max_int = 1600;
  mode_to_adv_interval(mode, &min_int, &max_int);

  bool interval_changed = (s_adv_params.adv_int_min != min_int);

  s_adv_params.adv_int_min = min_int;
  s_adv_params.adv_int_max = max_int;

  ESP_LOGI(TAG,
           "update: batt=%u mode=%u adv_int=%.2fs (batt_chg=%d mode_chg=%d "
           "int_chg=%d first=%d)",
           batt_pct, (unsigned)mode, (double)min_int * 0.000625, batt_changed,
           mode_changed, interval_changed, first_update);

  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);

  // ---- Advertising packet (keeps MAC tail + batt + mode) ----
  uint8_t adv[3 + 2 + 8] = {0};
  // Flags
  adv[0] = 0x02;
  adv[1] = 0x01;
  adv[2] = 0x06;
  // Manufacturer data length (1 type + 7 bytes payload)
  adv[3] = 1 + 7;
  adv[4] = 0xFF;
  adv[5] = 0xE5;
  adv[6] = 0x02;          // company ID: Espressif (0x02E5)
  adv[7] = 0x01;          // version
  adv[8] = batt_pct;      // battery %
  adv[9] = (uint8_t)mode; // mode
  adv[10] = mac[3];       // short ID
  adv[11] = mac[4];
  adv[12] = mac[5];

  esp_err_t err = esp_ble_gap_config_adv_data_raw(adv, sizeof(adv));
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "set adv data failed: %s", esp_err_to_name(err));
    return;
  }
  ESP_LOGI(TAG, "configured adv data");

  // ---- Scan response adds a human-readable name with batt+mode ----
  const char *mode_tag = mode_to_tag(mode);

  char name[24];
  int name_len = snprintf(name, sizeof(name), "MSN-B%03u-%s-%02X%02X%02X",
                          (unsigned)batt_pct, mode_tag, mac[3], mac[4], mac[5]);
  if (name_len < 0)
    name_len = 0;
  if (name_len > (int)sizeof(name))
    name_len = sizeof(name);

  uint8_t scan_rsp[2 + 20] = {0};
  if (name_len > 0 && name_len <= 20) {
    scan_rsp[0] = (uint8_t)(1 + name_len); // length includes type
    scan_rsp[1] = 0x09;                    // Complete Local Name
    memcpy(&scan_rsp[2], name, (size_t)name_len);

    err = esp_ble_gap_config_scan_rsp_data_raw(scan_rsp,
                                               (uint32_t)(name_len + 2));
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "set scan rsp failed: %s", esp_err_to_name(err));
      return;
    }
    ESP_LOGI(TAG, "configured scan rsp");
  }

  // Now restart advertising with new data and params
  // For first boot, start fresh. For updates, stop then start.
  if (!first_update) {
    esp_err_t stop_rc = esp_ble_gap_stop_advertising();
    // FIX: Handle ESP_ERR_INVALID_STATE (rc=2) as success - advertising already stopped
    // Also handle ESP_OK as success. Only warn on other errors.
    if (stop_rc != ESP_OK && stop_rc != ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "stop adv rc=%s", esp_err_to_name(stop_rc));
    }
    // Only delay if we actually stopped something
    if (stop_rc == ESP_OK) {
      vTaskDelay(pdMS_TO_TICKS(50)); // Brief delay for clean stop
      ESP_LOGI(TAG, "stopped advertising");
    } else if (stop_rc == ESP_ERR_INVALID_STATE) {
      ESP_LOGD(TAG, "advertising already stopped, proceeding");
    }
  }

  err = esp_ble_gap_start_advertising(&s_adv_params);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "start adv failed: %s", esp_err_to_name(err));
  } else {
    ESP_LOGI(TAG, "restarted advertising");
  }

  first_update = false;
}
