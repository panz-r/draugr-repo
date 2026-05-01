/**
 * Draugr Hash Table — Layered Implementation
 *
 * Two layers:
 *   1. Bare table (ht_bare_t): uint64_t hash → uint32_t val
 *      Robin-Hood linear probing + Graveyard tombstones + Zombie rebuild.
 *      No key/value storage, no hash function. Caller provides hash.
 *
 *   2. High-level table (ht_table_t): key → value
 *      Wraps ht_bare_t, adds hash function, key comparison, arena storage.
 *      Current public API (ht_insert, ht_find, etc.) unchanged.
 *
 * SoA probe layout (both layers):
 *   hash_pd[i] — uint64_t: lower 48 bits = hash, upper 16 bits = probe_dist
 *   vals[i]    — uint32_t: value (UINT32_MAX = empty/tomb)
 *
 * Sentinels:
 *   hash_pd lower 48 bits == 0  →  unoccupied
 *   hash_pd lower 48 bits == 1  →  tombstone
 *   hash_pd lower 48 bits >= 2  →  live entry
 *
 * Hash values 0 and 1 (lower 48 bits) are reserved.  Entries whose hash
 * falls in this range go to a small "spill lane" instead of the main table.
 */

#include "draugr/ht.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Entry Type (high-level only)
// ============================================================================

typedef struct {
    uint16_t key_len;
    uint16_t hash_hi;      // upper 16 bits of full 64-bit hash
    uint32_t val_len;
    uint32_t arena_offset;
} ht_entry_t;  // 12 bytes

// ============================================================================
// Sentinels & Pack/Unpack
// ============================================================================

#define HASH_EMPTY     0ULL
#define HASH_TOMB      1ULL
#define HASH_MASK      0x0000FFFFFFFFFFFFULL
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
// Bare Table Structure
// ============================================================================

struct ht_bare {
    // Main table (SoA probe arrays)
    uint64_t   *hash_pd;
    uint32_t   *vals;
    size_t      capacity;
    size_t      size;
    size_t      tombstone_cnt;

    // Spill lane
    uint64_t   *spill_hash_pd;
    uint32_t   *spill_vals;
    size_t      spill_cap;
    size_t      spill_len;

    // Config
    double      max_load_factor;
    double      min_load_factor;
    double      tomb_threshold;
    size_t      zombie_window;

    // Zombie rebuild state
    size_t      zombie_cursor;

    bool        resizing;
};

// ============================================================================
// High-Level Table Structure
// ============================================================================

struct ht_table {
    ht_bare_t   bare;             // Embedded bare table

    // Entry storage
    ht_entry_t *entries;
    size_t      entry_count;
    size_t      entry_cap;

    // Arena (key+value bytes)
    uint8_t    *arena;
    size_t      arena_size;
    size_t      arena_cap;

    // Functions
    ht_hash_fn  hash_fn;
    ht_eq_fn    eq_fn;
    void       *user_ctx;
};

// ============================================================================
// Constants
// ============================================================================

#define C_P_DEFAULT 3.0
#define C_B_DEFAULT 3.0
#define SPILL_INITIAL 8
#define BSHIFT_CAP 16

static const ht_config_t default_cfg = {
    .initial_capacity = 64,
    .max_load_factor = 0.75,
    .min_load_factor = 0.20,
    .tomb_threshold = 0.20,
    .zombie_window = 16,
};

// Insert modes (internal)
#define INS_UPSERT  0
#define INS_ALWAYS  1
#define INS_UNIQUE  2

// ============================================================================
// Utility
// ============================================================================

static size_t next_pow2(size_t n) {
    size_t r = 1;
    while (r < n) r <<= 1;
    return r;
}

// ============================================================================
// Bare Internal: Compute X
// ============================================================================

static double bare_compute_x(const ht_bare_t *t) {
    double lf = (double)t->size / (double)t->capacity;
    if (lf >= 1.0) return (double)t->capacity;
    if (lf < 0.01) return 1.0;
    return 1.0 / (1.0 - lf);
}

// ============================================================================
// Bare Internal: Spill Lane
// ============================================================================

static bool bare_spill_grow(ht_bare_t *t) {
    size_t new_cap = t->spill_cap ? t->spill_cap * 2 : SPILL_INITIAL;

    uint64_t *new_hpd = realloc(t->spill_hash_pd, new_cap * sizeof(uint64_t));
    if (!new_hpd) return false;

    uint32_t *new_vals = realloc(t->spill_vals, new_cap * sizeof(uint32_t));
    if (!new_vals) {
        t->spill_hash_pd = new_hpd;
        return false;
    }

    memset(new_hpd + t->spill_cap, 0, (new_cap - t->spill_cap) * sizeof(uint64_t));
    memset(new_vals + t->spill_cap, 0xFF, (new_cap - t->spill_cap) * sizeof(uint32_t));

    t->spill_hash_pd = new_hpd;
    t->spill_vals = new_vals;
    t->spill_cap = new_cap;
    return true;
}

static bool bare_spill_insert(ht_bare_t *t, uint64_t h48, uint32_t val) {
    if (t->spill_len >= t->spill_cap) {
        if (!bare_spill_grow(t)) return false;
    }
    t->spill_hash_pd[t->spill_len] = hpd_pack(h48, 0);
    t->spill_vals[t->spill_len] = val;
    t->spill_len++;
    t->size++;
    return true;
}

static bool bare_spill_find(const ht_bare_t *t, uint64_t h48, uint32_t *out_val) {
    for (size_t i = 0; i < t->spill_len; i++) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48) {
            if (out_val) *out_val = t->spill_vals[i];
            return true;
        }
    }
    return false;
}

static void bare_spill_find_all(const ht_bare_t *t, uint64_t h48,
                                ht_bare_callback cb, void *user_ctx) {
    for (size_t i = 0; i < t->spill_len; i++) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48) {
            if (!cb(t->spill_vals[i], user_ctx))
                return;
        }
    }
}

static size_t bare_spill_remove(ht_bare_t *t, uint64_t h48) {
    size_t removed = 0;
    for (size_t i = 0; i < t->spill_len; ) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48) {
            memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                    (t->spill_len - i - 1) * sizeof(uint64_t));
            memmove(&t->spill_vals[i], &t->spill_vals[i + 1],
                    (t->spill_len - i - 1) * sizeof(uint32_t));
            t->spill_len--;
            t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
            t->spill_vals[t->spill_len] = VAL_NONE;
            t->size--;
            removed++;
        } else {
            i++;
        }
    }
    return removed;
}

static bool bare_spill_remove_val(ht_bare_t *t, uint64_t h48, uint32_t val) {
    for (size_t i = 0; i < t->spill_len; i++) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48 && t->spill_vals[i] == val) {
            memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                    (t->spill_len - i - 1) * sizeof(uint64_t));
            memmove(&t->spill_vals[i], &t->spill_vals[i + 1],
                    (t->spill_len - i - 1) * sizeof(uint32_t));
            t->spill_len--;
            t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
            t->spill_vals[t->spill_len] = VAL_NONE;
            t->size--;
            return true;
        }
    }
    return false;
}

// ============================================================================
// Bare Internal: Robin-Hood Insert (always-add)
// ============================================================================

static bool bare_resize_table(ht_bare_t *t);

