#ifndef DRAUGR_HT_H
#define DRAUGR_HT_H

/**
 * Draugr: Hash Table with Robin-Hood, Graveyard & Zombie Hashing
 *
 * Combines three techniques for O(x) expected operations at load factor 1-1/x:
 *
 * - Robin-Hood linear probing: entries with larger probe distance rob slots
 *   from entries with smaller probe distance, keeping probe distances balanced.
 * - Graveyard hashing: prophylactic tombstones placed at evenly-spaced
 *   positions during rebuilds to break up primary clustering.
 *   (Bender, Kuszmaul, Kuszmaul, FOCS 2021)
 * - Zombie hashing: de-amortized tombstone redistribution via incremental
 *   interval rebuilds. One interval of c_b*x slots is rebuilt per insert,
 *   cleaning excess tombstones and maintaining primitive positions.
 *   (Chesetti, Shi, Phillips, Pandey, SIGMOD 2025)
 */

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct arena;

typedef struct ht_bare ht_bare_t;
typedef struct ht_table ht_table_t;

struct ht_iter {
    size_t idx;
    bool started;
};
typedef struct ht_iter ht_iter_t;

typedef uint64_t (*ht_hash_fn)(const void *key, size_t key_len, void *user_ctx);
typedef bool (*ht_eq_fn)(const void *key_a, size_t len_a,
                         const void *key_b, size_t len_b, void *user_ctx);

typedef struct {
    size_t initial_capacity;   // Power of two (default 64)
    double max_load_factor;    // Grow when exceeded (default 0.75)
    double min_load_factor;    // Shrink when below (default 0.20, 0 disables)
    double tomb_threshold;     // Trigger rebuild when tombstone ratio exceeds (default 0.20)
    size_t zombie_window;      // Slots per zombie rebuild step (0 = disable, default 16)
    size_t max_probe_dist;     // Max probe distance before overflow stash (0 = no limit, default 255)
} ht_config_t;

typedef bool (*ht_dup_callback)(const void *key, size_t key_len,
                                const void *value, size_t value_len,
                                void *user_ctx);

// ============================================================================
// Bare Table: hash → uint32_t
// ============================================================================

// Callback for ht_bare_find_all. Return false to stop iteration.
// Note: UINT32_MAX (0xFFFFFFFF) is reserved and cannot be stored as a value.
typedef bool (*ht_bare_callback)(uint32_t val, void *user_ctx);

ht_bare_t *ht_bare_create(const ht_config_t *cfg, struct arena *arena);
void ht_bare_destroy(ht_bare_t *t);
void ht_bare_clear(ht_bare_t *t);

bool ht_bare_insert(ht_bare_t *t, uint64_t hash, uint32_t val);
bool ht_bare_find(const ht_bare_t *t, uint64_t hash, uint32_t *out_val);
void ht_bare_find_all(const ht_bare_t *t, uint64_t hash,
                      ht_bare_callback cb, void *user_ctx);
size_t ht_bare_remove(ht_bare_t *t, uint64_t hash);
bool ht_bare_remove_val(ht_bare_t *t, uint64_t hash, uint32_t val);

bool ht_bare_resize(ht_bare_t *t, size_t new_capacity);
void ht_bare_compact(ht_bare_t *t);

ht_iter_t ht_bare_iter_begin(const ht_bare_t *t);
bool ht_bare_iter_next(ht_bare_t *t, ht_iter_t *iter,
                       uint64_t *out_hash, uint32_t *out_val);

// ============================================================================
// High-Level Table: key → value
// ============================================================================

// Ownership: key/value data passed to insert/upsert/unsert is copied into
// internal storage owned by the table.  The caller retains ownership of the
// original buffers.  When an entry is removed (remove, upsert-dedup, destroy,
// clear), the table frees its internal copy.
//
// Iterator invalidation: pointers returned by ht_iter_next point into the
// table's internal storage and become dangling after the entry is removed,
// replaced by upsert (with a different value size), or after resize/compact/
// clear/destroy.  Copy key data before removing if needed after the call.

// ============================================================================
// Lifecycle
// ============================================================================

// ht_create: allocate a table backed by malloc.
ht_table_t *ht_create(const ht_config_t *cfg,
                       ht_hash_fn hash_fn, ht_eq_fn eq_fn,
                       void *user_ctx);

// ht_create_with_arena: allocate a table backed by the given arena.
// Internal kv storage is allocated via arena_alloc and freed via arena_free
// on removal, destroy, and clear.
ht_table_t *ht_create_with_arena(const ht_config_t *cfg,
                                    ht_hash_fn hash_fn, ht_eq_fn eq_fn,
                                    void *user_ctx, struct arena *arena);

// ht_destroy: free all internal storage and the table itself.
// With arena mode, each live entry's kv storage is returned via arena_free.
void ht_destroy(ht_table_t *t);

// ht_clear: remove all entries, keeping the table usable.
// Malloc mode: frees each entry's kv storage individually.
// Arena mode: calls arena_clear (reclaims all arena memory in bulk).
void ht_clear(ht_table_t *t);

// ============================================================================
// Insertion & Updates
// ============================================================================

typedef enum {
    HT_INSERT_FAILED = -1,
    HT_INSERT_UPDATE =  0,
    HT_INSERT_OK     =  1,
} ht_insert_result_t;

