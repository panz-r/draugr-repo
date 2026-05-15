#ifndef DRAUGR_HTC_KV_H
#define DRAUGR_HTC_KV_H

/**
 * htc_kv.h — High-level concurrent key-value store backed by htc
 *
 * Wraps htc_table_t with a hash-plus-key-comparison interface.
 * Stores (hash, key_ptr, key_len, value_ptr, value_len) in a single
 * arena record. Keys must outlive the table (caller-managed) or be
 * copied in via htc_kv_insert_copy.
 *
 * Thread safety: same as htc (sharded writers + optimistic reads).
 */

#include "draugr/htc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct htc_kv htc_kv_t;

/* ─── Lifecycle ─────────────────────────────────────────────── */
htc_kv_t *htc_kv_create(const htc_config_t *cfg);
void      htc_kv_destroy(htc_kv_t *kv);

/* ─── Core operations ───────────────────────────────────────── */
/* Insert key-value pair. Key pointer must remain valid for the
 * lifetime of the entry. For copy semantics, use htc_kv_insert_copy. */
bool htc_kv_insert(htc_kv_t *kv, const void *key, size_t klen,
                   const void *val, size_t vlen);

/* Insert with internal copy of key and value. */
bool htc_kv_insert_copy(htc_kv_t *kv, const void *key, size_t klen,
                        const void *val, size_t vlen);

/* Find value by key. Returns false if key not found.
 * If val is NULL, only checks existence. */
bool htc_kv_find(htc_kv_t *kv, const void *key, size_t klen,
                 void *val, size_t *vlen);

/* Remove key-value pair. Returns false if key not found. */
bool htc_kv_remove(htc_kv_t *kv, const void *key, size_t klen);

/* Attach an AMQ filter to accelerate negative lookups.
 * The filter is NOT owned by htc_kv — caller must destroy it. */
void htc_kv_set_filter(htc_kv_t *kv, const htc_amq_filter_t *amq);

/* ─── Iterator ──────────────────────────────────────────────────
 * Snapshot iterator: walks all live arena records at creation time.
 * Not safe for concurrent insert/remove during iteration.
 * ─────────────────────────────────────────────────────────────── */
typedef struct {
    const htc_kv_t *kv;
    uint32_t        idx;         /* current arena index */
    uint32_t        count;       /* arena count at snapshot */
    const void     *key;
    size_t          klen;
    const void     *val;
    size_t          vlen;
} htc_kv_iter_t;

/* Initialize iterator. Call before first htc_kv_iter_next. */
void htc_kv_iter_init(htc_kv_iter_t *it, const htc_kv_t *kv);

/* Advance to next live entry. Returns false at end. */
bool htc_kv_iter_next(htc_kv_iter_t *it);

/* ─── Stats ─────────────────────────────────────────────────── */
size_t htc_kv_count(const htc_kv_t *kv);
size_t htc_kv_memory_bytes(const htc_kv_t *kv);

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_HTC_KV_H */
