/**
 * Draugr Hash Table Implementation — Structure-of-Arrays (SoA)
 *
 * Robin-Hood linear probing + Graveyard prophylactic tombstones +
 * Zombie de-amortized rebuild.
 *
 * SoA Layout:
 *   hash_pd[i]  — uint64_t: lower 48 bits = hash, upper 16 bits = probe_dist
 *   data_idx[i] — uint32_t: index into entries[] (DATA_IDX_NONE if empty/tomb)
 *
 *   hash_pd and data_idx are position-synced: moving an entry during
 *   Robin-Hood swaps exchanges only these two values (12 bytes) — no
 *   key/value bytes are copied during displacement.
 *
 *   entries[] is a separate array of ht_entry_t (12 bytes each), storing
 *   key_len, val_len, and arena_offset.  Accessed only after a probe hit,
 *   never touched during the probe scan itself.
 *
 * Sentinels:
 *   hash == HASH_EMPTY (0)  →  unoccupied slot
 *   hash == HASH_TOMB  (1)  →  tombstone (deleted entry)
 *   hash >= 2               →  live entry
 *
 * Hash values 0 and 1 (in the lower 48 bits) are reserved.  Entries whose
 * hash falls in this range go to a small "spill lane" instead of the main
 * table.  Probability: 2/2^48 per entry.
 *
 * Spill lane:
 *   Same SoA pattern (spill_hash_pd[], spill_data_idx[]), shares entries[]
 *   and arena with the main table.  Linear scan, no tombstones — removal
 *   compacts immediately.
 */

#include "draugr/ht.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// SoA Type Definitions
// ============================================================================

typedef struct {
    uint16_t key_len;
    uint16_t _pad;
    uint32_t val_len;
    uint32_t arena_offset;
} ht_entry_t;  // 12 bytes

// ============================================================================
// Sentinels & Pack/Unpack
// ============================================================================

#define HASH_EMPTY     0ULL
#define HASH_TOMB      1ULL
#define HASH_MASK      0x0000FFFFFFFFFFFFULL
#define DATA_IDX_NONE  UINT32_MAX

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
// Table Structure
// ============================================================================

struct ht_table {
    // Main table (SoA probe arrays)
    uint64_t   *hash_pd;
    uint32_t   *data_idx;
    size_t      capacity;
    size_t      size;
    size_t      tombstone_cnt;

    // Spill lane (SoA)
    uint64_t   *spill_hash_pd;
    uint32_t   *spill_data_idx;
    size_t      spill_cap;
    size_t      spill_len;

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

// ============================================================================
// Utility
// ============================================================================

static size_t next_pow2(size_t n) {
    size_t r = 1;
    while (r < n) r <<= 1;
    return r;
}

// ============================================================================
// Arena Management
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
// Entry Management
// ============================================================================

// Allocate an entry in entries[], copy key+value to arena.
// Returns entry index, or DATA_IDX_NONE on failure.
static uint32_t alloc_entry(ht_table_t *t,
                            const void *key, size_t key_len,
                            const void *value, size_t value_len) {
    if (key_len > UINT16_MAX) return DATA_IDX_NONE;

    if (t->entry_count >= t->entry_cap) {
        size_t new_cap = t->entry_cap ? t->entry_cap * 2 : 64;
        ht_entry_t *ne = realloc(t->entries, new_cap * sizeof(ht_entry_t));
        if (!ne) return DATA_IDX_NONE;
        t->entries = ne;
        t->entry_cap = new_cap;
    }

    void *data = arena_alloc(t, key_len + value_len);
    if (!data) return DATA_IDX_NONE;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + key_len, value, value_len);

    uint32_t eidx = (uint32_t)t->entry_count++;
    t->entries[eidx].key_len = (uint16_t)key_len;
    t->entries[eidx]._pad = 0;
    t->entries[eidx].val_len = (uint32_t)value_len;
    t->entries[eidx].arena_offset = (uint32_t)((uint8_t *)data - t->arena);
    return eidx;
}

// Update an existing entry's value (for UPSERT in-place update).
static bool update_entry_value(ht_table_t *t, uint32_t eidx,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len) {
    void *data = arena_alloc(t, key_len + value_len);
    if (!data) return false;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + key_len, value, value_len);
    t->entries[eidx].val_len = (uint32_t)value_len;
    t->entries[eidx].arena_offset = (uint32_t)((uint8_t *)data - t->arena);
    return true;
}

// ============================================================================
// Key / Value Matching
// ============================================================================

