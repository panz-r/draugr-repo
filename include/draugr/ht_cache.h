#ifndef DRAUGR_HT_CACHE_H
#define DRAUGR_HT_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ht_cache ht_cache_t;

typedef uint64_t (*ht_cache_hash_fn)(const void *key, size_t key_len, void *ctx);
typedef bool     (*ht_cache_eq_fn)(const void *key, size_t key_len,
                                   const void *entry, size_t entry_size, void *ctx);

/* Scan callback for ht_cache_find. Receives each entry matching a hash.
 * Return true to continue scanning, false to stop (match found). */
typedef bool (*ht_cache_scan_fn)(void *entry, void *ctx);

typedef struct {
    size_t           capacity;
    size_t           entry_size;
    ht_cache_hash_fn hash_fn;
    ht_cache_eq_fn   eq_fn;
    void            *user_ctx;
} ht_cache_config_t;

ht_cache_t *ht_cache_create(const ht_cache_config_t *cfg);
void        ht_cache_destroy(ht_cache_t *c);
void        ht_cache_clear(ht_cache_t *c);

/* Put: always-insert. Evicts LRU if full.
 * hash_fn is called on entry_data to compute hash.
 * Returns pointer to stored entry, NULL on OOM. */
void *ht_cache_put(ht_cache_t *c, const void *entry_data, size_t entry_size);

/* Get: find entry matching key via eq_fn. Auto-promotes on hit. */
void *ht_cache_get(ht_cache_t *c, const void *key, size_t key_len);

/* Find: scan all entries matching hash. No auto-promote.
 * Returns entry for which scan_fn returned false, or NULL. */
void *ht_cache_find(ht_cache_t *c, uint64_t hash,
                    ht_cache_scan_fn scan_fn, void *scan_ctx);

/* Promote: move entry (returned by find) to MRU position. */
void ht_cache_promote(ht_cache_t *c, void *entry);

/* Remove: delete entry by key. Returns true if found and removed. */
bool ht_cache_remove(ht_cache_t *c, const void *key, size_t key_len);

/* Evict: remove the LRU entry. Returns true if something was evicted. */
bool ht_cache_evict(ht_cache_t *c);

size_t ht_cache_size(const ht_cache_t *c);
size_t ht_cache_capacity(const ht_cache_t *c);

typedef struct { size_t idx; bool started; } ht_cache_iter_t;

ht_cache_iter_t ht_cache_iter_begin(const ht_cache_t *c);
bool ht_cache_iter_next(ht_cache_t *c, ht_cache_iter_t *iter, void **out_entry);

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_HT_CACHE_H */
