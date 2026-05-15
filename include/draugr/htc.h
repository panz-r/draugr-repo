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

/* ─── Error codes (Battery 9 Q10/Q27) ───────────────────── */
typedef enum {
    HTC_OK               = 0,
    HTC_ERR_DUPLICATE    = 1,  /* key already exists */
    HTC_ERR_NOT_FOUND    = 2,  /* key not present */
    HTC_ERR_OOM          = 3,  /* allocation failure */
    HTC_ERR_PATHOLOGICAL = 4,  /* adversarial hash — too many retries/grows */
    HTC_ERR_THREAD_LIMIT = 5,  /* max threads exceeded (epoch slot exhaustion) */
    HTC_ERR_BAD_CONFIG   = 6,  /* invalid configuration parameter */
} htc_error_t;

/*
 * ─── Public API contract (Battery 13 Q1) ────────────────────────
 *
 * htc_table_t is keyed by uint64_t identity_hash.
 *   Different user keys with the same 64-bit hash ALIAS at this layer.
 *
 * Operations guarantee:
 *   insert(H,V):  returns HTC_OK or HTC_ERR_DUPLICATE / HTC_ERR_OOM /
 *                 HTC_ERR_PATHOLOGICAL.  Never replaces existing H.
 *   upsert(H,V):  returns HTC_OK or HTC_ERR_OOM / HTC_ERR_PATHOLOGICAL.
 *                 Leaves exactly one logical entry for H.
 *   update(H,V):  returns HTC_OK or HTC_ERR_NOT_FOUND.
 *                 Atomically updates value at declared linearization point
 *                 (RELEASE store to user_value).
 *   find(H):      returns HTC_OK with value, or HTC_ERR_NOT_FOUND.
 *   remove(H):    returns HTC_OK or HTC_ERR_NOT_FOUND.
 *                 Delete linearizes at flags=DELETED (RELEASE store).
 *                 All find paths check flags before returning.
 *
 * Thread safety: fully concurrent readers + writers.
 *   Readers are lock-free (seq validation, no shard locks).
 *   Writers serialize via per-shard spinlocks (ascending shard ID order).
 *
 * Memory model:
 *   user_value is stored with __ATOMIC_RELEASE and loaded with __ATOMIC_ACQUIRE.
 *   If storing a pointer, the pointed-to object must be initialized and
 *   globally visible before being stored as the value.
 *
 * Epoch-based reclamation (EBR):
 *   Every reader must call htc_epoch_pin before accessing records and
 *   htc_epoch_unpin after.  Up to HTC_EPOCH_MAX_THREADS (64) threads.
 *   htc_thread_detach() clears front cache and releases epoch slot.
 *
 * Destroy:
 *   htc_destroy is QUIESCENT-ONLY: no concurrent operations may be in
 *   flight during destroy.  Debug builds may assert pinned epochs.
 *
 * Not supported:
 *   Async cancellation or signal handling inside table operations.
 *   Crash consistency (in-memory only; process abort may corrupt).
 *
 * Front cache (optional):
 *   Per-thread, validated by table pointer + table_id + generation +
 *   identity_hash + flags (all checked before returning user_value).
 *   Disable with HTC_CFG_DISABLE_FRONT_CACHE.
 *
 * Stats counters (optional):
 *   Enabled by #define HTC_STATS at compile time.
 *   See htc_stats_print() — zero overhead when disabled.
 * ──────────────────────────────────────────────────────────────────
 *
 * ─── Spec deviation waiver (Battery 19 Q29) ─────────────────────
 * Known deviations from ideal behavior, accepted for v1:
 *
 * 1. Destroy is quiescent-only, not synchronized.
 *    Caller guarantees no concurrent operations during htc_destroy().
 *    Debug builds may assert pinned epochs.
 *
 * 2. Record generation is 32-bit.
 *    Front-cache ABA after 2^32 reuses of the same arena index is
 *    theoretically possible.  Not expected in practice (2^32 reuses
 *    requires billions of insert/remove cycles on a single index).
 *
 * 3. htc_epoch_retire allocates retire nodes dynamically.
 *    If allocation fails, the record is freed synchronously (safe —
 *    slot was already cleared under seq protection).  This means
 *    remove is not allocation-free in the retire-node sense.
 *
 * 4. htc_epoch_retire_gen may fail (OOM).
 *    On failure, the old generation remains reachable via the gen
 *    chain and is freed during htc_destroy().  No leak, but memory
 *    pressure may accumulate until destroy.
 *
 * 5. TSAN may report false positives on custom atomics.
 *    The implementation uses __atomic builtins which TSAN generally
 *    understands, but some benign races in stats counters may be
 *    flagged.
 *
 * 6. PATHOLOGICAL vs OOM:
 *    htc_insert returns HTC_ERR_PATHOLOGICAL after 4 failed
 *    grow/reseed attempts.  This means the table could not find a
 *    valid placement within configured budgets.  Prior successful
 *    inserts remain findable.  Distinguished from OOM.
 *
 * 7. Reseed changes placement for all entries.
 *    This is intentional: reseed redistributes across the key space.
 *    Front cache entries remain valid because they are keyed by
 *    identity_hash, not placement.
 * ──────────────────────────────────────────────────────────────────
 *
 * ─── Progress guarantees (Battery 21 Q26) ────────────────────────
 *
 *   find:         lock-free (no shard locks, no allocation).
 *                 Readers may spin on bucket seq busy if a writer
 *                 stalls between seq_begin and seq_end.
 *
 *   insert:       blocking (acquires up to 2 shard locks).
 *                 BFS, stash, and grow paths bounded by config
 *                 (HTC_BFS_BUCKET_BUDGET, HTC_STASH_MAX, 4 max grows).
 *
 *   remove:       blocking (acquires up to 2 shard locks, no grow).
 *
 *   update:       lock-free (no locks, no allocation, no structural
 *                 mutation).  May race with concurrent remove (finds
 *                 slot deleted -> returns HTC_ERR_NOT_FOUND).
 *
 *   upsert:       blocking if insert required, lock-free if update
 *                 only (delegates to htc_place_entry under shard locks).
 *
 *   grow:         blocking (acquires grow_lock, then all shard locks).
 *                 Waits for in-flight writers to drain after freeze.
 *
 * NOT async-signal-safe.  NOT cancellation-safe inside any operation.
 * Threads must not be asynchronously cancelled or killed while holding
 * shard locks or inside seq_begin/seq_end regions.
 * ──────────────────────────────────────────────────────────────────
 */

htc_table_t *htc_create(const htc_config_t *cfg);
htc_table_t *htc_create_with_arena(const htc_config_t *cfg, struct arena *arena);
void         htc_destroy(htc_table_t *t);
void         htc_clear(htc_table_t *t);

htc_error_t htc_insert(htc_table_t *t, uint64_t hash, uint64_t value);
htc_error_t htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value);
htc_error_t htc_update(htc_table_t *t, uint64_t hash, uint64_t value);

htc_error_t htc_find(const htc_table_t *t, uint64_t hash, uint64_t *out_value);

htc_error_t htc_remove(htc_table_t *t, uint64_t hash);

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