static bool bare_rh_insert(ht_bare_t *t, uint64_t h48, uint32_t val) {
    size_t cap_mask = t->capacity - 1;
    size_t ideal = h48 & cap_mask;

    uint32_t cur_val = val;
    size_t idx = ideal;
    uint16_t dist = 0;

    while (1) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_available(slot_hpd)) {
            if (hpd_tomb(slot_hpd)) {
                bool blocked = false;
                for (size_t k = 1; k <= BSHIFT_CAP; k++) {
                    size_t chk = (idx + k) & cap_mask;
                    uint64_t chk_hpd = t->hash_pd[chk];
                    if (hpd_empty(chk_hpd)) break;
                    if (hpd_tomb(chk_hpd)) continue;
                    if (hpd_pd(chk_hpd) > dist + (uint16_t)k) {
                        blocked = true;
                        break;
                    }
                }
                if (blocked) {
                    idx = (idx + 1) & cap_mask;
                    dist++;
                    continue;
                }
                t->tombstone_cnt--;
            }
            t->hash_pd[idx] = hpd_pack(h48, dist);
            t->vals[idx] = cur_val;
            t->size++;
            return true;
        }

        if (hpd_pd(slot_hpd) < dist) {
            uint32_t old_val = t->vals[idx];

            t->hash_pd[idx] = hpd_pack(h48, dist);
            t->vals[idx] = cur_val;

            h48 = hpd_hash(slot_hpd);
            dist = hpd_pd(slot_hpd) + 1;
            cur_val = old_val;

            idx = (idx + 1) & cap_mask;
            continue;
        }

        idx = (idx + 1) & cap_mask;
        dist++;

        if (dist > t->capacity) {
            if (!bare_resize_table(t)) return false;
            return bare_rh_insert(t, h48, cur_val);
        }
    }
}

// ============================================================================
// Bare Internal: Delete Compact / Backward Shift
// ============================================================================

static bool bare_verify_ideal_safe(const ht_bare_t *t, size_t idx, size_t len) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t pos = (idx + 1 + i) & cap_mask;
        uint64_t hpd = t->hash_pd[pos];
        if (!hpd_live(hpd)) continue;

        size_t target = (idx + write_offset) & cap_mask;
        size_t ideal = hpd_hash(hpd) & cap_mask;
        if (ideal > target && (ideal - target) < t->capacity / 2) return false;
        write_offset++;
    }
    return true;
}

static void bare_commit_backward_shift(ht_bare_t *t, size_t idx, size_t len) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t read_pos = (idx + 1 + i) & cap_mask;
        uint64_t hpd = t->hash_pd[read_pos];

        if (hpd_live(hpd)) {
            size_t write_pos = (idx + write_offset) & cap_mask;
            size_t shift = (i + 1) - write_offset;
            t->hash_pd[write_pos] = hpd_pack(hpd_hash(hpd), hpd_pd(hpd) - (uint16_t)shift);
            t->vals[write_pos] = t->vals[read_pos];
            write_offset++;
        } else {
            t->tombstone_cnt--;
        }
        t->hash_pd[read_pos] = HASH_EMPTY;
        t->vals[read_pos] = VAL_NONE;
    }
    t->tombstone_cnt--;
}

static void bare_delete_compact(ht_bare_t *t, size_t idx) {
    size_t cap_mask = t->capacity - 1;

    size_t chain_len = 0;
    size_t live_count = 0;
    bool ends_at_empty = false;

    size_t scan = (idx + 1) & cap_mask;
    for (size_t steps = 0; steps < BSHIFT_CAP; steps++) {
        uint64_t hpd = t->hash_pd[scan];

        if (hpd_empty(hpd)) { ends_at_empty = true; break; }
        if (hpd_tomb(hpd)) {
            chain_len++;
            scan = (scan + 1) & cap_mask;
            continue;
        }
        if (hpd_pd(hpd) == 0) break;
        live_count++;
        chain_len++;
        scan = (scan + 1) & cap_mask;
    }

    if (ends_at_empty && live_count > 0 &&
        bare_verify_ideal_safe(t, idx, chain_len)) {
        bare_commit_backward_shift(t, idx, chain_len);
    }
}

// ============================================================================
// Bare Internal: Zombie Step
// ============================================================================

static void bare_zombie_step(ht_bare_t *t) {
    if (t->zombie_window == 0) return;

    double x = bare_compute_x(t);
    size_t step = (size_t)(x * C_B_DEFAULT);
    if (step < t->zombie_window) step = t->zombie_window;
    if (step > t->capacity) step = t->capacity;

    size_t cap_mask = t->capacity - 1;

    for (size_t n = 0; n < step; n++) {
        size_t idx = t->zombie_cursor;
        uint64_t hpd = t->hash_pd[idx];

        double prim_spacing = x * C_P_DEFAULT;
        size_t spacing = (size_t)prim_spacing;
        if (spacing < 2) spacing = 2;
        bool is_prim = (idx % spacing == 0);

        if (hpd_tomb(hpd)) {
            // Non-primitive tombstone: skip
        } else if (is_prim && hpd_empty(hpd) && t->size > 0) {
            // Don't place primitive tombstones during zombie scan
        }

        t->zombie_cursor = (idx + 1) & cap_mask;
    }
}

// ============================================================================
// Bare Internal: Prophylactic Tombstones & Reinsert
// ============================================================================

static void bare_place_prophylactic_tombstones(ht_bare_t *t) {
    double x = bare_compute_x(t);
    size_t spacing = (size_t)(x * C_P_DEFAULT);
    if (spacing < 4) spacing = 4;

    for (size_t pos = 0; pos < t->capacity; pos += spacing) {
        if (hpd_empty(t->hash_pd[pos])) {
            t->hash_pd[pos] = HASH_TOMB;
            t->vals[pos] = VAL_NONE;
            t->tombstone_cnt++;
        }
    }
}

static void bare_reinsert_main(ht_bare_t *t,
                               const uint64_t *old_hash_pd,
                               const uint32_t *old_vals,
                               size_t old_cap) {
    for (size_t i = 0; i < old_cap; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        bare_rh_insert(t, hpd_hash(hpd), old_vals[i]);
    }
}

static void bare_reinsert_spill(ht_bare_t *t,
                                const uint64_t *old_spill_hash_pd,
                                const uint32_t *old_spill_vals,
                                size_t old_spill_len) {
    for (size_t i = 0; i < old_spill_len; i++) {
        uint32_t val = old_spill_vals[i];
        if (val == VAL_NONE) continue;
        bare_spill_insert(t, hpd_hash(old_spill_hash_pd[i]), val);
    }
}

// ============================================================================
// Bare Public API: Lifecycle
// ============================================================================

