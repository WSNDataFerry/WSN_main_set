#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
} blockbuf_t;

// prefer_psram: non-zero = try PSRAM first, then fallback to internal.
int blockbuf_init(blockbuf_t *b, size_t cap_bytes, int prefer_psram);
void blockbuf_free(blockbuf_t *b);

// Append bytes. Returns 0 on success, negative on failure.
int blockbuf_append(blockbuf_t *b, const uint8_t *data, size_t n);

// Returns 1 if len >= threshold, else 0.
int blockbuf_should_flush(const blockbuf_t *b, size_t threshold);

// Reset buffer length to 0 (keeps allocation).
static inline void blockbuf_reset(blockbuf_t *b) { b->len = 0; }
