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
} ht_config_t;

typedef bool (*ht_dup_callback)(const void *key, size_t key_len,
                                const void *value, size_t value_len,
                                void *user_ctx);

// ============================================================================
// Lifecycle
// ============================================================================

ht_table_t *ht_create(const ht_config_t *cfg,
                       ht_hash_fn hash_fn, ht_eq_fn eq_fn,
                       void *user_ctx);
void ht_destroy(ht_table_t *t);
void ht_clear(ht_table_t *t);

// ============================================================================
// Insertion & Updates
// ============================================================================

bool ht_insert(ht_table_t *t, const void *key, size_t key_len,
               const void *value, size_t value_len);
bool ht_insert_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len);
int64_t ht_inc(ht_table_t *t, const void *key, size_t key_len, int64_t delta);

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

// ============================================================================
// Deletion
// ============================================================================

bool ht_remove(ht_table_t *t, const void *key, size_t key_len);
bool ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len);

// ============================================================================
// Resizing & Compaction
// ============================================================================

bool ht_resize(ht_table_t *t, size_t new_capacity);
void ht_compact(ht_table_t *t);

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

// Returns NULL if invariants hold, or a static error string.
// Invariants checked:
//   1. probe_dist == (pos - ideal) % capacity for every live entry
//   2. probe_dists are non-decreasing within clusters (Robin-Hood invariant)
//   3. size matches actual count of live entries
//   4. tombstone_cnt matches actual count of tombstones
const char *ht_check_invariants(const ht_table_t *t);

#ifdef __cplusplus
}
#endif

#endif // DRAUGR_HT_H
