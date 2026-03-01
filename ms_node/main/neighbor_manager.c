#include "neighbor_manager.h"
#include "config.h"
#include "esp_log.h"
#include "esp_now_manager.h"
#include "persistence.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <math.h>
#include <stddef.h>
#include <string.h>

static const char *TAG = "NEIGHBOR";

static neighbor_entry_t neighbor_table[MAX_NEIGHBORS];
static size_t neighbor_count = 0;
// Mutex to protect neighbor_table and neighbor_count
static SemaphoreHandle_t neighbor_mutex = NULL;

void neighbor_manager_init(void) {
  // Create mutex if not exists
  if (neighbor_mutex == NULL) {
    neighbor_mutex = xSemaphoreCreateMutex();
    if (neighbor_mutex == NULL) {
      ESP_LOGE(TAG, "Failed to create neighbor mutex!");
      return;
    }
  }

  // Take mutex to safely initialize
  if (xSemaphoreTake(neighbor_mutex, portMAX_DELAY) == pdTRUE) {
    memset(neighbor_table, 0, sizeof(neighbor_table));
    neighbor_count = 0;
    ESP_LOGI(TAG, "Neighbor manager initialized");
    xSemaphoreGive(neighbor_mutex);
  }
}

void neighbor_manager_update(uint32_t node_id, const uint8_t *mac_addr,
                             int8_t rssi, float score, float battery,
                             uint64_t uptime, float trust, float link_quality,
                             bool is_ch, uint8_t seq_num) {
  if (neighbor_mutex == NULL)
    return;

  // Try to take mutex with timeout (don't block BLE task too long)
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mutex for update");
    return;
  }

  uint64_t now_ms = esp_timer_get_time() / 1000;

  // Find existing entry
  for (size_t i = 0; i < neighbor_count; i++) {
    if (neighbor_table[i].node_id == node_id) {
      // Update existing
      neighbor_entry_t *entry = &neighbor_table[i];

      // Calculate missed packets
      // Example: last=5, curr=7 -> missed=1 (packet 6)
      // Example: last=255, curr=1 -> missed=1 (packet 0)
      int diff = (int)seq_num - (int)entry->last_seq_num;
      if (diff < 0)
        diff += 256; // Handle wrap-around

      int missed = 0;
      if (diff > 1) {
        missed = diff - 1;
      }

      // Sanity check: if diff is huge (e.g. node rebooted), ignore
      if (missed > 20)
        missed = 0;

      // Update PER metrics (1 received, N missed)
      metrics_record_ble_reception(1, missed);

      entry->last_seq_num = seq_num;

      // Update MAC if changed (unlikely but possible if device replaced)
      if (mac_addr) {
        memcpy(entry->mac_addr, mac_addr, 6);
        // Register as ESP-NOW peer (assuming internal logic handles dupes)
        esp_now_manager_register_peer(mac_addr, false);
      }

      // Update RSSI EWMA
      if (entry->rssi_ewma == 0.0f) {
        entry->rssi_ewma = (float)rssi;
      } else {
        entry->rssi_ewma = RSSI_EWMA_ALPHA * (float)rssi +
                           (1.0f - RSSI_EWMA_ALPHA) * entry->rssi_ewma;
      }

      entry->last_rssi = rssi;
      entry->score = score;
      entry->battery = battery;
      entry->uptime_seconds = uptime;
      entry->trust = trust;
      entry->link_quality = link_quality;
      entry->last_seen_ms = now_ms;
      entry->is_ch = is_ch;
      if (is_ch) {
        entry->ch_announce_timestamp = now_ms;
        ESP_LOGD(TAG, "CH beacon from node_%lu: timestamp updated to %llu ms",
                 node_id, (unsigned long long)now_ms);
      }
      entry->verified = true;

      xSemaphoreGive(neighbor_mutex);
      return;
    }
  }

  // Add new neighbor
  if (neighbor_count < MAX_NEIGHBORS) {
    neighbor_entry_t *entry = &neighbor_table[neighbor_count];
    entry->node_id = node_id;
    if (mac_addr) {
      memcpy(entry->mac_addr, mac_addr, 6);
      esp_now_manager_register_peer(mac_addr, false);
    }
    entry->rssi_ewma = (float)rssi;
    entry->last_rssi = rssi;
    entry->score = score;
    entry->battery = battery;
    entry->uptime_seconds = uptime;
    float use_trust = trust;
    if (persistence_get_initial_trust(node_id, &use_trust)) {
      entry->trust = use_trust;
    } else {
      entry->trust = trust;
    }
    entry->link_quality = link_quality;
    entry->last_seen_ms = now_ms;
    entry->is_ch = is_ch;
    entry->ch_announce_timestamp = is_ch ? now_ms : 0;
    entry->verified = true;
    entry->last_seq_num = seq_num; // Initialize sequence number
    neighbor_count++;

    ESP_LOGI(TAG, "Added neighbor: node_id=%lu, RSSI=%d, Seq=%d", node_id, rssi,
             seq_num);
  } else {
    // Only warn occasionally to prevent log flooding
    static uint64_t last_warn = 0;
    if (now_ms - last_warn > 5000) {
      ESP_LOGW(TAG, "Neighbor table full, cannot add node %lu", node_id);
      last_warn = now_ms;
    }
  }

  xSemaphoreGive(neighbor_mutex);
}