ht_bare_t *ht_bare_create(const ht_config_t *cfg) {
    ht_bare_t *t = calloc(1, sizeof(ht_bare_t));
    if (!t) return NULL;

    ht_config_t c = default_cfg;
    if (cfg) c = *cfg;
    if (c.initial_capacity < 4) c.initial_capacity = 4;

    t->capacity = next_pow2(c.initial_capacity);

    t->hash_pd = calloc(t->capacity, sizeof(uint64_t));
    if (!t->hash_pd) { free(t); return NULL; }

    t->vals = malloc(t->capacity * sizeof(uint32_t));
    if (!t->vals) { free(t->hash_pd); free(t); return NULL; }
    memset(t->vals, 0xFF, t->capacity * sizeof(uint32_t));

    t->spill_cap = SPILL_INITIAL;
    t->spill_hash_pd = calloc(t->spill_cap, sizeof(uint64_t));
    if (!t->spill_hash_pd) { free(t->vals); free(t->hash_pd); free(t); return NULL; }

    t->spill_vals = malloc(t->spill_cap * sizeof(uint32_t));
    if (!t->spill_vals) {
        free(t->spill_hash_pd); free(t->vals); free(t->hash_pd); free(t);
        return NULL;
    }
    memset(t->spill_vals, 0xFF, t->spill_cap * sizeof(uint32_t));

    t->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
                         (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
    t->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    t->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    t->zombie_window = c.zombie_window;

    return t;
}

void ht_bare_destroy(ht_bare_t *t) {
    if (!t) return;
    free(t->spill_hash_pd);
    free(t->spill_vals);
    free(t->hash_pd);
    free(t->vals);
    free(t);
}

void ht_bare_clear(ht_bare_t *t) {
    if (!t) return;
    memset(t->hash_pd, 0, t->capacity * sizeof(uint64_t));
    memset(t->spill_hash_pd, 0, t->spill_cap * sizeof(uint64_t));
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_len = 0;
    t->zombie_cursor = 0;
}

// ============================================================================
// Bare Public API: Insert
// ============================================================================

bool ht_bare_insert(ht_bare_t *t, uint64_t hash, uint32_t val) {
    if (!t || val == VAL_NONE) return false;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return bare_spill_insert(t, h48, val);

    if (!t->resizing && (double)(t->size + 1) / t->capacity > t->max_load_factor)
        ht_bare_resize(t, t->capacity * 2);

    double total = t->size + t->tombstone_cnt;
    if (total > 0 && (double)t->tombstone_cnt / total > t->tomb_threshold) {
        for (int i = 0; i < 4; i++) bare_zombie_step(t);
    }

    bool result = bare_rh_insert(t, h48, val);

    if (result) bare_zombie_step(t);

    return result;
}

// ============================================================================
// Bare Public API: Find
// ============================================================================

bool ht_bare_find(const ht_bare_t *t, uint64_t hash, uint32_t *out_val) {
    if (!t) return false;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return bare_spill_find(t, h48, out_val);

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) return false;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) return false;

        if (hpd_hash(slot_hpd) == h48) {
            if (out_val) *out_val = t->vals[idx];
            return true;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return false;
}

void ht_bare_find_all(const ht_bare_t *t, uint64_t hash,
                      ht_bare_callback cb, void *user_ctx) {
    if (!t || !cb) return;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2) {
        bare_spill_find_all(t, h48, cb, user_ctx);
        return;
    }

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) return;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) return;

        if (hpd_hash(slot_hpd) == h48) {
            if (!cb(t->vals[idx], user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }
}

// ============================================================================
// Bare Public API: Remove
// ============================================================================

size_t ht_bare_remove(ht_bare_t *t, uint64_t hash) {
    if (!t) return 0;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return bare_spill_remove(t, h48);

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;
    size_t removed = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;

        if (hpd_hash(slot_hpd) == h48) {
            t->size--;
            t->hash_pd[idx] = HASH_TOMB;
            t->vals[idx] = VAL_NONE;
            t->tombstone_cnt++;
            removed++;
            bare_delete_compact(t, idx);
            continue;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return removed;
}

bool ht_bare_remove_val(ht_bare_t *t, uint64_t hash, uint32_t val) {
    if (!t) return false;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return bare_spill_remove_val(t, h48, val);

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) return false;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) return false;

        if (hpd_hash(slot_hpd) == h48 && t->vals[idx] == val) {
            t->size--;
            t->hash_pd[idx] = HASH_TOMB;
            t->vals[idx] = VAL_NONE;
            t->tombstone_cnt++;
            bare_delete_compact(t, idx);
            return true;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return false;
}

// ============================================================================
// Bare Public API: Resize / Compact
// ============================================================================

static bool bare_resize_table(ht_bare_t *t) {
    return ht_bare_resize(t, t->capacity * 2);
}

bool ht_bare_resize(ht_bare_t *t, size_t new_capacity) {
    if (!t) return false;
    if (new_capacity < t->size) return false;
    if (t->resizing) return true;

    t->resizing = true;
    new_capacity = next_pow2(new_capacity);

    if (new_capacity == t->capacity) {
        t->resizing = false;
        return true;
    }

    // Save old state
    uint64_t *old_hash_pd = t->hash_pd;
    uint32_t *old_vals = t->vals;
    size_t old_cap = t->capacity;

    uint64_t *old_spill_hash_pd = t->spill_hash_pd;
    uint32_t *old_spill_vals = t->spill_vals;
    size_t old_spill_len = t->spill_len;

    // Allocate new main table
    uint64_t *new_hash_pd = calloc(new_capacity, sizeof(uint64_t));
    if (!new_hash_pd) { t->resizing = false; return false; }

    uint32_t *new_vals = malloc(new_capacity * sizeof(uint32_t));
    if (!new_vals) {
        free(new_hash_pd);
        t->resizing = false;
        return false;
    }
    memset(new_vals, 0xFF, new_capacity * sizeof(uint32_t));

    // Allocate new spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_vals); free(new_hash_pd);
        t->resizing = false;
        return false;
    }

    uint32_t *new_spill_vals = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_vals) {
        free(new_spill_hash_pd); free(new_vals); free(new_hash_pd);
        t->resizing = false;
        return false;
    }
    memset(new_spill_vals, 0xFF, new_spill_cap * sizeof(uint32_t));

    // Swap to new state
    t->hash_pd = new_hash_pd;
    t->vals = new_vals;
    t->capacity = new_capacity;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_hash_pd = new_spill_hash_pd;
    t->spill_vals = new_spill_vals;
    t->spill_cap = new_spill_cap;
    t->spill_len = 0;
    t->zombie_cursor = 0;

    bare_reinsert_main(t, old_hash_pd, old_vals, old_cap);
    bare_reinsert_spill(t, old_spill_hash_pd, old_spill_vals, old_spill_len);
    bare_place_prophylactic_tombstones(t);

    free(old_hash_pd);
    free(old_vals);
    free(old_spill_hash_pd);
    free(old_spill_vals);
    t->resizing = false;
    return true;
}

void ht_bare_compact(ht_bare_t *t) {
    if (!t) return;

    uint64_t *old_hash_pd = t->hash_pd;
    uint32_t *old_vals = t->vals;
    size_t old_cap = t->capacity;

    uint64_t *old_spill_hash_pd = t->spill_hash_pd;
    uint32_t *old_spill_vals = t->spill_vals;
    size_t old_spill_len = t->spill_len;

    uint64_t *new_hash_pd = calloc(old_cap, sizeof(uint64_t));
    if (!new_hash_pd) return;

    uint32_t *new_vals = malloc(old_cap * sizeof(uint32_t));
    if (!new_vals) {
        free(new_hash_pd);
        return;
    }
    memset(new_vals, 0xFF, old_cap * sizeof(uint32_t));

    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_vals); free(new_hash_pd);
        return;
    }

    uint32_t *new_spill_vals = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_vals) {
        free(new_spill_hash_pd); free(new_vals); free(new_hash_pd);
        return;
    }
    memset(new_spill_vals, 0xFF, new_spill_cap * sizeof(uint32_t));

    t->hash_pd = new_hash_pd;
    t->vals = new_vals;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_hash_pd = new_spill_hash_pd;
    t->spill_vals = new_spill_vals;
    t->spill_cap = new_spill_cap;
    t->spill_len = 0;
    t->zombie_cursor = 0;

    bare_reinsert_main(t, old_hash_pd, old_vals, old_cap);
    bare_reinsert_spill(t, old_spill_hash_pd, old_spill_vals, old_spill_len);
    bare_place_prophylactic_tombstones(t);

    free(old_hash_pd);
    free(old_vals);
    free(old_spill_hash_pd);
    free(old_spill_vals);
}

