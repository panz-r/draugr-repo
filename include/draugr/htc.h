#ifndef DRAUGR_HTC_H
#define DRAUGR_HTC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct arena;

typedef struct htc_table htc_table_t;

/* ─── Generic AMQ filter interface (spec §25) ────────────────
 * Allows attaching any hash-based AMQ filter (cuckoo, vacuum, etc.)
 * to an htc table for negative lookup acceleration.
 *
 * All function pointers take the opaque filter pointer and a uint64_t
 * hash. The filter is NOT owned by the table — caller must destroy it.
 * ─────────────────────────────────────────────────────────────── */
typedef struct {
    void   *filter;        /* opaque filter handle */
    bool  (*insert)(void *filter, uint64_t hash);
    bool  (*lookup)(void *filter, uint64_t hash);
    bool  (*delete)(void *filter, uint64_t hash);
} htc_amq_filter_t;

typedef struct {
    uint32_t initial_buckets;
    double   max_load_factor;
    uint32_t shard_count;       /* default 128, 0 = use default */
    uint32_t flags;             /* bitfield: 1 = disable front cache */
} htc_config_t;

#define HTC_CFG_DISABLE_FRONT_CACHE 1u

htc_table_t *htc_create(const htc_config_t *cfg);
htc_table_t *htc_create_with_arena(const htc_config_t *cfg, struct arena *arena);
void         htc_destroy(htc_table_t *t);
void         htc_clear(htc_table_t *t);

bool htc_insert(htc_table_t *t, uint64_t hash, uint64_t value);
bool htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value);
bool htc_update(htc_table_t *t, uint64_t hash, uint64_t value);

bool htc_find(const htc_table_t *t, uint64_t hash, uint64_t *out_value);

bool htc_remove(htc_table_t *t, uint64_t hash);

size_t htc_size(const htc_table_t *t);

/* Optional AMQ filter (spec §25). Attach a filter for negative lookup
 * acceleration. The filter is NOT owned by the table — caller must
 * destroy it after htc_destroy. Pass NULL to detach. */
void        htc_set_filter(htc_table_t *t, const htc_amq_filter_t *amq);
const htc_amq_filter_t *htc_get_filter(const htc_table_t *t);

/* Convenience wrappers for built-in filter types */
#include "draugr/cuckoo_filter.h"
#include "draugr/vacuum_filter.h"

static inline bool htc_amq_cuckoo_insert(void *cf, uint64_t h) {
    return cuckoo_filter_insert((cuckoo_filter_t *)cf, h) == CUCKOO_OK;
}
static inline bool htc_amq_cuckoo_delete(void *cf, uint64_t h) {
    return cuckoo_filter_delete((cuckoo_filter_t *)cf, h) == CUCKOO_OK;
}
static inline htc_amq_filter_t htc_amq_cuckoo(cuckoo_filter_t *cf) {
    htc_amq_filter_t f = { .filter = cf,
        .insert = htc_amq_cuckoo_insert,
        .lookup = (bool (*)(void *, uint64_t))cuckoo_filter_lookup,
        .delete = htc_amq_cuckoo_delete };
    return f;
}

static inline bool htc_amq_vacuum_insert(void *vf, uint64_t h) {
    return vacuum_filter_insert((vacuum_filter_t *)vf, h) == VACUUM_OK;
}
static inline bool htc_amq_vacuum_delete(void *vf, uint64_t h) {
    return vacuum_filter_delete((vacuum_filter_t *)vf, h) == VACUUM_OK;
}
static inline htc_amq_filter_t htc_amq_vacuum(vacuum_filter_t *vf) {
    htc_amq_filter_t f = { .filter = vf,
        .insert = htc_amq_vacuum_insert,
        .lookup = (bool (*)(void *, uint64_t))vacuum_filter_lookup,
        .delete = htc_amq_vacuum_delete };
    return f;
}

#ifdef __cplusplus
}
#endif

#endif