bool neighbor_manager_get(uint32_t node_id, neighbor_entry_t *out_entry) {
  if (neighbor_mutex == NULL || out_entry == NULL)
    return false;

  bool found = false;

  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < neighbor_count; i++) {
      if (neighbor_table[i].node_id == node_id) {
        *out_entry = neighbor_table[i]; // Safe copy
        found = true;
        break;
      }
    }
    xSemaphoreGive(neighbor_mutex);
  }
  return found;
}

bool neighbor_manager_get_by_mac(const uint8_t *mac_addr,
                                 neighbor_entry_t *out_entry) {
  if (neighbor_mutex == NULL || mac_addr == NULL || out_entry == NULL)
    return false;

  bool found = false;

  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < neighbor_count; i++) {
      if (memcmp(neighbor_table[i].mac_addr, mac_addr, 6) == 0) {
        *out_entry = neighbor_table[i]; // Safe copy
        found = true;
        break;
      }
    }
    xSemaphoreGive(neighbor_mutex);
  }
  return found;
}

size_t neighbor_manager_get_all(neighbor_entry_t *neighbors, size_t max_count) {
  if (neighbor_mutex == NULL)
    return 0;
  if (neighbors == NULL || max_count == 0)
    return 0;

  size_t count = 0;

  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
    count = (neighbor_count < max_count) ? neighbor_count : max_count;
    if (count > 0) {
      memcpy(neighbors, neighbor_table, count * sizeof(neighbor_entry_t));
    }
    xSemaphoreGive(neighbor_mutex);
  } else {
    ESP_LOGE(TAG, "Failed to get neighbor mutex for report!");
  }
  return count;
}

void neighbor_manager_cleanup_stale(void) {
  if (neighbor_mutex == NULL)
    return;
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    ESP_LOGW(TAG, "Failed to take mutex for cleanup");
    return;
  }

  uint64_t now_ms = esp_timer_get_time() / 1000;
  size_t write_idx = 0;

  for (size_t i = 0; i < neighbor_count; i++) {
    if (now_ms - neighbor_table[i].last_seen_ms < NEIGHBOR_TIMEOUT_MS) {
      // Keep this neighbor
      if (write_idx != i) {
        neighbor_table[write_idx] = neighbor_table[i];
      }
      write_idx++;
    } else {
      ESP_LOGI(TAG, "Removed stale neighbor: node_id=%lu",
               neighbor_table[i].node_id);
    }
  }

  neighbor_count = write_idx;
  xSemaphoreGive(neighbor_mutex);
}

bool neighbor_manager_is_in_cluster(const neighbor_entry_t *neighbor) {
  return neighbor->rssi_ewma >= CLUSTER_RADIUS_RSSI_THRESHOLD;
}

