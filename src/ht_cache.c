#include "draugr/ht_cache.h"
#include "draugr/ht.h"
#include <stdlib.h>
#include <string.h>

#define NONE UINT32_MAX

struct ht_cache {
    ht_bare_t *bare;

    uint8_t  *entries;
    uint64_t *hashes;
    uint8_t  *live;
    uint32_t *lru_prev;
    uint32_t *lru_next;
    uint32_t *free_stack;
    size_t    free_top;

    uint32_t lru_head;
    uint32_t lru_tail;

    size_t capacity;
    size_t entry_size;
    size_t size;

    ht_cache_hash_fn hash_fn;
    ht_cache_eq_fn   eq_fn;
    void            *user_ctx;
};

static size_t cache_next_pow2(size_t n) {
    size_t r = 1;
    while (r < n) r <<= 1;
    return r;
}

/* LRU helpers */
static void lru_add_head(ht_cache_t *c, uint32_t idx) {
    c->lru_prev[idx] = NONE;
    c->lru_next[idx] = c->lru_head;
    if (c->lru_head != NONE)
        c->lru_prev[c->lru_head] = idx;
    c->lru_head = idx;
    if (c->lru_tail == NONE)
        c->lru_tail = idx;
}

static void lru_remove(ht_cache_t *c, uint32_t idx) {
    uint32_t prev = c->lru_prev[idx];
    uint32_t next = c->lru_next[idx];
    if (prev != NONE) c->lru_next[prev] = next;
    else              c->lru_head = next;
    if (next != NONE) c->lru_prev[next] = prev;
    else              c->lru_tail = prev;
    c->lru_prev[idx] = NONE;
    c->lru_next[idx] = NONE;
}

static void lru_promote(ht_cache_t *c, uint32_t idx) {
    if (c->lru_head == idx) return;
    lru_remove(c, idx);
    lru_add_head(c, idx);
}

/* Get entry pointer from index */
static void *entry_at(const ht_cache_t *c, uint32_t idx) {
    return c->entries + (size_t)idx * c->entry_size;
}

/* Get index from entry pointer */
static uint32_t entry_index(const ht_cache_t *c, const void *entry) {
    size_t offset = (size_t)((const uint8_t *)entry - c->entries);
    return (uint32_t)(offset / c->entry_size);
}

/* Internal scan context for ht_cache_get */
typedef struct {
    ht_cache_t    *cache;
    const void    *key;
    size_t         key_len;
    uint32_t       matched_idx;
} get_scan_ctx_t;

/* Bare callback for ht_cache_get: uses eq_fn to match */
static bool get_scan_cb(uint32_t val, void *ctx) {
    get_scan_ctx_t *s = ctx;
    ht_cache_t *c = s->cache;
    if (val >= c->capacity) return true;
    void *entry = entry_at(c, val);
    if (!c->live[val]) return true;
    if (c->eq_fn) {
        if (c->eq_fn(s->key, s->key_len, entry, c->entry_size, c->user_ctx)) {
            s->matched_idx = val;
            return false;
        }
    } else {
        /* No eq_fn: first hash match wins */
        s->matched_idx = val;
        return false;
    }
    return true;
}

/* Internal scan context for ht_cache_find */
typedef struct {
    ht_cache_t     *cache;
    ht_cache_scan_fn user_scan;
    void           *user_ctx;
    uint32_t        matched_idx;
    bool            found;
} find_scan_ctx_t;

/* Bare callback for ht_cache_find: wraps user's scan_fn */
static bool find_scan_cb(uint32_t val, void *ctx) {
    find_scan_ctx_t *s = ctx;
    ht_cache_t *c = s->cache;
    if (val >= c->capacity) return true;
    if (!c->live[val]) return true;
    void *entry = entry_at(c, val);
    if (!s->user_scan(entry, s->user_ctx)) {
        s->matched_idx = val;
        s->found = true;
        return false;
    }
    return true;
}