// ============================================================================
// Bare Public API: Iterator
// ============================================================================

ht_iter_t ht_bare_iter_begin(const ht_bare_t *t) {
    ht_iter_t iter = {0, false};
    (void)t;
    return iter;
}

bool ht_bare_iter_next(ht_bare_t *t, ht_iter_t *iter,
                       uint64_t *out_hash, uint32_t *out_val) {
    if (!t || !iter) return false;

    while (iter->idx < t->capacity) {
        uint64_t hpd = t->hash_pd[iter->idx];
        uint32_t val = t->vals[iter->idx];
        iter->idx++;
        if (hpd_live(hpd) && val != VAL_NONE) {
            if (out_hash) *out_hash = hpd_hash(hpd);
            if (out_val) *out_val = val;
            return true;
        }
    }

    size_t spill_idx = iter->idx - t->capacity;
    while (spill_idx < t->spill_len) {
        uint32_t sval = t->spill_vals[spill_idx];
        uint64_t shpd = t->spill_hash_pd[spill_idx];
        spill_idx++;
        iter->idx = t->capacity + spill_idx;
        if (sval != VAL_NONE) {
            if (out_hash) *out_hash = hpd_hash(shpd);
            if (out_val) *out_val = sval;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Bare Public API: Statistics
// ============================================================================

void ht_bare_stats(const ht_bare_t *t, ht_stats_t *out_stats) {
    if (!t || !out_stats) return;
    out_stats->size = t->size;
    out_stats->capacity = t->capacity;
    out_stats->tombstone_cnt = t->tombstone_cnt;
    out_stats->load_factor = (double)t->size / t->capacity;
    out_stats->tombstone_ratio = (t->size + t->tombstone_cnt > 0)
        ? (double)t->tombstone_cnt / (t->size + t->tombstone_cnt)
        : 0.0;
}

const char *ht_bare_check_invariants(const ht_bare_t *t) {
    if (!t) return "table is NULL";
    size_t cap_mask = t->capacity - 1;

    size_t live_count = 0;
    size_t tomb_count = 0;
    size_t spill_live = 0;

    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        if (hpd_empty(hpd)) continue;
        if (hpd_tomb(hpd)) {
            tomb_count++;
            continue;
        }
        live_count++;

        uint64_t h48 = hpd_hash(hpd);
        uint16_t pd = hpd_pd(hpd);
        size_t ideal = h48 & cap_mask;
        size_t expected_dist = (i >= ideal) ? (i - ideal) : (t->capacity - ideal + i);
        if (pd != expected_dist) {
            static char buf[256];
            snprintf(buf, sizeof(buf),
                     "slot[%zu]: probe_dist=%u but expected %zu (hash=0x%" PRIx64 " ideal=%zu)",
                     i, pd, expected_dist, h48, ideal);
            return buf;
        }
    }

    for (size_t i = 0; i < t->spill_len; i++) {
        if (t->spill_vals[i] != VAL_NONE)
            spill_live++;
    }

    if (t->size != live_count + spill_live) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "size=%zu but found %zu live (%zu main + %zu spill)",
                 t->size, live_count + spill_live, live_count, spill_live);
        return buf;
    }

    if (t->tombstone_cnt != tomb_count) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "tombstone_cnt=%zu but found %zu tombs",
                 t->tombstone_cnt, tomb_count);
        return buf;
    }

    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        if (!hpd_live(hpd)) continue;

        uint64_t h48 = hpd_hash(hpd);
        uint16_t pd = hpd_pd(hpd);
        size_t ideal = h48 & cap_mask;
        uint16_t dist = 0;
        for (size_t steps = 0; steps <= t->capacity; steps++) {
            size_t pos = (ideal + dist) & cap_mask;
            if (pos == i) break;

            uint64_t scan_hpd = t->hash_pd[pos];
            if (hpd_empty(scan_hpd)) {
                static char buf[256];
                snprintf(buf, sizeof(buf),
                         "slot[%zu] (hash=0x%" PRIx64 " ideal=%zu dist=%u) unreachable: "
                         "hit EMPTY at [%zu] while probing from ideal",
                         i, h48, ideal, pd, pos);
                return buf;
            }
            if (hpd_tomb(scan_hpd)) {
                dist++;
                continue;
            }
            if (hpd_pd(scan_hpd) < dist) {
                static char buf[256];
                snprintf(buf, sizeof(buf),
                         "slot[%zu] (hash=0x%" PRIx64 " ideal=%zu dist=%u) unreachable: "
                         "early termination at [%zu] (dist=%u < %u)",
                         i, h48, ideal, pd,
                         pos, hpd_pd(scan_hpd), dist);
                return buf;
            }
            dist++;
        }
    }

    return NULL;
}

void ht_bare_dump(const ht_bare_t *t, uint64_t hash, size_t count) {
    if (!t) return;
    uint64_t h48 = hash & HASH_MASK;
    size_t start_idx = h48 & (t->capacity - 1);
    printf("Dump for h48=0x%" PRIx64 ", ideal_idx=%zu:\n", h48, start_idx);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start_idx + i) & (t->capacity - 1);
        uint64_t hpd = t->hash_pd[idx];
        const char *tag = hpd_empty(hpd) ? "EMPTY" : hpd_tomb(hpd) ? "TOMB" : "LIVE";
        if (hpd_live(hpd)) {
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s] val=%" PRIu32 "\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag, t->vals[idx]);
        } else {
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s]\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag);
        }
    }
    if (t->spill_len > 0) {
        printf("  Spill lane (%zu entries):\n", t->spill_len);
        for (size_t i = 0; i < t->spill_len; i++) {
            uint64_t shpd = t->spill_hash_pd[i];
            printf("  spill[%zu]: hash=0x%08" PRIx64 " val=%" PRIu32 "\n",
                   i, hpd_hash(shpd), t->spill_vals[i]);
        }
    }
}

// ============================================================================
// High-Level Internal: Arena Management
// ============================================================================

static bool grow_arena(ht_table_t *t, size_t needed) {
    if (t->arena_size + needed <= t->arena_cap) return true;
    size_t new_cap = t->arena_cap ? t->arena_cap * 2 : 1024;
    while (new_cap < t->arena_size + needed) new_cap *= 2;
    if (new_cap > UINT32_MAX) return false;
    uint8_t *p = realloc(t->arena, new_cap);
    if (!p) return false;
    t->arena = p;
    t->arena_cap = new_cap;
    return true;
}

static void *arena_alloc(ht_table_t *t, size_t n) {
    if (!grow_arena(t, n)) return NULL;
    void *p = t->arena + t->arena_size;
    t->arena_size += n;
    return p;
}

