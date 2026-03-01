#include "storage_manager.h"
#include "compression.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "rom/crc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>
#include <unistd.h>

static const char *TAG = "STORAGE";

#define BASE_PATH "/spiffs"
#define DATA_FILE "/spiffs/data.txt"
#define DATA_FILE_COMPRESSED "/spiffs/data.lz"
#define QUEUE_FILE "/spiffs/queue.txt"

// Compression configuration
#define COMPRESSION_MIN_BYTES 1024        // Only compress if data >= 1KB
#define COMPRESSION_MIN_SAVINGS_DIV 20     // Require >5% savings
#define COMPRESSION_LEVEL 3               // Balanced compression level
#define COMPRESSION_MIN_STACK_FREE 1024   // Disable compression if <1KB stack free

// MSLG Chunk Header (32 bytes)
typedef struct __attribute__((packed)) {
    uint32_t magic;     // 0x4D534C47 ('MSLG')
    uint16_t version;   // Format version (2)
    uint8_t  algo;      // 0=raw, 1=miniz(deflate)
    uint8_t  level;     // Compression level (1-9)
    uint32_t raw_len;   // Original data size before compression
    uint32_t data_len;  // Actual stored data size after header
    uint32_t crc32;     // CRC32 checksum of payload data
    uint64_t node_id;   // Unique node identifier (from MAC address)
    uint32_t timestamp; // Seconds since boot (can be synced)
    uint32_t reserved;  // Reserved for future use
} mslg_chunk_hdr_t;

static const uint32_t MSLG_MAGIC = 0x4D534C47U; // MSLG
static const uint16_t MSLG_VERSION = 2;
static uint64_t s_node_id = 0;  // Cached node ID

// Future: Add SD Card support here
// static bool using_sd_card = false;
void storage_manager_display_status(void) {
  size_t total = 0, used = 0;
  if (esp_spiffs_info(NULL, &total, &used) == ESP_OK) {
    float total_mb = total / (1024.0f * 1024.0f);
    float free_mb = (total - used) / (1024.0f * 1024.0f);
    uint32_t used_pct = (total > 0) ? ((used * 100) / total) : 0;

    printf("Storage: %.3f MB / %.3f MB remaining\n", free_mb, total_mb);

    if (used_pct >= 90) {
      printf("WARNING: Storage is 90%% full or more! Please offload data.\n");
    }
  } else {
    ESP_LOGE(TAG, "Failed to get SPIFFS info for status display");
  }
}

// Calculate CRC32 of data
static uint32_t calc_crc32(const uint8_t *data, size_t len) {
    return crc32_le(0, data, len);
}

// Get node ID from MAC address
static uint64_t get_node_id_from_mac(void) {
    if (s_node_id != 0) {
        return s_node_id;  // Return cached value
    }
    
    uint8_t mac[6];
    esp_err_t ret = esp_efuse_mac_get_default(mac);
    if (ret == ESP_OK) {
        // Pack MAC into 64-bit ID (48 bits used)
        s_node_id = ((uint64_t)mac[0] << 40) |
                    ((uint64_t)mac[1] << 32) |
                    ((uint64_t)mac[2] << 24) |
                    ((uint64_t)mac[3] << 16) |
                    ((uint64_t)mac[4] << 8) |
                    ((uint64_t)mac[5]);
        ESP_LOGI(TAG, "Node ID: %02X:%02X:%02X:%02X:%02X:%02X (0x%llX)",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                 (unsigned long long)s_node_id);
    } else {
        ESP_LOGW(TAG, "Failed to get MAC, using fallback node ID");
        s_node_id = 0xFFFFFFFFFFFFULL;
    }
    return s_node_id;
}

// Get current timestamp (seconds since boot)
static uint32_t get_timestamp(void) {
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

esp_err_t storage_manager_init(void) {
  ESP_LOGI(TAG, "Initializing SPIFFS...");

  esp_vfs_spiffs_conf_t conf = {.base_path = BASE_PATH,
                                .partition_label = NULL,
                                .max_files = 5,
                                .format_if_mount_failed = true};

  esp_err_t ret = esp_vfs_spiffs_register(&conf);

  if (ret != ESP_OK) {
    if (ret == ESP_ERR_INVALID_STATE) {
      ESP_LOGW(TAG, "SPIFFS already mounted by Logger, skipping registration");
      ret = ESP_OK;
    } else if (ret == ESP_FAIL) {
      ESP_LOGE(TAG, "Failed to mount or format filesystem");
    } else if (ret == ESP_ERR_NOT_FOUND) {
      ESP_LOGE(TAG, "Failed to find SPIFFS partition");
    } else {
      ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
    }
    if (ret != ESP_OK)
      return ret;
  }

  size_t total = 0, used = 0;
  ret = esp_spiffs_info(NULL, &total, &used);
  if (ret != ESP_OK) {
    ESP_LOGE(TAG, "Failed to get SPIFFS info (%s)", esp_err_to_name(ret));
  } else {
    ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);
  }

  // Initialize node ID
  (void)get_node_id_from_mac();

  return ESP_OK;
}

