#include "esp_now_manager.h"
#include "ble_manager.h"
#include "compression.h"
#include "config.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_now.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "metrics.h"
#include "neighbor_manager.h"
#include "state_machine.h"
#include "storage_manager.h"
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "ESP_NOW";
static schedule_msg_t g_current_schedule = {0};

// Send-complete semaphore: prevents calling esp_now_send() while a previous
// frame is still in-flight. Without this, the burst-send loop + BLE callbacks
// competing on CPU0 can starve the interrupt watchdog (Guru Meditation WDT).
// Pattern: take before send, give in send_cb (called from Wi-Fi driver ISR).
static SemaphoreHandle_t s_send_sem = NULL;
// Completion semaphore and status for synchronous send operations
static SemaphoreHandle_t s_send_done = NULL;
static int s_last_send_status = -1;

// Helper: Process verified items (Recursive safe)
static void process_packet(const esp_now_recv_info_t *info, const uint8_t *data,
                           int len) {
  // 1. Check for Schedule Packet (Time Slicing Novelty)
  if (len == sizeof(schedule_msg_t)) {
    const schedule_msg_t *sched = (const schedule_msg_t *)data;
    if (sched->magic == ESP_NOW_MAGIC_SCHEDULE) {
      // Clock Sync Fix: Normalize CH's epoch to local time
      // Assumption: Message is received "now". Cycle start was (Slot *
      // Duration) ago.
      int64_t now_us = esp_timer_get_time();
      schedule_msg_t local_sched = *sched;

      int64_t time_into_cycle =
          (int64_t)sched->slot_index * sched->slot_duration_sec * 1000000LL;
      local_sched.epoch_us = now_us - time_into_cycle;

      g_current_schedule = local_sched;

      // PHASE SYNC FIX: Synchronize the local superframe phase timer to the
      // CH's superframe. The CH sets epoch_us = data_phase_start + GUARD.
      // This call resets s_phase_start_ms so that the member enters DATA phase
      // at exactly the same time as the CH, fixing the desynchronized phases.
      state_machine_sync_phase_from_epoch(sched->epoch_us);

      ESP_LOGI(TAG,
               "RX Schedule (Sync): OrigEpoch=%lld -> LocalEpoch=%lld, Slot=%d",
               sched->epoch_us, local_sched.epoch_us, sched->slot_index);
      return;
    }
  }

  // 2. Check for Sensor Data
  if (len == sizeof(sensor_payload_t)) {
    const sensor_payload_t *payload = (const sensor_payload_t *)data;
    ESP_LOGI(TAG,
             "RX Sensor Data from node_%lu: Seq=%lu, T=%.1fC, H=%.1f%%, "
             "P=%luhPa, AQI=%d, eCO2=%d, TVOC=%d, Mag=(%.1f,%.1f,%.1f)μT, "
             "Audio=%.3f",
             payload->node_id, payload->seq_num, payload->temp_c,
             payload->hum_pct, (unsigned long)payload->pressure_hpa,
             payload->aqi, payload->eco2_ppm, payload->tvoc_ppb, payload->mag_x,
             payload->mag_y, payload->mag_z, payload->audio_rms);

    // Store data for UAV (Full JSON with all sensor fields)
    char log_line[384];
    snprintf(log_line, sizeof(log_line),
             "{\"id\":%lu,\"seq\":%lu,\"mac\":\"%02x%02x%02x%02x%02x%02x\","
             "\"ts\":%llu,"
             "\"t\":%.1f,\"h\":%.1f,\"p\":%lu,\"q\":%d,\"eco2\":%d,\"tvoc\":%d,"
             "\"mx\":%.2f,\"my\":%.2f,\"mz\":%.2f,\"a\":%.3f}",
             payload->node_id, payload->seq_num, payload->mac_addr[0],
             payload->mac_addr[1], payload->mac_addr[2], payload->mac_addr[3],
             payload->mac_addr[4], payload->mac_addr[5], payload->timestamp_ms,
             payload->temp_c, payload->hum_pct,
             (unsigned long)payload->pressure_hpa, payload->aqi,
             payload->eco2_ppm, payload->tvoc_ppb, payload->mag_x,
             payload->mag_y, payload->mag_z, payload->audio_rms);
    storage_manager_write_compressed(log_line, true);
    storage_manager_display_status();

    // Update neighbor trust
    neighbor_entry_t n;
    if (neighbor_manager_get_by_mac(info->src_addr, &n)) {
      neighbor_manager_update_trust(n.node_id, true);
    }
    return;
  }

  // 3. Check for Historical Data (JSON String)
  if (len > 0 && data[0] == '{') {
    char *json_data = malloc(len + 1);
    if (json_data) {
      memcpy(json_data, data, len);
      json_data[len] = '\0';
      ESP_LOGI(TAG, "RX Historical Data: %s", json_data);
      storage_manager_write_compressed(json_data, true);
      storage_manager_display_status();
      free(json_data);

      neighbor_entry_t n;
      if (neighbor_manager_get_by_mac(info->src_addr, &n)) {
        neighbor_manager_update_trust(n.node_id, true);
      }
    }
    return;
  }

  if (len < sizeof(uint32_t)) {
    ESP_LOGW(TAG, "Received invalid data length: %d", len);
    return;
  }

  // Unknown packet type
  neighbor_entry_t n;
  if (neighbor_manager_get_by_mac(info->src_addr, &n)) {
    neighbor_manager_update_trust(n.node_id, true);
  }
}