/* Internal scan context for ht_cache_remove */
typedef struct {
    ht_cache_t    *cache;
    const void    *key;
    size_t         key_len;
    uint64_t       hash;
    uint32_t       matched_idx;
} remove_scan_ctx_t;

static bool remove_scan_cb(uint32_t val, void *ctx) {
    remove_scan_ctx_t *s = ctx;
    ht_cache_t *c = s->cache;
    if (val >= c->capacity) return true;
    void *entry = entry_at(c, val);
    if (!c->live[val]) return true;
    if (c->eq_fn) {
        if (c->eq_fn(s->key, s->key_len, entry, c->entry_size, c->user_ctx)) {
            s->matched_idx = val;
            return false;
        }
    } else {
        s->matched_idx = val;
        return false;
    }
    return true;
}

ht_cache_t *ht_cache_create(const ht_cache_config_t *cfg) {
    if (!cfg || cfg->capacity == 0 || cfg->entry_size == 0 || !cfg->hash_fn)
        return NULL;

    ht_cache_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;

    c->capacity   = cfg->capacity;
    c->entry_size = cfg->entry_size;
    c->hash_fn    = cfg->hash_fn;
    c->eq_fn      = cfg->eq_fn;
    c->user_ctx   = cfg->user_ctx;
    c->lru_head   = NONE;
    c->lru_tail   = NONE;

    size_t cap = cfg->capacity;
    c->entries    = calloc(cap, cfg->entry_size);
    c->hashes     = calloc(cap, sizeof(uint64_t));
    c->live       = calloc(cap, 1);
    c->lru_prev   = malloc(cap * sizeof(uint32_t));
    c->lru_next   = malloc(cap * sizeof(uint32_t));
    c->free_stack = malloc(cap * sizeof(uint32_t));

    if (!c->entries || !c->hashes || !c->live ||
        !c->lru_prev || !c->lru_next || !c->free_stack) {
        ht_cache_destroy(c);
        return NULL;
    }

    for (size_t i = 0; i < cap; i++) {
        c->lru_prev[i] = NONE;
        c->lru_next[i] = NONE;
        c->free_stack[i] = (uint32_t)(cap - 1 - i);
    }
    c->free_top = cap;

    /* Bare table at ~2× capacity for tombstone headroom */
    size_t bare_cap = cache_next_pow2(cap * 2);
    if (bare_cap < 64) bare_cap = 64;

    ht_config_t bare_cfg = {
        .initial_capacity = bare_cap,
        .max_load_factor  = 0.75,
        .min_load_factor  = 0,
        .tomb_threshold   = 0.30,
        .zombie_window    = 16,
    };
    c->bare = ht_bare_create(&bare_cfg);
    if (!c->bare) {
        ht_cache_destroy(c);
        return NULL;
    }

    return c;
}

void ht_cache_destroy(ht_cache_t *c) {
    if (!c) return;
    ht_bare_destroy(c->bare);
    free(c->entries);
    free(c->hashes);
    free(c->live);
    free(c->lru_prev);
    free(c->lru_next);
    free(c->free_stack);
    free(c);
}

void ht_cache_clear(ht_cache_t *c) {
    if (!c) return;
    ht_bare_clear(c->bare);
    size_t cap = c->capacity;
    memset(c->live, 0, cap);
    memset(c->hashes, 0, cap * sizeof(uint64_t));
    c->size = 0;
    c->lru_head = NONE;
    c->lru_tail = NONE;
    for (size_t i = 0; i < cap; i++) {
        c->lru_prev[i] = NONE;
        c->lru_next[i] = NONE;
        c->free_stack[i] = (uint32_t)(cap - 1 - i);
    }
    c->free_top = cap;
}