// ============================================================================
// High-Level Internal: Entry Management
// ============================================================================

static uint32_t alloc_entry(ht_table_t *t, uint16_t hash_hi,
                            const void *key, size_t key_len,
                            const void *value, size_t value_len) {
    if (key_len > UINT16_MAX) return VAL_NONE;

    if (t->entry_count >= t->entry_cap) {
        size_t new_cap = t->entry_cap ? t->entry_cap * 2 : 64;
        ht_entry_t *ne = realloc(t->entries, new_cap * sizeof(ht_entry_t));
        if (!ne) return VAL_NONE;
        t->entries = ne;
        t->entry_cap = new_cap;
    }

    void *data = arena_alloc(t, key_len + value_len);
    if (!data) return VAL_NONE;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + key_len, value, value_len);

    uint32_t eidx = (uint32_t)t->entry_count++;
    t->entries[eidx].key_len = (uint16_t)key_len;
    t->entries[eidx].hash_hi = hash_hi;
    t->entries[eidx].val_len = (uint32_t)value_len;
    t->entries[eidx].arena_offset = (uint32_t)((uint8_t *)data - t->arena);
    return eidx;
}

static bool update_entry_value(ht_table_t *t, uint32_t eidx,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len) {
    ht_entry_t *e = &t->entries[eidx];
    if (value_len == e->val_len) {
        memcpy(t->arena + e->arena_offset + e->key_len, value, value_len);
        return true;
    }
    void *data = arena_alloc(t, key_len + value_len);
    if (!data) return false;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + key_len, value, value_len);
    e->val_len = (uint32_t)value_len;
    e->arena_offset = (uint32_t)((uint8_t *)data - t->arena);
    return true;
}

// ============================================================================
// High-Level Internal: Key / Value Matching
// ============================================================================

static inline bool keys_match(const ht_table_t *t, uint32_t eidx,
                              uint16_t hash_hi,
                              const void *key, size_t key_len) {
    const ht_entry_t *e = &t->entries[eidx];
    if (e->hash_hi != hash_hi) return false;
    if (e->key_len != key_len) return false;
    const void *entry_key = t->arena + e->arena_offset;
    if (t->eq_fn)
        return t->eq_fn(entry_key, e->key_len, key, key_len, t->user_ctx);
    return memcmp(entry_key, key, key_len) == 0;
}

static inline bool vals_match(const ht_table_t *t, uint32_t eidx,
                              const void *val, size_t val_len) {
    const ht_entry_t *e = &t->entries[eidx];
    if (e->val_len != val_len) return false;
    const void *entry_val = t->arena + e->arena_offset + e->key_len;
    return memcmp(entry_val, val, val_len) == 0;
}

// ============================================================================
// High-Level Internal: Scan Callbacks
// ============================================================================

struct hl_key_scan_ctx {
    ht_table_t *t;
    uint16_t hash_hi;
    const void *key;
    size_t key_len;
    uint32_t *matches;
    size_t match_count;
    size_t match_cap;
};

static bool hl_key_scan_cb(uint32_t val, void *user_ctx) {
    struct hl_key_scan_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len)) {
        if (ctx->match_count < ctx->match_cap)
            ctx->matches[ctx->match_count++] = val;
    }
    return true;
}

struct hl_find_one_ctx {
    const ht_table_t *t;
    uint16_t hash_hi;
    const void *key;
    size_t key_len;
    uint32_t eidx;
    bool found;
};

static bool hl_find_one_cb(uint32_t val, void *user_ctx) {
    struct hl_find_one_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len)) {
        ctx->eidx = val;
        ctx->found = true;
        return false;
    }
    return true;
}

struct hl_find_all_ctx {
    const ht_table_t *t;
    ht_dup_callback user_cb;
    void *user_ctx;
};

static bool hl_find_all_cb(uint32_t val, void *user_ctx) {
    struct hl_find_all_ctx *ctx = user_ctx;
    const ht_entry_t *e = &ctx->t->entries[val];
    return ctx->user_cb(ctx->t->arena + e->arena_offset, e->key_len,
                        ctx->t->arena + e->arena_offset + e->key_len, e->val_len,
                        ctx->user_ctx);
}

struct hl_key_find_ctx {
    const ht_table_t *t;
    uint16_t hash_hi;
    const void *key;
    size_t key_len;
    ht_dup_callback user_cb;
    void *user_ctx;
};

static bool hl_key_find_cb(uint32_t val, void *user_ctx) {
    struct hl_key_find_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len)) {
        const ht_entry_t *e = &ctx->t->entries[val];
        return ctx->user_cb(ctx->t->arena + e->arena_offset, e->key_len,
                            ctx->t->arena + e->arena_offset + e->key_len, e->val_len,
                            ctx->user_ctx);
    }
    return true;
}

struct hl_kv_find_ctx {
    const ht_table_t *t;
    uint16_t hash_hi;
    const void *key;
    size_t key_len;
    const void *value;
    size_t value_len;
    uint32_t eidx;
    bool found;
};

static bool hl_kv_find_cb(uint32_t val, void *user_ctx) {
    struct hl_kv_find_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len) &&
        vals_match(ctx->t, val, ctx->value, ctx->value_len)) {
        ctx->eidx = val;
        ctx->found = true;
        return false;
    }
    return true;
}

struct hl_kv_scan_ctx {
    const ht_table_t *t;
    uint16_t hash_hi;
    const void *key;
    size_t key_len;
    const void *value;
    size_t value_len;
    bool kv_found;
};

static bool hl_kv_scan_cb(uint32_t val, void *user_ctx) {
    struct hl_kv_scan_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len) &&
        vals_match(ctx->t, val, ctx->value, ctx->value_len)) {
        ctx->kv_found = true;
        return false;
    }
    return true;
}

// ============================================================================
// High-Level Public API: Lifecycle
// ============================================================================

