#ifndef DRAUGR_HT_CACHE_INTERNAL_H
#define DRAUGR_HT_CACHE_INTERNAL_H

/**
 * Draugr Cache Internal API — for testing and low-level implementors
 *
 * Include this header to get full access to cache internal structs and helpers.
 */

#include "draugr/ht_cache.h"
#include "draugr/ht_internal.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Constants
// ============================================================================

#define NONE UINT32_MAX

// ============================================================================
// Cache Structure
// ============================================================================

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

// ============================================================================
// Cache Internal Functions
// ============================================================================

size_t cache_next_pow2(size_t n);

void   lru_add_head(ht_cache_t *c, uint32_t idx);
void   lru_remove(ht_cache_t *c, uint32_t idx);
void   lru_promote(ht_cache_t *c, uint32_t idx);
void   lru_clear(ht_cache_t *c);

uint32_t cache_alloc_slot(ht_cache_t *c);
void     cache_free_slot(ht_cache_t *c, uint32_t idx);

uint64_t cache_compute_hash(ht_cache_t *c, const void *key, size_t key_len);
uint32_t cache_find_slot(ht_cache_t *c, uint64_t hash, const void *key, size_t key_len);
uint32_t cache_find_slot_in_buckets(ht_cache_t *c, uint64_t hash, const void *key, size_t key_len);

bool cache_add_to_buckets(ht_cache_t *c, uint64_t hash, uint32_t slot);
bool cache_remove_from_buckets(ht_cache_t *c, uint64_t hash, uint32_t slot);
void cache_clear_buckets(ht_cache_t *c);

bool cache_buckets_grow(ht_cache_t *c);
void cache_buckets_shrink(ht_cache_t *c);

#ifdef __cplusplus
}
#endif

#endif // DRAUGR_HT_CACHE_INTERNAL_H