static inline bool keys_match(const ht_table_t *t, uint32_t eidx,
                              const void *key, size_t key_len) {
    const ht_entry_t *e = &t->entries[eidx];
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

// Insert modes (internal)
#define INS_UPSERT  0
#define INS_ALWAYS  1
#define INS_UNIQUE  2

static double compute_x(const ht_table_t *t) {
    double lf = (double)t->size / (double)t->capacity;
    if (lf >= 1.0) return (double)t->capacity;
    if (lf < 0.01) return 1.0;
    return 1.0 / (1.0 - lf);
}

// ============================================================================
// Spill Lane Operations
// ============================================================================

static bool spill_grow(ht_table_t *t) {
    size_t new_cap = t->spill_cap ? t->spill_cap * 2 : SPILL_INITIAL;

    uint64_t *new_hpd = realloc(t->spill_hash_pd, new_cap * sizeof(uint64_t));
    if (!new_hpd) return false;

    uint32_t *new_didx = realloc(t->spill_data_idx, new_cap * sizeof(uint32_t));
    if (!new_didx) {
        t->spill_hash_pd = new_hpd;
        return false;
    }

    memset(new_hpd + t->spill_cap, 0, (new_cap - t->spill_cap) * sizeof(uint64_t));
    memset(new_didx + t->spill_cap, 0xFF, (new_cap - t->spill_cap) * sizeof(uint32_t));

    t->spill_hash_pd = new_hpd;
    t->spill_data_idx = new_didx;
    t->spill_cap = new_cap;
    return true;
}

static bool spill_insert_ex(ht_table_t *t, uint64_t h48,
                             const void *key, size_t key_len,
                             const void *value, size_t value_len,
                             int mode) {
    if (mode == INS_ALWAYS) {
        // Skip scan — always append
    } else if (mode == INS_UPSERT) {
        bool found = false;
        for (size_t i = 0; i < t->spill_len; ) {
            uint64_t shpd = t->spill_hash_pd[i];
            if (hpd_hash(shpd) == h48) {
                uint32_t eidx = t->spill_data_idx[i];
                if (keys_match(t, eidx, key, key_len)) {
                    if (!found) {
                        if (!update_entry_value(t, eidx, key, key_len, value, value_len))
                            return false;
                        found = true;
                        i++;
                    } else {
                        memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                                (t->spill_len - i - 1) * sizeof(uint64_t));
                        memmove(&t->spill_data_idx[i], &t->spill_data_idx[i + 1],
                                (t->spill_len - i - 1) * sizeof(uint32_t));
                        t->spill_len--;
                        t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
                        t->spill_data_idx[t->spill_len] = DATA_IDX_NONE;
                        t->size--;
                    }
                } else {
                    i++;
                }
            } else {
                i++;
            }
        }
        if (found) return false;
    } else { // INS_UNIQUE
        for (size_t i = 0; i < t->spill_len; i++) {
            uint64_t shpd = t->spill_hash_pd[i];
            if (hpd_hash(shpd) == h48 &&
                keys_match(t, t->spill_data_idx[i], key, key_len) &&
                vals_match(t, t->spill_data_idx[i], value, value_len))
                return false;
        }
    }

    // Append new entry
    if (t->spill_len >= t->spill_cap) {
        if (!spill_grow(t)) return false;
    }
    uint32_t eidx = alloc_entry(t, key, key_len, value, value_len);
    if (eidx == DATA_IDX_NONE) return false;
    t->spill_hash_pd[t->spill_len] = hpd_pack(h48, 0);
    t->spill_data_idx[t->spill_len] = eidx;
    t->spill_len++;
    t->size++;
    return true;
}

static bool spill_insert(ht_table_t *t, uint64_t h48,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    return spill_insert_ex(t, h48, key, key_len, value, value_len, INS_UPSERT);
}

static const void *spill_find(const ht_table_t *t, uint64_t h48,
                              const void *key, size_t key_len,
                              size_t *out_value_len) {
    for (size_t i = 0; i < t->spill_len; i++) {
        uint64_t shpd = t->spill_hash_pd[i];
        if (hpd_hash(shpd) == h48) {
            uint32_t eidx = t->spill_data_idx[i];
            if (keys_match(t, eidx, key, key_len)) {
                const ht_entry_t *e = &t->entries[eidx];
                if (out_value_len) *out_value_len = e->val_len;
                return t->arena + e->arena_offset + e->key_len;
            }
        }
    }
    return NULL;
}

static bool spill_remove(ht_table_t *t, uint64_t h48,
                         const void *key, size_t key_len) {
    for (size_t i = 0; i < t->spill_len; i++) {
        uint64_t shpd = t->spill_hash_pd[i];
        if (hpd_hash(shpd) == h48) {
            uint32_t eidx = t->spill_data_idx[i];
            if (keys_match(t, eidx, key, key_len)) {
                memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                        (t->spill_len - i - 1) * sizeof(uint64_t));
                memmove(&t->spill_data_idx[i], &t->spill_data_idx[i + 1],
                        (t->spill_len - i - 1) * sizeof(uint32_t));
                t->spill_len--;
                t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
                t->spill_data_idx[t->spill_len] = DATA_IDX_NONE;
                t->size--;
                return true;
            }
        }
    }
    return false;
}

