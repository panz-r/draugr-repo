#ifndef DRAUGR_HTC_FILTER_H
#define DRAUGR_HTC_FILTER_H

/**
 * htc_filter.h — Concurrent AMQ filter wrapper (spec §25)
 *
 * Thread-safe wrapper around any htc_amq_filter_t-compatible filter.
 * Uses a spinlock to serialize access, following the ht/htc pattern:
 *   filter (single-threaded) → htc_filter (concurrent wrapper)
 *
 * Usage:
 *   cuckoo_filter_t *cf = cuckoo_filter_create(...);
 *   htc_filter_t *hf = htc_filter_create(htc_amq_cuckoo(cf));
 *   htc_set_filter(t, htc_amq_htc_filter(hf));
 */

#include "draugr/htc.h"
#include <stdlib.h>
#include <stdatomic.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    htc_amq_filter_t inner;        /* the wrapped filter */
    _Atomic uint8_t  lock;         /* spinlock */
} htc_filter_t;

/* Create a concurrent filter wrapping an existing AMQ filter.
 * The inner filter must outlive the wrapper. */
static inline htc_filter_t *htc_filter_create(htc_amq_filter_t inner) {
    htc_filter_t *hf = (htc_filter_t *)malloc(sizeof(htc_filter_t));
    if (!hf) return NULL;
    hf->inner = inner;
    hf->lock = 0;
    return hf;
}

static inline void htc_filter_destroy(htc_filter_t *hf) {
    free(hf);
}

static inline bool htc_filter_insert(void *hf, uint64_t hash) {
    htc_filter_t *f = (htc_filter_t *)hf;
    while (__atomic_exchange_n(&f->lock, 1, __ATOMIC_ACQUIRE));
    bool r = f->inner.insert(f->inner.filter, hash);
    __atomic_store_n(&f->lock, 0, __ATOMIC_RELEASE);
    return r;
}

static inline bool htc_filter_lookup(void *hf, uint64_t hash) {
    htc_filter_t *f = (htc_filter_t *)hf;
    while (__atomic_exchange_n(&f->lock, 1, __ATOMIC_ACQUIRE));
    bool r = f->inner.lookup(f->inner.filter, hash);
    __atomic_store_n(&f->lock, 0, __ATOMIC_RELEASE);
    return r;
}

static inline bool htc_filter_delete(void *hf, uint64_t hash) {
    htc_filter_t *f = (htc_filter_t *)hf;
    while (__atomic_exchange_n(&f->lock, 1, __ATOMIC_ACQUIRE));
    bool r = f->inner.delete(f->inner.filter, hash);
    __atomic_store_n(&f->lock, 0, __ATOMIC_RELEASE);
    return r;
}

/* Build an htc_amq_filter_t from an htc_filter_t for use with htc_set_filter */
static inline htc_amq_filter_t htc_amq_htc_filter(htc_filter_t *hf) {
    htc_amq_filter_t f = { .filter = hf,
        .insert = htc_filter_insert,
        .lookup = htc_filter_lookup,
        .delete = htc_filter_delete };
    return f;
}

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_HTC_FILTER_H */