static void esp_now_recv_cb(const esp_now_recv_info_t *info,
                            const uint8_t *data, int len) {
  // Check for Compression (Novelty)
  if (len > 1 && data[0] == ESP_NOW_MAGIC_COMPRESSED) {
    uint8_t decomp_buf[250]; // ESP-NOW max payload
    size_t decomp_len = sizeof(decomp_buf);
    comp_stats_t stats = {0};

    // Skip magic byte (offset 1)
    esp_err_t err = lz_decompress_miniz(
        data + 1, len - 1, decomp_buf, sizeof(decomp_buf), &decomp_len, &stats);

    if (err == ESP_OK) {
      ESP_LOGI(TAG, "RX Compressed: %d -> %d bytes", len, (int)decomp_len);
      // Recursively process the decompressed data
      process_packet(info, decomp_buf, (int)decomp_len);
      return;
    } else {
      ESP_LOGE(TAG, "Decompression failed");
      return;
    }
  }

  // Standard processing
  process_packet(info, data, len);
}

static void esp_now_send_cb(const void *arg, esp_now_send_status_t status) {
  // arg is peer MAC (6 bytes) when available
  if (arg) {
    const uint8_t *mac = (const uint8_t *)arg;
    ESP_LOGW(TAG, "ESP-NOW send status=%d for %02x:%02x:%02x:%02x:%02x:%02x",
             status, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  } else {
    ESP_LOGW(TAG, "ESP-NOW send status=%d (peer unknown)", status);
  }

  // Record last send status and notify waiting sender
  s_last_send_status = (int)status;
  if (s_send_done) {
    xSemaphoreGive(s_send_done);
  }

  // Release the send semaphore so the next send can proceed.
  if (s_send_sem) {
    xSemaphoreGive(s_send_sem);
  }
}

esp_err_t esp_now_manager_init(void) {
  ESP_LOGI(TAG, "Initializing ESP-NOW...");

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
  // Disable Wi-Fi power saving to improve ESP-NOW transmission reliability
  // (minimize driver-induced delays/latency during MAC ack handling).
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

  ESP_ERROR_CHECK(esp_now_init());
  ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)esp_now_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)ESP_NOW_PMK));

  // Create send semaphore (starts available — first send can proceed
  // immediately)
  s_send_sem = xSemaphoreCreateBinary();
  if (s_send_sem == NULL) {
    ESP_LOGE(TAG, "Failed to create send semaphore!");
    return ESP_ERR_NO_MEM;
  }
  xSemaphoreGive(s_send_sem); // Make it available for the first send

  // Create send completion semaphore
  s_send_done = xSemaphoreCreateBinary();
  if (s_send_done == NULL) {
    ESP_LOGE(TAG, "Failed to create send-done semaphore!");
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "ESP-NOW initialized on channel %d", ESP_NOW_CHANNEL);
  return ESP_OK;
}

esp_err_t esp_now_manager_register_peer(const uint8_t *peer_addr,
                                        bool encrypt) {
  esp_now_peer_info_t peer;
  memset(&peer, 0, sizeof(esp_now_peer_info_t));
  memcpy(peer.peer_addr, peer_addr, ESP_NOW_ETH_ALEN);
  peer.channel = ESP_NOW_CHANNEL;
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = encrypt;

  if (encrypt) {
    memcpy(peer.lmk, ESP_NOW_LMK, ESP_NOW_KEY_LEN);
  }

  if (esp_now_is_peer_exist(peer_addr)) {
    return ESP_OK;
  }
  return esp_now_add_peer(&peer);
}

schedule_msg_t esp_now_get_current_schedule(void) { return g_current_schedule; }

