// Compression benchmarks: Huffman vs. miniz on representative buffers.

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "compression.h"

static const char *TAG = "comp_bench";

typedef struct {
    const char *name;
    const uint8_t *buf;
    size_t len;
} bench_case_t;

typedef struct {
    size_t comp_len;
    size_t decomp_len;
    int64_t comp_us;
    int64_t decomp_us;
    int free_before_int;
    int free_after_int;
    int min_after_int;
} bench_result_t;

static void log_heap_snapshot(int *free_int, int *min_int)
{
    if (free_int) *free_int = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
    if (min_int) *min_int = (int)heap_caps_get_minimum_free_size(MALLOC_CAP_8BIT);
}

static bool verify_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    return (memcmp(a, b, len) == 0);
}

static bool bench_miniz(const bench_case_t *c, bench_result_t *r)
{
    size_t out_max = lz_miniz_bound(c->len);

    uint8_t *compressed = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        compressed = heap_caps_malloc(out_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!compressed) {
        compressed = heap_caps_malloc(out_max, MALLOC_CAP_8BIT);
    }

    uint8_t *decompressed = heap_caps_malloc(c->len + 64, MALLOC_CAP_8BIT);

    if (!compressed || !decompressed) {
        ESP_LOGE(TAG, "%s: miniz alloc failed", c->name);
        if (compressed) heap_caps_free(compressed);
        if (decompressed) heap_caps_free(decompressed);
        return false;
    }

    comp_stats_t cs = {0}, ds = {0};
    size_t comp_len = out_max;
    size_t decomp_len = c->len + 64;

    if (lz_compress_miniz(c->buf, c->len, compressed, out_max, &comp_len, 3, &cs) != ESP_OK) {
        ESP_LOGE(TAG, "%s: miniz compress failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (lz_decompress_miniz(compressed, comp_len, decompressed, c->len + 64, &decomp_len, &ds) != ESP_OK) {
        ESP_LOGE(TAG, "%s: miniz decompress failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (decomp_len != c->len || !verify_equal(decompressed, c->buf, c->len)) {
        ESP_LOGE(TAG, "%s: miniz verify failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (r) {
        r->comp_len = comp_len;
        r->decomp_len = decomp_len;
        r->comp_us = cs.time_us;
        r->decomp_us = ds.time_us;
    }

    heap_caps_free(compressed);
    heap_caps_free(decompressed);
    return true;
}

static bool bench_huffman(const bench_case_t *c, bench_result_t *r)
{
    size_t out_max = huffman_bound(c->len);

    uint8_t *compressed = NULL;
    if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) > 0) {
        compressed = heap_caps_malloc(out_max, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!compressed) {
        compressed = heap_caps_malloc(out_max, MALLOC_CAP_8BIT);
    }

    uint8_t *decompressed = heap_caps_malloc(c->len + 64, MALLOC_CAP_8BIT);

    if (!compressed || !decompressed) {
        ESP_LOGE(TAG, "%s: huffman alloc failed", c->name);
        if (compressed) heap_caps_free(compressed);
        if (decompressed) heap_caps_free(decompressed);
        return false;
    }

    comp_stats_t cs = {0}, ds = {0};
    size_t comp_len = out_max;
    size_t decomp_len = c->len + 64;

    if (huffman_compress(c->buf, c->len, compressed, out_max, &comp_len, &cs) != ESP_OK) {
        ESP_LOGE(TAG, "%s: huffman compress failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (huffman_decompress(compressed, comp_len, decompressed, c->len + 64, &decomp_len, &ds) != ESP_OK) {
        ESP_LOGE(TAG, "%s: huffman decompress failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (decomp_len != c->len || !verify_equal(decompressed, c->buf, c->len)) {
        ESP_LOGE(TAG, "%s: huffman verify failed", c->name);
        heap_caps_free(compressed);
        heap_caps_free(decompressed);
        return false;
    }

    if (r) {
        r->comp_len = comp_len;
        r->decomp_len = decomp_len;
        r->comp_us = cs.time_us;
        r->decomp_us = ds.time_us;
    }

    heap_caps_free(compressed);
    heap_caps_free(decompressed);
    return true;
}

static bool bench_baseline(const bench_case_t *c, bench_result_t *r)
{
    uint8_t *copy = heap_caps_malloc(c->len, MALLOC_CAP_8BIT);
    if (!copy) {
        ESP_LOGE(TAG, "%s: baseline alloc failed", c->name);
        return false;
    }

    int64_t start = esp_timer_get_time();
    memcpy(copy, c->buf, c->len);
    int64_t copy_us = esp_timer_get_time() - start;

    start = esp_timer_get_time();
    // Simulate "read" (already in memory, so just touch)
    volatile uint8_t sink = copy[0];
    (void)sink;
    int64_t read_us = esp_timer_get_time() - start;

    if (r) {
        r->comp_len = c->len;  // No compression: output = input
        r->decomp_len = c->len;
        r->comp_us = copy_us;
        r->decomp_us = read_us;
    }

    heap_caps_free(copy);
    return true;
}

static void log_result(const char *algo, const bench_case_t *c, const bench_result_t *r,
                       int free_before, int min_before)
{
    float ratio = (c->len > 0) ? ((float)r->comp_len / (float)c->len) : 0.0f;
    int free_after = r->free_after_int;
    int min_after = r->min_after_int;
    int free_drop = free_before - free_after;
    int min_drop = min_before - min_after;

    ESP_LOGI(TAG, "%s | %s | in=%u out=%u ratio=%.3f comp=%lldus decomp=%lldus heap_drop=%d min_drop=%d",
             algo, c->name,
             (unsigned)c->len, (unsigned)r->comp_len, ratio,
             (long long)r->comp_us, (long long)r->decomp_us,
             free_drop, min_drop);
}

// Build a small delta-encoded payload for numeric streams.
static size_t build_delta_payload(uint8_t *out, size_t out_max)
{
    // Example readings (VOC ppb, temp x100, humidity x100)
    const uint16_t samples[][3] = {
        {220, 2710, 7020}, {221, 2715, 7010}, {219, 2705, 7030}, {222, 2712, 7025}
    };
    const size_t count = sizeof(samples) / sizeof(samples[0]);
    if (out_max < 1 + count * 3 * 2) return 0;

    size_t pos = 0;
    out[pos++] = (uint8_t)count;
    uint16_t prev[3] = {0, 0, 0};
    for (size_t i = 0; i < count; i++) {
        for (int f = 0; f < 3; f++) {
            int16_t delta = (int16_t)(samples[i][f] - prev[f]);
            out[pos++] = (uint8_t)(delta & 0xFF);
            out[pos++] = (uint8_t)((delta >> 8) & 0xFF);
            prev[f] = samples[i][f];
        }
    }
    return pos;
}

// Simple synthetic audio snippet (16-bit PCM) to avoid large tables.
static size_t build_audio_payload(uint8_t *out, size_t out_max)
{
    const uint16_t samples = 256; // short chunk
    const size_t need = samples * sizeof(int16_t);
    if (need > out_max) return 0;
    for (uint16_t i = 0; i < samples; i++) {
        // Triangular waveform to stay cheap (no math funcs).
        uint16_t v = (i % 64) * 512; // spans 0..32768 roughly
        ((int16_t *)out)[i] = (int16_t)(v - 16384);
    }
    return need;
}

void compression_bench_run_once(void)
{
    static const char *json_sample =
        "{\"ts_ms\":1,\"env\":{\"t\":27.1,\"h\":70.2},\"gas\":{\"tvoc\":220},\"mag\":{\"x\":1.2}}\n"
        "{\"ts_ms\":2,\"env\":{\"t\":27.2,\"h\":70.1},\"gas\":{\"tvoc\":221},\"mag\":{\"x\":1.2}}\n"
        "{\"ts_ms\":3,\"env\":{\"t\":27.1,\"h\":70.2},\"gas\":{\"tvoc\":220},\"mag\":{\"x\":1.2}}\n";

    uint8_t delta_buf[96] = {0};
    size_t delta_len = build_delta_payload(delta_buf, sizeof(delta_buf));

    uint8_t audio_buf[512] = {0};
    size_t audio_len = build_audio_payload(audio_buf, sizeof(audio_buf));

    const bench_case_t cases[] = {
        {"json", (const uint8_t *)json_sample, strlen(json_sample)},
        {"delta", delta_buf, delta_len},
        {"audio", audio_buf, audio_len},
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        const bench_case_t *c = &cases[i];
        if (c->len == 0) continue;

        bench_result_t res = {0};
        int free_before = 0, min_before = 0;

        // Baseline (no compression)
        log_heap_snapshot(&free_before, &min_before);
        if (bench_baseline(c, &res)) {
            log_heap_snapshot(NULL, &res.min_after_int);
            res.free_after_int = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
            log_result("BASELINE", c, &res, free_before, min_before);
        }

        // Huffman
        memset(&res, 0, sizeof(res));
        log_heap_snapshot(&free_before, &min_before);
        if (bench_huffman(c, &res)) {
            log_heap_snapshot(NULL, &res.min_after_int); // capture min after operations
            res.free_after_int = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
            log_result("HUFF", c, &res, free_before, min_before);
        }

        // miniz
        memset(&res, 0, sizeof(res));
        log_heap_snapshot(&free_before, &min_before);
        if (bench_miniz(c, &res)) {
            log_heap_snapshot(NULL, &res.min_after_int);
            res.free_after_int = (int)heap_caps_get_free_size(MALLOC_CAP_8BIT);
            log_result("MINIZ", c, &res, free_before, min_before);
        }
    }
}
