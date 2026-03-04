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
#include <stdlib.h>
#include <string.h>

static const char *TAG = "ESP_NOW";
static schedule_msg_t g_current_schedule = {0};

// ------------------------------------------------------------------
// Deduplication tracker: per-node last-stored sequence number
// Prevents writing the same (node_id, seq) to storage repeatedly.
// Members resend the same reading every loop (~100ms) but seq only
// advances every ~120s, causing massive duplicates without this.
// ------------------------------------------------------------------
#define DEDUP_MAX_NODES 8
#define DEDUP_HISTORY_PER_NODE 32  // Track last N seq nums per node (was 4; burst drain sends up to 24 chunks rapid-fire, need enough room)
typedef struct {
  uint32_t node_id;
  uint32_t recent_seqs[DEDUP_HISTORY_PER_NODE];
  int seq_idx;  // Circular index into recent_seqs
  int count;    // How many entries are valid (0..DEDUP_HISTORY_PER_NODE)
} dedup_entry_t;
static dedup_entry_t s_dedup_table[DEDUP_MAX_NODES];
static int s_dedup_count = 0;

// Returns true if this (node_id, seq) should be stored (first time seen)
// Tracks a small sliding window of recent seq nums per node to handle
// both live duplicates and historical data interleaved.
static bool dedup_should_store(uint32_t node_id, uint32_t seq_num) {
  dedup_entry_t *entry = NULL;
  
  // Find existing entry for this node
  for (int i = 0; i < s_dedup_count; i++) {
    if (s_dedup_table[i].node_id == node_id) {
      entry = &s_dedup_table[i];
      break;
    }
  }
  
  // New node — add to table
  if (!entry) {
    if (s_dedup_count < DEDUP_MAX_NODES) {
      entry = &s_dedup_table[s_dedup_count++];
    } else {
      entry = &s_dedup_table[0];  // Evict oldest
    }
    entry->node_id = node_id;
    entry->seq_idx = 0;
    entry->count = 0;
  }
  
  // Check if seq already in recent history
  int check_count = (entry->count < DEDUP_HISTORY_PER_NODE) ? entry->count : DEDUP_HISTORY_PER_NODE;
  for (int i = 0; i < check_count; i++) {
    if (entry->recent_seqs[i] == seq_num) {
      return false;  // Already stored recently
    }
  }
  
  // New seq — add to circular buffer
  entry->recent_seqs[entry->seq_idx] = seq_num;
  entry->seq_idx = (entry->seq_idx + 1) % DEDUP_HISTORY_PER_NODE;
  if (entry->count < DEDUP_HISTORY_PER_NODE) {
    entry->count++;
  }
  
  return true;
}

