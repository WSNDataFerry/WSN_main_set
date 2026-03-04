#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h> // For FILE*

/**
 * @brief Initialize storage manager (mounts SPIFFS and/or SD Card)
 * @return ESP_OK or error
 */
esp_err_t storage_manager_init(void);

/**
 * @brief Write data line to storage (appends newline)
 * @param data String data to write
 * @return ESP_OK on success
 */
esp_err_t storage_manager_write(const char *data);

/**
 * @brief Read all data from storage (for UAV upload)
 * @param buffer Output buffer
 * @param max_len Buffer size
 * @return Number of bytes read
 */
size_t storage_manager_read_all(char *buffer, size_t max_len);

/**
 * @brief Clear all stored data
 * @return ESP_OK on success
 */
esp_err_t storage_manager_clear(void);

/**
 * @brief Get storage usage stats
 * @param used_out Bytes used
 * @param total_out Total bytes
 * @return ESP_OK on success
 */
esp_err_t storage_manager_get_usage(size_t *used_out, size_t *total_out);

/**
 * @brief Read and remove the oldest line from storage
 * @param buffer Output buffer
 * @param max_len Buffer size
 * @return ESP_OK if line popped, ESP_FAIL on error, ESP_ERR_NOT_FOUND if empty
 */
esp_err_t storage_manager_pop_line(char *buffer, size_t max_len);

/**
 * @brief Rename data.txt to queue.txt for batch upload
 * @return ESP_OK if renamed or queue already exists, ESP_ERR_NOT_FOUND if no
 * data
 */
esp_err_t storage_manager_prepare_upload(void);

/**
 * @brief Open the queue file for reading
 * @return FILE* pointer or NULL
 */
FILE *storage_manager_open_queue(void);

/**
 * @brief Delete the queue file (after successful upload)
 * @return ESP_OK
 */
esp_err_t storage_manager_remove_queue(void);

// ----------------------------------------------------------------------
// Compression Support (MSLG Format)
// ----------------------------------------------------------------------

/**
 * @brief Write data with optional compression (MSLG format)
 * @param data String data to write
 * @param enable_compression Enable compression if data >= threshold
 * @return ESP_OK on success
 */
esp_err_t storage_manager_write_compressed(const char *data, bool enable_compression);

/**
 * @brief Read all data from storage and decompress if needed
 * @param buffer Output buffer (heap-allocated, caller must free)
 * @param max_len Maximum buffer size
 * @param bytes_read_out Actual bytes read (output)
 * @return ESP_OK on success, ESP_FAIL on error
 * @note Buffer is heap-allocated, caller must free() it
 */
esp_err_t storage_manager_read_all_decompressed(char **buffer, size_t max_len, size_t *bytes_read_out);

/**
 * @brief Get compression statistics
 * @param raw_bytes_out Total raw bytes stored
 * @param compressed_bytes_out Total compressed bytes stored
 * @param compression_ratio_pct Compression ratio percentage (output)
 * @return ESP_OK on success
 */
esp_err_t storage_manager_get_compression_stats(size_t *raw_bytes_out, size_t *compressed_bytes_out, float *compression_ratio_pct);

/**
 * @brief Pop the oldest MSLG chunk from the compressed data file.
 *
 * The function removes the first MSLG chunk from the file and returns its
 * payload (heap-allocated). Caller must free(*out_payload).
 *
 * @param out_payload Pointer to buffer pointer that will be allocated and set
 * @param out_payload_len Length of the payload in bytes
 * @param out_raw_len Original uncompressed length (hdr.raw_len)
 * @param out_algo Algorithm indicator (0=raw,1=miniz)
 * @param out_timestamp Header timestamp (seconds)
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if file is empty
 */
esp_err_t storage_manager_pop_mslg_chunk(uint8_t **out_payload, size_t *out_payload_len,
										uint32_t *out_raw_len, uint8_t *out_algo,
										uint32_t *out_timestamp);

/**
 * @brief Descriptor for a single MSLG chunk returned by batch pop.
 * Caller must free(payload) for each entry.
 */
typedef struct {
    uint8_t *payload;      /**< Heap-allocated compressed/raw data */
    size_t   payload_len;  /**< Bytes in payload */
    uint32_t raw_len;      /**< Original uncompressed length */
    uint8_t  algo;         /**< 0=raw, 1=miniz */
    uint32_t timestamp;    /**< Header timestamp (seconds since boot) */
} mslg_popped_chunk_t;

/**
 * @brief Pop up to max_chunks MSLG chunks in ONE SPIFFS pass.
 *
 * Reads N chunk headers + payloads sequentially, copies the remaining
 * tail of the file once, then replaces the original.  This is O(1) file
 * rewrites vs O(N) for calling pop_mslg_chunk() N times.
 *
 * @param[out] out_chunks   Caller-provided array of mslg_popped_chunk_t
 * @param      max_chunks   Size of out_chunks array
 * @param[out] out_count    Number of chunks actually popped
 * @return ESP_OK, ESP_ERR_NOT_FOUND (file empty), or error
 */
esp_err_t storage_manager_pop_mslg_chunks_batch(mslg_popped_chunk_t *out_chunks,
                                                 int max_chunks,
                                                 int *out_count);


/**
 * @brief Count the number of MSLG chunks currently stored in the compressed data file.
 * @return Number of valid MSLG chunks (0 if file empty or missing)
 */
int storage_manager_get_mslg_chunk_count(void);

/**
 * @brief Check SPIFFS usage and purge ALL stored data when capacity >= 90%.
 *
 * Deletes data.lz, data.txt, and queue.txt so the node can continue
 * collecting fresh sensor readings instead of silently failing writes.
 * Called automatically before every MSLG write, but can also be invoked
 * manually (e.g. from a diagnostic command).
 *
 * @return true  if a purge was performed
 * @return false if usage is below threshold (no action taken)
 */
bool storage_manager_purge_if_full_public(void);

void storage_manager_display_status(void);