void *ht_cache_put(ht_cache_t *c, const void *entry_data, size_t entry_size) {
    if (!c || !entry_data) return NULL;
    if (entry_size != c->entry_size) return NULL;

    if (c->free_top == 0) {
        if (!ht_cache_evict(c))
            return NULL;
    }

    uint32_t slot = c->free_stack[--c->free_top];
    void *dst = entry_at(c, slot);
    memcpy(dst, entry_data, entry_size);

    uint64_t hash = c->hash_fn(entry_data, entry_size, c->user_ctx);
    c->hashes[slot] = hash;
    c->live[slot] = 1;

    if (!ht_bare_insert(c->bare, hash, slot)) {
        c->live[slot] = 0;
        c->free_stack[c->free_top++] = slot;
        return NULL;
    }

    lru_add_head(c, slot);
    c->size++;
    return dst;
}

void *ht_cache_get(ht_cache_t *c, const void *key, size_t key_len) {
    if (!c || !key) return NULL;

    uint64_t hash = c->hash_fn(key, key_len, c->user_ctx);

    get_scan_ctx_t ctx = {
        .cache = c,
        .key = key,
        .key_len = key_len,
        .matched_idx = NONE,
    };
    ht_bare_find_all(c->bare, hash, get_scan_cb, &ctx);

    if (ctx.matched_idx == NONE) return NULL;
    lru_promote(c, ctx.matched_idx);
    return entry_at(c, ctx.matched_idx);
}

void *ht_cache_find(ht_cache_t *c, uint64_t hash,
                    ht_cache_scan_fn scan_fn, void *scan_ctx) {
    if (!c || !scan_fn) return NULL;

    find_scan_ctx_t ctx = {
        .cache = c,
        .user_scan = scan_fn,
        .user_ctx = scan_ctx,
        .matched_idx = NONE,
        .found = false,
    };
    ht_bare_find_all(c->bare, hash, find_scan_cb, &ctx);

    if (!ctx.found) return NULL;
    return entry_at(c, ctx.matched_idx);
}

void ht_cache_promote(ht_cache_t *c, void *entry) {
    if (!c || !entry) return;
    uint32_t idx = entry_index(c, entry);
    if (idx >= c->capacity || !c->live[idx]) return;
    lru_promote(c, idx);
}

bool ht_cache_remove(ht_cache_t *c, const void *key, size_t key_len) {
    if (!c || !key) return false;

    uint64_t hash = c->hash_fn(key, key_len, c->user_ctx);

    remove_scan_ctx_t ctx = {
        .cache = c,
        .key = key,
        .key_len = key_len,
        .hash = hash,
        .matched_idx = NONE,
    };
    ht_bare_find_all(c->bare, hash, remove_scan_cb, &ctx);

    if (ctx.matched_idx == NONE) return false;

    ht_bare_remove_val(c->bare, hash, ctx.matched_idx);
    lru_remove(c, ctx.matched_idx);
    c->live[ctx.matched_idx] = 0;
    c->free_stack[c->free_top++] = ctx.matched_idx;
    c->size--;
    return true;
}

bool ht_cache_evict(ht_cache_t *c) {
    if (!c || c->size == 0) return false;

    uint32_t tail = c->lru_tail;
    if (tail == NONE) return false;

    ht_bare_remove_val(c->bare, c->hashes[tail], tail);
    lru_remove(c, tail);
    c->live[tail] = 0;
    c->free_stack[c->free_top++] = tail;
    c->size--;
    return true;
}

size_t ht_cache_size(const ht_cache_t *c) {
    return c ? c->size : 0;
}

size_t ht_cache_capacity(const ht_cache_t *c) {
    return c ? c->capacity : 0;
}

ht_cache_iter_t ht_cache_iter_begin(const ht_cache_t *c) {
    ht_cache_iter_t it = {0, false};
    (void)c;
    return it;
}

bool ht_cache_iter_next(ht_cache_t *c, ht_cache_iter_t *iter, void **out_entry) {
    if (!c || !iter || !out_entry) return false;
    if (!iter->started) {
        iter->started = true;
        iter->idx = 0;
    } else {
        iter->idx++;
    }
    while (iter->idx < c->capacity) {
        if (c->live[iter->idx]) {
            *out_entry = entry_at(c, (uint32_t)iter->idx);
            return true;
        }
        iter->idx++;
    }
    return false;
}
