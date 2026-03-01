#include "persistence.h"
#include "config.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include <stdio.h>
#include <string.h>

#define REPUTATION_FILE SPIFFS_BASE_PATH "/reputation.txt"
#define REPUTATION_CACHE_MAX 32

static const char *TAG = "PERSISTENCE";
static bool persistence_initialized = false;

typedef struct {
  uint32_t node_id;
  float trust;
} reputation_entry_t;
static reputation_entry_t s_reputation_cache[REPUTATION_CACHE_MAX];
static size_t s_reputation_cache_count = 0;

void persistence_init(void) {
  if (persistence_initialized) {
    return;
  }

  esp_vfs_spiffs_conf_t conf = {
      .base_path = SPIFFS_BASE_PATH,
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true};

  esp_err_t ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    return;
  }

  persistence_initialized = true;
  ESP_LOGI(TAG, "Persistence system initialized");
}

void persistence_save_reputations(const uint32_t *node_ids, const float *trusts,
                                  size_t count) {
  if (!persistence_initialized || node_ids == NULL || trusts == NULL) {
    return;
  }
  FILE *f = fopen(REPUTATION_FILE, "w");
  if (f == NULL) {
    ESP_LOGW(TAG, "Could not open %s for write", REPUTATION_FILE);
    return;
  }
  for (size_t i = 0; i < count; i++) {
    fprintf(f, "%lu %.4f\n", (unsigned long)node_ids[i],
            (double)(trusts[i] < 0.0f ? 0.0f : trusts[i] > 1.0f ? 1.0f : trusts[i]));
  }
  fclose(f);
  ESP_LOGD(TAG, "Saved %u reputation entries", (unsigned)count);
}

void persistence_load_reputations(void) {
  if (!persistence_initialized) {
    return;
  }
  s_reputation_cache_count = 0;
  FILE *f = fopen(REPUTATION_FILE, "r");
  if (f == NULL) {
    return;
  }
  unsigned long node_id;
  double trust;
  while (s_reputation_cache_count < REPUTATION_CACHE_MAX &&
         fscanf(f, "%lu %lf", &node_id, &trust) == 2) {
    s_reputation_cache[s_reputation_cache_count].node_id = (uint32_t)node_id;
    float t = (float)trust;
    s_reputation_cache[s_reputation_cache_count].trust =
        (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    s_reputation_cache_count++;
  }
  fclose(f);
  if (s_reputation_cache_count > 0) {
    ESP_LOGI(TAG, "Loaded %u reputation entries", (unsigned)s_reputation_cache_count);
  }
}

bool persistence_get_initial_trust(uint32_t node_id, float *trust) {
  if (trust == NULL) {
    return false;
  }
  for (size_t i = 0; i < s_reputation_cache_count; i++) {
    if (s_reputation_cache[i].node_id == node_id) {
      *trust = s_reputation_cache[i].trust;
      return true;
    }
  }
  return false;
}