// Write MSLG chunk (raw or compressed)
static esp_err_t write_mslg_chunk(const uint8_t *data, size_t data_len, 
                                   uint32_t raw_len, bool compressed, 
                                   uint8_t *compressed_buf, size_t compressed_len) {
    FILE *f = fopen(DATA_FILE_COMPRESSED, "ab");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open compressed data file for writing");
        return ESP_FAIL;
    }

    const uint8_t *payload = compressed ? compressed_buf : data;
    size_t payload_len = compressed ? compressed_len : data_len;
    
    mslg_chunk_hdr_t hdr = {
        .magic = MSLG_MAGIC,
        .version = MSLG_VERSION,
        .algo = compressed ? 1 : 0,
        .level = compressed ? COMPRESSION_LEVEL : 0,
        .raw_len = raw_len,
        .data_len = (uint32_t)payload_len,
        .crc32 = calc_crc32(payload, payload_len),
        .node_id = get_node_id_from_mac(),
        .timestamp = get_timestamp(),
        .reserved = 0,
    };

    bool write_ok = true;
    if (fwrite(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        write_ok = false;
    } else if (payload_len > 0 && fwrite(payload, 1, payload_len, f) != payload_len) {
        write_ok = false;
    }
    fclose(f);

    if (!write_ok) {
        ESP_LOGE(TAG, "Failed to write MSLG chunk");
        return ESP_FAIL;
    }

    if (compressed) {
        ESP_LOGI(TAG, "MSLG chunk written: COMPRESSED %u→%u bytes (%.1f%%) | CRC32=0x%08X",
                 (unsigned)raw_len, (unsigned)payload_len, 
                 100.0f * payload_len / raw_len, (unsigned)hdr.crc32);
    } else {
        ESP_LOGI(TAG, "MSLG chunk written: RAW %u bytes | CRC32=0x%08X",
                 (unsigned)raw_len, (unsigned)hdr.crc32);
    }
    return ESP_OK;
}

esp_err_t storage_manager_write(const char *data) {
  if (!data)
    return ESP_ERR_INVALID_ARG;

  // Backward compatibility: Write plain text format
  // Use storage_manager_write_compressed() for MSLG format with compression
  FILE *f = fopen(DATA_FILE, "a");
  if (f == NULL) {
    ESP_LOGE(TAG, "Failed to open file for writing");
    return ESP_FAIL;
  }

  fprintf(f, "%s\n", data);
  fclose(f);
  return ESP_OK;
}

size_t storage_manager_read_all(char *buffer, size_t max_len) {
  if (!buffer || max_len == 0)
    return 0;

  FILE *f = fopen(DATA_FILE, "r");
  if (f == NULL) {
    ESP_LOGW(TAG, "No data file found to read");
    return 0;
  }

  size_t read_bytes = fread(buffer, 1, max_len - 1, f);
  buffer[read_bytes] = 0; // Null terminate
  fclose(f);
  return read_bytes;
}

esp_err_t storage_manager_clear(void) {
  if (unlink(DATA_FILE) == 0) {
    ESP_LOGI(TAG, "Data file deleted");
  }
  if (unlink(DATA_FILE_COMPRESSED) == 0) {
    ESP_LOGI(TAG, "Compressed data file deleted");
  }
  // If file doesn't exist, unlink returns -1 but errno=ENOENT. We consider that success.
  return ESP_OK;
}

esp_err_t storage_manager_get_usage(size_t *used_out, size_t *total_out) {
  return esp_spiffs_info(NULL, total_out, used_out);
}

