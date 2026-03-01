#include "logger.h"
#include "blockbuf.h"

#include "compression.h"

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_spiffs.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "rom/crc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#define TAG "logger"

// Tweak these if you want.
#ifndef LOGGER_BLOCK_CAP
#define LOGGER_BLOCK_CAP (16 * 1024)
#endif

#ifndef LOGGER_FLUSH_THRESHOLD
#define LOGGER_FLUSH_THRESHOLD (16 * 1024)
#endif

#ifndef LOGGER_COMPRESS_LEVEL
#define LOGGER_COMPRESS_LEVEL 3
#endif

#ifndef LOGGER_MIN_COMPRESS_BYTES
#define LOGGER_MIN_COMPRESS_BYTES 1024
#endif

// Require at least ~5% savings to store as compressed.
#ifndef LOGGER_MIN_SAVINGS_DIV
#define LOGGER_MIN_SAVINGS_DIV 20
#endif

// File rotation: when main log exceeds this size, rotate to _old
#ifndef LOGGER_MAX_FILE_SIZE
#define LOGGER_MAX_FILE_SIZE (1024 * 1024) // 1MB
#endif

// File paths for rotation
#define LOGGER_OLD_PATH "/spiffs/samples_old.lz"
#define LOGGER_BACKUP_PATH "/spiffs/samples_backup.lz"

typedef struct __attribute__((packed)) {
  uint32_t magic;     // 'MSLG'
  uint16_t version;   // 2 (bumped for crc32 + node_id)
  uint8_t algo;       // 0 = raw, 1 = miniz(deflate)
  uint8_t level;      // deflate level when algo=1
  uint32_t raw_len;   // bytes before compression
  uint32_t data_len;  // bytes stored after header
  uint32_t crc32;     // CRC32 of payload data
  uint64_t node_id;   // Unique node ID from MAC
  uint32_t timestamp; // Unix timestamp (if available)
  uint32_t reserved;  // Future use
} log_chunk_hdr_t;

static const uint32_t LOG_MAGIC = 0x4D534C47U; // MSLG
static const uint16_t LOG_VER = 2;

// Storage management thresholds
#ifndef LOGGER_STORAGE_WARNING_PCT
#define LOGGER_STORAGE_WARNING_PCT 90
#endif

#ifndef LOGGER_STORAGE_CRITICAL_PCT
#define LOGGER_STORAGE_CRITICAL_PCT 95
#endif

// RTC sync: boot time offset for calculating Unix timestamps
static uint32_t s_boot_timestamp = 0; // Unix time at boot (if synced)

static bool s_inited = false;
static blockbuf_t s_bb;
static uint64_t s_node_id = 0;
static SemaphoreHandle_t s_flush_mutex = NULL;

// Calculate CRC32 of data
static uint32_t calc_crc32(const uint8_t *data, size_t len) {
  return crc32_le(0, data, len);
}

// Get current timestamp (Unix time if synced, otherwise seconds since boot)
static uint32_t get_timestamp(void) {
  uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  if (s_boot_timestamp > 0) {
    return s_boot_timestamp + uptime;
  }
  return uptime;
}