// ht_insert: always-add (multi-value). Same k,v can exist N times.
// Returns HT_INSERT_FAILED on error, HT_INSERT_OK on success.
ht_insert_result_t ht_insert(ht_table_t *t, const void *key, size_t key_len,
                            const void *value, size_t value_len);
ht_insert_result_t ht_insert_with_hash(ht_table_t *t, uint64_t hash,
                                      const void *key, size_t key_len,
                                      const void *value, size_t value_len);

// ht_upsert: replace all entries for key with a single new value.
// If the key already exists, the old entry's kv storage is freed and
// duplicates (from prior ht_insert) are removed and freed.
// Returns HT_INSERT_FAILED on error, HT_INSERT_OK if new entry,
// or HT_INSERT_UPDATE if replaced existing.
ht_insert_result_t ht_upsert(ht_table_t *t, const void *key, size_t key_len,
                             const void *value, size_t value_len);
ht_insert_result_t ht_upsert_with_hash(ht_table_t *t, uint64_t hash,
                                       const void *key, size_t key_len,
                                       const void *value, size_t value_len);

// ht_unsert (unique-insert): insert only if exact k,v pair doesn't exist.
// Returns HT_INSERT_FAILED on error, HT_INSERT_OK if new entry,
// or HT_INSERT_UPDATE if duplicate key found (no insert).
ht_insert_result_t ht_unsert(ht_table_t *t, const void *key, size_t key_len,
                             const void *value, size_t value_len);
ht_insert_result_t ht_unsert_with_hash(ht_table_t *t, uint64_t hash,
                                       const void *key, size_t key_len,
                                       const void *value, size_t value_len);

int64_t ht_inc(ht_table_t *t, const void *key, size_t key_len, int64_t delta);
int64_t ht_inc_with_hash(ht_table_t *t, uint64_t hash,
                          const void *key, size_t key_len, int64_t delta,
                          bool *ok);

// ============================================================================
// Lookup
// ============================================================================

const void *ht_find(const ht_table_t *t, const void *key, size_t key_len,
                    size_t *out_value_len);
const void *ht_find_with_hash(const ht_table_t *t, uint64_t hash,
                              const void *key, size_t key_len,
                              size_t *out_value_len);
void ht_find_all(const ht_table_t *t, uint64_t hash,
                 ht_dup_callback cb, void *user_ctx);

// ht_find_key_all: iterate all entries matching exact key.
void ht_find_key_all(const ht_table_t *t, const void *key, size_t key_len,
                     ht_dup_callback cb, void *user_ctx);
void ht_find_key_all_with_hash(const ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               ht_dup_callback cb, void *user_ctx);

// ht_find_kv: find first entry matching exact key AND value.
const void *ht_find_kv(const ht_table_t *t, const void *key, size_t key_len,
                       const void *value, size_t value_len,
                       size_t *out_value_len);
const void *ht_find_kv_with_hash(const ht_table_t *t, uint64_t hash,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 size_t *out_value_len);

// ============================================================================
// Deletion
// ============================================================================

// All remove functions free the table's internal copy of the removed entry's
// key+value storage.  The caller's key/value buffers (passed by pointer) are
// not freed — only the table's copy.

// ht_remove: remove ALL entries for key, return count removed.
size_t ht_remove(ht_table_t *t, const void *key, size_t key_len);
size_t ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                            const void *key, size_t key_len);

// ht_remove_kv: remove ALL matching k,v pairs, return count removed.
size_t ht_remove_kv(ht_table_t *t, const void *key, size_t key_len,
                    const void *value, size_t value_len);
size_t ht_remove_kv_with_hash(ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len);

// ht_remove_kv_one: remove one matching k,v pair, return true/false.
bool ht_remove_kv_one(ht_table_t *t, const void *key, size_t key_len,
                      const void *value, size_t value_len);
bool ht_remove_kv_one_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len);

// ============================================================================
// Resizing & Compaction
// ============================================================================

bool ht_resize(ht_table_t *t, size_t new_capacity);
bool ht_compact(ht_table_t *t);

// ============================================================================
// Iteration
// ============================================================================

ht_iter_t ht_iter_begin(const ht_table_t *t);
bool ht_iter_next(ht_table_t *t, ht_iter_t *iter,
                  const void **out_key, size_t *out_key_len,
                  const void **out_value, size_t *out_value_len);

// ============================================================================
// Statistics
// ============================================================================

typedef struct {
    size_t  size;
    size_t  capacity;
    size_t  tombstone_cnt;
    double  load_factor;
    double  tombstone_ratio;
} ht_stats_t;

void ht_stats(const ht_table_t *t, ht_stats_t *out_stats);
void ht_dump(const ht_table_t *t, uint32_t h32, size_t count);
size_t ht_size(const ht_table_t *t);

void ht_bare_stats(const ht_bare_t *t, ht_stats_t *out_stats);
const char *ht_bare_check_invariants(const ht_bare_t *t);
void ht_bare_dump(const ht_bare_t *t, uint64_t hash, size_t count);

// Returns NULL if invariants hold, or a static error string.
// Invariants checked:
//   1. probe_dist == ((pos - ideal + capacity) % capacity) for every live entry
//   2. probe_dists are non-decreasing within clusters (Robin-Hood invariant)
//   3. size matches actual count of live entries
//   4. tombstone_cnt matches actual count of tombstones
const char *ht_check_invariants(const ht_table_t *t);

#ifdef __cplusplus
}
#endif

#endif // DRAUGR_HT_H