static void spill_find_all(const ht_table_t *t, uint64_t h48,
                           ht_dup_callback cb, void *user_ctx) {
    for (size_t i = 0; i < t->spill_len; i++) {
        uint64_t shpd = t->spill_hash_pd[i];
        if (hpd_hash(shpd) == h48) {
            uint32_t eidx = t->spill_data_idx[i];
            const ht_entry_t *e = &t->entries[eidx];
            if (!cb(t->arena + e->arena_offset, e->key_len,
                    t->arena + e->arena_offset + e->key_len, e->val_len, user_ctx))
                return;
        }
    }
}

// ============================================================================
// Robin-Hood Insert (main table only)
// ============================================================================

static bool resize_table(ht_table_t *t);

static bool rh_insert_ex(ht_table_t *t, uint64_t h48,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len,
                         int mode) {
    size_t cap_mask = t->capacity - 1;
    size_t ideal = h48 & cap_mask;

    // Phase 1: Scan probe chain for existing entries.
    if (mode != INS_ALWAYS) {
        size_t idx = ideal;
        uint16_t dist = 0;
        bool upsert_updated = false;
        for (size_t steps = 0; steps <= t->capacity; steps++) {
            uint64_t slot_hpd = t->hash_pd[idx];

            if (hpd_empty(slot_hpd)) break;

            if (hpd_tomb(slot_hpd)) {
                idx = (idx + 1) & cap_mask;
                dist++;
                continue;
            }

            if (hpd_pd(slot_hpd) < dist) break;

            if (hpd_hash(slot_hpd) == h48 && keys_match(t, t->data_idx[idx], key, key_len)) {
                if (mode == INS_UNIQUE) {
                    if (vals_match(t, t->data_idx[idx], value, value_len))
                        return false;
                } else { // INS_UPSERT
                    if (!upsert_updated) {
                        if (!update_entry_value(t, t->data_idx[idx],
                                                key, key_len, value, value_len))
                            return false;
                        upsert_updated = true;
                    } else {
                        t->hash_pd[idx] = HASH_TOMB;
                        t->data_idx[idx] = DATA_IDX_NONE;
                        t->tombstone_cnt++;
                        t->size--;
                    }
                }
            }

            idx = (idx + 1) & cap_mask;
            dist++;
        }

        if (upsert_updated) return false;
    }

    // Phase 2: Robin-Hood insert (new entry).
    {
        uint32_t eidx = alloc_entry(t, key, key_len, value, value_len);
        if (eidx == DATA_IDX_NONE) return false;

        uint32_t cur_didx = eidx;
        size_t idx = ideal;
        uint16_t dist = 0;

        while (1) {
            uint64_t slot_hpd = t->hash_pd[idx];

            // Empty or tombstone — place entry here
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
                t->data_idx[idx] = cur_didx;
                t->size++;
                return true;
            }

            // Robin-Hood swap: occupant has shorter probe distance
            if (hpd_pd(slot_hpd) < dist) {
                uint32_t old_didx = t->data_idx[idx];

                t->hash_pd[idx] = hpd_pack(h48, dist);
                t->data_idx[idx] = cur_didx;

                h48 = hpd_hash(slot_hpd);
                dist = hpd_pd(slot_hpd) + 1;
                cur_didx = old_didx;

                idx = (idx + 1) & cap_mask;
                continue;
            }

            idx = (idx + 1) & cap_mask;
            dist++;

            if (dist > t->capacity) {
                // Save current entry's key/val to stack before resize
                ht_entry_t *e = &t->entries[cur_didx];
                size_t ek = e->key_len, ev = e->val_len;
                void *sk = alloca(ek), *sv = alloca(ev);
                memcpy(sk, t->arena + e->arena_offset, ek);
                memcpy(sv, t->arena + e->arena_offset + ek, ev);
                if (!resize_table(t)) return false;
                return rh_insert_ex(t, h48, sk, ek, sv, ev, mode);
            }
        }
    }
}

static bool rh_insert(ht_table_t *t, uint64_t h48,
                      const void *key, size_t key_len,
                      const void *value, size_t value_len) {
    return rh_insert_ex(t, h48, key, key_len, value, value_len, INS_UPSERT);
}

// ============================================================================
// Zombie Interval Rebuild
// ============================================================================

static bool verify_ideal_safe(const ht_table_t *t, size_t idx, size_t len) {
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

static void commit_backward_shift(ht_table_t *t, size_t idx, size_t len) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t read_pos = (idx + 1 + i) & cap_mask;
        uint64_t hpd = t->hash_pd[read_pos];

        if (hpd_live(hpd)) {
            size_t write_pos = (idx + write_offset) & cap_mask;
            size_t shift = (i + 1) - write_offset;
            t->hash_pd[write_pos] = hpd_pack(hpd_hash(hpd), hpd_pd(hpd) - (uint16_t)shift);
            t->data_idx[write_pos] = t->data_idx[read_pos];
            write_offset++;
        } else {
            t->tombstone_cnt--;
        }
        t->hash_pd[read_pos] = HASH_EMPTY;
        t->data_idx[read_pos] = DATA_IDX_NONE;
    }
    t->tombstone_cnt--;
}