esp_err_t storage_manager_pop_line(char *buffer, size_t max_len) {
  if (!buffer || max_len == 0)
    return ESP_ERR_INVALID_ARG;

  // Rename current file to temp
  struct stat st;
  if (stat(DATA_FILE, &st) != 0) {
    return ESP_ERR_NOT_FOUND; // File doesn't exist
  }

  // Creating a temp file for read/write
  const char *TEMP_FILE = "/spiffs/temp.txt";

  // Open original file for reading
  FILE *in = fopen(DATA_FILE, "r");
  if (!in)
    return ESP_FAIL;

  // Read first line
  if (!fgets(buffer, max_len, in)) {
    // Empty file or error
    fclose(in);
    unlink(DATA_FILE); // Helper: just delete empty file
    return ESP_ERR_NOT_FOUND;
  }

  // Remove newline if present
  size_t len = strlen(buffer);
  if (len > 0 && buffer[len - 1] == '\n') {
    buffer[len - 1] = '\0';
  }

  // Open temp file for writing remaining lines
  FILE *out = fopen(TEMP_FILE, "w");
  if (!out) {
    fclose(in);
    return ESP_FAIL;
  }

  // Copy rest of file
  char temp_buf[256];
  while (fgets(temp_buf, sizeof(temp_buf), in)) {
    fputs(temp_buf, out);
  }

  fclose(in);
  fclose(out);

  // atomic replace: delete old, rename temp to old
  unlink(DATA_FILE);
  rename(TEMP_FILE, DATA_FILE);

  return ESP_OK;
}

// ----------------------------------------------------------------------
// Store-First: Rename & Drain (Phase 28)
// ----------------------------------------------------------------------

esp_err_t storage_manager_prepare_upload(void) {
  // Check if DATA file exists and is not empty
  struct stat st;
  if (stat(DATA_FILE, &st) != 0 || st.st_size == 0) {
    return ESP_ERR_NOT_FOUND; // Nothing to upload
  }

  // If QUEUE file already exists, we must process it first.
  // Don't overwrite it!
  if (stat(QUEUE_FILE, &st) == 0) {
    return ESP_OK; // Queue ready
  }

  // Rename DATA -> QUEUE
  if (rename(DATA_FILE, QUEUE_FILE) != 0) {
    ESP_LOGE(TAG, "Failed to rename data file to queue");
    return ESP_FAIL;
  }

  ESP_LOGI(TAG, "Renamed data.txt -> queue.txt for upload");
  return ESP_OK;
}

esp_err_t storage_manager_remove_queue(void) {
  unlink(QUEUE_FILE);
  return ESP_OK;
}

FILE *storage_manager_open_queue(void) { return fopen(QUEUE_FILE, "r"); }

// ----------------------------------------------------------------------
// Compression Support Implementation
// ----------------------------------------------------------------------

esp_err_t storage_manager_write_compressed(const char *data, bool enable_compression) {
    if (!data) {
        return ESP_ERR_INVALID_ARG;
    }
    
    // Check stack availability
    UBaseType_t stack_free = uxTaskGetStackHighWaterMark(NULL);
    if (stack_free < COMPRESSION_MIN_STACK_FREE) {
        ESP_LOGW(TAG, "Low stack (%u bytes), forcing raw storage", stack_free);
        enable_compression = false;
    }
    
    size_t data_len = strlen(data);
    
    // Only compress if enabled and data is large enough
    if (enable_compression && data_len >= COMPRESSION_MIN_BYTES) {
        // Allocate compression buffer on HEAP (not stack!)
        size_t out_max = lz_miniz_bound(data_len);
        uint8_t *compressed = heap_caps_malloc(out_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!compressed) {
            compressed = heap_caps_malloc(out_max, MALLOC_CAP_8BIT);
        }
        
        if (compressed) {
            size_t compressed_len = out_max;
            comp_stats_t stats = {0};
            esp_err_t ret = lz_compress_miniz((const uint8_t *)data, data_len,
                                               compressed, out_max, &compressed_len,
                                               COMPRESSION_LEVEL, &stats);
            
            // Only use compression if it saves >5% space
            size_t overhead = sizeof(mslg_chunk_hdr_t) + compressed_len;
            size_t raw_overhead = sizeof(mslg_chunk_hdr_t) + data_len;
            
            if (ret == ESP_OK && overhead < (raw_overhead - (raw_overhead / COMPRESSION_MIN_SAVINGS_DIV))) {
                // Write compressed chunk
                esp_err_t write_ret = write_mslg_chunk((const uint8_t *)data, data_len,
                                                         data_len, true, compressed, compressed_len);
                heap_caps_free(compressed);
                return write_ret;
            } else {
                // Compression didn't save enough, fallback to raw
                heap_caps_free(compressed);
            }
        }
    }
    
    // Fallback to raw storage
    return write_mslg_chunk((const uint8_t *)data, data_len, data_len, false, NULL, 0);
}

