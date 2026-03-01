// Byte-wise Huffman codec for ESP-IDF.
//
// Output format:
//   4 bytes: magic "HUF1" (0x48554631)
//   4 bytes: original length (uint32 little-endian)
//   256 bytes: code lengths for symbols 0..255 (0..32, 0 = unused)
//   ...: bitstream (MSB-first within each byte)
//
// Notes:
// - This is meant for learning and small payloads. It is not a DEFLATE drop-in.
// - Code lengths >32 are rejected to keep encoding simple and safe.

#include "compression.h"

#include <string.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"

static const char *TAG = "huffman";

// Huffman previously used several large stack allocations (multiple KB).
// On ESP-IDF the main task stack can be small, which can silently corrupt
// memory and cause crashes later in unrelated code (often inside ESP_LOG).
// To avoid that, allocate the big temporary tables on the heap.
static void *huf_alloc(size_t size)
{
    // Prefer internal RAM for speed; fall back to any 8-bit capable region
    // (can include PSRAM) if internal is tight.
    void *p = heap_caps_malloc(size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!p) {
        p = heap_caps_malloc(size, MALLOC_CAP_8BIT);
    }
    return p;
}

static void huf_free(void *p)
{
    if (p) {
        heap_caps_free(p);
    }
}

#define HUF_MAGIC 0x48554631u // 'H' 'U' 'F' '1'

typedef struct {
    uint32_t freq;
    int16_t left;
    int16_t right;
    int16_t sym; // 0..255 for leaf, -1 for internal
} huf_node_t;

typedef struct {
    uint8_t *dst;
    size_t cap;
    size_t pos;
    uint64_t bitbuf;
    uint8_t bitcount; // number of bits currently stored in bitbuf
} bitw_t;

typedef struct {
    const uint8_t *src;
    size_t len;
    size_t pos;
    uint64_t bitbuf;
    uint8_t bitcount;
} bitr_t;

