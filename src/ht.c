/**
 * Draugr Hash Table Implementation
 *
 * Robin-Hood linear probing + Graveyard prophylactic tombstones +
 * Zombie de-amortized rebuild.
 *
 * Sentinel encoding:
 *   hash == HASH_EMPTY (0)  →  unoccupied slot
 *   hash == HASH_TOMB  (1)  →  tombstone (deleted entry)
 *   hash >= 2               →  live entry
 *
 * The hash values 0 and 1 are reserved and can never appear in a live
 * entry's stored hash.  When a user-supplied hash has lower 32 bits of
 * 0 or 1, the entry is placed in a small "spill lane" instead of the
 * main table.  This is exceedingly rare (2 out of 2^32) but guarantees
 * correctness for every possible hash value.
 *
 * Spill lane:
 *   A separate small array of slots, searched linearly.  Insert / find /
 *   remove / iterate all check the spill lane in addition to the main
 *   table.  The spill lane never stores tombstones — removal simply
 *   shifts subsequent entries down (like a tiny open-addressing array
 *   with immediate compaction).
 *
 * Robin-Hood insertion (main table only):
 *   Classic Robin-Hood with probe_dist comparison.  On insert, if the
 *   current slot's occupant has a smaller probe_dist, they swap.  Tomb-
 *   stones (HASH_TOMB) are "available" and can be overwritten.
 *
 * Lookup / Delete (main table):
 *   Probe from ideal position.  Skip HASH_TOMB.  Stop when:
 *     - HASH_EMPTY found (key not present in main table)
 *     - slot.probe_dist < our distance (Robin-Hood invariant)
 *   Then also check the spill lane.
 *
 * Graveyard (Bender et al., FOCS 2021):
 *   On rebuild, place prophylactic tombstones at evenly-spaced positions
 *   to break up primary clustering.  c_p = 3.
 *
 * Zombie (Chesetti et al., SIGMOD 2025):
 *   De-amortized tombstone redistribution via incremental interval
 *   rebuilds.  One interval of c_b * x slots per insert.  c_b = 3.
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Slot Layout
// ============================================================================

typedef struct {
    uint32_t hash;        // HASH_EMPTY, HASH_TOMB, or live hash (>= 2)
    uint16_t probe_dist;  // Distance from ideal position
    uint16_t key_len;
    uint16_t val_len;
    uint32_t offset;      // Byte offset into data arena
} ht_slot_t;

// ============================================================================
// Sentinels
// ============================================================================

#define HASH_EMPTY 0u
#define HASH_TOMB  1u

// Can this hash value go into the main table?
static inline bool hash_is_live(uint32_t h) {
    return h >= 2;
}

// ============================================================================
// Table Structure
// ============================================================================

struct ht_table {
    ht_slot_t    *slots;
    size_t        capacity;       // Main table size (power of 2)
    size_t        size;           // Live entries (main + spill)
    size_t        tombstone_cnt;  // Tombstones in main table

    // Spill lane: entries whose hash collides with sentinels (0 or 1).
    // Linear array, no tombstones — removal compacts immediately.
    ht_slot_t    *spill;
    size_t        spill_cap;
    size_t        spill_len;      // Number of live entries in spill lane

    // Data arena (key+value bytes)
    uint8_t      *data_arena;
    size_t        data_size;
    size_t        data_cap;

    // Functions
    ht_hash_fn    hash_fn;
    ht_eq_fn      eq_fn;
    void         *user_ctx;

    // Config
    double        max_load_factor;
    double        min_load_factor;
    double        tomb_threshold;
    size_t        zombie_window;

    // Zombie rebuild state
    size_t        zombie_cursor;
    size_t        rebuild_age;

    double        c_p;            // Primitive tombstone spacing factor
    bool          resizing;
};

// ============================================================================
// Constants
// ============================================================================

#define C_P_DEFAULT 3.0
#define C_B_DEFAULT 3.0
#define SPILL_INITIAL 8

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

static bool grow_arena(ht_table_t *t, size_t needed) {
    if (t->data_size + needed <= t->data_cap) return true;
    size_t new_cap = t->data_cap ? t->data_cap * 2 : 1024;
    while (new_cap < t->data_size + needed) new_cap *= 2;
    uint8_t *p = realloc(t->data_arena, new_cap);
    if (!p) return false;
    t->data_arena = p;
    t->data_cap = new_cap;
    return true;
}

static void *arena_alloc(ht_table_t *t, size_t n) {
    if (!grow_arena(t, n)) return NULL;
    void *p = t->data_arena + t->data_size;
    t->data_size += n;
    return p;
}

static inline bool slot_empty(const ht_slot_t *s) {
    return s->hash == HASH_EMPTY;
}

static inline bool slot_is_tomb(const ht_slot_t *s) {
    return s->hash == HASH_TOMB;
}

static inline bool slot_available(const ht_slot_t *s) {
    return s->hash == HASH_EMPTY || s->hash == HASH_TOMB;
}

static inline const void *slot_key_ptr(const ht_table_t *t, const ht_slot_t *s) {
    return t->data_arena + s->offset;
}

static inline const void *slot_val_ptr(const ht_table_t *t, const ht_slot_t *s) {
    return t->data_arena + s->offset + s->key_len;
}

static inline bool keys_match(const ht_table_t *t, const ht_slot_t *s,
                              const void *key, size_t key_len) {
    if (s->key_len != key_len) return false;
    if (t->eq_fn)
        return t->eq_fn(slot_key_ptr(t, s), s->key_len, key, key_len, t->user_ctx);
    return memcmp(slot_key_ptr(t, s), key, key_len) == 0;
}

static double compute_x(const ht_table_t *t) {
    double lf = (double)t->size / (double)t->capacity;
    if (lf >= 1.0) return (double)t->capacity;
    if (lf < 0.01) return 1.0;
    return 1.0 / (1.0 - lf);
}

// ============================================================================
// Place entry into a slot (copies key+value into arena)
// ============================================================================

static bool place_entry(ht_table_t *t, ht_slot_t *slot,
                        uint32_t h32, uint16_t probe_dist,
                        const void *key, size_t key_len,
                        const void *value, size_t value_len) {
    if (!grow_arena(t, key_len + value_len)) return false;
    void *data = arena_alloc(t, key_len + value_len);
    if (!data) return false;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + key_len, value, value_len);

    slot->hash = h32;
    slot->probe_dist = probe_dist;
    slot->key_len = (uint16_t)key_len;
    slot->val_len = (uint16_t)value_len;
    slot->offset = (uint32_t)((uint8_t *)data - t->data_arena);
    return true;
}

// ============================================================================
// Spill Lane Operations
//
// The spill lane holds entries whose hash (lower 32 bits) is 0 or 1 —
// values reserved as HASH_EMPTY and HASH_TOMB sentinels.  This is
// exceedingly rare (probability 2/2^32 per entry) but must exist for
// correctness.  No tombstones — removal compacts immediately.
// ============================================================================

static bool spill_grow(ht_table_t *t) {
    size_t new_cap = t->spill_cap ? t->spill_cap * 2 : SPILL_INITIAL;
    ht_slot_t *ns = realloc(t->spill, new_cap * sizeof(ht_slot_t));
    if (!ns) return false;
    // Zero new entries
    memset(ns + t->spill_cap, 0, (new_cap - t->spill_cap) * sizeof(ht_slot_t));
    t->spill = ns;
    t->spill_cap = new_cap;
    return true;
}

// Insert into spill lane.  Returns true if new entry, false if update.
static bool spill_insert(ht_table_t *t, uint32_t h32,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    // Check for existing key (update)
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            // Update value
            if (!grow_arena(t, key_len + value_len)) return false;
            void *data = arena_alloc(t, key_len + value_len);
            if (!data) return false;
            memcpy(data, key, key_len);
            memcpy((uint8_t *)data + key_len, value, value_len);
            s->val_len = (uint16_t)value_len;
            s->offset = (uint32_t)((uint8_t *)data - t->data_arena);
            return false;
        }
    }

    // Append new entry
    if (t->spill_len >= t->spill_cap) {
        if (!spill_grow(t)) return false;
    }
    if (!place_entry(t, &t->spill[t->spill_len], h32, 0,
                     key, key_len, value, value_len))
        return false;
    t->spill_len++;
    t->size++;
    return true;
}

// Find in spill lane
static const void *spill_find(const ht_table_t *t, uint32_t h32,
                              const void *key, size_t key_len,
                              size_t *out_value_len) {
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            if (out_value_len) *out_value_len = s->val_len;
            return slot_val_ptr(t, s);
        }
    }
    return NULL;
}

// Remove from spill lane (compact after removal)
static bool spill_remove(ht_table_t *t, uint32_t h32,
                         const void *key, size_t key_len) {
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            // Shift remaining entries down
            memmove(&t->spill[i], &t->spill[i + 1],
                    (t->spill_len - i - 1) * sizeof(ht_slot_t));
            t->spill_len--;
            t->spill[t->spill_len] = (ht_slot_t){0};  // clear tail
            t->size--;
            return true;
        }
    }
    return false;
}

// Iterate all spill-lane entries via callback
static void spill_find_all(const ht_table_t *t, uint32_t h32,
                           ht_dup_callback cb, void *user_ctx) {
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32) {
            if (!cb(slot_key_ptr(t, s), s->key_len,
                    slot_val_ptr(t, s), s->val_len, user_ctx))
                return;
        }
    }
}

// ============================================================================
// Robin-Hood Insert (main table only)
// ============================================================================

static bool resize_table(ht_table_t *t);

static bool rh_insert(ht_table_t *t, uint32_t h32,
                      const void *key, size_t key_len,
                      const void *value, size_t value_len) {
    size_t cap_mask = t->capacity - 1;
    size_t ideal = h32 & cap_mask;

    // Phase 1: Scan probe chain for existing key (update).
    // Must scan the entire chain (no early termination) because
    // prophylactic tombstones placed after rebuild can break the
    // Robin-Hood probe_dist invariant.
    {
        size_t idx = ideal;
        for (size_t steps = 0; steps <= t->capacity; steps++) {
            ht_slot_t *s = &t->slots[idx];

            if (slot_empty(s)) break;

            if (slot_is_tomb(s)) {
                idx = (idx + 1) & cap_mask;
                continue;
            }

            if (s->hash == h32 && keys_match(t, s, key, key_len)) {
                // Update existing entry
                if (!grow_arena(t, key_len + value_len)) return false;
                void *data = arena_alloc(t, key_len + value_len);
                if (!data) return false;
                memcpy(data, key, key_len);
                memcpy((uint8_t *)data + key_len, value, value_len);
                s->val_len = (uint16_t)value_len;
                s->offset = (uint32_t)((uint8_t *)data - t->data_arena);
                return false; // updated, not inserted
            }

            idx = (idx + 1) & cap_mask;
        }
    }

    // Phase 2: Key not found — Robin-Hood insert (new entry).
    {
        size_t idx = ideal;
        uint16_t dist = 0;

        const void *cur_key = key;
        const void *cur_val = value;
        size_t cur_klen = key_len;
        size_t cur_vlen = value_len;

        while (1) {
            ht_slot_t *s = &t->slots[idx];

            // Empty or tombstone — place entry here
            if (slot_available(s)) {
                if (slot_is_tomb(s)) t->tombstone_cnt--;
                if (!place_entry(t, s, h32, dist, cur_key, cur_klen, cur_val, cur_vlen))
                    return false;
                t->size++;
                return true;
            }

            // Robin-Hood swap: occupant has shorter probe distance
            if (s->probe_dist < dist) {
                uint32_t old_hash = s->hash;
                uint16_t old_dist = s->probe_dist;
                uint16_t old_klen = s->key_len;
                uint16_t old_vlen = s->val_len;
                uint32_t old_offset = s->offset;

                void *old_key_copy = alloca(old_klen);
                void *old_val_copy = alloca(old_vlen);
                memcpy(old_key_copy, t->data_arena + old_offset, old_klen);
                memcpy(old_val_copy, t->data_arena + old_offset + old_klen, old_vlen);

                if (!place_entry(t, s, h32, dist, cur_key, cur_klen, cur_val, cur_vlen))
                    return false;

                h32 = old_hash;
                dist = old_dist + 1;
                cur_key = old_key_copy;
                cur_val = old_val_copy;
                cur_klen = old_klen;
                cur_vlen = old_vlen;
                idx = (idx + 1) & cap_mask;
                continue;
            }

            idx = (idx + 1) & cap_mask;
            dist++;

            if (dist > t->capacity) {
                if (!resize_table(t)) return false;
                return rh_insert(t, h32, cur_key, cur_klen, cur_val, cur_vlen);
            }
        }
    }
}

// ============================================================================
// Zombie Interval Rebuild
// ============================================================================

// Delete compaction: replaces the old capped_backward_shift with a three-tier
// strategy combining backward shift and ZombieHTDelete-style push-forward.
//
// After the caller places a tombstone at `idx`, this function tries:
//
//   Outcome A (best):  Full backward shift — tombstone eliminated entirely.
//       Requires the chain to end at EMPTY within the scan limit and that no
//       entry would shift past its ideal position.
//
//   Outcome B (good):  Push-forward to primitive position — tombstone placed
//       at a useful prophylactic-spacing slot.  Shifts entries from idx+1 up
//       to the primitive target backward, absorbing delete-tombstones along
//       the way.  The primitive target must be occupied by a live entry.
//
//   Outcome C (fallback): Tombstone stays at idx.
//
// Scan limit is derived from the prophylactic spacing (c_p * x * 2), bounded
// to [BSHIFT_CAP_MIN, BSHIFT_CAP_MAX].  This ensures we scan far enough to
// see at least one primitive position (pos % spacing == 0).

#define BSHIFT_CAP_MIN 4
#define BSHIFT_CAP_MAX 16

// Verify ideal-position safety for entries in the range [idx+1, idx+1+len).
// Returns true if all live entries can safely shift to their compacted positions.
static bool verify_ideal_safe(const ht_table_t *t, size_t idx, size_t len) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t pos = (idx + 1 + i) & cap_mask;
        const ht_slot_t *s = &t->slots[pos];
        if (!hash_is_live(s->hash)) continue;

        size_t target = (idx + write_offset) & cap_mask;
        size_t ideal = s->hash & cap_mask;
        if (ideal > target && (ideal - target) < t->capacity / 2) return false;
        write_offset++;
    }
    return true;
}

// Commit a full backward shift for entries in [idx+1, idx+1+len).
// Absorbs delete-tombstones, adjusts probe_dist, decrements tombstone_cnt.
static void commit_backward_shift(ht_table_t *t, size_t idx, size_t len,
                                   size_t del_tomb_count) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t read_pos = (idx + 1 + i) & cap_mask;
        ht_slot_t *s = &t->slots[read_pos];

        if (hash_is_live(s->hash)) {
            size_t write_pos = (idx + write_offset) & cap_mask;
            size_t shift = (i + 1) - write_offset;
            t->slots[write_pos] = *s;
            t->slots[write_pos].probe_dist -= (uint16_t)shift;
            write_offset++;
        } else {
            t->tombstone_cnt--;
        }
        t->slots[read_pos] = (ht_slot_t){0};
    }
    // Caller's tombstone at idx was overwritten by the first live entry.
    t->tombstone_cnt--;
    (void)del_tomb_count;
}

static void delete_compact(ht_table_t *t, size_t idx) {
    size_t cap_mask = t->capacity - 1;

    size_t spacing = (size_t)(compute_x(t) * t->c_p);
    if (spacing < 2) spacing = 2;
    size_t scan_limit = spacing * 2;
    if (scan_limit < BSHIFT_CAP_MIN) scan_limit = BSHIFT_CAP_MIN;
    if (scan_limit > BSHIFT_CAP_MAX) scan_limit = BSHIFT_CAP_MAX;

    // Phase 1: Forward scan from idx+1
    size_t live_count = 0, del_tomb_count = 0, chain_len = 0;
    bool ends_at_empty = false;
    size_t prim_target = SIZE_MAX;
    size_t prim_offset = SIZE_MAX;

    size_t scan = (idx + 1) & cap_mask;
    while (chain_len < scan_limit) {
        ht_slot_t *s = &t->slots[scan];

        if (slot_empty(s)) { ends_at_empty = true; break; }
        if (slot_is_tomb(s) && s->key_len == 0) break;  // prophylactic barrier
        if (slot_is_tomb(s)) {
            // Delete-tombstone: absorb during shift, not a push target
            del_tomb_count++;
            chain_len++;
            scan = (scan + 1) & cap_mask;
            continue;
        }
        if (s->probe_dist == 0) break;  // chain boundary

        // Live entry
        if (prim_target == SIZE_MAX && scan % spacing == 0) {
            prim_target = scan;
            prim_offset = chain_len;
        }
        live_count++;
        chain_len++;
        scan = (scan + 1) & cap_mask;
    }

    // Outcome A: Full backward shift — eliminate tombstone
    if (ends_at_empty && live_count > 0 &&
        verify_ideal_safe(t, idx, chain_len)) {
        commit_backward_shift(t, idx, chain_len, del_tomb_count);
        return;
    }

    // Outcome B: Push-forward to primitive position
    // Simple swap: move the live entry at prim_target to idx (filling the
    // gap left by the deleted entry), then place a prophylactic tombstone at
    // prim_target.  Intermediate entries (live and del-tombs) between idx and
    // prim_target are NOT shifted — they maintain chain continuity for entries
    // after prim_target.
    if (prim_target != SIZE_MAX) {
        // Verify the entry at prim_target can move to idx (ideal-position check)
        ht_slot_t *target_s = &t->slots[prim_target];
        if (hash_is_live(target_s->hash)) {
            size_t ideal = target_s->hash & cap_mask;
            size_t target = idx;
            bool safe = !(ideal > target && (ideal - target) < t->capacity / 2);

            if (safe) {
                size_t shift = prim_offset + 1;
                // Move entry from prim_target to idx
                t->slots[idx] = *target_s;
                t->slots[idx].probe_dist -= (uint16_t)shift;
                // Place prophylactic tombstone at prim_target
                t->slots[prim_target] = (ht_slot_t){
                    .hash = HASH_TOMB,
                    .probe_dist = 0,
                    .key_len = 0,
                    .val_len = 0,
                    .offset = 0
                };
                // Caller's delete-tombstone at idx was replaced by the live entry
                // from prim_target, and a new prophylactic tombstone was placed at
                // prim_target.  Net tombstone_cnt change: -1 + 1 = 0.
                return;
            }
        }
    }

    // Outcome C: Fallback — tombstone stays at idx
}

// Full backward-shift: used during rebuild (resize/compact) where cost
// doesn't matter since the entire table is being rebuilt anyway.
static void full_backward_shift(ht_table_t *t, size_t idx) {
    size_t cap_mask = t->capacity - 1;
    while (1) {
        size_t next = (idx + 1) & cap_mask;
        ht_slot_t *s_next = &t->slots[next];

        if (slot_empty(s_next) || slot_is_tomb(s_next) || s_next->probe_dist == 0)
            break;

        t->slots[idx] = *s_next;
        t->slots[idx].probe_dist--;
        idx = next;
    }
    t->slots[idx] = (ht_slot_t){0};
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
        ht_slot_t *s = &t->slots[idx];

        double prim_spacing = x * C_P_DEFAULT;
        size_t spacing = (size_t)prim_spacing;
        if (spacing < 2) spacing = 2;
        bool is_prim = (idx % spacing == 0);

        if (slot_is_tomb(s)) {
            // Non-primitive tombstone: skip (don't clear).
        } else if (is_prim && slot_empty(s) && t->size > 0) {
            // Don't place primitive tombstones during zombie scan.
            // Placing tombstones at EMPTY positions in the middle of probe
            // chains can make previously-unreachable entries reachable,
            // breaking the consistency of insert/update/remove operations.
            // Prophylactic tombstones are placed during rebuild (compact/resize).
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
    t->slots = calloc(t->capacity, sizeof(ht_slot_t));
    if (!t->slots) { free(t); return NULL; }

    t->spill_cap = SPILL_INITIAL;
    t->spill = calloc(t->spill_cap, sizeof(ht_slot_t));
    if (!t->spill) { free(t->slots); free(t); return NULL; }

    t->data_arena = malloc(1024);
    t->data_cap = 1024;
    if (!t->data_arena) { free(t->spill); free(t->slots); free(t); return NULL; }

    t->hash_fn = hash_fn;
    t->eq_fn = eq_fn;
    t->user_ctx = user_ctx;
    t->max_load_factor = (c.max_load_factor > 0) ? c.max_load_factor : 0.75;
    t->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    t->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    t->zombie_window = c.zombie_window;
    t->c_p = C_P_DEFAULT;

    return t;
}

void ht_destroy(ht_table_t *t) {
    if (!t) return;
    free(t->spill);
    free(t->slots);
    free(t->data_arena);
    free(t);
}

void ht_clear(ht_table_t *t) {
    if (!t) return;
    memset(t->slots, 0, t->capacity * sizeof(ht_slot_t));
    memset(t->spill, 0, t->spill_cap * sizeof(ht_slot_t));
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_len = 0;
    t->data_size = 0;
    t->zombie_cursor = 0;
}

// ============================================================================
// Insert
// ============================================================================

bool ht_insert(ht_table_t *t, const void *key, size_t key_len,
               const void *value, size_t value_len) {
    if (!t || !key) return false;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_insert_with_hash(t, hash, key, key_len, value, value_len);
}

bool ht_insert_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    if (!t || !key) return false;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane: hashes 0 or 1 collide with sentinels
    if (!hash_is_live(h32)) {
        return spill_insert(t, h32, key, key_len, value, value_len);
    }

    if (!t->resizing && (double)(t->size + 1) / t->capacity > t->max_load_factor) {
        ht_resize(t, t->capacity * 2);
    }

    double total = t->size + t->tombstone_cnt;
    if (total > 0 && (double)t->tombstone_cnt / total > t->tomb_threshold) {
        for (int i = 0; i < 4; i++) zombie_step(t);
    }

    bool result = rh_insert(t, h32, key, key_len, value, value_len);

    if (result) {
        zombie_step(t);
        t->rebuild_age++;
    }

    return result;
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

    uint32_t h32 = (uint32_t)hash;

    // Spill lane
    if (!hash_is_live(h32)) {
        return spill_find(t, h32, key, key_len, out_value_len);
    }

    // Main table: Robin-Hood probe
    size_t cap_mask = t->capacity - 1;
    size_t idx = h32 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        ht_slot_t *s = &t->slots[idx];

        if (slot_empty(s)) return NULL;

        if (slot_is_tomb(s)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        // NOTE: We do NOT use Robin-Hood early termination (probe_dist < dist)
        // because tombstone-based deletion can break the probe-chain invariant.
        // A newly inserted entry at dist=0 can appear in the middle of another
        // entry's probe chain, making early termination incorrect.

        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            if (out_value_len) *out_value_len = s->val_len;
            return slot_val_ptr(t, s);
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return NULL;
}

void ht_find_all(const ht_table_t *t, uint64_t hash,
                 ht_dup_callback cb, void *user_ctx) {
    if (!t || !cb) return;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane first
    if (!hash_is_live(h32)) {
        spill_find_all(t, h32, cb, user_ctx);
        return;
    }

    // Main table
    size_t cap_mask = t->capacity - 1;
    size_t idx = h32 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        ht_slot_t *s = &t->slots[idx];

        if (slot_empty(s)) return;

        if (slot_is_tomb(s)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        // No early termination: tombstones break Robin-Hood probe-chain invariant

        if (s->hash == h32) {
            if (!cb(slot_key_ptr(t, s), s->key_len,
                    slot_val_ptr(t, s), s->val_len, user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }
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
    ht_insert(t, key, key_len, &new_val, sizeof(new_val));
    return new_val;
}

// ============================================================================
// Delete
// ============================================================================

bool ht_remove(ht_table_t *t, const void *key, size_t key_len) {
    if (!t || !key) return false;
    uint64_t hash = t->hash_fn(key, key_len, t->user_ctx);
    return ht_remove_with_hash(t, hash, key, key_len);
}

bool ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                         const void *key, size_t key_len) {
    if (!t || !key) return false;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane
    if (!hash_is_live(h32)) {
        return spill_remove(t, h32, key, key_len);
    }

    // Main table
    size_t cap_mask = t->capacity - 1;
    size_t idx = h32 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        ht_slot_t *s = &t->slots[idx];

        if (slot_empty(s)) return false;

        if (slot_is_tomb(s)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        // No early termination: tombstones break Robin-Hood probe-chain invariant

        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            t->size--;
            s->hash = HASH_TOMB;
            t->tombstone_cnt++;
            // Try cache-line-local compaction: backward-shift or push-forward
            // to eliminate or relocate the tombstone.
            delete_compact(t, idx);

            if (t->min_load_factor > 0 && t->size > 0 &&
                (double)t->size / t->capacity < t->min_load_factor &&
                t->capacity > 64) {
                size_t new_cap = t->capacity / 2;
                if (new_cap >= 64 && new_cap >= t->size * 2) {
                    ht_resize(t, new_cap);
                }
            }

            return true;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    return false;
}

// ============================================================================
// Resize
// ============================================================================

static bool resize_table(ht_table_t *t) {
    return ht_resize(t, t->capacity * 2);
}

// Helper: reinsert all live entries from a slot array into the current table.
// Used by both resize and compact.
static void reinsert_live(ht_table_t *t, ht_slot_t *old, size_t old_cap,
                          uint8_t *old_arena) {
    for (size_t i = 0; i < old_cap; i++) {
        ht_slot_t *s = &old[i];
        if (hash_is_live(s->hash) && s->key_len > 0) {
            const void *k = old_arena + s->offset;
            const void *v = old_arena + s->offset + s->key_len;
            rh_insert(t, s->hash, k, s->key_len, v, s->val_len);
        }
    }
}

// Helper: place prophylactic tombstones after rebuild.
static void place_prophylactic_tombstones(ht_table_t *t) {
    double x = compute_x(t);
    size_t spacing = (size_t)(x * C_P_DEFAULT);
    if (spacing < 4) spacing = 4;

    for (size_t pos = 0; pos < t->capacity; pos += spacing) {
        ht_slot_t *s = &t->slots[pos];
        if (slot_empty(s)) {
            s->hash = HASH_TOMB;
            s->probe_dist = 0;
            s->key_len = 0;
            s->val_len = 0;
            s->offset = 0;
            t->tombstone_cnt++;
        }
    }
}

// Helper: save spill-lane entries to stack, then reinsert into table.
// Called after the old arena is still valid but the table has been reset.
// spill entries reference offsets into old_arena which is about to be freed.
static void reinsert_spill(ht_table_t *t, ht_slot_t *old_spill, size_t old_spill_len,
                           uint8_t *old_arena) {
    for (size_t i = 0; i < old_spill_len; i++) {
        ht_slot_t *s = &old_spill[i];
        if (s->key_len > 0) {
            const void *k = old_arena + s->offset;
            const void *v = old_arena + s->offset + s->key_len;
            spill_insert(t, s->hash, k, s->key_len, v, s->val_len);
        }
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
    ht_slot_t *old_slots = t->slots;
    size_t old_cap = t->capacity;
    uint8_t *old_arena = t->data_arena;
    ht_slot_t *old_spill = t->spill;
    size_t old_spill_len = t->spill_len;

    // Allocate new main table
    ht_slot_t *new_slots = calloc(new_capacity, sizeof(ht_slot_t));
    if (!new_slots) { t->resizing = false; return false; }

    uint8_t *new_arena = malloc(t->data_cap > 0 ? t->data_cap : 1024);
    if (!new_arena) {
        free(new_slots);
        t->resizing = false;
        return false;
    }

    // Allocate fresh spill lane
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    ht_slot_t *new_spill = calloc(new_spill_cap, sizeof(ht_slot_t));
    if (!new_spill) {
        free(new_arena);
        free(new_slots);
        t->resizing = false;
        return false;
    }

    // Swap to new state
    t->slots = new_slots;
    t->capacity = new_capacity;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->data_arena = new_arena;
    t->data_size = 0;
    t->data_cap = t->data_cap > 0 ? t->data_cap : 1024;
    t->spill = new_spill;
    t->spill_cap = new_spill_cap;
    t->spill_len = 0;
    t->zombie_cursor = 0;
    t->rebuild_age = 0;

    reinsert_live(t, old_slots, old_cap, old_arena);
    reinsert_spill(t, old_spill, old_spill_len, old_arena);
    place_prophylactic_tombstones(t);

    free(old_slots);
    free(old_arena);
    free(old_spill);
    t->resizing = false;
    return true;
}

void ht_compact(ht_table_t *t) {
    if (!t) return;

    // Save spill-lane entries
    ht_slot_t *old_spill = t->spill;
    size_t old_spill_len = t->spill_len;

    t->spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    t->spill = calloc(t->spill_cap, sizeof(ht_slot_t));
    if (!t->spill) {
        t->spill = old_spill;
        return;
    }
    t->spill_len = 0;

    ht_slot_t *old_slots = t->slots;
    size_t old_cap = t->capacity;
    uint8_t *old_arena = t->data_arena;
    size_t old_data_cap = t->data_cap;

    t->slots = calloc(old_cap, sizeof(ht_slot_t));
    t->data_arena = malloc(old_data_cap > 0 ? old_data_cap : 1024);
    t->data_size = 0;
    t->data_cap = old_data_cap > 0 ? old_data_cap : 1024;
    t->size = 0;
    t->tombstone_cnt = 0;
    t->zombie_cursor = 0;
    t->rebuild_age = 0;

    reinsert_live(t, old_slots, old_cap, old_arena);
    reinsert_spill(t, old_spill, old_spill_len, old_arena);
    place_prophylactic_tombstones(t);

    free(old_slots);
    free(old_arena);
    free(old_spill);
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
        ht_slot_t *s = &t->slots[iter->idx++];
        if (hash_is_live(s->hash) && s->key_len > 0) {
            if (out_key) *out_key = slot_key_ptr(t, s);
            if (out_key_len) *out_key_len = s->key_len;
            if (out_value) *out_value = slot_val_ptr(t, s);
            if (out_value_len) *out_value_len = s->val_len;
            return true;
        }
    }

    // Then iterate spill lane (encode idx as capacity + spill index)
    size_t spill_idx = iter->idx - t->capacity;
    while (spill_idx < t->spill_len) {
        ht_slot_t *s = &t->spill[spill_idx++];
        iter->idx = t->capacity + spill_idx;  // update so next call resumes
        if (s->key_len > 0) {
            if (out_key) *out_key = slot_key_ptr(t, s);
            if (out_key_len) *out_key_len = s->key_len;
            if (out_value) *out_value = slot_val_ptr(t, s);
            if (out_value_len) *out_value_len = s->val_len;
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
        ht_slot_t *s = &t->slots[idx];
        const char *tag = slot_empty(s) ? "EMPTY" : slot_is_tomb(s) ? "TOMB" : "LIVE";
        printf("  [%4zu]: hash=0x%08x dist=%3u [%s] klen=%3u vlen=%3u off=%5u\n",
               idx, s->hash, s->probe_dist, tag,
               s->key_len, s->val_len, s->offset);
    }
    // Also dump spill lane
    if (t->spill_len > 0) {
        printf("  Spill lane (%zu entries):\n", t->spill_len);
        for (size_t i = 0; i < t->spill_len; i++) {
            ht_slot_t *s = &t->spill[i];
            printf("  spill[%zu]: hash=0x%08x klen=%3u vlen=%3u off=%5u\n",
                   i, s->hash, s->key_len, s->val_len, s->offset);
        }
    }
}