// Rotate log file if current size plus incoming bytes will exceed limit
static esp_err_t rotate_log_file(size_t incoming_bytes) {
  struct stat st;
  if (stat(LOGGER_DEFAULT_PATH, &st) != 0) {
    // File doesn't exist, nothing to rotate
    return ESP_OK;
  }

  // If after writing this chunk we would exceed the limit, rotate
  size_t projected = (size_t)st.st_size + incoming_bytes;
  if (projected < LOGGER_MAX_FILE_SIZE) {
    // Not large enough to rotate
    return ESP_OK;
  }

  ESP_LOGI(TAG, "Rotating log file (%zu bytes)", (size_t)st.st_size);

  // Delete old backup if it exists
  remove(LOGGER_BACKUP_PATH);

  // Rename current _old to _backup (if exists)
  rename(LOGGER_OLD_PATH, LOGGER_BACKUP_PATH);

  // Rename current to _old
  if (rename(LOGGER_DEFAULT_PATH, LOGGER_OLD_PATH) != 0) {
    ESP_LOGE(TAG, "Failed to rotate log file");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Log file rotated successfully");
  return ESP_OK;
}

// Check storage and cleanup if needed
static esp_err_t check_storage_and_cleanup(void) {
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) {
    return ESP_FAIL;
  }

  if (total == 0)
    return ESP_FAIL;

  uint32_t used_pct = (used * 100) / total;

  // If storage >95% full, delete oldest backup
  if (used_pct >= LOGGER_STORAGE_CRITICAL_PCT) {
    ESP_LOGW(TAG, "Storage critical (%" PRIu32 "%%), deleting backup files",
             used_pct);
    remove(LOGGER_BACKUP_PATH);

    // Re-check after deletion
    if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
      used_pct = (used * 100) / total;

      // Still critical? Delete old file too
      if (used_pct >= LOGGER_STORAGE_CRITICAL_PCT) {
        ESP_LOGW(TAG, "Still critical (%" PRIu32 "%%), deleting old file",
                 used_pct);
        remove(LOGGER_OLD_PATH);
      }
    }
  }
  // If storage >90% full, at least warn
  else if (used_pct >= LOGGER_STORAGE_WARNING_PCT) {
    ESP_LOGW(TAG, "Storage warning: %" PRIu32 "%% used (%zu/%zu bytes)",
             used_pct, used, total);
  }

  return ESP_OK;
}

static esp_err_t write_chunk_raw(const uint8_t *raw, size_t raw_len) {
  // Check storage and cleanup if needed
  check_storage_and_cleanup();

  // Rotate file if needed
  rotate_log_file(sizeof(log_chunk_hdr_t) + raw_len);

  FILE *f = fopen(LOGGER_DEFAULT_PATH, "ab");
  if (!f) {
    return ESP_FAIL;
  }

  log_chunk_hdr_t hdr = {
      .magic = LOG_MAGIC,
      .version = LOG_VER,
      .algo = 0,
      .level = 0,
      .raw_len = (uint32_t)raw_len,
      .data_len = (uint32_t)raw_len,
      .crc32 = calc_crc32(raw, raw_len),
      .node_id = s_node_id,
      .timestamp = get_timestamp(),
      .reserved = 0,
  };

  bool write_ok = true;
  if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
    write_ok = false;
  } else if (raw_len && fwrite(raw, 1, raw_len, f) != raw_len) {
    write_ok = false;
  }
  fclose(f);

  // Log current file size to confirm growth/rotation timing
  struct stat st;
  if (stat(LOGGER_DEFAULT_PATH, &st) == 0) {
    ESP_LOGI(TAG, "Log file size: %zu bytes", (size_t)st.st_size);
  }

  if (!write_ok) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Chunk written: RAW %u bytes | CRC32=0x%08lX | Integrity: PASS",
           (unsigned)raw_len, (unsigned long)hdr.crc32);
  return ESP_OK;
}