static void delete_compact(ht_table_t *t, size_t idx) {
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
        verify_ideal_safe(t, idx, chain_len)) {
        commit_backward_shift(t, idx, chain_len);
    }
}

static void zombie_step(ht_table_t *t) {
    if (t->zombie_window == 0) return;

    double x = compute_x(t);
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
// Public API: Lifecycle
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

    t->capacity = next_pow2(c.initial_capacity);

    t->hash_pd = calloc(t->capacity, sizeof(uint64_t));
    if (!t->hash_pd) { free(t); return NULL; }

    t->data_idx = malloc(t->capacity * sizeof(uint32_t));
    if (!t->data_idx) { free(t->hash_pd); free(t); return NULL; }
    memset(t->data_idx, 0xFF, t->capacity * sizeof(uint32_t));

    t->spill_cap = SPILL_INITIAL;
    t->spill_hash_pd = calloc(t->spill_cap, sizeof(uint64_t));
    if (!t->spill_hash_pd) { free(t->data_idx); free(t->hash_pd); free(t); return NULL; }

    t->spill_data_idx = malloc(t->spill_cap * sizeof(uint32_t));
    if (!t->spill_data_idx) {
        free(t->spill_hash_pd); free(t->data_idx); free(t->hash_pd); free(t);
        return NULL;
    }
    memset(t->spill_data_idx, 0xFF, t->spill_cap * sizeof(uint32_t));

    t->entries = calloc(64, sizeof(ht_entry_t));
    t->entry_cap = 64;
    if (!t->entries) {
        free(t->spill_data_idx); free(t->spill_hash_pd);
        free(t->data_idx); free(t->hash_pd); free(t);
        return NULL;
    }

    t->arena = malloc(1024);
    t->arena_cap = 1024;
    if (!t->arena) {
        free(t->entries); free(t->spill_data_idx); free(t->spill_hash_pd);
        free(t->data_idx); free(t->hash_pd); free(t);
        return NULL;
    }

    t->hash_fn = hash_fn;
    t->eq_fn = eq_fn;
    t->user_ctx = user_ctx;
    t->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
                         (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
    t->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    t->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    t->zombie_window = c.zombie_window;

    return t;
}

void ht_destroy(ht_table_t *t) {
    if (!t) return;
    free(t->spill_hash_pd);
    free(t->spill_data_idx);
    free(t->hash_pd);
    free(t->data_idx);
    free(t->entries);
    free(t->arena);
    free(t);
}

void ht_clear(ht_table_t *t) {
    if (!t) return;
    memset(t->hash_pd, 0, t->capacity * sizeof(uint64_t));
    memset(t->spill_hash_pd, 0, t->spill_cap * sizeof(uint64_t));
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_len = 0;
    t->arena_size = 0;
    t->entry_count = 0;
    t->zombie_cursor = 0;
}

// ============================================================================
// Insert / Upsert / Unsert
// ============================================================================

static bool do_insert_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len,
                                int mode) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return spill_insert_ex(t, h48, key, key_len, value, value_len, mode);

    if (!t->resizing && (double)(t->size + 1) / t->capacity > t->max_load_factor)
        ht_resize(t, t->capacity * 2);

    double total = t->size + t->tombstone_cnt;
    if (total > 0 && (double)t->tombstone_cnt / total > t->tomb_threshold) {
        for (int i = 0; i < 4; i++) zombie_step(t);
    }

    bool result = rh_insert_ex(t, h48, key, key_len, value, value_len, mode);

    if (result) {
        zombie_step(t);
    }

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
// Lookup
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

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return spill_find(t, h48, key, key_len, out_value_len);

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) return NULL;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) return NULL;

        if (hpd_hash(slot_hpd) == h48 && keys_match(t, t->data_idx[idx], key, key_len)) {
            const ht_entry_t *e = &t->entries[t->data_idx[idx]];
            if (out_value_len) *out_value_len = e->val_len;
            return t->arena + e->arena_offset + e->key_len;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return NULL;
}