uint32_t neighbor_manager_get_current_ch(void) {
  if (neighbor_mutex == NULL)
    return 0;

  uint32_t best_ch = 0;
  float best_score = -1.0f;
  uint64_t now_ms = esp_timer_get_time() / 1000;

  // Checking CH needs to be atomic
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < neighbor_count; i++) {
      // Debug: Log all neighbors and why they might not qualify as CH
      if (neighbor_table[i].is_ch) {
        uint64_t timestamp_age =
            now_ms - neighbor_table[i].ch_announce_timestamp;

        bool is_verified = neighbor_table[i].verified;
        bool is_trusted = neighbor_table[i].trust >= TRUST_FLOOR;
        bool is_timely = timestamp_age < CH_BEACON_TIMEOUT_MS;

        if (!is_verified || !is_trusted || !is_timely) {
          ESP_LOGW(TAG,
                   "CH Candidate Rejected node_%lu: Ver=%d, Trust=%.2f (Req "
                   "%.2f), Age=%llu ms (Max %d)",
                   neighbor_table[i].node_id, is_verified,
                   neighbor_table[i].trust, TRUST_FLOOR,
                   (unsigned long long)timestamp_age, CH_BEACON_TIMEOUT_MS);
        }
      }

      // Must be CH, verified, trusted, AND recently announced (within 30s)
      if (neighbor_table[i].is_ch && neighbor_table[i].verified &&
          neighbor_table[i].trust >= TRUST_FLOOR &&
          (now_ms - neighbor_table[i].ch_announce_timestamp <
           CH_BEACON_TIMEOUT_MS)) {

        if (neighbor_table[i].score > best_score) {
          best_score = neighbor_table[i].score;
          best_ch = neighbor_table[i].node_id;
        }
      }
    }
    xSemaphoreGive(neighbor_mutex);
  }

  return best_ch;
}

bool neighbor_manager_get_ch_mac(uint8_t *mac_out) {
  if (neighbor_mutex == NULL)
    return false;

  bool found = false;
  uint64_t now_ms = esp_timer_get_time() / 1000;

  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < neighbor_count; i++) {
      if (neighbor_table[i].is_ch && neighbor_table[i].verified) {
        if (now_ms - neighbor_table[i].ch_announce_timestamp <
            CH_BEACON_TIMEOUT_MS) {
          if (mac_out) {
            memcpy(mac_out, neighbor_table[i].mac_addr, 6);
          }
          found = true;
          break; // Found one, good enough
        }
      }
    }
    xSemaphoreGive(neighbor_mutex);
  }
  return found;
}

void neighbor_manager_update_trust(uint32_t node_id, bool success) {
  if (neighbor_mutex == NULL)
    return;

  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    // Find manually since we hold lock
    neighbor_entry_t *entry = NULL;
    for (size_t i = 0; i < neighbor_count; i++) {
      if (neighbor_table[i].node_id == node_id) {
        entry = &neighbor_table[i];
        break;
      }
    }

    if (entry) {
      // Update trust metrics using EWMA
      // Alpha = 0.1 (slowly adapt)
      float target = success ? 1.0f : 0.0f;
      entry->trust = 0.9f * entry->trust + 0.1f * target;

      // Mark as verified if trust is high enough
      if (entry->trust > 0.3f) {
        entry->verified = true;
      }

      ESP_LOGI(
          TAG,
          "[TEST] Trust Updated for Node %lu: New Score = %.2f (Success=%d)",
          node_id, entry->trust, success);
    }
    xSemaphoreGive(neighbor_mutex);
  }
}

void neighbor_manager_set_trust_value(uint32_t node_id, float trust) {
  if (neighbor_mutex == NULL)
    return;
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) != pdTRUE)
    return;
  for (size_t i = 0; i < neighbor_count; i++) {
    if (neighbor_table[i].node_id == node_id) {
      neighbor_table[i].trust = (trust < 0.0f) ? 0.0f : (trust > 1.0f) ? 1.0f : trust;
      if (neighbor_table[i].trust > 0.3f) {
        neighbor_table[i].verified = true;
      }
      break;
    }
  }
  xSemaphoreGive(neighbor_mutex);
}

size_t neighbor_manager_get_count(void) {
  if (neighbor_mutex == NULL)
    return 0;
  size_t count = 0;
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
    count = neighbor_count;
    xSemaphoreGive(neighbor_mutex);
  }
  return count;
}

size_t neighbor_manager_get_member_count(void) {
  if (neighbor_mutex == NULL)
    return 0;
  size_t n = 0;
  if (xSemaphoreTake(neighbor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
    for (size_t i = 0; i < neighbor_count; i++) {
      if (neighbor_table[i].verified &&
          neighbor_manager_is_in_cluster(&neighbor_table[i]) &&
          !neighbor_table[i].is_ch) {
        n++;
      }
    }
    xSemaphoreGive(neighbor_mutex);
  }
  return n;
}