ht_table_t *ht_create(const ht_config_t *cfg,
                       ht_hash_fn hash_fn, ht_eq_fn eq_fn,
                       void *user_ctx) {
    if (!hash_fn) return NULL;

    ht_table_t *t = calloc(1, sizeof(ht_table_t));
    if (!t) return NULL;

    ht_config_t c = default_cfg;
    if (cfg) c = *cfg;
    if (c.initial_capacity < 4) c.initial_capacity = 4;

    ht_bare_t *b = &t->bare;
    b->capacity = next_pow2(c.initial_capacity);

    b->hash_pd = calloc(b->capacity, sizeof(uint64_t));
    if (!b->hash_pd) { free(t); return NULL; }

    b->vals = malloc(b->capacity * sizeof(uint32_t));
    if (!b->vals) { free(b->hash_pd); free(t); return NULL; }
    memset(b->vals, 0xFF, b->capacity * sizeof(uint32_t));

    b->spill_cap = SPILL_INITIAL;
    b->spill_hash_pd = calloc(b->spill_cap, sizeof(uint64_t));
    if (!b->spill_hash_pd) { free(b->vals); free(b->hash_pd); free(t); return NULL; }

    b->spill_vals = malloc(b->spill_cap * sizeof(uint32_t));
    if (!b->spill_vals) {
        free(b->spill_hash_pd); free(b->vals); free(b->hash_pd); free(t);
        return NULL;
    }
    memset(b->spill_vals, 0xFF, b->spill_cap * sizeof(uint32_t));

    b->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
                         (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
    b->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    b->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    b->zombie_window = c.zombie_window;

    t->entries = calloc(64, sizeof(ht_entry_t));
    t->entry_cap = 64;
    if (!t->entries) {
        free(b->spill_vals); free(b->spill_hash_pd);
        free(b->vals); free(b->hash_pd); free(t);
        return NULL;
    }

    t->arena = malloc(1024);
    t->arena_cap = 1024;
    if (!t->arena) {
        free(t->entries); free(b->spill_vals); free(b->spill_hash_pd);
        free(b->vals); free(b->hash_pd); free(t);
        return NULL;
    }

    t->hash_fn = hash_fn;
    t->eq_fn = eq_fn;
    t->user_ctx = user_ctx;

    return t;
}

void ht_destroy(ht_table_t *t) {
    if (!t) return;
    ht_bare_t *b = &t->bare;
    free(b->spill_hash_pd);
    free(b->spill_vals);
    free(b->hash_pd);
    free(b->vals);
    free(t->entries);
    free(t->arena);
    free(t);
}

void ht_clear(ht_table_t *t) {
    if (!t) return;
    ht_bare_clear(&t->bare);
    t->arena_size = 0;
    t->entry_count = 0;
}

// ============================================================================
// High-Level Public API: Insert / Upsert / Unsert
// ============================================================================

static bool do_insert_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len,
                                int mode) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;

    uint64_t h48 = hash & HASH_MASK;
    uint16_t hash_hi = (uint16_t)(hash >> 48);

    // Phase 1: Scan for existing entries (UPSERT/UNIQUE only)
    if (mode == INS_UPSERT) {
        uint32_t matches[64];
        struct hl_key_scan_ctx ctx = {
            .t = t, .hash_hi = hash_hi, .key = key, .key_len = key_len,
            .matches = matches, .match_count = 0, .match_cap = 64,
        };
        ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

        if (ctx.match_count > 0) {
            if (!update_entry_value(t, ctx.matches[0], key, key_len, value, value_len))
                return false;
            for (size_t i = 1; i < ctx.match_count; i++)
                ht_bare_remove_val(&t->bare, hash, ctx.matches[i]);
            return false;
        }
    } else if (mode == INS_UNIQUE) {
        struct hl_kv_scan_ctx ctx = {
            .t = t, .hash_hi = hash_hi, .key = key, .key_len = key_len,
            .value = value, .value_len = value_len,
        };
        ht_bare_find_all(&t->bare, hash, hl_kv_scan_cb, &ctx);
        if (ctx.kv_found) return false;
    }

    // Phase 2: Insert new entry
    ht_bare_t *b = &t->bare;
    if (!b->resizing && (double)(b->size + 1) / b->capacity > b->max_load_factor)
        ht_resize(t, b->capacity * 2);

    double total = b->size + b->tombstone_cnt;
    if (total > 0 && (double)b->tombstone_cnt / total > b->tomb_threshold) {
        for (int i = 0; i < 4; i++) bare_zombie_step(b);
    }

    uint32_t eidx = alloc_entry(t, hash_hi, key, key_len, value, value_len);
    if (eidx == VAL_NONE) return false;

    bool result;
    if (h48 < 2)
        result = bare_spill_insert(b, h48, eidx);
    else
        result = bare_rh_insert(b, h48, eidx);

    if (result) bare_zombie_step(b);

    return result;
}

bool ht_insert_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_ALWAYS);
}

bool ht_insert(ht_table_t *t, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;
    return ht_insert_with_hash(t, t->hash_fn(key, key_len, t->user_ctx),
                               key, key_len, value, value_len);
}

bool ht_upsert_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_UPSERT);
}

bool ht_upsert(ht_table_t *t, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;
    return ht_upsert_with_hash(t, t->hash_fn(key, key_len, t->user_ctx),
                               key, key_len, value, value_len);
}

bool ht_unsert_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_UNIQUE);
}

bool ht_unsert(ht_table_t *t, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;
    return ht_unsert_with_hash(t, t->hash_fn(key, key_len, t->user_ctx),
                               key, key_len, value, value_len);
}

// ============================================================================
// High-Level Public API: Lookup
// ============================================================================

const void *ht_find(const ht_table_t *t, const void *key, size_t key_len,
                    size_t *out_value_len) {
    if (!t || !key) return NULL;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_find_with_hash(t, hash, key, key_len, out_value_len);
}

const void *ht_find_with_hash(const ht_table_t *t, uint64_t hash,
                              const void *key, size_t key_len,
                              size_t *out_value_len) {
    if (!t || !key) return NULL;

    struct hl_find_one_ctx ctx = {
        .t = t,
        .hash_hi = (uint16_t)(hash >> 48),
        .key = key,
        .key_len = key_len,
    };
    ht_bare_find_all(&t->bare, hash, hl_find_one_cb, &ctx);

    if (!ctx.found) return NULL;
    const ht_entry_t *e = &t->entries[ctx.eidx];
    if (out_value_len) *out_value_len = e->val_len;
    return t->arena + e->arena_offset + e->key_len;
}

void ht_find_all(const ht_table_t *t, uint64_t hash,
                 ht_dup_callback cb, void *user_ctx) {
    if (!t || !cb) return;
    struct hl_find_all_ctx ctx = {
        .t = t, .user_cb = cb, .user_ctx = user_ctx,
    };
    ht_bare_find_all(&t->bare, hash, hl_find_all_cb, &ctx);
}

void ht_find_key_all_with_hash(const ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               ht_dup_callback cb, void *user_ctx) {
    if (!t || !key || !cb) return;
    struct hl_key_find_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .user_cb = cb, .user_ctx = user_ctx,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_find_cb, &ctx);
}

void ht_find_key_all(const ht_table_t *t, const void *key, size_t key_len,
                     ht_dup_callback cb, void *user_ctx) {
    if (!t || !key || !cb) return;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    ht_find_key_all_with_hash(t, hash, key, key_len, cb, user_ctx);
}

const void *ht_find_kv_with_hash(const ht_table_t *t, uint64_t hash,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 size_t *out_value_len) {
    if (!t || !key || !value) return NULL;

    struct hl_kv_find_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .value = value, .value_len = value_len,
    };
    ht_bare_find_all(&t->bare, hash, hl_kv_find_cb, &ctx);

    if (!ctx.found) return NULL;
    const ht_entry_t *e = &t->entries[ctx.eidx];
    if (out_value_len) *out_value_len = e->val_len;
    return t->arena + e->arena_offset + e->key_len;
}

const void *ht_find_kv(const ht_table_t *t, const void *key, size_t key_len,
                       const void *value, size_t value_len,
                       size_t *out_value_len) {
    if (!t || !key || !value) return NULL;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_find_kv_with_hash(t, hash, key, key_len, value, value_len, out_value_len);
}

// ============================================================================
// High-Level Public API: Increment
// ============================================================================

int64_t ht_inc(ht_table_t *t, const void *key, size_t key_len, int64_t delta) {
    if (!t || !key) return 0;

    size_t val_len;
    const void *found = ht_find(t, key, key_len, &val_len);

    int64_t new_val;
    if (found && val_len == sizeof(int64_t)) {
        new_val = *(const int64_t *)found + delta;
    } else {
        new_val = delta;
    }
    ht_upsert(t, key, key_len, &new_val, sizeof(new_val));
    return new_val;
}