esp_err_t storage_manager_read_all_decompressed(char **buffer, size_t max_len, size_t *bytes_read_out) {
    if (!buffer || !bytes_read_out || max_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *buffer = NULL;
    *bytes_read_out = 0;
    
    // Try MSLG compressed format first
    FILE *f = fopen(DATA_FILE_COMPRESSED, "rb");
    if (f) {
        // Allocate output buffer on HEAP
        char *out_buf = heap_caps_malloc(max_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!out_buf) {
            out_buf = heap_caps_malloc(max_len, MALLOC_CAP_8BIT);
        }
        
        if (!out_buf) {
            fclose(f);
            ESP_LOGE(TAG, "Failed to allocate decompression buffer");
            return ESP_ERR_NO_MEM;
        }
        
        size_t total_written = 0;
        
        // Read MSLG chunks
        while (total_written < max_len - 1) {
            mslg_chunk_hdr_t hdr;
            if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
                break;  // EOF or error
            }
            
            if (hdr.magic != MSLG_MAGIC) {
                ESP_LOGW(TAG, "Invalid MSLG magic, stopping read");
                break;
            }
            
            // Read payload
            uint8_t *chunk_data = heap_caps_malloc(hdr.data_len, MALLOC_CAP_8BIT);
            if (!chunk_data) {
                ESP_LOGE(TAG, "Failed to allocate chunk buffer");
                break;
            }
            
            if (fread(chunk_data, 1, hdr.data_len, f) != hdr.data_len) {
                heap_caps_free(chunk_data);
                break;
            }
            
            // Verify CRC32
            uint32_t calc_crc = calc_crc32(chunk_data, hdr.data_len);
            if (calc_crc != hdr.crc32) {
                ESP_LOGW(TAG, "CRC32 mismatch (expected 0x%08X, got 0x%08X), skipping chunk",
                         (unsigned)hdr.crc32, (unsigned)calc_crc);
                heap_caps_free(chunk_data);
                continue;
            }
            
            // Decompress if needed
            uint8_t *decompressed = chunk_data;
            size_t decompressed_len = hdr.data_len;
            
            if (hdr.algo == 1) {  // Compressed
                decompressed = heap_caps_malloc(hdr.raw_len, MALLOC_CAP_8BIT);
                if (!decompressed) {
                    ESP_LOGE(TAG, "Failed to allocate decompression buffer");
                    heap_caps_free(chunk_data);
                    break;
                }
                
                size_t out_len = hdr.raw_len;
                comp_stats_t stats = {0};
                esp_err_t ret = lz_decompress_miniz(chunk_data, hdr.data_len,
                                                      decompressed, hdr.raw_len,
                                                      &out_len, &stats);
                
                if (ret != ESP_OK || out_len != hdr.raw_len) {
                    ESP_LOGE(TAG, "Decompression failed or size mismatch");
                    heap_caps_free(decompressed);
                    heap_caps_free(chunk_data);
                    break;
                }
                
                decompressed_len = out_len;
                heap_caps_free(chunk_data);  // Free compressed data
            }
            
            // Append to output buffer
            size_t copy_len = (decompressed_len < (max_len - total_written - 1)) 
                              ? decompressed_len : (max_len - total_written - 1);
            memcpy(out_buf + total_written, decompressed, copy_len);
            total_written += copy_len;
            
            heap_caps_free(decompressed);
            
            // Stop if we've filled the buffer
            if (total_written >= max_len - 1) {
                break;
            }
        }
        
        fclose(f);
        
        if (total_written > 0) {
            out_buf[total_written] = '\0';  // Null terminate
            *buffer = out_buf;
            *bytes_read_out = total_written;
            return ESP_OK;
        } else {
            heap_caps_free(out_buf);
        }
    }
    
    // Fallback to plain-text format
    FILE *plain_f = fopen(DATA_FILE, "r");
    if (plain_f) {
        char *out_buf = heap_caps_malloc(max_len, MALLOC_CAP_8BIT);
        if (out_buf) {
            size_t read_bytes = fread(out_buf, 1, max_len - 1, plain_f);
            out_buf[read_bytes] = '\0';
            fclose(plain_f);
            *buffer = out_buf;
            *bytes_read_out = read_bytes;
            return ESP_OK;
        }
        fclose(plain_f);
    }
    
    return ESP_ERR_NOT_FOUND;
}