void ht_find_all(const ht_table_t *t, uint64_t hash,
                 ht_dup_callback cb, void *user_ctx) {
    if (!t || !cb) return;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2) {
        spill_find_all(t, h48, cb, user_ctx);
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
            uint32_t eidx = t->data_idx[idx];
            const ht_entry_t *e = &t->entries[eidx];
            if (!cb(t->arena + e->arena_offset, e->key_len,
                    t->arena + e->arena_offset + e->key_len, e->val_len, user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }
}

void ht_find_key_all_with_hash(const ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               ht_dup_callback cb, void *user_ctx) {
    if (!t || !key || !cb) return;

    uint64_t h48 = hash & HASH_MASK;

    // Spill lane
    for (size_t i = 0; i < t->spill_len; i++) {
        uint64_t shpd = t->spill_hash_pd[i];
        if (hpd_hash(shpd) == h48) {
            uint32_t eidx = t->spill_data_idx[i];
            if (keys_match(t, eidx, key, key_len)) {
                const ht_entry_t *e = &t->entries[eidx];
                if (!cb(t->arena + e->arena_offset, e->key_len,
                        t->arena + e->arena_offset + e->key_len, e->val_len, user_ctx))
                    return;
            }
        }
    }

    // Main table
    if (h48 < 2) return;

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

        if (hpd_hash(slot_hpd) == h48 && keys_match(t, t->data_idx[idx], key, key_len)) {
            uint32_t eidx = t->data_idx[idx];
            const ht_entry_t *e = &t->entries[eidx];
            if (!cb(t->arena + e->arena_offset, e->key_len,
                    t->arena + e->arena_offset + e->key_len, e->val_len, user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }
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

    uint64_t h48 = hash & HASH_MASK;

    // Spill lane
    for (size_t i = 0; i < t->spill_len; i++) {
        uint64_t shpd = t->spill_hash_pd[i];
        if (hpd_hash(shpd) == h48) {
            uint32_t eidx = t->spill_data_idx[i];
            if (keys_match(t, eidx, key, key_len) &&
                vals_match(t, eidx, value, value_len)) {
                const ht_entry_t *e = &t->entries[eidx];
                if (out_value_len) *out_value_len = e->val_len;
                return t->arena + e->arena_offset + e->key_len;
            }
        }
    }

    // Main table
    if (h48 < 2) return NULL;

    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) return NULL;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) return NULL;

        if (hpd_hash(slot_hpd) == h48 &&
            keys_match(t, t->data_idx[idx], key, key_len) &&
            vals_match(t, t->data_idx[idx], value, value_len)) {
            const ht_entry_t *e = &t->entries[t->data_idx[idx]];
            if (out_value_len) *out_value_len = e->val_len;
            return t->arena + e->arena_offset + e->key_len;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return NULL;
}

const void *ht_find_kv(const ht_table_t *t, const void *key, size_t key_len,
                       const void *value, size_t value_len,
                       size_t *out_value_len) {
    if (!t || !key || !value) return NULL;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_find_kv_with_hash(t, hash, key, key_len, value, value_len, out_value_len);
}

// ============================================================================
// Increment
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

// ============================================================================
// Delete
// ============================================================================

size_t ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                            const void *key, size_t key_len) {
    if (!t || !key) return 0;

    uint64_t h48 = hash & HASH_MASK;
    size_t removed = 0;

    // Spill lane
    if (h48 < 2) {
        for (size_t i = 0; i < t->spill_len; ) {
            uint64_t shpd = t->spill_hash_pd[i];
            if (hpd_hash(shpd) == h48) {
                uint32_t eidx = t->spill_data_idx[i];
                if (keys_match(t, eidx, key, key_len)) {
                    memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint64_t));
                    memmove(&t->spill_data_idx[i], &t->spill_data_idx[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint32_t));
                    t->spill_len--;
                    t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
                    t->spill_data_idx[t->spill_len] = DATA_IDX_NONE;
                    t->size--;
                    removed++;
                } else {
                    i++;
                }
            } else {
                i++;
            }
        }
        return removed;
    }

    // Main table
    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;

        if (hpd_hash(slot_hpd) == h48 && keys_match(t, t->data_idx[idx], key, key_len)) {
            t->size--;
            t->hash_pd[idx] = HASH_TOMB;
            t->data_idx[idx] = DATA_IDX_NONE;
            t->tombstone_cnt++;
            removed++;
            delete_compact(t, idx);
            continue;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    if (removed > 0 && t->min_load_factor > 0 && t->size > 0 &&
        (double)t->size / t->capacity < t->min_load_factor &&
        t->capacity > 64) {
        size_t new_cap = t->capacity / 2;
        if (new_cap >= 64 && new_cap >= t->size * 2)
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

    uint64_t h48 = hash & HASH_MASK;
    size_t removed = 0;

    // Spill lane
    if (h48 < 2) {
        for (size_t i = 0; i < t->spill_len; ) {
            uint64_t shpd = t->spill_hash_pd[i];
            if (hpd_hash(shpd) == h48) {
                uint32_t eidx = t->spill_data_idx[i];
                if (keys_match(t, eidx, key, key_len) &&
                    vals_match(t, eidx, value, value_len)) {
                    memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint64_t));
                    memmove(&t->spill_data_idx[i], &t->spill_data_idx[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint32_t));
                    t->spill_len--;
                    t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
                    t->spill_data_idx[t->spill_len] = DATA_IDX_NONE;
                    t->size--;
                    removed++;
                } else {
                    i++;
                }
            } else {
                i++;
            }
        }
        return removed;
    }

    // Main table
    size_t cap_mask = t->capacity - 1;
    size_t idx = h48 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        uint64_t slot_hpd = t->hash_pd[idx];

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;

        if (hpd_hash(slot_hpd) == h48 &&
            keys_match(t, t->data_idx[idx], key, key_len) &&
            vals_match(t, t->data_idx[idx], value, value_len)) {
            t->size--;
            t->hash_pd[idx] = HASH_TOMB;
            t->data_idx[idx] = DATA_IDX_NONE;
            t->tombstone_cnt++;
            removed++;
            delete_compact(t, idx);
            continue;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
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

    uint64_t h48 = hash & HASH_MASK;

    // Spill lane
    if (h48 < 2) {
        for (size_t i = 0; i < t->spill_len; i++) {
            uint64_t shpd = t->spill_hash_pd[i];
            if (hpd_hash(shpd) == h48) {
                uint32_t eidx = t->spill_data_idx[i];
                if (keys_match(t, eidx, key, key_len) &&
                    vals_match(t, eidx, value, value_len)) {
                    memmove(&t->spill_hash_pd[i], &t->spill_hash_pd[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint64_t));
                    memmove(&t->spill_data_idx[i], &t->spill_data_idx[i + 1],
                            (t->spill_len - i - 1) * sizeof(uint32_t));
                    t->spill_len--;
                    t->spill_hash_pd[t->spill_len] = HASH_EMPTY;
                    t->spill_data_idx[t->spill_len] = DATA_IDX_NONE;
                    t->size--;
                    return true;
                }
            }
        }
        return false;
    }

    // Main table
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

        if (hpd_hash(slot_hpd) == h48 &&
            keys_match(t, t->data_idx[idx], key, key_len) &&
            vals_match(t, t->data_idx[idx], value, value_len)) {
            t->size--;
            t->hash_pd[idx] = HASH_TOMB;
            t->data_idx[idx] = DATA_IDX_NONE;
            t->tombstone_cnt++;
            delete_compact(t, idx);

            if (t->min_load_factor > 0 && t->size > 0 &&
                (double)t->size / t->capacity < t->min_load_factor &&
                t->capacity > 64) {
                size_t new_cap = t->capacity / 2;
                if (new_cap >= 64 && new_cap >= t->size * 2)
                    ht_resize(t, new_cap);
            }

            return true;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
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
// Resize
// ============================================================================

static bool resize_table(ht_table_t *t) {
    return ht_resize(t, t->capacity * 2);
}

static void reinsert_live(ht_table_t *t,
                          const uint64_t *old_hash_pd, const uint32_t *old_data_idx,
                          const ht_entry_t *old_entries, const uint8_t *old_arena,
                          size_t old_cap) {
    for (size_t i = 0; i < old_cap; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        uint32_t eidx = old_data_idx[i];
        const ht_entry_t *e = &old_entries[eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        rh_insert(t, hpd_hash(hpd), k, e->key_len, v, e->val_len);
    }
}

static void place_prophylactic_tombstones(ht_table_t *t) {
    double x = compute_x(t);
    size_t spacing = (size_t)(x * C_P_DEFAULT);
    if (spacing < 4) spacing = 4;

    for (size_t pos = 0; pos < t->capacity; pos += spacing) {
        if (hpd_empty(t->hash_pd[pos])) {
            t->hash_pd[pos] = HASH_TOMB;
            t->data_idx[pos] = DATA_IDX_NONE;
            t->tombstone_cnt++;
        }
    }
}

static void reinsert_spill(ht_table_t *t,
                           const uint64_t *old_spill_hash_pd,
                           const uint32_t *old_spill_data_idx,
                           const ht_entry_t *old_entries,
                           const uint8_t *old_arena,
                           size_t old_spill_len) {
    for (size_t i = 0; i < old_spill_len; i++) {
        uint32_t eidx = old_spill_data_idx[i];
        if (eidx == DATA_IDX_NONE) continue;
        const ht_entry_t *e = &old_entries[eidx];
        if (e->key_len == 0) continue;
        const void *k = old_arena + e->arena_offset;
        const void *v = old_arena + e->arena_offset + e->key_len;
        spill_insert(t, hpd_hash(old_spill_hash_pd[i]), k, e->key_len, v, e->val_len);
    }
}

bool ht_resize(ht_table_t *t, size_t new_capacity) {
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
    uint32_t *old_data_idx = t->data_idx;
    ht_entry_t *old_entries = t->entries;
    uint8_t *old_arena = t->arena;
    size_t old_cap = t->capacity;
    size_t old_arena_cap = t->arena_cap;

    uint64_t *old_spill_hash_pd = t->spill_hash_pd;
    uint32_t *old_spill_data_idx = t->spill_data_idx;
    size_t old_spill_len = t->spill_len;

    // Allocate new main table
    uint64_t *new_hash_pd = calloc(new_capacity, sizeof(uint64_t));
    if (!new_hash_pd) { t->resizing = false; return false; }

    uint32_t *new_data_idx = malloc(new_capacity * sizeof(uint32_t));
    if (!new_data_idx) {
        free(new_hash_pd);
        t->resizing = false;
        return false;
    }
    memset(new_data_idx, 0xFF, new_capacity * sizeof(uint32_t));

    // Allocate new entries
    size_t new_entry_cap = t->entry_cap;
    ht_entry_t *new_entries = calloc(new_entry_cap, sizeof(ht_entry_t));
    if (!new_entries) {
        free(new_data_idx); free(new_hash_pd);
        t->resizing = false;
        return false;
    }

    // Allocate new arena
    uint8_t *new_arena = malloc(old_arena_cap > 0 ? old_arena_cap : 1024);
    if (!new_arena) {
        free(new_entries); free(new_data_idx); free(new_hash_pd);
        t->resizing = false;
        return false;
    }

    // Allocate new spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_arena); free(new_entries); free(new_data_idx); free(new_hash_pd);
        t->resizing = false;
        return false;
    }

    uint32_t *new_spill_data_idx = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_data_idx) {
        free(new_spill_hash_pd); free(new_arena); free(new_entries);
        free(new_data_idx); free(new_hash_pd);
        t->resizing = false;
        return false;
    }
    memset(new_spill_data_idx, 0xFF, new_spill_cap * sizeof(uint32_t));

    // Swap to new state
    t->hash_pd = new_hash_pd;
    t->data_idx = new_data_idx;
    t->capacity = new_capacity;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->entries = new_entries;
    t->entry_count = 0;
    t->entry_cap = new_entry_cap;
    t->arena = new_arena;
    t->arena_size = 0;
    t->arena_cap = old_arena_cap > 0 ? old_arena_cap : 1024;
    t->spill_hash_pd = new_spill_hash_pd;
    t->spill_data_idx = new_spill_data_idx;
    t->spill_cap = new_spill_cap;
    t->spill_len = 0;
    t->zombie_cursor = 0;

    reinsert_live(t, old_hash_pd, old_data_idx, old_entries, old_arena, old_cap);
    reinsert_spill(t, old_spill_hash_pd, old_spill_data_idx, old_entries, old_arena, old_spill_len);
    place_prophylactic_tombstones(t);

    free(old_hash_pd);
    free(old_data_idx);
    free(old_entries);
    free(old_arena);
    free(old_spill_hash_pd);
    free(old_spill_data_idx);
    t->resizing = false;
    return true;
}

void ht_compact(ht_table_t *t) {
    if (!t) return;

    // Save old state
    uint64_t *old_hash_pd = t->hash_pd;
    uint32_t *old_data_idx = t->data_idx;
    ht_entry_t *old_entries = t->entries;
    uint8_t *old_arena = t->arena;
    size_t old_cap = t->capacity;
    size_t old_arena_cap = t->arena_cap;

    uint64_t *old_spill_hash_pd = t->spill_hash_pd;
    uint32_t *old_spill_data_idx = t->spill_data_idx;
    size_t old_spill_len = t->spill_len;

    // Allocate new main table (same capacity)
    uint64_t *new_hash_pd = calloc(old_cap, sizeof(uint64_t));
    if (!new_hash_pd) return;

    uint32_t *new_data_idx = malloc(old_cap * sizeof(uint32_t));
    if (!new_data_idx) {
        free(new_hash_pd);
        return;
    }
    memset(new_data_idx, 0xFF, old_cap * sizeof(uint32_t));

    // Allocate new entries
    size_t new_entry_cap = t->entry_cap;
    ht_entry_t *new_entries = calloc(new_entry_cap, sizeof(ht_entry_t));
    if (!new_entries) {
        free(new_data_idx); free(new_hash_pd);
        return;
    }

    // Allocate new arena
    uint8_t *new_arena = malloc(old_arena_cap > 0 ? old_arena_cap : 1024);
    if (!new_arena) {
        free(new_entries); free(new_data_idx); free(new_hash_pd);
        return;
    }

    // Allocate new spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    uint64_t *new_spill_hash_pd = calloc(new_spill_cap, sizeof(uint64_t));
    if (!new_spill_hash_pd) {
        free(new_arena); free(new_entries); free(new_data_idx); free(new_hash_pd);
        return;
    }

    uint32_t *new_spill_data_idx = malloc(new_spill_cap * sizeof(uint32_t));
    if (!new_spill_data_idx) {
        free(new_spill_hash_pd); free(new_arena); free(new_entries);
        free(new_data_idx); free(new_hash_pd);
        return;
    }
    memset(new_spill_data_idx, 0xFF, new_spill_cap * sizeof(uint32_t));

    // Swap to new state
    t->hash_pd = new_hash_pd;
    t->data_idx = new_data_idx;
    t->entries = new_entries;
    t->entry_count = 0;
    t->arena = new_arena;
    t->arena_size = 0;
    t->arena_cap = old_arena_cap > 0 ? old_arena_cap : 1024;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_hash_pd = new_spill_hash_pd;
    t->spill_data_idx = new_spill_data_idx;
    t->spill_cap = new_spill_cap;
    t->spill_len = 0;
    t->zombie_cursor = 0;

    reinsert_live(t, old_hash_pd, old_data_idx, old_entries, old_arena, old_cap);
    reinsert_spill(t, old_spill_hash_pd, old_spill_data_idx, old_entries, old_arena, old_spill_len);
    place_prophylactic_tombstones(t);

    free(old_hash_pd);
    free(old_data_idx);
    free(old_entries);
    free(old_arena);
    free(old_spill_hash_pd);
    free(old_spill_data_idx);
}

// ============================================================================
// Iterator
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

    while (iter->idx < t->capacity) {
        uint64_t hpd = t->hash_pd[iter->idx];
        uint32_t didx = t->data_idx[iter->idx];
        iter->idx++;
        if (hpd_live(hpd) && didx != DATA_IDX_NONE) {
            const ht_entry_t *e = &t->entries[didx];
            if (out_key) *out_key = t->arena + e->arena_offset;
            if (out_key_len) *out_key_len = e->key_len;
            if (out_value) *out_value = t->arena + e->arena_offset + e->key_len;
            if (out_value_len) *out_value_len = e->val_len;
            return true;
        }
    }

    // Then iterate spill lane (no tombstones — all entries within spill_len are live)
    size_t spill_idx = iter->idx - t->capacity;
    while (spill_idx < t->spill_len) {
        uint32_t sdidx = t->spill_data_idx[spill_idx];
        spill_idx++;
        iter->idx = t->capacity + spill_idx;
        if (sdidx != DATA_IDX_NONE) {
            const ht_entry_t *e = &t->entries[sdidx];
            if (out_key) *out_key = t->arena + e->arena_offset;
            if (out_key_len) *out_key_len = e->key_len;
            if (out_value) *out_value = t->arena + e->arena_offset + e->key_len;
            if (out_value_len) *out_value_len = e->val_len;
            return true;
        }
    }

    return false;
}