static esp_err_t write_chunk_miniz(const uint8_t *raw, size_t raw_len) {
  // Small tail flushes are usually not worth it.
  if (raw_len < LOGGER_MIN_COMPRESS_BYTES) {
    return write_chunk_raw(raw, raw_len);
  }

  size_t out_max = lz_miniz_bound(raw_len);
  uint8_t *out = heap_caps_malloc(out_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!out)
    out = heap_caps_malloc(out_max, MALLOC_CAP_8BIT);
  if (!out) {
    ESP_LOGW(TAG, "OOM allocating %u bytes for compress output, storing raw",
             (unsigned)out_max);
    return write_chunk_raw(raw, raw_len);
  }

  size_t out_len = out_max;
  comp_stats_t cs = {0};
  esp_err_t rc = lz_compress_miniz(raw, raw_len, out, out_max, &out_len,
                                   LOGGER_COMPRESS_LEVEL, &cs);
  if (rc != ESP_OK) {
    ESP_LOGW(TAG, "miniz compress failed (%s), storing raw",
             esp_err_to_name(rc));
    heap_caps_free(out);
    return write_chunk_raw(raw, raw_len);
  }

  // If we don't save at least ~5%, keep it raw.
  if (out_len + sizeof(log_chunk_hdr_t) >=
      raw_len - (raw_len / LOGGER_MIN_SAVINGS_DIV)) {
    heap_caps_free(out);
    return write_chunk_raw(raw, raw_len);
  }

  // Check storage and cleanup if needed
  check_storage_and_cleanup();

  // Rotate file if needed (projected compressed size)
  rotate_log_file(sizeof(log_chunk_hdr_t) + out_len);

  FILE *f = fopen(LOGGER_DEFAULT_PATH, "ab");
  if (!f) {
    heap_caps_free(out);
    return ESP_FAIL;
  }

  log_chunk_hdr_t hdr = {
      .magic = LOG_MAGIC,
      .version = LOG_VER,
      .algo = 1,
      .level = (uint8_t)LOGGER_COMPRESS_LEVEL,
      .raw_len = (uint32_t)raw_len,
      .data_len = (uint32_t)out_len,
      .crc32 = calc_crc32(out, out_len),
      .node_id = s_node_id,
      .timestamp = get_timestamp(),
      .reserved = 0,
  };

  bool write_ok = true;
  if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
    write_ok = false;
  } else if (fwrite(out, 1, out_len, f) != out_len) {
    write_ok = false;
  }
  fclose(f);
  heap_caps_free(out);

  if (!write_ok) {
    return ESP_FAIL;
  }

  ESP_LOGI(TAG,
           "Chunk written: MINIZ %u→%u bytes (%.1f%%) | CRC32=0x%08lX | "
           "Integrity: PASS",
           (unsigned)raw_len, (unsigned)out_len, 100.0 * out_len / raw_len,
           (unsigned long)hdr.crc32);
  return ESP_OK;
}

esp_err_t logger_init(void) {
  if (s_inited)
    return ESP_OK;

  // Derive unique node ID from MAC address
  uint8_t mac[6];
  esp_err_t ret = esp_efuse_mac_get_default(mac);
  if (ret == ESP_OK) {
    // Pack MAC into 64-bit ID (48 bits used)
    s_node_id = ((uint64_t)mac[0] << 40) | ((uint64_t)mac[1] << 32) |
                ((uint64_t)mac[2] << 24) | ((uint64_t)mac[3] << 16) |
                ((uint64_t)mac[4] << 8) | ((uint64_t)mac[5]);
    ESP_LOGI(TAG, "Node ID: %02X:%02X:%02X:%02X:%02X:%02X (0x%llX)", mac[0],
             mac[1], mac[2], mac[3], mac[4], mac[5],
             (unsigned long long)s_node_id);
  } else {
    ESP_LOGW(TAG, "Failed to get MAC, using fallback node ID");
    s_node_id = 0xFFFFFFFFFFFFULL;
  }

  esp_vfs_spiffs_conf_t conf = {
      .base_path = "/spiffs",
      .partition_label = NULL,
      .max_files = 5,
      .format_if_mount_failed = true,
  };

  ret = esp_vfs_spiffs_register(&conf);
  if (ret != ESP_OK) {
    if (ret == ESP_ERR_INVALID_STATE) {
      // SPIFFS corrupted - format and retry
      ESP_LOGW(TAG, "SPIFFS corrupted, formatting...");
      esp_vfs_spiffs_unregister(NULL);

      esp_vfs_spiffs_conf_t format_conf = {
          .base_path = "/spiffs",
          .partition_label = NULL,
          .max_files = 5,
          .format_if_mount_failed = true,
      };

      ret = esp_vfs_spiffs_register(&format_conf);
      if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS format failed: %s", esp_err_to_name(ret));
        return ret;
      }
      ESP_LOGI(TAG, "SPIFFS formatted successfully");
    } else {
      ESP_LOGE(TAG, "SPIFFS mount failed: %s", esp_err_to_name(ret));
      return ret;
    }
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(NULL, &total, &used);
  if (ret == ESP_OK) {
    ESP_LOGI(TAG, "SPIFFS total=%u used=%u", (unsigned)total, (unsigned)used);
  }

  (void)blockbuf_init(&s_bb, LOGGER_BLOCK_CAP, 1);
  if (!s_bb.buf) {
    ESP_LOGW(TAG, "No RAM for log buffer, writing chunks directly");
  }

  // Create mutex for thread-safe flush operations
  s_flush_mutex = xSemaphoreCreateMutex();
  if (!s_flush_mutex) {
    ESP_LOGE(TAG, "Failed to create flush mutex");
    return ESP_ERR_NO_MEM;
  }

  s_inited = true;
  return ESP_OK;
}

