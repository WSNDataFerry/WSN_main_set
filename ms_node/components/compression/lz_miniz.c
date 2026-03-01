#include "compression.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_timer.h"

#include "miniz.h"

static const char *TAG = "lz_miniz";

// -----------------------------------------------------------------------------
// Practical ESP32-S3 defaults
//
// IMPORTANT: miniz's zlib-compatible deflateInit2() only accepts window_bits
// equal to MZ_DEFAULT_WINDOW_BITS (typically 15) or its negative (raw deflate).
// Any other window_bits value returns MZ_PARAM_ERROR.
//
// So we keep window_bits at the default and only set a valid mem_level.
// -----------------------------------------------------------------------------

static void choose_deflate_params(size_t in_len, int *window_bits,
                                  int *mem_level) {
  (void)in_len;
  *window_bits = MZ_DEFAULT_WINDOW_BITS; // 15
  *mem_level = 8; // must be 1..9 (miniz largely ignores it)
}

static inline void log_heap_snapshot(void) {
  const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_8BIT);
  const size_t lfb_int = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
  const size_t free_ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  const size_t lfb_ps = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);

  ESP_LOGI(TAG,
           "heap int: free=%u largest=%u | psram: free=%u largest=%u | "
           "psram_init=%d",
           (unsigned)free_int, (unsigned)lfb_int, (unsigned)free_ps,
           (unsigned)lfb_ps, (int)esp_psram_is_initialized());
}

// PSRAM-first allocator for miniz internal state.
static void *idf_alloc(size_t n) {
  void *p = NULL;

  if (esp_psram_is_initialized()) {
    p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  }

  if (!p) {
    p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
  }

  return p;
}

static void idf_free(void *p) {
  if (p) {
    heap_caps_free(p);
  }
}

static voidpf mz_idf_zalloc(voidpf opaque, unsigned items, unsigned size) {
  (void)opaque;
  const size_t n = (size_t)items * (size_t)size;
  return (voidpf)idf_alloc(n);
}

static void mz_idf_zfree(voidpf opaque, voidpf address) {
  (void)opaque;
  idf_free(address);
}

esp_err_t lz_miniz_init(void) {
  // Nothing to init for miniz.
  return ESP_OK;
}

size_t lz_miniz_bound(size_t in_len) {
  // miniz uses mz_ulong. On ESP32, this is 32-bit.
  // For our use-case (KB batches), this is safe.
  return (size_t)mz_compressBound((mz_ulong)in_len);
}

esp_err_t lz_compress_miniz(const uint8_t *in, size_t in_len, uint8_t *out,
                            size_t out_max, size_t *out_len, int level,
                            comp_stats_t *stats) {
  if (!in || !out || !out_len || (out_max == 0)) {
    return ESP_ERR_INVALID_ARG;
  }

  // Clamp level (miniz follows zlib levels 0..9; 1 or 3 recommended for ESP32).
  if (level < 0)
    level = 1;
  if (level > 9)
    level = 9;

  int window_bits = 15;
  int mem_level = 8;
  choose_deflate_params(in_len, &window_bits, &mem_level);

  mz_stream s;
  memset(&s, 0, sizeof(s));
  s.zalloc = mz_idf_zalloc;
  s.zfree = mz_idf_zfree;
  s.opaque = NULL;

  s.next_in = (unsigned char *)in;
  s.avail_in = (mz_uint)in_len;
  s.next_out = (unsigned char *)out;
  s.avail_out = (mz_uint)out_max;

  const int64_t t0 = esp_timer_get_time();

  // window_bits > 0 => zlib header (recommended). We keep it positive.
  int rc = mz_deflateInit2(&s, level, MZ_DEFLATED, window_bits, mem_level,
                           MZ_DEFAULT_STRATEGY);
  if (rc != MZ_OK) {
    ESP_LOGE(
        TAG,
        "deflateInit2 failed rc=%d (%s) level=%d window_bits=%d mem_level=%d",
        rc, mz_error(rc), level, window_bits, mem_level);
    log_heap_snapshot();
    return ESP_FAIL;
  }

  rc = mz_deflate(&s, MZ_FINISH);
  if (rc != MZ_STREAM_END) {
    // Most common cause: output buffer too small => rc == MZ_BUF_ERROR.
    ESP_LOGE(TAG, "deflate failed rc=%d (%s) in=%u out_max=%u wrote=%u", rc,
             mz_error(rc), (unsigned)in_len, (unsigned)out_max,
             (unsigned)s.total_out);
    mz_deflateEnd(&s);
    log_heap_snapshot();
    return ESP_FAIL;
  }

  rc = mz_deflateEnd(&s);
  if (rc != MZ_OK) {
    ESP_LOGE(TAG, "deflateEnd failed rc=%d (%s)", rc, mz_error(rc));
    return ESP_FAIL;
  }

  const int64_t t1 = esp_timer_get_time();

  *out_len = (size_t)s.total_out;

  if (stats) {
    stats->time_us = (t1 - t0);
    stats->input_len = in_len;
    stats->output_len = *out_len;
  }

  return ESP_OK;
}

esp_err_t lz_decompress_miniz(const uint8_t *in, size_t in_len, uint8_t *out,
                              size_t out_max, size_t *out_len,
                              comp_stats_t *stats) {
  if (!in || !out || !out_len || (out_max == 0)) {
    return ESP_ERR_INVALID_ARG;
  }

  mz_stream s;
  memset(&s, 0, sizeof(s));
  s.zalloc = mz_idf_zalloc;
  s.zfree = mz_idf_zfree;
  s.opaque = NULL;

  s.next_in = (unsigned char *)in;
  s.avail_in = (mz_uint)in_len;
  s.next_out = (unsigned char *)out;
  s.avail_out = (mz_uint)out_max;

  const int64_t t0 = esp_timer_get_time();

  int rc = mz_inflateInit(&s);
  if (rc != MZ_OK) {
    ESP_LOGE(TAG, "inflateInit failed rc=%d (%s)", rc, mz_error(rc));
    log_heap_snapshot();
    return ESP_FAIL;
  }

  rc = mz_inflate(&s, MZ_FINISH);
  if (rc != MZ_STREAM_END) {
    ESP_LOGE(TAG, "inflate failed rc=%d (%s) in=%u out_max=%u wrote=%u", rc,
             mz_error(rc), (unsigned)in_len, (unsigned)out_max,
             (unsigned)s.total_out);
    mz_inflateEnd(&s);
    log_heap_snapshot();
    return ESP_FAIL;
  }

  rc = mz_inflateEnd(&s);
  if (rc != MZ_OK) {
    ESP_LOGE(TAG, "inflateEnd failed rc=%d (%s)", rc, mz_error(rc));
    return ESP_FAIL;
  }

  const int64_t t1 = esp_timer_get_time();

  *out_len = (size_t)s.total_out;

  if (stats) {
    stats->time_us = (t1 - t0);
    stats->input_len = in_len;
    stats->output_len = *out_len;
  }

  return ESP_OK;
}