// ============================================================================
// Statistics
// ============================================================================

void ht_stats(const ht_table_t *t, ht_stats_t *out_stats) {
    if (!t || !out_stats) return;
    out_stats->size = t->size;
    out_stats->capacity = t->capacity;
    out_stats->tombstone_cnt = t->tombstone_cnt;
    out_stats->load_factor = (double)t->size / t->capacity;
    out_stats->tombstone_ratio = (t->size + t->tombstone_cnt > 0)
        ? (double)t->tombstone_cnt / (t->size + t->tombstone_cnt)
        : 0.0;
}

void ht_dump(const ht_table_t *t, uint32_t h32, size_t count) {
    if (!t) return;
    size_t start_idx = h32 & (t->capacity - 1);
    printf("Dump for h32=0x%x, ideal_idx=%zu:\n", h32, start_idx);
    for (size_t i = 0; i < count; i++) {
        size_t idx = (start_idx + i) & (t->capacity - 1);
        uint64_t hpd = t->hash_pd[idx];
        const char *tag = hpd_empty(hpd) ? "EMPTY" : hpd_tomb(hpd) ? "TOMB" : "LIVE";
        if (hpd_live(hpd)) {
            uint32_t eidx = t->data_idx[idx];
            const ht_entry_t *e = &t->entries[eidx];
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s] klen=%3u vlen=%3u off=%5" PRIu32 "\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag,
                   e->key_len, e->val_len, e->arena_offset);
        } else {
            printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s]\n",
                   idx, hpd_hash(hpd), hpd_pd(hpd), tag);
        }
    }
    if (t->spill_len > 0) {
        printf("  Spill lane (%zu entries):\n", t->spill_len);
        for (size_t i = 0; i < t->spill_len; i++) {
            uint64_t shpd = t->spill_hash_pd[i];
            uint32_t eidx = t->spill_data_idx[i];
            const ht_entry_t *e = &t->entries[eidx];
            printf("  spill[%zu]: hash=0x%08" PRIx64 " klen=%3u vlen=%3u off=%5" PRIu32 "\n",
                   i, hpd_hash(shpd), e->key_len, e->val_len, e->arena_offset);
        }
    }
}

// ============================================================================
// Invariant Checker
// ============================================================================

const char *ht_check_invariants(const ht_table_t *t) {
    if (!t) return "table is NULL";
    size_t cap_mask = t->capacity - 1;

    size_t live_count = 0;
    size_t tomb_count = 0;
    size_t spill_live = 0;

    // Invariant 1: probe_dist == (pos - ideal) % capacity for every live entry
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

    // Count spill lane (no tombstones — all within spill_len are live)
    for (size_t i = 0; i < t->spill_len; i++) {
        if (t->spill_data_idx[i] != DATA_IDX_NONE)
            spill_live++;
    }

    // Invariant 2: size matches actual live count
    if (t->size != live_count + spill_live) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "size=%zu but found %zu live (%zu main + %zu spill)",
                 t->size, live_count + spill_live, live_count, spill_live);
        return buf;
    }

    // Invariant 3: tombstone_cnt matches actual tombstone count
    if (t->tombstone_cnt != tomb_count) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "tombstone_cnt=%zu but found %zu tombs",
                 t->tombstone_cnt, tomb_count);
        return buf;
    }

    // Invariant 4: No live entry exists that early termination would skip.
    {
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
    }

    return NULL;
}