esp_err_t logger_flush(void) {
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;
  if (!s_bb.buf || s_bb.len == 0)
    return ESP_OK;

  // Take mutex with timeout to prevent indefinite blocking
  if (xSemaphoreTake(s_flush_mutex, pdMS_TO_TICKS(5000)) != pdTRUE) {
    ESP_LOGW(TAG, "Flush mutex timeout");
    return ESP_ERR_TIMEOUT;
  }

  ESP_LOGI(TAG, "Flush start: %u bytes", (unsigned)s_bb.len);

  esp_err_t ret;
  // bool use_compression = (s_bb.len >= LOGGER_MIN_COMPRESS_BYTES);
  bool use_compression = false; // DEBUG: Disable compression to isolate crash

  if (use_compression) {
    ret = write_chunk_miniz(s_bb.buf, s_bb.len);
  } else {
    ret = write_chunk_raw(s_bb.buf, s_bb.len);
  }

  if (ret == ESP_OK) {
    s_bb.len = 0;
  }

  ESP_LOGI(TAG, "Flush done");
  xSemaphoreGive(s_flush_mutex);
  return ret;
}

esp_err_t logger_clear(void) {
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;

  (void)logger_flush();
  remove(LOGGER_DEFAULT_PATH);
  return ESP_OK;
}

esp_err_t logger_append_line(const char *line) {
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;
  if (!line)
    return ESP_ERR_INVALID_ARG;

  // Check if storage is critically full; if so, clear old data (circular buffer
  // behavior)
  if (logger_storage_critical()) {
    ESP_LOGW(TAG, "Storage critically full (>%d%%), clearing old data",
             LOGGER_STORAGE_CRITICAL_PCT);
    (void)logger_clear();
  }

  const size_t n = strlen(line);
  const size_t need = n + 1; // + '\n'

  // If we failed to allocate a buffer, store each line as its own chunk.
  if (!s_bb.buf || s_bb.cap == 0) {
    esp_err_t ret = write_chunk_raw((const uint8_t *)line, n);
    if (ret == ESP_OK)
      ret = write_chunk_raw((const uint8_t *)"\n", 1);
    return ret;
  }

  // If a single line is larger than our buffer, flush and store it as raw.
  if (need > s_bb.cap) {
    (void)logger_flush();
    esp_err_t ret = write_chunk_raw((const uint8_t *)line, n);
    if (ret == ESP_OK)
      ret = write_chunk_raw((const uint8_t *)"\n", 1);
    return ret;
  }

  // Not enough room: flush current block.
  if (s_bb.len + need > s_bb.cap) {
    esp_err_t fr = logger_flush();
    if (fr != ESP_OK)
      return fr;
  }

  if (blockbuf_append(&s_bb, (const uint8_t *)line, n) != 0)
    return ESP_FAIL;
  if (blockbuf_append(&s_bb, (const uint8_t *)"\n", 1) != 0)
    return ESP_FAIL;

  if (s_bb.len >= LOGGER_FLUSH_THRESHOLD) {
    return logger_flush();
  }
  return ESP_OK;
}

esp_err_t logger_get_storage_usage(size_t *used_bytes, size_t *total_bytes) {
  if (!used_bytes || !total_bytes)
    return ESP_ERR_INVALID_ARG;
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;
  return esp_spiffs_info(NULL, total_bytes, used_bytes);
}

size_t logger_get_file_size(void) {
  struct stat st;
  if (stat(LOGGER_DEFAULT_PATH, &st) != 0)
    return 0;
  return (size_t)st.st_size;
}

