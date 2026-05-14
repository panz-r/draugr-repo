#ifndef DRAUGR_HT_INTERNAL_H
#define DRAUGR_HT_INTERNAL_H

/**
 * Draugr Internal API — for testing and low-level implementors
 *
 * Include this header to get full access to internal structs, constants,
 * sentinel values, and helper functions. Most users do NOT need this.
 *
 * Exposes:
 *   - Full ht_bare_t struct definition
 *   - Full ht_table_t struct definition
 *   - ht_entry_t struct
 *   - Sentinel constants (HASH_EMPTY, HASH_TOMB, etc.)
 *   - Pack/unpack inline helpers
 *   - All bare table internal helpers
 *   - Arena helpers
 */

#include "draugr/ht.h"
#include "draugr/alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// ============================================================================
// Sentinels & Pack/Unpack
// ============================================================================

// Hash values with lower 48 bits 0 or 1 are reserved:
//   0 → unoccupied (HASH_EMPTY)
//   1 → tombstone (HASH_TOMB)
//   ≥2 → live entry
#define HASH_EMPTY     0ULL
#define HASH_TOMB      1ULL
#define HASH_MASK      0x0000FFFFFFFFFFFFULL  // Lower 48 bits of 64-bit hash
#define VAL_NONE       UINT32_MAX

static inline uint64_t hpd_hash(uint64_t hpd) {
    return hpd & HASH_MASK;
}

static inline uint16_t hpd_pd(uint64_t hpd) {
    return (uint16_t)(hpd >> 48);
}

static inline bool hpd_empty(uint64_t hpd) {
    return hpd_hash(hpd) == HASH_EMPTY;
}

static inline bool hpd_tomb(uint64_t hpd) {
    return hpd_hash(hpd) == HASH_TOMB;
}

static inline bool hpd_available(uint64_t hpd) {
    return hpd_hash(hpd) <= HASH_TOMB;
}

static inline bool hpd_live(uint64_t hpd) {
    return hpd_hash(hpd) >= 2;
}

static inline uint64_t hpd_pack(uint64_t hash, uint16_t probe_dist) {
    return ((uint64_t)probe_dist << 48) | (hash & HASH_MASK);
}

// ============================================================================
// Constants
// ============================================================================

#define C_P_DEFAULT     3.0
#define C_B_DEFAULT     3.0
#define SPILL_INITIAL   8
#define BSHIFT_CAP      16

// Overflow stash capacity (fixed, embedded in ht_bare_t)
#define HT_OVERFLOW_STASH_CAP 32

// Insert modes (internal)
#define INS_UPSERT  0
#define INS_ALWAYS  1
#define INS_UNIQUE  2

// ============================================================================
// Entry Type (high-level only)
// ============================================================================

typedef struct {
	uint16_t key_len;
	uint16_t hash_hi;
	uint32_t val_len;
	uint8_t *kv_ptr;
} ht_entry_t;

static inline size_t ht_entry_key_storage(size_t key_len) {
	return (key_len + 7) & ~(size_t)7;
}

static inline const void *ht_entry_key(const ht_entry_t *e) {
	return e->kv_ptr;
}

static inline const void *ht_entry_val(const ht_entry_t *e) {
	return e->kv_ptr + ht_entry_key_storage(e->key_len);
}

// ============================================================================
// Bare Table Structure
// ============================================================================

struct ht_bare {
    uint64_t *hash_pd;
    uint32_t *vals;
    uint8_t *main_block;
    size_t capacity;
    size_t size;
    size_t tombstone_cnt;

    uint8_t *spill_block;
    uint64_t *spill_hash_pd;
    uint32_t *spill_vals;
    size_t spill_cap;
    size_t spill_len;
    bool spill_in_block;

    double max_load_factor;
    double min_load_factor;
    double tomb_threshold;
    size_t zombie_window;
    size_t max_probe_dist;

    size_t overflow_len;
    uint64_t overflow_hash_pd[HT_OVERFLOW_STASH_CAP];
    uint32_t overflow_vals[HT_OVERFLOW_STASH_CAP];

    bool overflow_violated;  // Policy B was triggered; entries may have dist > max_probe_dist
    bool resizing;
};

// ============================================================================
// High-Level Table Structure
// ============================================================================

struct ht_table {
    ht_bare_t bare;

    ht_entry_t *entries;
    uint8_t *table_block;
    size_t entry_count;
    size_t entry_cap;
    bool entries_in_block;

    struct arena *allocator;

    ht_hash_fn hash_fn;
    ht_eq_fn eq_fn;
    void *user_ctx;
};

// ============================================================================
// Bare Table Internal Functions
// ============================================================================

size_t  next_pow2(size_t n);
double  bare_compute_x(const ht_bare_t *t);

bool    bare_spill_grow(ht_bare_t *t);
bool    bare_spill_insert(ht_bare_t *t, uint64_t h48, uint32_t val);
bool    bare_spill_find(const ht_bare_t *t, uint64_t h48, uint32_t *out_val);
void    bare_spill_find_all(const ht_bare_t *t, uint64_t h48,
                           ht_bare_callback cb, void *user_ctx);
size_t  bare_spill_remove(ht_bare_t *t, uint64_t h48);
bool    bare_spill_remove_val(ht_bare_t *t, uint64_t h48, uint32_t val);

bool    bare_resize_table(ht_bare_t *t);
bool    bare_rh_insert_bounded(ht_bare_t *t, uint64_t h48, uint32_t val);

bool    bare_verify_ideal_safe(const ht_bare_t *t, size_t idx, size_t len);
void    bare_commit_backward_shift(ht_bare_t *t, size_t idx, size_t len);
void    bare_delete_compact(ht_bare_t *t, size_t idx);

void    bare_place_prophylactic_tombstones(ht_bare_t *t);
void    bare_reinsert_main(ht_bare_t *t,
                           const uint64_t *old_hash_pd,
                           const uint32_t *old_vals,
                           size_t old_cap);
void    bare_reinsert_spill(ht_bare_t *t,
                            const uint64_t *old_spill_hash_pd,
                            const uint32_t *old_spill_vals,
                            size_t old_spill_len);

// ============================================================================
// Arena Helpers
// ============================================================================

#ifdef __cplusplus
}
#endif

#endif // DRAUGR_HT_INTERNAL_H
