#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    size_t input_len;
    size_t output_len;
    int64_t time_us; // compression/decompression time in microseconds
} comp_stats_t;

// Maximum possible compressed size for the given input length.
size_t lz_miniz_bound(size_t in_len);

// Compress input buffer into output buffer using miniz (DEFLATE).
// level: 1 (fastest) .. 9 (best ratio). Recommended 1-3 for ESP32 power tests.
esp_err_t lz_compress_miniz(const uint8_t *in, size_t in_len,
                            uint8_t *out, size_t out_max,
                            size_t *out_len,
                            int level,
                            comp_stats_t *stats);

// Decompress miniz-compressed buffer into output buffer.
esp_err_t lz_decompress_miniz(const uint8_t *in, size_t in_len,
                              uint8_t *out, size_t out_max,
                              size_t *out_len,
                              comp_stats_t *stats);

// -----------------------------
// Huffman (byte-wise) codec
// -----------------------------
// Format: 4B magic 'HUF1' | 4B original_len | 256B code_lengths | bitstream
// - code_lengths[i] is the bit-length (0..32) for symbol byte value i
// - bitstream is MSB-first per byte

size_t huffman_bound(size_t in_len);

esp_err_t huffman_compress(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_max,
                           size_t *out_len,
                           comp_stats_t *stats);

esp_err_t huffman_decompress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_max,
                             size_t *out_len,
                             comp_stats_t *stats);

#ifdef __cplusplus
}
#endif