// CH Status tracking (for UAV onboarding notification)
static bool s_ch_busy = false;              // Is CH busy with UAV onboarding?
static uint64_t s_ch_status_time_ms = 0;    // Last CH status update time
static uint32_t s_ch_status_node_id = 0;    // CH node ID that sent status

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
  // 0. Check for CH Status Message (UAV Onboarding Notification)
  if (len == sizeof(ch_status_msg_t)) {
    const ch_status_msg_t *status_msg = (const ch_status_msg_t *)data;
    if (status_msg->magic == ESP_NOW_MAGIC_CH_STATUS) {
      s_ch_status_time_ms = esp_timer_get_time() / 1000;
      s_ch_status_node_id = status_msg->ch_node_id;
      
      if (status_msg->status == CH_STATUS_UAV_BUSY) {
        s_ch_busy = true;
        ESP_LOGW(TAG, "RX CH_STATUS: CH node_%lu is BUSY with UAV onboarding - HOLD DATA",
                 status_msg->ch_node_id);
      } else if (status_msg->status == CH_STATUS_RESUME) {
        s_ch_busy = false;
        ESP_LOGI(TAG, "RX CH_STATUS: CH node_%lu RESUMED - TDMA can continue",
                 status_msg->ch_node_id);
      } else {
        s_ch_busy = false;
        ESP_LOGI(TAG, "RX CH_STATUS: CH node_%lu status NORMAL",
                 status_msg->ch_node_id);
      }
      return;
    }
  }

  // 1. Check for Schedule Packet (Time Slicing Novelty)
  if (len == sizeof(schedule_msg_t)) {
    const schedule_msg_t *sched = (const schedule_msg_t *)data;
    if (sched->magic == ESP_NOW_MAGIC_SCHEDULE) {
      // If the schedule has a target_node_id, only accept if it's for us.
      // Broadcast schedules carry target_node_id so each member can filter
      // to its own slot assignment.
      extern uint32_t g_node_id;
      if (sched->target_node_id != 0 && sched->target_node_id != g_node_id) {
        ESP_LOGD(TAG, "Schedule for node_%lu, not us (node_%lu) — ignoring",
                 sched->target_node_id, g_node_id);
        return;
      }

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
               "┌─ TIME SLICE RECEIVED ─────────────────────────────────┐");
      ESP_LOGI(TAG,
               "│ Slot: %d | Duration: %ds | Epoch: %lld (local: %lld) │",
               sched->slot_index, sched->slot_duration_sec,
               sched->epoch_us, local_sched.epoch_us);
      ESP_LOGI(TAG,
               "└───────────────────────────────────────────────────────┘");
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

    // Dedup: Only store if this is a NEW sequence number for this node
    if (!dedup_should_store(payload->node_id, payload->seq_num)) {
      ESP_LOGD(TAG, "Dedup: Skipping node_%lu seq=%lu (already stored)",
               payload->node_id, payload->seq_num);
      // Still update neighbor trust even for duplicates
      neighbor_entry_t n;
      if (neighbor_manager_get_by_mac(info->src_addr, &n)) {
        neighbor_manager_update_trust(n.node_id, true);
      }
      return;
    }

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
      
      // Dedup: Extract node_id and seq from JSON to check for duplicates
      // Format: {"id":NNNN,"seq":NNNN,...}
      uint32_t hist_id = 0, hist_seq = 0;
      bool parsed_ok = false;
      const char *id_ptr = strstr(json_data, "\"id\":");
      const char *seq_ptr = strstr(json_data, "\"seq\":");
      if (id_ptr && seq_ptr) {
        hist_id = (uint32_t)strtoul(id_ptr + 5, NULL, 10);
        hist_seq = (uint32_t)strtoul(seq_ptr + 6, NULL, 10);
        parsed_ok = true;
      }
      
      if (parsed_ok && !dedup_should_store(hist_id, hist_seq)) {
        ESP_LOGD(TAG, "Dedup: Skipping historical node_%lu seq=%lu (already stored)",
                 hist_id, hist_seq);
        free(json_data);
        // Still update trust
        neighbor_entry_t n;
        if (neighbor_manager_get_by_mac(info->src_addr, &n)) {
          neighbor_manager_update_trust(n.node_id, true);
        }
        return;
      }
      
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

esp_err_t esp_now_manager_deinit(void) {
  ESP_LOGI(TAG, "Deinitializing ESP-NOW for UAV onboarding...");
  
  // Deinit ESP-NOW first (must be done before wifi changes)
  esp_err_t err = esp_now_deinit();
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_NOT_INIT) {
    ESP_LOGE(TAG, "Failed to deinit ESP-NOW: %s", esp_err_to_name(err));
    return err;
  }
  
  // Stop WiFi (will be restarted by uav_client in STA mode to connect to AP)
  err = esp_wifi_stop();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGE(TAG, "Failed to stop WiFi: %s", esp_err_to_name(err));
    return err;
  }
  
  ESP_LOGI(TAG, "ESP-NOW deinitialized, WiFi stopped");
  return ESP_OK;
}

esp_err_t esp_now_manager_reinit(void) {
  ESP_LOGI(TAG, "Reinitializing ESP-NOW after UAV onboarding...");
  
  // Disconnect from any AP first
  esp_wifi_disconnect();
  
  // Stop WiFi cleanly
  esp_err_t err = esp_wifi_stop();
  if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) {
    ESP_LOGW(TAG, "WiFi stop warning: %s", esp_err_to_name(err));
  }
  
  // Reconfigure WiFi for ESP-NOW (STA mode, fixed channel)
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_ERROR_CHECK(esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE));
  ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
  
  // Reinitialize ESP-NOW
  err = esp_now_init();
  if (err != ESP_OK && err != ESP_ERR_ESPNOW_EXIST) {
    ESP_LOGE(TAG, "Failed to reinit ESP-NOW: %s", esp_err_to_name(err));
    return err;
  }
  
  ESP_ERROR_CHECK(esp_now_register_send_cb((esp_now_send_cb_t)esp_now_send_cb));
  ESP_ERROR_CHECK(esp_now_register_recv_cb(esp_now_recv_cb));
  ESP_ERROR_CHECK(esp_now_set_pmk((uint8_t *)ESP_NOW_PMK));
  
  ESP_LOGI(TAG, "ESP-NOW reinitialized on channel %d", ESP_NOW_CHANNEL);
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

/* ─────────────────────────────────────────────────────────────────────────
 * FAST SEND — Used exclusively during DATA-phase burst drain.
 *
 * The normal send_data() path includes per-frame BLE stop/start and a
 * 120 ms radio-quiet window.  During the DATA phase BLE is already off
 * (state_machine stops it at the STELLAR→DATA transition), so these
 * operations are pure waste.
 *
 * Removing the 120 ms quiet + BLE overhead per chunk turns a 7.5 s TDMA
 * slot from draining ≈6 chunks to draining ≈50+ chunks.
 * ───────────────────────────────────────────────────────────────────── */