static inline void wr_u32_le(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static inline uint32_t rd_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline void bitw_init(bitw_t *w, uint8_t *dst, size_t cap, size_t start_pos) {
    memset(w, 0, sizeof(*w));
    w->dst = dst;
    w->cap = cap;
    w->pos = start_pos;
}

static esp_err_t bitw_put_bits(bitw_t *w, uint32_t code, uint8_t nbits) {
    if (nbits == 0) return ESP_OK;
    if (nbits > 32) return ESP_ERR_INVALID_SIZE;

    // Ensure we never overflow bitbuf (64 bits).
    if (w->bitcount + nbits > 64) {
        return ESP_ERR_INVALID_SIZE;
    }

    // Append MSB-first code bits into the stream.
    w->bitbuf = (w->bitbuf << nbits) | ((uint64_t)code & ((nbits == 32) ? 0xFFFFFFFFull : ((1ull << nbits) - 1ull)));
    w->bitcount += nbits;

    while (w->bitcount >= 8) {
        if (w->pos >= w->cap) return ESP_ERR_NO_MEM;
        uint8_t outb = (uint8_t)((w->bitbuf >> (w->bitcount - 8)) & 0xFF);
        w->dst[w->pos++] = outb;
        w->bitcount -= 8;
        // Keep only remaining bits (lower bitcount bits)
        if (w->bitcount == 0) {
            w->bitbuf = 0;
        } else {
            w->bitbuf &= (1ull << w->bitcount) - 1ull;
        }
    }
    return ESP_OK;
}

static esp_err_t bitw_flush(bitw_t *w) {
    if (w->bitcount == 0) return ESP_OK;
    if (w->pos >= w->cap) return ESP_ERR_NO_MEM;
    // Pad remaining bits with zeros to a full byte.
    uint8_t outb = (uint8_t)((w->bitbuf << (8 - w->bitcount)) & 0xFF);
    w->dst[w->pos++] = outb;
    w->bitbuf = 0;
    w->bitcount = 0;
    return ESP_OK;
}

static inline void bitr_init(bitr_t *r, const uint8_t *src, size_t len, size_t start_pos) {
    memset(r, 0, sizeof(*r));
    r->src = src;
    r->len = len;
    r->pos = start_pos;
}

static esp_err_t bitr_get_bit(bitr_t *r, uint8_t *bit_out) {
    if (r->bitcount == 0) {
        if (r->pos >= r->len) return ESP_ERR_INVALID_SIZE;
        r->bitbuf = r->src[r->pos++];
        r->bitcount = 8;
    }
    // MSB-first in each byte
    *bit_out = (uint8_t)((r->bitbuf >> (r->bitcount - 1)) & 1ull);
    r->bitcount--;
    return ESP_OK;
}

static int16_t pick_min_node(const huf_node_t *nodes, const uint8_t *alive, int n_nodes) {
    int16_t best = -1;
    uint32_t bestf = 0;
    for (int i = 0; i < n_nodes; i++) {
        if (!alive[i]) continue;
        if (best < 0 || nodes[i].freq < bestf) {
            best = (int16_t)i;
            bestf = nodes[i].freq;
        }
    }
    return best;
}

static esp_err_t build_code_lengths(const uint32_t freq[256], uint8_t lens_out[256], uint8_t *max_len_out)
{
    // Allocate the working set on the heap to avoid blowing the main task stack.
    huf_node_t *nodes = (huf_node_t *)huf_alloc(sizeof(huf_node_t) * 512);
    uint8_t *alive = (uint8_t *)huf_alloc(512);
    typedef struct { int16_t idx; uint8_t depth; } stack_item_t;
    stack_item_t *stack = (stack_item_t *)huf_alloc(sizeof(stack_item_t) * 512);

    if (!nodes || !alive || !stack) {
        huf_free(stack);
        huf_free(alive);
        huf_free(nodes);
        return ESP_ERR_NO_MEM;
    }

    memset(lens_out, 0, 256);
    *max_len_out = 0;
    memset(alive, 0, 512);

    // Create initial leaf nodes.
    int n_nodes = 0;
    int n_alive = 0;
    for (int s = 0; s < 256; s++) {
        if (freq[s] == 0) continue;
        nodes[n_nodes] = (huf_node_t){ .freq = freq[s], .left = -1, .right = -1, .sym = (int16_t)s };
        alive[n_nodes] = 1;
        n_nodes++;
        n_alive++;
    }

    if (n_alive == 0) {
        huf_free(stack);
        huf_free(alive);
        huf_free(nodes);
        return ESP_ERR_INVALID_ARG;
    }

    if (n_alive == 1) {
        // Special case: only one symbol. Give it length 1.
        for (int i = 0; i < n_nodes; i++) {
            if (alive[i]) {
                lens_out[(uint8_t)nodes[i].sym] = 1;
                *max_len_out = 1;
                huf_free(stack);
                huf_free(alive);
                huf_free(nodes);
                return ESP_OK;
            }
        }
    }

    // Build tree by repeatedly combining two smallest-frequency nodes.
    while (n_alive > 1) {
        int16_t a = pick_min_node(nodes, alive, n_nodes);
        alive[a] = 0;
        n_alive--;

        int16_t b = pick_min_node(nodes, alive, n_nodes);
        alive[b] = 0;
        n_alive--;

        nodes[n_nodes] = (huf_node_t){
            .freq = nodes[a].freq + nodes[b].freq,
            .left = a,
            .right = b,
            .sym = -1,
        };
        alive[n_nodes] = 1;
        n_nodes++;
        n_alive++;
    }

    // Find root.
    int16_t root = -1;
    for (int i = 0; i < n_nodes; i++) {
        if (alive[i]) { root = (int16_t)i; break; }
    }
    if (root < 0) {
        huf_free(stack);
        huf_free(alive);
        huf_free(nodes);
        return ESP_FAIL;
    }

    // DFS to compute lengths.
    int sp = 0;
    stack[sp++] = (stack_item_t){ .idx = root, .depth = 0 };
    while (sp > 0) {
        stack_item_t it = stack[--sp];
        huf_node_t *n = &nodes[it.idx];
        if (n->sym >= 0) {
            uint8_t d = it.depth;
            if (d == 0) d = 1; // should not happen here, but keep safe
            if (d > 32) {
                ESP_LOGE(TAG, "Huffman code length too large (%u). Rejecting.", d);
                huf_free(stack);
                huf_free(alive);
                huf_free(nodes);
                return ESP_ERR_INVALID_SIZE;
            }
            lens_out[(uint8_t)n->sym] = d;
            if (d > *max_len_out) *max_len_out = d;
        } else {
            // Push children.
            if (n->left >= 0) stack[sp++] = (stack_item_t){ .idx = n->left, .depth = (uint8_t)(it.depth + 1) };
            if (n->right >= 0) stack[sp++] = (stack_item_t){ .idx = n->right, .depth = (uint8_t)(it.depth + 1) };
        }
    }

    huf_free(stack);
    huf_free(alive);
    huf_free(nodes);
    return ESP_OK;
}

typedef struct {
    uint8_t sym;
    uint8_t len;
} sym_len_t;

static int cmp_sym_len(const void *a, const void *b) {
    const sym_len_t *x = (const sym_len_t *)a;
    const sym_len_t *y = (const sym_len_t *)b;
    if (x->len != y->len) return (int)x->len - (int)y->len;
    return (int)x->sym - (int)y->sym;
}

static esp_err_t build_canonical_codes(const uint8_t lens[256], uint32_t codes[256], uint8_t *max_len_out) {
    memset(codes, 0, 256 * sizeof(uint32_t));
    uint8_t max_len = 0;

    sym_len_t list[256];
    int n = 0;
    for (int s = 0; s < 256; s++) {
        uint8_t l = lens[s];
        if (l == 0) continue;
        if (l > 32) return ESP_ERR_INVALID_SIZE;
        list[n++] = (sym_len_t){ .sym = (uint8_t)s, .len = l };
        if (l > max_len) max_len = l;
    }
    if (n == 0) return ESP_ERR_INVALID_ARG;

    qsort(list, n, sizeof(list[0]), cmp_sym_len);

    uint32_t code = 0;
    uint8_t prev_len = list[0].len;
    // first code stays 0

    for (int i = 0; i < n; i++) {
        uint8_t l = list[i].len;
        if (l > prev_len) {
            code <<= (l - prev_len);
            prev_len = l;
        }
        codes[list[i].sym] = code;
        code++;
    }

    *max_len_out = max_len;
    return ESP_OK;
}

typedef struct {
    int16_t left;
    int16_t right;
    int16_t sym;
} dec_node_t;

static esp_err_t build_decode_tree(const uint8_t lens[256], const uint32_t codes[256], dec_node_t *tree, int tree_cap, int *root_out, int *nodes_used_out) {
    // Tree nodes stored in array. Node 0 is root.
    tree[0] = (dec_node_t){ .left = -1, .right = -1, .sym = -1 };
    int used = 1;

    for (int s = 0; s < 256; s++) {
        uint8_t l = lens[s];
        if (l == 0) continue;
        uint32_t code = codes[s];

        int cur = 0;
        for (int i = (int)l - 1; i >= 0; i--) {
            uint8_t bit = (uint8_t)((code >> i) & 1u);
            int16_t *nextp = bit ? &tree[cur].right : &tree[cur].left;
            if (*nextp < 0) {
                if (used >= tree_cap) return ESP_ERR_NO_MEM;
                tree[used] = (dec_node_t){ .left = -1, .right = -1, .sym = -1 };
                *nextp = (int16_t)used;
                used++;
            }
            cur = *nextp;
        }
        // Leaf
        tree[cur].sym = (int16_t)s;
    }

    *root_out = 0;
    *nodes_used_out = used;
    return ESP_OK;
}

size_t huffman_bound(size_t in_len) {
    // Header (magic + original_len + lengths) + worst-case bitstream (~32 bits per byte)
    // This is intentionally conservative.
    size_t header = 4 + 4 + 256;
    size_t bits = in_len * 32;
    size_t bytes = (bits + 7) / 8;
    return header + bytes;
}

esp_err_t huffman_compress(const uint8_t *in, size_t in_len,
                           uint8_t *out, size_t out_max,
                           size_t *out_len,
                           comp_stats_t *stats) {
    if (!in || !out || !out_len) return ESP_ERR_INVALID_ARG;
    if (in_len > 0xFFFFFFFFu) return ESP_ERR_INVALID_SIZE;

    int64_t t0 = esp_timer_get_time();

    uint32_t freq[256] = {0};
    for (size_t i = 0; i < in_len; i++) {
        freq[in[i]]++;
    }

    uint8_t lens[256];
    uint8_t max_len = 0;
    esp_err_t err = build_code_lengths(freq, lens, &max_len);
    if (err != ESP_OK) return err;

    uint32_t codes[256];
    err = build_canonical_codes(lens, codes, &max_len);
    if (err != ESP_OK) return err;

    // Write header.
    const size_t header_sz = 4 + 4 + 256;
    if (out_max < header_sz) return ESP_ERR_NO_MEM;

    wr_u32_le(out + 0, HUF_MAGIC);
    wr_u32_le(out + 4, (uint32_t)in_len);
    memcpy(out + 8, lens, 256);

    // Write bitstream.
    bitw_t w;
    bitw_init(&w, out, out_max, header_sz);

    for (size_t i = 0; i < in_len; i++) {
        uint8_t s = in[i];
        uint8_t l = lens[s];
        if (l == 0) return ESP_FAIL;
        err = bitw_put_bits(&w, codes[s], l);
        if (err != ESP_OK) return err;
    }
    err = bitw_flush(&w);
    if (err != ESP_OK) return err;

    *out_len = w.pos;

    if (stats) {
        stats->input_len = in_len;
        stats->output_len = *out_len;
        stats->time_us = esp_timer_get_time() - t0;
    }

    return ESP_OK;
}

esp_err_t huffman_decompress(const uint8_t *in, size_t in_len,
                             uint8_t *out, size_t out_max,
                             size_t *out_len,
                             comp_stats_t *stats) {
    if (!in || !out || !out_len) return ESP_ERR_INVALID_ARG;

    int64_t t0 = esp_timer_get_time();

    const size_t header_sz = 4 + 4 + 256;
    if (in_len < header_sz) return ESP_ERR_INVALID_SIZE;

    uint32_t magic = rd_u32_le(in + 0);
    if (magic != HUF_MAGIC) return ESP_ERR_INVALID_ARG;

    uint32_t orig_len = rd_u32_le(in + 4);
    if (orig_len > out_max) return ESP_ERR_NO_MEM;

    uint8_t lens[256];
    memcpy(lens, in + 8, 256);
    for (int i = 0; i < 256; i++) {
        if (lens[i] > 32) return ESP_ERR_INVALID_SIZE;
    }

    uint32_t codes[256];
    uint8_t max_len = 0;
    esp_err_t err = build_canonical_codes(lens, codes, &max_len);
    if (err != ESP_OK) return err;

    // Build decode tree.
    // NOTE: This used to be on the stack, but can be large enough to blow the
    // default main task stack. Keep it on the heap to avoid silent corruption.
    dec_node_t *tree = (dec_node_t *)huf_alloc(sizeof(dec_node_t) * 2048);
    if (!tree) return ESP_ERR_NO_MEM;
    int root = 0;
    int used = 0;
    err = build_decode_tree(lens, codes, tree, 2048, &root, &used);
    if (err != ESP_OK) {
        huf_free(tree);
        return err;
    }

    // Special case: single-symbol tree (root becomes leaf).
    // Detect by checking if root has sym and no children.
    if (tree[root].sym >= 0 && tree[root].left < 0 && tree[root].right < 0) {
        memset(out, (uint8_t)tree[root].sym, orig_len);
        *out_len = orig_len;
        if (stats) {
            stats->input_len = in_len;
            stats->output_len = *out_len;
            stats->time_us = esp_timer_get_time() - t0;
        }
        huf_free(tree);
        return ESP_OK;
    }

    bitr_t r;
    bitr_init(&r, in, in_len, header_sz);

    size_t produced = 0;
    int cur = root;
    while (produced < orig_len) {
        uint8_t bit;
        err = bitr_get_bit(&r, &bit);
        if (err != ESP_OK) {
            huf_free(tree);
            return err;
        }

        cur = bit ? tree[cur].right : tree[cur].left;
        if (cur < 0) {
            huf_free(tree);
            return ESP_FAIL;
        }
        if (tree[cur].sym >= 0) {
            out[produced++] = (uint8_t)tree[cur].sym;
            cur = root;
        }
    }

    *out_len = produced;

    if (stats) {
        stats->input_len = in_len;
        stats->output_len = *out_len;
        stats->time_us = esp_timer_get_time() - t0;
    }
    huf_free(tree);
    return ESP_OK;
}