int64_t ht_inc_with_hash(ht_table_t *t, uint64_t hash,
                          const void *key, size_t key_len, int64_t delta,
                          bool *ok) {
    if (!t || !key) { if (ok) *ok = false; return 0; }

    size_t val_len;
    const void *found = ht_find_with_hash(t, hash, key, key_len, &val_len);

    int64_t new_val;
    if (found && val_len == sizeof(int64_t)) {
        new_val = *(const int64_t *)found + delta;
    } else {
        new_val = delta;
    }
    bool inserted = ht_upsert_with_hash(t, hash, key, key_len, &new_val, sizeof(new_val));
    if (!found && !inserted) {
        if (ok) *ok = false;
        return 0;
    }
    if (ok) *ok = true;
    return new_val;
}

// ============================================================================
// High-Level Public API: Delete
// ============================================================================

size_t ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                            const void *key, size_t key_len) {
    if (!t || !key) return 0;

    uint32_t matches[64];
    struct hl_key_scan_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

    size_t removed = ctx.match_count;
    for (size_t i = 0; i < ctx.match_count; i++)
        ht_bare_remove_val(&t->bare, hash, ctx.matches[i]);

    if (removed > 0 && t->bare.min_load_factor > 0 && t->bare.size > 0 &&
        (double)t->bare.size / t->bare.capacity < t->bare.min_load_factor &&
        t->bare.capacity > 64) {
        size_t new_cap = t->bare.capacity / 2;
        if (new_cap >= 64 && new_cap >= t->bare.size * 2)
            ht_resize(t, new_cap);
    }

    return removed;
}

size_t ht_remove(ht_table_t *t, const void *key, size_t key_len) {
    if (!t || !key) return 0;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_remove_with_hash(t, hash, key, key_len);
}

size_t ht_remove_kv_with_hash(ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len) {
    if (!t || !key || !value) return 0;

    // Collect entries matching key
    uint32_t key_matches[64];
    struct hl_key_scan_ctx kctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = key_matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &kctx);

    // Filter by value match and remove
    size_t removed = 0;
    for (size_t i = 0; i < kctx.match_count; i++) {
        if (vals_match(t, key_matches[i], value, value_len)) {
            ht_bare_remove_val(&t->bare, hash, key_matches[i]);
            removed++;
        }
    }

    return removed;
}

size_t ht_remove_kv(ht_table_t *t, const void *key, size_t key_len,
                    const void *value, size_t value_len) {
    if (!t || !key || !value) return 0;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_remove_kv_with_hash(t, hash, key, key_len, value, value_len);
}

bool ht_remove_kv_one_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len) {
    if (!t || !key || !value) return false;

    uint32_t matches[64];
    struct hl_key_scan_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

    for (size_t i = 0; i < ctx.match_count; i++) {
        if (vals_match(t, matches[i], value, value_len)) {
            ht_bare_remove_val(&t->bare, hash, matches[i]);

            if (t->bare.min_load_factor > 0 && t->bare.size > 0 &&
                (double)t->bare.size / t->bare.capacity < t->bare.min_load_factor &&
                t->bare.capacity > 64) {
                size_t new_cap = t->bare.capacity / 2;
                if (new_cap >= 64 && new_cap >= t->bare.size * 2)
                    ht_resize(t, new_cap);
            }

            return true;
        }
    }

    return false;
}

bool ht_remove_kv_one(ht_table_t *t, const void *key, size_t key_len,
                      const void *value, size_t value_len) {
    if (!t || !key || !value) return false;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_remove_kv_one_with_hash(t, hash, key, key_len, value, value_len);
}

// ============================================================================
// High-Level Public API: Resize / Compact
// ============================================================================

bool ht_resize(ht_table_t *t, size_t new_capacity) {
    if (!t) return false;
    if (new_capacity < t->bare.size) return false;
    if (t->bare.resizing) return true;

    t->bare.resizing = true;
    new_capacity = next_pow2(new_capacity);

    if (new_capacity == t->bare.capacity) {
        t->bare.resizing = false;
        return true;
    }

    ht_bare_t *b = &t->bare;

    // Save old state
    uint64_t *old_hash_pd = b->hash_pd;
    uint32_t *old_vals = b->vals;
    ht_entry_t *old_entries = t->entries;
    uint8_t *old_arena = t->arena;
    size_t old_cap = b->capacity;
    size_t old_arena_cap = t->arena_cap;

    uint64_t *old_spill_hash_pd = b->spill_hash_pd;
    uint32_t *old_spill_vals = b->spill_vals;
    size_t old_spill_len = b->spill_len;

    // Allocate new main table
    uint64_t *new_hash_pd = calloc(new_capacity, sizeof(uint64_t));
    if (!new_hash_pd) { b->resizing = false; return false; }

    uint32_t *new_vals = malloc(new_capacity * sizeof(uint32_t));
    if (!new_vals) {
        free(new_hash_pd);
        b->resizing = false;
        return false;
    }
    memset(new_vals, 0xFF, new_capacity * sizeof(uint32_t));

    // Allocate new entries
    size_t new_entry_cap = t->entry_cap;
    ht_entry_t *new_entries = calloc(new_entry_cap, sizeof(ht_entry_t));
    if (!new_entries) {
        free(new_vals); free(new_hash_pd);
        b->resizing = false;
        return false;
    }

    // Allocate new arena
    uint8_t *new_arena = malloc(old_arena_cap > 0 ? old_arena_cap : 1024);
    if (!new_arena) {
        free(new_entries); free(new_vals); free(new_hash_pd);
        b->resizing = false;
        return false;
    }

    // Allocate new spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_arena); free(new_entries); free(new_vals); free(new_hash_pd);
        b->resizing = false;
        return false;
    }

    uint32_t *new_spill_vals = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_vals) {
        free(new_spill_hash_pd); free(new_arena); free(new_entries);
        free(new_vals); free(new_hash_pd);
        b->resizing = false;
        return false;
    }
    memset(new_spill_vals, 0xFF, new_spill_cap * sizeof(uint32_t));

    // Swap to new state
    b->hash_pd = new_hash_pd;
    b->vals = new_vals;
    b->capacity = new_capacity;
    b->size = 0;
    b->tombstone_cnt = 0;
    b->spill_hash_pd = new_spill_hash_pd;
    b->spill_vals = new_spill_vals;
    b->spill_cap = new_spill_cap;
    b->spill_len = 0;
    b->zombie_cursor = 0;

    t->entries = new_entries;
    t->entry_count = 0;
    t->entry_cap = new_entry_cap;
    t->arena = new_arena;
    t->arena_size = 0;
    t->arena_cap = old_arena_cap > 0 ? old_arena_cap : 1024;

    // Reinsert main table
    for (size_t i = 0; i < old_cap; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        uint32_t old_eidx = old_vals[i];
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        uint32_t new_eidx = alloc_entry(t, e->hash_hi, k, e->key_len, v, e->val_len);
        if (new_eidx == VAL_NONE) continue;
        uint64_t h48 = hpd_hash(hpd);
        if (h48 < 2)
            bare_spill_insert(b, h48, new_eidx);
        else
            bare_rh_insert(b, h48, new_eidx);
    }

    // Reinsert spill
    for (size_t i = 0; i < old_spill_len; i++) {
        uint32_t old_eidx = old_spill_vals[i];
        if (old_eidx == VAL_NONE) continue;
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        uint32_t new_eidx = alloc_entry(t, e->hash_hi, k, e->key_len, v, e->val_len);
        if (new_eidx == VAL_NONE) continue;
        bare_spill_insert(b, hpd_hash(old_spill_hash_pd[i]), new_eidx);
    }

    bare_place_prophylactic_tombstones(b);

    free(old_hash_pd);
    free(old_vals);
    free(old_entries);
    free(old_arena);
    free(old_spill_hash_pd);
    free(old_spill_vals);
    b->resizing = false;
    return true;
}

