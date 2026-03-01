#include "auth.h"
#include "config.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <string.h>
#include <time.h>

static const char *TAG = "AUTH";
#define REPLAY_WINDOW_MS 60000 // 1 minute replay protection
#define HMAC_LENGTH 16
#define NVS_AUTH_NAMESPACE "auth"
#define NVS_CLUSTER_KEY_KEY "cluster_key"

// Default key (fallback when NVS has no key - dev/test only; use NVS for prod)
static const uint8_t s_default_cluster_key[CLUSTER_KEY_SIZE] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    0x0C, 0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x1F};

// Global cluster key (loaded from NVS or default)
uint8_t g_cluster_key[CLUSTER_KEY_SIZE];

// Replay protection mutex
static SemaphoreHandle_t s_replay_mutex = NULL;

// Simple replay protection: store last timestamp per node
typedef struct {
  uint32_t node_id;
  uint64_t last_timestamp;
} replay_entry_t;

#define MAX_REPLAY_ENTRIES 20
static replay_entry_t replay_table[MAX_REPLAY_ENTRIES];
static size_t replay_count = 0;

bool auth_generate_hmac(const uint8_t *message, size_t msg_len,
                        const uint8_t *key, uint8_t *hmac_out) {
  mbedtls_md_context_t ctx;
  const mbedtls_md_info_t *md_info;

  md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
  if (!md_info) {
    ESP_LOGE(TAG, "Failed to get MD info");
    return false;
  }

  mbedtls_md_init(&ctx);
  if (mbedtls_md_setup(&ctx, md_info, 1) != 0) { // 1 = HMAC mode
    ESP_LOGE(TAG, "Failed to setup MD context");
    mbedtls_md_free(&ctx);
    return false;
  }

  if (mbedtls_md_hmac_starts(&ctx, key, CLUSTER_KEY_SIZE) != 0 ||
      mbedtls_md_hmac_update(&ctx, message, msg_len) != 0 ||
      mbedtls_md_hmac_finish(&ctx, hmac_out) != 0) {
    ESP_LOGE(TAG, "HMAC computation failed");
    mbedtls_md_free(&ctx);
    return false;
  }

  mbedtls_md_free(&ctx);
  return true;
}

bool auth_verify_hmac(const uint8_t *message, size_t msg_len,
                      const uint8_t *received_hmac, const uint8_t *key) {
  uint8_t computed_hmac[32]; // Full SHA256
  uint8_t truncated_hmac[HMAC_LENGTH];

  if (!auth_generate_hmac(message, msg_len, key, computed_hmac)) {
    return false;
  }

  // Truncate to HMAC_LENGTH (16 bytes for general use, but BLE uses 3)
  size_t compare_len = HMAC_LENGTH;
  if (compare_len > 32)
    compare_len = 32; // Safety check
  memcpy(truncated_hmac, computed_hmac, compare_len);

  // Constant-time comparison
  int diff = 0;
  for (size_t i = 0; i < compare_len; i++) {
    diff |= (truncated_hmac[i] ^ received_hmac[i]);
  }

  return diff == 0;
}

bool auth_check_replay(uint64_t timestamp, uint32_t node_id) {
  if (s_replay_mutex &&
      xSemaphoreTake(s_replay_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
    ESP_LOGW(TAG, "Replay mutex timeout, rejecting");
    return false;
  }

  uint64_t now_ms = esp_timer_get_time() / 1000;

  // Check if timestamp is too old or too far in future
  if (timestamp > now_ms + REPLAY_WINDOW_MS ||
      timestamp < now_ms - REPLAY_WINDOW_MS) {
    if (s_replay_mutex)
      xSemaphoreGive(s_replay_mutex);
    return false;
  }

  bool ok = true;

  // Check replay table
  for (size_t i = 0; i < replay_count; i++) {
    if (replay_table[i].node_id == node_id) {
      if (timestamp <= replay_table[i].last_timestamp) {
        ESP_LOGW(TAG, "Replay detected from node %lu", node_id);
        ok = false;
      } else {
        replay_table[i].last_timestamp = timestamp;
      }
      goto done;
    }
  }

  // New node, add to table
  if (replay_count < MAX_REPLAY_ENTRIES) {
    replay_table[replay_count].node_id = node_id;
    replay_table[replay_count].last_timestamp = timestamp;
    replay_count++;
  } else {
    // Table full, evict oldest (simple FIFO)
    memmove(&replay_table[0], &replay_table[1],
            (MAX_REPLAY_ENTRIES - 1) * sizeof(replay_entry_t));
    replay_table[MAX_REPLAY_ENTRIES - 1].node_id = node_id;
    replay_table[MAX_REPLAY_ENTRIES - 1].last_timestamp = timestamp;
  }

done:
  if (s_replay_mutex)
    xSemaphoreGive(s_replay_mutex);
  return ok;
}

void auth_init(void) {
  s_replay_mutex = xSemaphoreCreateMutex();
  if (s_replay_mutex == NULL) {
    ESP_LOGE(TAG, "Failed to create replay mutex");
  }

  memset(replay_table, 0, sizeof(replay_table));
  replay_count = 0;

  // Load cluster key from NVS; fallback to default for dev/test
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_AUTH_NAMESPACE, NVS_READONLY, &nvs);
  if (err == ESP_OK) {
    size_t len = CLUSTER_KEY_SIZE;
    err = nvs_get_blob(nvs, NVS_CLUSTER_KEY_KEY, g_cluster_key, &len);
    nvs_close(nvs);
    if (err == ESP_OK && len == CLUSTER_KEY_SIZE) {
      ESP_LOGI(TAG, "Cluster key loaded from NVS");
    } else {
      memcpy(g_cluster_key, s_default_cluster_key, CLUSTER_KEY_SIZE);
      ESP_LOGW(TAG, "NVS key invalid/missing, using default (dev only)");
    }
  } else {
    memcpy(g_cluster_key, s_default_cluster_key, CLUSTER_KEY_SIZE);
    ESP_LOGW(TAG, "NVS open failed, using default key (dev only)");
  }

  ESP_LOGI(TAG, "Authentication system initialized");
}

bool auth_provision_cluster_key(const uint8_t *key) {
  if (!key)
    return false;
  nvs_handle_t nvs;
  esp_err_t err = nvs_open(NVS_AUTH_NAMESPACE, NVS_READWRITE, &nvs);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "NVS open failed: %s", esp_err_to_name(err));
    return false;
  }
  err = nvs_set_blob(nvs, NVS_CLUSTER_KEY_KEY, key, CLUSTER_KEY_SIZE);
  if (err == ESP_OK) {
    err = nvs_commit(nvs);
  }
  nvs_close(nvs);
  if (err == ESP_OK) {
    memcpy(g_cluster_key, key, CLUSTER_KEY_SIZE);
    ESP_LOGI(TAG, "Cluster key provisioned to NVS");
    return true;
  }
  ESP_LOGE(TAG, "Failed to provision key: %s", esp_err_to_name(err));
  return false;
}