esp_err_t esp_now_manager_send_data_fast(const uint8_t *peer_addr,
                                          const uint8_t *data, size_t len) {
  const int MAX_RETRIES = 3;
  const TickType_t SEND_SEM_TIMEOUT = pdMS_TO_TICKS(200);
  const TickType_t SEND_DONE_TIMEOUT = pdMS_TO_TICKS(500);

  /* Peer keepalive — same as normal path */
  if (!esp_now_is_peer_exist(peer_addr)) {
    esp_err_t reg = esp_now_manager_register_peer(peer_addr, false);
    if (reg != ESP_OK) {
      ESP_LOGE(TAG, "fast send: cannot register peer: %s", esp_err_to_name(reg));
      return ESP_FAIL;
    }
  }

  esp_err_t result = ESP_FAIL;

  for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {

    /* ── NO BLE stop/start, NO 120 ms quiet window ── */

    /* Ensure radio is on ESP-NOW channel */
    esp_wifi_set_channel(ESP_NOW_CHANNEL, WIFI_SECOND_CHAN_NONE);

    /* Semaphore: wait for any previous in-flight frame */
    if (s_send_sem) {
      if (xSemaphoreTake(s_send_sem, SEND_SEM_TIMEOUT) != pdTRUE) {
        ESP_LOGW(TAG, "fast send: sem timeout attempt=%d", attempt);
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }

    s_last_send_status = -1;
    if (s_send_done) {
      xSemaphoreTake(s_send_done, 0);
    }

    esp_err_t ret = esp_now_send(peer_addr, data, len);
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "fast send: esp_now_send() error: %s attempt=%d",
               esp_err_to_name(ret), attempt);
      if (s_send_sem) xSemaphoreGive(s_send_sem);
      vTaskDelay(pdMS_TO_TICKS(30));
      continue;
    }

    /* Wait for MAC-layer ACK */
    bool got_cb = false;
    if (s_send_done) {
      got_cb = (xSemaphoreTake(s_send_done, SEND_DONE_TIMEOUT) == pdTRUE);
    }

    if (!got_cb) {
      ESP_LOGW(TAG, "fast send: ACK timeout attempt=%d", attempt);
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    if (s_last_send_status == ESP_NOW_SEND_SUCCESS) {
      result = ESP_OK;
      break;
    }

    ESP_LOGW(TAG, "fast send: delivery failed status=%d attempt=%d",
             s_last_send_status, attempt);
    vTaskDelay(pdMS_TO_TICKS(50 * (1 << attempt)));
  }

  return result;
}

// Broadcast address for ESP-NOW
static const uint8_t BROADCAST_ADDR[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

esp_err_t esp_now_manager_broadcast_ch_status(uint8_t status) {
  // Get own node ID
  extern uint32_t g_node_id;
  
  ch_status_msg_t msg = {
    .magic = ESP_NOW_MAGIC_CH_STATUS,
    .ch_node_id = g_node_id,
    .status = status,
    .reserved = {0}
  };
  
  // Ensure broadcast peer is registered
  if (!esp_now_is_peer_exist(BROADCAST_ADDR)) {
    esp_now_peer_info_t peer = {0};
    memcpy(peer.peer_addr, BROADCAST_ADDR, 6);
    peer.channel = ESP_NOW_CHANNEL;
    peer.ifidx = WIFI_IF_STA;
    peer.encrypt = false;
    esp_now_add_peer(&peer);
  }
  
  ESP_LOGI(TAG, "Broadcasting CH_STATUS: %s (node_%lu)",
           status == CH_STATUS_UAV_BUSY ? "UAV_BUSY" :
           status == CH_STATUS_RESUME ? "RESUME" : "NORMAL",
           g_node_id);
  
  // Send multiple times for reliability (members may be in different states)
  esp_err_t ret = ESP_OK;
  for (int i = 0; i < 3; i++) {
    ret = esp_now_send(BROADCAST_ADDR, (uint8_t *)&msg, sizeof(msg));
    if (ret != ESP_OK) {
      ESP_LOGW(TAG, "CH_STATUS broadcast attempt %d failed: %s", i, esp_err_to_name(ret));
    }
    vTaskDelay(pdMS_TO_TICKS(50)); // Small delay between broadcasts
  }
  
  return ret;
}

bool esp_now_manager_is_ch_busy(void) {
  // Auto-clear busy status if stale (>60 seconds without update)
  uint64_t now_ms = esp_timer_get_time() / 1000;
  if (s_ch_busy && (now_ms - s_ch_status_time_ms) > 60000) {
    ESP_LOGW(TAG, "CH_BUSY status stale (>60s), auto-clearing");
    s_ch_busy = false;
  }
  return s_ch_busy;
}

uint64_t esp_now_manager_get_ch_status_time(void) {
  return s_ch_status_time_ms;
}