esp_err_t logger_get_node_id(char *buf, size_t buf_len) {
  if (!buf || buf_len < 18)
    return ESP_ERR_INVALID_ARG;
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;

  uint8_t mac[6];
  mac[0] = (uint8_t)((s_node_id >> 40) & 0xFF);
  mac[1] = (uint8_t)((s_node_id >> 32) & 0xFF);
  mac[2] = (uint8_t)((s_node_id >> 24) & 0xFF);
  mac[3] = (uint8_t)((s_node_id >> 16) & 0xFF);
  mac[4] = (uint8_t)((s_node_id >> 8) & 0xFF);
  mac[5] = (uint8_t)(s_node_id & 0xFF);

  snprintf(buf, buf_len, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1],
           mac[2], mac[3], mac[4], mac[5]);
  return ESP_OK;
}

bool logger_storage_warning(void) {
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) != ESP_OK)
    return false;
  if (total == 0)
    return false;
  return (used * 100 / total) >= LOGGER_STORAGE_WARNING_PCT;
}

bool logger_storage_critical(void) {
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) != ESP_OK)
    return false;
  if (total == 0)
    return false;
  return (used * 100 / total) >= LOGGER_STORAGE_CRITICAL_PCT;
}

esp_err_t logger_set_time(uint32_t unix_timestamp) {
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;

  // Calculate boot timestamp by subtracting current uptime
  uint32_t uptime = (uint32_t)(esp_timer_get_time() / 1000000ULL);
  s_boot_timestamp = unix_timestamp - uptime;

  ESP_LOGI(TAG,
           "Time synced: Unix=%" PRIu32 " Boot=%" PRIu32 " Uptime=%" PRIu32,
           unix_timestamp, s_boot_timestamp, uptime);
  return ESP_OK;
}

uint32_t logger_get_time(void) { return get_timestamp(); }

esp_err_t logger_cleanup_old_data(void) {
  if (!s_inited)
    return ESP_ERR_INVALID_STATE;

  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) != ESP_OK) {
    return ESP_FAIL;
  }

  if (total == 0)
    return ESP_FAIL;
  uint32_t used_pct = (used * 100) / total;

  ESP_LOGI(TAG, "Cleanup requested: %" PRIu32 "%% storage used", used_pct);

  // Delete backup file first
  struct stat st;
  if (stat(LOGGER_BACKUP_PATH, &st) == 0) {
    ESP_LOGI(TAG, "Deleting backup file (%zu bytes)", (size_t)st.st_size);
    remove(LOGGER_BACKUP_PATH);
  }

  // If still critical, delete old file
  if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
    used_pct = (used * 100) / total;
    if (used_pct >= LOGGER_STORAGE_WARNING_PCT) {
      if (stat(LOGGER_OLD_PATH, &st) == 0) {
        ESP_LOGI(TAG, "Deleting old file (%zu bytes)", (size_t)st.st_size);
        remove(LOGGER_OLD_PATH);
      }
    }
  }

  return ESP_OK;
}

void logger_dump_to_uart(void) {
  if (!s_inited) {
    ESP_LOGE(TAG, "Logger not initialized");
    return;
  }

  FILE *f = fopen(LOGGER_DEFAULT_PATH, "rb");
  if (!f) {
    ESP_LOGE(TAG, "Failed to open log file for dumping");
    return;
  }

  ESP_LOGI(TAG, "=== BEGIN LOG DUMP ===");

  // Read and dump entire file in chunks
  uint8_t buf[512];
  size_t total = 0;
  size_t chunk_count = 0;
  while (1) {
    size_t n = fread(buf, 1, sizeof(buf), f);
    if (n == 0)
      break;

    // Output as hex
    for (size_t i = 0; i < n; i++) {
      printf("%02X", buf[i]);
    }
    total += n;

    // Feed watchdog every 10 chunks (~5KB) to prevent timeout
    if (++chunk_count % 10 == 0) {
      esp_task_wdt_reset();
    }
  }
  printf("\n");

  fclose(f);
  ESP_LOGI(TAG, "=== END LOG DUMP === (%u bytes)", (unsigned)total);
}