esp_err_t storage_manager_get_compression_stats(size_t *raw_bytes_out, size_t *compressed_bytes_out, float *compression_ratio_pct) {
    if (!raw_bytes_out || !compressed_bytes_out || !compression_ratio_pct) {
        return ESP_ERR_INVALID_ARG;
    }
    
    *raw_bytes_out = 0;
    *compressed_bytes_out = 0;
    *compression_ratio_pct = 0.0f;
    
    FILE *f = fopen(DATA_FILE_COMPRESSED, "rb");
    if (!f) {
        return ESP_ERR_NOT_FOUND;
    }
    
    while (1) {
        mslg_chunk_hdr_t hdr;
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
            break;
        }
        
        if (hdr.magic != MSLG_MAGIC) {
            break;
        }
        
        *raw_bytes_out += hdr.raw_len;
        *compressed_bytes_out += hdr.data_len;
        
        // Skip payload
        fseek(f, hdr.data_len, SEEK_CUR);
    }
    
    fclose(f);
    
    if (*raw_bytes_out > 0) {
        *compression_ratio_pct = 100.0f * (*compressed_bytes_out) / (*raw_bytes_out);
    }
    
    return ESP_OK;
}

// ----------------------------------------------------------------------
// Pop-first MSLG chunk implementation
// ----------------------------------------------------------------------
esp_err_t storage_manager_pop_mslg_chunk(uint8_t **out_payload, size_t *out_payload_len,
                    uint32_t *out_raw_len, uint8_t *out_algo,
                    uint32_t *out_timestamp) {
  if (!out_payload || !out_payload_len) {
    return ESP_ERR_INVALID_ARG;
  }

  *out_payload = NULL;
  *out_payload_len = 0;
  if (out_raw_len) *out_raw_len = 0;
  if (out_algo) *out_algo = 0;
  if (out_timestamp) *out_timestamp = 0;

  FILE *f = fopen(DATA_FILE_COMPRESSED, "rb");
  if (!f) {
    return ESP_ERR_NOT_FOUND;
  }

  // Read first header
  mslg_chunk_hdr_t hdr;
  if (fread(&hdr, sizeof(hdr), 1, f) != 1) {
    fclose(f);
    return ESP_ERR_NOT_FOUND;
  }

  if (hdr.magic != MSLG_MAGIC) {
    ESP_LOGW(TAG, "Invalid MSLG magic while popping chunk");
    fclose(f);
    return ESP_ERR_INVALID_STATE;
  }

  // Read payload
  uint8_t *payload = heap_caps_malloc(hdr.data_len, MALLOC_CAP_8BIT);
  if (!payload) {
    // Try SPIRAM as fallback
    payload = heap_caps_malloc(hdr.data_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
      fclose(f);
      return ESP_ERR_NO_MEM;
    }
  }

  if (fread(payload, 1, hdr.data_len, f) != hdr.data_len) {
    heap_caps_free(payload);
    fclose(f);
    return ESP_FAIL;
  }

  // Now copy remaining bytes to a temp file
  const char *TEMP_FILE = "/spiffs/temp.mslg";
  FILE *out = fopen(TEMP_FILE, "wb");
  if (!out) {
    heap_caps_free(payload);
    fclose(f);
    return ESP_FAIL;
  }

  uint8_t buf[1024];
  size_t r;
  while ((r = fread(buf, 1, sizeof(buf), f)) > 0) {
    if (fwrite(buf, 1, r, out) != r) {
      heap_caps_free(payload);
      fclose(f);
      fclose(out);
      unlink(TEMP_FILE);
      return ESP_FAIL;
    }
  }

  fclose(f);
  fclose(out);

  // Replace original file with temp (atomic-ish)
  // Remove original and rename temp
  unlink(DATA_FILE_COMPRESSED);
  if (rename(TEMP_FILE, DATA_FILE_COMPRESSED) != 0) {
    // If rename failed, try to restore by deleting temp
    unlink(TEMP_FILE);
    heap_caps_free(payload);
    return ESP_FAIL;
  }

  // Success: set outputs
  *out_payload = payload;
  *out_payload_len = hdr.data_len;
  if (out_raw_len) *out_raw_len = hdr.raw_len;
  if (out_algo) *out_algo = hdr.algo;
  if (out_timestamp) *out_timestamp = hdr.timestamp;

  ESP_LOGI(TAG, "Popped MSLG chunk: algo=%u raw=%u stored=%u ts=%u",
       hdr.algo, (unsigned)hdr.raw_len, (unsigned)hdr.data_len,
       (unsigned)hdr.timestamp);

  return ESP_OK;
}
