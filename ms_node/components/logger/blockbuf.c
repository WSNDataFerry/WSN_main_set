#include "blockbuf.h"

#include "esp_heap_caps.h"

#include <string.h>

int blockbuf_init(blockbuf_t *b, size_t cap_bytes, int prefer_psram)
{
    if (!b || cap_bytes == 0) return -1;
    memset(b, 0, sizeof(*b));
    b->cap = cap_bytes;

    if (prefer_psram) {
        b->buf = heap_caps_malloc(cap_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!b->buf) {
        b->buf = heap_caps_malloc(cap_bytes, MALLOC_CAP_8BIT);
    }
    if (!b->buf) {
        b->cap = 0;
        return -2;
    }

    b->len = 0;
    return 0;
}

void blockbuf_free(blockbuf_t *b)
{
    if (!b) return;
    if (b->buf) heap_caps_free(b->buf);
    memset(b, 0, sizeof(*b));
}

int blockbuf_append(blockbuf_t *b, const uint8_t *data, size_t n)
{
    if (!b || !b->buf || !data) return -1;
    if (b->len + n > b->cap) return -2;
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return 0;
}

int blockbuf_should_flush(const blockbuf_t *b, size_t threshold)
{
    if (!b) return 0;
    return (b->len >= threshold) ? 1 : 0;
}
