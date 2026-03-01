#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Default log file path (binary chunks, some may be compressed)
#define LOGGER_DEFAULT_PATH "/spiffs/samples.lz"

// Mount SPIFFS and prepare logger
esp_err_t logger_init(void);

// Append a line to the log file (adds '\n' automatically)
esp_err_t logger_append_line(const char *line);

// Flush any buffered log bytes to SPIFFS. Call this before deep sleep.
esp_err_t logger_flush(void);

// Optional helpers
esp_err_t logger_clear(void);
esp_err_t logger_get_storage_usage(size_t *used_bytes, size_t *total_bytes);
size_t logger_get_file_size(void);

// Get unique node ID (derived from MAC address)
esp_err_t logger_get_node_id(char *buf, size_t buf_len);

// Check if storage is near full (>90% used) or critically full (>95%)
bool logger_storage_warning(void);
bool logger_storage_critical(void);

// RTC time sync: set current Unix timestamp (seconds since 1970)
esp_err_t logger_set_time(uint32_t unix_timestamp);

// Get current Unix timestamp (0 if not synced)
uint32_t logger_get_time(void);

// Circular buffer: delete oldest chunks if storage >90% full
esp_err_t logger_cleanup_old_data(void);

// Dump entire log file to UART for extraction
void logger_dump_to_uart(void);

#ifdef __cplusplus
}
#endif