void ht_compact(ht_table_t *t) {
    if (!t) return;

    ht_bare_t *b = &t->bare;

    // Save old state
    uint64_t *old_hash_pd = b->hash_pd;
    uint32_t *old_vals = b->vals;
    ht_entry_t *old_entries = t->entries;
    uint8_t *old_arena = t->arena;
    size_t old_cap = b->capacity;
    size_t old_arena_cap = t->arena_cap;

    uint64_t *old_spill_hash_pd = b->spill_hash_pd;
    uint32_t *old_spill_vals = b->spill_vals;
    size_t old_spill_len = b->spill_len;

    // Allocate new main table (same capacity)
    uint64_t *new_hash_pd = calloc(old_cap, sizeof(uint64_t));
    if (!new_hash_pd) return;

    uint32_t *new_vals = malloc(old_cap * sizeof(uint32_t));
    if (!new_vals) {
        free(new_hash_pd);
        return;
    }
    memset(new_vals, 0xFF, old_cap * sizeof(uint32_t));

    // Allocate new entries
    size_t new_entry_cap = t->entry_cap;
    ht_entry_t *new_entries = calloc(new_entry_cap, sizeof(ht_entry_t));
    if (!new_entries) {
        free(new_vals); free(new_hash_pd);
        return;
    }

    // Allocate new arena
    uint8_t *new_arena = malloc(old_arena_cap > 0 ? old_arena_cap : 1024);
    if (!new_arena) {
        free(new_entries); free(new_vals); free(new_hash_pd);
        return;
    }

    // Allocate new spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_arena); free(new_entries); free(new_vals); free(new_hash_pd);
        return;
    }

    uint32_t *new_spill_vals = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_vals) {
        free(new_spill_hash_pd); free(new_arena); free(new_entries);
        free(new_vals); free(new_hash_pd);
        return;
    }
    memset(new_spill_vals, 0xFF, new_spill_cap * sizeof(uint32_t));

    // Swap to new state
    b->hash_pd = new_hash_pd;
    b->vals = new_vals;
    b->size = 0;
    b->tombstone_cnt = 0;
    b->spill_hash_pd = new_spill_hash_pd;
    b->spill_vals = new_spill_vals;
    b->spill_cap = new_spill_cap;
    b->spill_len = 0;
    b->zombie_cursor = 0;

    t->entries = new_entries;
    t->entry_count = 0;
    t->arena = new_arena;
    t->arena_size = 0;
    t->arena_cap = old_arena_cap > 0 ? old_arena_cap : 1024;

    // Reinsert main table
    for (size_t i = 0; i < old_cap; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        uint32_t old_eidx = old_vals[i];
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        uint32_t new_eidx = alloc_entry(t, e->hash_hi, k, e->key_len, v, e->val_len);
        if (new_eidx == VAL_NONE) continue;
        uint64_t h48 = hpd_hash(hpd);
        if (h48 < 2)
            bare_spill_insert(b, h48, new_eidx);
        else
            bare_rh_insert(b, h48, new_eidx);
    }

    // Reinsert spill
    for (size_t i = 0; i < old_spill_len; i++) {
        uint32_t old_eidx = old_spill_vals[i];
        if (old_eidx == VAL_NONE) continue;
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        uint32_t new_eidx = alloc_entry(t, e->hash_hi, k, e->key_len, v, e->val_len);
        if (new_eidx == VAL_NONE) continue;
        bare_spill_insert(b, hpd_hash(old_spill_hash_pd[i]), new_eidx);
    }

    bare_place_prophylactic_tombstones(b);

    free(old_hash_pd);
    free(old_vals);
    free(old_entries);
    free(old_arena);
    free(old_spill_hash_pd);
    free(old_spill_vals);
}

// ============================================================================
// High-Level Public API: Iterator
// ============================================================================

ht_iter_t ht_iter_begin(const ht_table_t *t) {
    ht_iter_t iter = {0, false};
    (void)t;
    return iter;
}

bool ht_iter_next(ht_table_t *t, ht_iter_t *iter,
                  const void **out_key, size_t *out_key_len,
                  const void **out_value, size_t *out_value_len) {
    if (!t || !iter) return false;

    uint64_t hash;
    uint32_t val;

    while (ht_bare_iter_next(&t->bare, iter, &hash, &val)) {
        if (val == VAL_NONE) continue;
        const ht_entry_t *e = &t->entries[val];
        if (out_key) *out_key = t->arena + e->arena_offset;
        if (out_key_len) *out_key_len = e->key_len;
        if (out_value) *out_value = t->arena + e->arena_offset + e->key_len;
        if (out_value_len) *out_value_len = e->val_len;
        return true;
    }

    return false;
}

// ============================================================================
// High-Level Public API: Statistics
// ============================================================================

void ht_stats(const ht_table_t *t, ht_stats_t *out_stats) {
    ht_bare_stats(&t->bare, out_stats);
}

size_t ht_size(const ht_table_t *t) {
    if (!t) return 0;
    return t->bare.size;
}

void ht_dump(const ht_table_t *t, uint32_t h32, size_t count) {
    if (!t) return;
    const ht_bare_t *b = &t->bare;
    size_t start_idx = h32 & (b->capacity - 1);
    printf("Dump for h32=0x%x, ideal_idx=%zu:\n", h32, start_idx);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start_idx + i) & (b->capacity - 1);
        uint64_t hpd = b->hash_pd[idx];
        const char *tag = hpd_empty(hpd) ? "EMPTY" : hpd_tomb(hpd) ? "TOMB" : "LIVE";
        if (hpd_live(hpd)) {
            uint32_t eidx = b->vals[idx];
            const ht_entry_t *e = &t->entries[eidx];
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s] klen=%3u vlen=%3u off=%5" PRIu32 "\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag,
                   e->key_len, e->val_len, e->arena_offset);
        } else {
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s]\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag);
        }
    }
    if (b->spill_len > 0) {
        printf("  Spill lane (%zu entries):\n", b->spill_len);
        for (size_t i = 0; i < b->spill_len; i++) {
            uint64_t shpd = b->spill_hash_pd[i];
            uint32_t eidx = b->spill_vals[i];
            const ht_entry_t *e = &t->entries[eidx];
            printf("  spill[%zu]: hash=0x%08" PRIx64 " klen=%3u vlen=%3u off=%5" PRIu32 "\n",
                   i, hpd_hash(shpd), e->key_len, e->val_len, e->arena_offset);
        }
    }
}

// ============================================================================
// High-Level Public API: Invariant Checker
// ============================================================================

const char *ht_check_invariants(const ht_table_t *t) {
    return ht_bare_check_invariants(&t->bare);
}