esp_err_t esp_now_manager_send_data(const uint8_t *peer_addr,
                                    const uint8_t *data, size_t len) {
  const int MAX_RETRIES = 3;
  const TickType_t SEND_SEM_TIMEOUT = pdMS_TO_TICKS(400);
  const TickType_t SEND_DONE_TIMEOUT = pdMS_TO_TICKS(1000);
  // How long to wait for BLE to go quiet before each send attempt.
  // NimBLE scan window is 50ms; 120ms ensures any in-progress scan event drains.
  const TickType_t BLE_QUIET_MS = pdMS_TO_TICKS(120);

  // -----------------------------------------------------------------------
  // PEER KEEPALIVE: ensure the peer is registered before we attempt any send.
  // The neighbor_manager may have discovered the peer via BLE but the ESP-NOW
  // peer table entry could be missing (e.g. first boot, table eviction).
  // -----------------------------------------------------------------------
  if (!esp_now_is_peer_exist(peer_addr)) {
    ESP_LOGW(TAG, "ESP-NOW peer not registered (%02x:%02x:%02x:%02x:%02x:%02x), registering now",
             peer_addr[0], peer_addr[1], peer_addr[2], peer_addr[3],
             peer_addr[4], peer_addr[5]);
    esp_err_t reg = esp_now_manager_register_peer(peer_addr, false);
    if (reg != ESP_OK) {
      ESP_LOGE(TAG, "Cannot register peer: %s — aborting send", esp_err_to_name(reg));
      return ESP_FAIL;
    }
  }

  esp_err_t result = ESP_FAIL;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {

    // -------------------------------------------------------------------
    // BLE QUIET WINDOW
    // BLE scanning and advertising share the 2.4 GHz radio with ESP-NOW on
    // ESP32-S3.  The NimBLE scan window (50 ms active, 100 ms interval) can
    // occupy the radio right when esp_now_send() fires, causing the peer to
    // never receive the frame and the MAC-layer ACK to never arrive
    // (status=1 / ESP_NOW_SEND_FAIL).
    //
    // Fix: momentarily stop BLE scan + advertising, lock the radio onto the
    // ESP-NOW channel, wait for the hardware to settle, then transmit.
    // Scan/advertising are restored after the send completes (or fails).
    // -------------------------------------------------------------------
    bool was_scanning    = ble_manager_is_scanning();
    bool was_advertising = ble_manager_is_advertising();

    if (was_scanning) {
      ble_manager_stop_scanning();
    }
    if (was_advertising) {
      ble_manager_stop_advertising();
    }

    // Switch Wi-Fi to ESP-NOW channel and let the radio settle.
    esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);
    vTaskDelay(BLE_QUIET_MS);

    // -------------------------------------------------------------------
    // SEMAPHORE: wait for any previous in-flight frame to complete.
    // -------------------------------------------------------------------
    if (s_send_sem) {
      if (xSemaphoreTake(s_send_sem, SEND_SEM_TIMEOUT) != pdTRUE) {
        ESP_LOGW(TAG, "Send semaphore timeout (radio busy), attempt=%d", attempt);
        // Restore BLE before backing off
        if (was_scanning)    ble_manager_start_scanning();
        if (was_advertising) ble_manager_start_advertising();
        vTaskDelay(pdMS_TO_TICKS(50));
        continue;
      }
    }

    // Arm done-semaphore before issuing the send so we never miss the callback.
    s_last_send_status = -1;
    if (s_send_done) {
      xSemaphoreTake(s_send_done, 0);
    }

    // Diagnostic log (kept from previous iteration — useful for field debug).
    uint8_t primary_chan = 0;
    wifi_second_chan_t secondary_chan = WIFI_SECOND_CHAN_NONE;
    esp_wifi_get_channel(&primary_chan, &secondary_chan);
    ESP_LOGI(TAG, "Sending ESP-NOW to %02x:%02x:%02x:%02x:%02x:%02x "
             "(attempt=%d, peer_ok=%d, chan=%d, ble_scan=%d, ble_adv=%d)",
             peer_addr[0], peer_addr[1], peer_addr[2],
             peer_addr[3], peer_addr[4], peer_addr[5],
             attempt,
             esp_now_is_peer_exist(peer_addr),
             (int)primary_chan,
             was_scanning, was_advertising);

    esp_err_t ret = esp_now_send(peer_addr, data, len);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "esp_now_send() error: %s (attempt=%d)", esp_err_to_name(ret), attempt);
      if (s_send_sem) xSemaphoreGive(s_send_sem);
      if (was_scanning)    ble_manager_start_scanning();
      if (was_advertising) ble_manager_start_advertising();
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    // Wait for the MAC-layer ACK callback.
    bool got_cb = false;
    if (s_send_done) {
      got_cb = (xSemaphoreTake(s_send_done, SEND_DONE_TIMEOUT) == pdTRUE);
    }

    // Restore BLE now that the frame has been handed to the driver.
    if (was_scanning)    ble_manager_start_scanning();
    if (was_advertising) ble_manager_start_advertising();

    if (!got_cb) {
      ESP_LOGW(TAG, "Send completion timeout (no callback), attempt=%d", attempt);
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }

    if (s_last_send_status == ESP_NOW_SEND_SUCCESS) {
      result = ESP_OK;
      break;
    }

    ESP_LOGW(TAG, "ESP-NOW delivery failed (status=%d), attempt=%d",
             s_last_send_status, attempt);
    // Exponential-ish backoff: 100 ms, 200 ms, 400 ms
    vTaskDelay(pdMS_TO_TICKS(100 * (1 << attempt)));
  }

  return result;
}
