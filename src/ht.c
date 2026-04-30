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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Slot Layout
// ============================================================================

typedef struct {
    uint32_t hash;        // HASH_EMPTY, HASH_TOMB, or live hash (>= 2)
    uint16_t probe_dist;  // Distance from ideal position
    uint32_t key_len;
    uint32_t val_len;
    uint64_t offset;      // Byte offset into data arena
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

// Insert modes (internal)
#define INS_UPSERT  0   // remove all for key, insert/update single value
#define INS_ALWAYS  1   // always insert new entry (multi-value)
#define INS_UNIQUE  2   // insert only if exact k,v pair not found

static inline bool vals_match(const ht_table_t *t, const ht_slot_t *s,
                              const void *val, size_t val_len) {
    if (s->val_len != val_len) return false;
    return memcmp(slot_val_ptr(t, s), val, val_len) == 0;
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
    slot->key_len = (uint32_t)key_len;
    slot->val_len = (uint32_t)value_len;
    slot->offset = (uint64_t)((uint8_t *)data - t->data_arena);
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

// Insert into spill lane with mode support.
// Returns true if new entry inserted, false if updated (UPSERT) or duplicate (UNIQUE).
static bool spill_insert_ex(ht_table_t *t, uint32_t h32,
                             const void *key, size_t key_len,
                             const void *value, size_t value_len,
                             int mode) {
    if (mode == INS_ALWAYS) {
        // Skip scan — always append
    } else if (mode == INS_UPSERT) {
        bool found = false;
        for (size_t i = 0; i < t->spill_len; ) {
            ht_slot_t *s = &t->spill[i];
            if (s->hash == h32 && keys_match(t, s, key, key_len)) {
                if (!found) {
                    // Update first match in-place
                    if (!grow_arena(t, key_len + value_len)) return false;
                    void *data = arena_alloc(t, key_len + value_len);
                    if (!data) return false;
                    memcpy(data, key, key_len);
                    memcpy((uint8_t *)data + key_len, value, value_len);
                    s->val_len = (uint32_t)value_len;
                    s->offset = (uint64_t)((uint8_t *)data - t->data_arena);
                    found = true;
                    i++;
                } else {
                    // Remove additional matches
                    memmove(&t->spill[i], &t->spill[i + 1],
                            (t->spill_len - i - 1) * sizeof(ht_slot_t));
                    t->spill_len--;
                    t->spill[t->spill_len] = (ht_slot_t){0};
                    t->size--;
                }
            } else {
                i++;
            }
        }
        if (found) return false;
    } else { // INS_UNIQUE
        for (size_t i = 0; i < t->spill_len; i++) {
            ht_slot_t *s = &t->spill[i];
            if (s->hash == h32 && keys_match(t, s, key, key_len) &&
                vals_match(t, s, value, value_len))
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

// Legacy wrapper for reinsert_spill (always behaves as UPSERT during rebuilds)
static bool spill_insert(ht_table_t *t, uint32_t h32,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len) {
    return spill_insert_ex(t, h32, key, key_len, value, value_len, INS_UPSERT);
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

static bool rh_insert_ex(ht_table_t *t, uint32_t h32,
                         const void *key, size_t key_len,
                         const void *value, size_t value_len,
                         int mode) {
    size_t cap_mask = t->capacity - 1;
    size_t ideal = h32 & cap_mask;

    // Phase 1: Scan probe chain.
    // INS_ALWAYS: skip Phase 1 entirely — go to Phase 2.
    // INS_UPSERT: on first key match, update in-place; tombstone additional matches.
    // INS_UNIQUE: if exact k,v pair found, return false; otherwise fall through.
    if (mode != INS_ALWAYS) {
        size_t idx = ideal;
        uint16_t dist = 0;
        bool upsert_updated = false;
        for (size_t steps = 0; steps <= t->capacity; steps++) {
            ht_slot_t *s = &t->slots[idx];

            if (slot_empty(s)) break;

            if (slot_is_tomb(s)) {
                idx = (idx + 1) & cap_mask;
                dist++;
                continue;
            }

            if (s->probe_dist < dist) break;

            if (s->hash == h32 && keys_match(t, s, key, key_len)) {
                if (mode == INS_UNIQUE) {
                    if (vals_match(t, s, value, value_len))
                        return false; // exact k,v exists
                    // Different value — keep scanning
                } else { // INS_UPSERT
                    if (!upsert_updated) {
                        // Update first match in-place
                        if (!grow_arena(t, key_len + value_len)) return false;
                        void *data = arena_alloc(t, key_len + value_len);
                        if (!data) return false;
                        memcpy(data, key, key_len);
                        memcpy((uint8_t *)data + key_len, value, value_len);
                        s->val_len = (uint32_t)value_len;
                        s->offset = (uint64_t)((uint8_t *)data - t->data_arena);
                        upsert_updated = true;
                    } else {
                        // Tombstone additional matches
                        s->hash = HASH_TOMB;
                        s->key_len = 0;
                        s->val_len = 0;
                        s->offset = 0;
                        t->tombstone_cnt++;
                        t->size--;
                    }
                }
            }

            idx = (idx + 1) & cap_mask;
            dist++;
        }

        if (upsert_updated) return false; // updated existing, no new entry
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
                // Stranding prevention: placing at a tombstone at position p
                // with dist d blocks searches for any entry at p+k whose
                // probe_dist > d+k (their ideal is before p, and early
                // termination at p stops the search).  Check nearby entries.
                if (slot_is_tomb(s)) {
                    bool blocked = false;
                    for (size_t k = 1; k <= BSHIFT_CAP; k++) {
                        size_t chk = (idx + k) & cap_mask;
                        ht_slot_t *sc = &t->slots[chk];
                        if (slot_empty(sc)) break;
                        if (slot_is_tomb(sc)) continue;
                        if (sc->probe_dist > dist + (uint16_t)k) {
                            blocked = true;
                            break;
                        }
                    }
                    if (blocked) {
                        // Skip tombstone — treat as unavailable
                        idx = (idx + 1) & cap_mask;
                        dist++;
                        continue;
                    }
                    t->tombstone_cnt--;
                }
                if (!place_entry(t, s, h32, dist, cur_key, cur_klen, cur_val, cur_vlen))
                    return false;
                t->size++;
                return true;
            }

            // Robin-Hood swap: occupant has shorter probe distance
            if (s->probe_dist < dist) {
                uint32_t old_hash = s->hash;
                uint16_t old_dist = s->probe_dist;
                uint32_t old_klen = s->key_len;
                uint32_t old_vlen = s->val_len;
                uint64_t old_offset = s->offset;

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
                return rh_insert_ex(t, h32, cur_key, cur_klen, cur_val, cur_vlen, mode);
            }
        }
    }
}

// Legacy wrapper for reinsert_live (always behaves as UPSERT during rebuilds)
static bool rh_insert(ht_table_t *t, uint32_t h32,
                      const void *key, size_t key_len,
                      const void *value, size_t value_len) {
    return rh_insert_ex(t, h32, key, key_len, value, value_len, INS_UPSERT);
}

// ============================================================================
// Zombie Interval Rebuild
// ============================================================================

// Delete compaction: capped backward shift after deleting an entry at `idx`.
//
// Scans forward at most BSHIFT_CAP entries, collecting tombstones and live
// entries until EMPTY or probe_dist==0.  If the chain ends within the cap
// and verify_ideal_safe passes, shifts all collected entries backward to
// eliminate the tombstone.
//
// Cost: O(BSHIFT_CAP) = O(1) worst case per delete.
//
// Stranding prevention: if the chain extends past the cap, the tombstone
// stays.  The insert path (rh_insert Phase 2) prevents new entries from
// placing at the tombstone position when it would block entries past it
// (the "dist==0 wall" check).

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
// Absorbs tombstones, adjusts probe_dist, decrements tombstone_cnt.
static void commit_backward_shift(ht_table_t *t, size_t idx, size_t len) {
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
            // Absorb tombstone (delete or prophylactic)
            t->tombstone_cnt--;
        }
        t->slots[read_pos] = (ht_slot_t){0};
    }
    // Caller's tombstone at idx was overwritten by the first live entry.
    t->tombstone_cnt--;
}

static void delete_compact(ht_table_t *t, size_t idx) {
    size_t cap_mask = t->capacity - 1;

    // Capped scan: at most BSHIFT_CAP entries.
    size_t chain_len = 0;
    size_t live_count = 0;
    bool ends_at_empty = false;

    size_t scan = (idx + 1) & cap_mask;
    for (size_t steps = 0; steps < BSHIFT_CAP; steps++) {
        ht_slot_t *s = &t->slots[scan];

        if (slot_empty(s)) { ends_at_empty = true; break; }
        if (slot_is_tomb(s)) {
            chain_len++;
            scan = (scan + 1) & cap_mask;
            continue;
        }
        if (s->probe_dist == 0) break;  // chain boundary
        live_count++;
        chain_len++;
        scan = (scan + 1) & cap_mask;
    }

    // Only compact if the chain ends cleanly within our scan window.
    if (ends_at_empty && live_count > 0 &&
        verify_ideal_safe(t, idx, chain_len)) {
        commit_backward_shift(t, idx, chain_len);
    }
    // Otherwise: tombstone stays at idx.  The insert path prevents
    // new entries from creating dist==0 walls that would block entries
    // past the tombstone.
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
    t->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
                         (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
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
// Insert / Upsert / Unsert
// ============================================================================

static bool do_insert_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len,
                                int mode) {
    if (!t || !key) return false;
    if (!value && value_len > 0) value_len = 0;

    uint32_t h32 = (uint32_t)hash;

    if (!hash_is_live(h32))
        return spill_insert_ex(t, h32, key, key_len, value, value_len, mode);

    if (!t->resizing && (double)(t->size + 1) / t->capacity > t->max_load_factor)
        ht_resize(t, t->capacity * 2);

    double total = t->size + t->tombstone_cnt;
    if (total > 0 && (double)t->tombstone_cnt / total > t->tomb_threshold) {
        for (int i = 0; i < 4; i++) zombie_step(t);
    }

    bool result = rh_insert_ex(t, h32, key, key_len, value, value_len, mode);

    if (result) {
        zombie_step(t);
        t->rebuild_age++;
    }

    return result;
}

// ht_insert: always-add (multi-value)
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

// ht_upsert: remove-all + insert single
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

// ht_unsert: insert only if exact k,v pair doesn't exist
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

        if (s->probe_dist < dist) return NULL;

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

        if (s->probe_dist < dist) return;

        if (s->hash == h32) {
            if (!cb(slot_key_ptr(t, s), s->key_len,
                    slot_val_ptr(t, s), s->val_len, user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }
}

// ht_find_key_all: iterate all entries matching exact key (not just hash).
void ht_find_key_all_with_hash(const ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               ht_dup_callback cb, void *user_ctx) {
    if (!t || !key || !cb) return;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            if (!cb(slot_key_ptr(t, s), s->key_len,
                    slot_val_ptr(t, s), s->val_len, user_ctx))
                return;
        }
    }

    // Main table
    if (!hash_is_live(h32)) return;

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

        if (s->probe_dist < dist) return;

        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            if (!cb(slot_key_ptr(t, s), s->key_len,
                    slot_val_ptr(t, s), s->val_len, user_ctx))
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

// ht_find_kv: find first entry matching exact key AND value.
const void *ht_find_kv_with_hash(const ht_table_t *t, uint64_t hash,
                                 const void *key, size_t key_len,
                                 const void *value, size_t value_len,
                                 size_t *out_value_len) {
    if (!t || !key || !value) return NULL;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane
    for (size_t i = 0; i < t->spill_len; i++) {
        ht_slot_t *s = &t->spill[i];
        if (s->hash == h32 && keys_match(t, s, key, key_len) &&
            vals_match(t, s, value, value_len)) {
            if (out_value_len) *out_value_len = s->val_len;
            return slot_val_ptr(t, s);
        }
    }

    // Main table
    if (!hash_is_live(h32)) return NULL;

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

        if (s->probe_dist < dist) return NULL;

        if (s->hash == h32 && keys_match(t, s, key, key_len) &&
            vals_match(t, s, value, value_len)) {
            if (out_value_len) *out_value_len = s->val_len;
            return slot_val_ptr(t, s);
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

// ht_remove: remove ALL entries for key, return count removed.
size_t ht_remove_with_hash(ht_table_t *t, uint64_t hash,
                            const void *key, size_t key_len) {
    if (!t || !key) return 0;

    uint32_t h32 = (uint32_t)hash;
    size_t removed = 0;

    // Spill lane
    if (!hash_is_live(h32)) {
        for (size_t i = 0; i < t->spill_len; ) {
            ht_slot_t *s = &t->spill[i];
            if (s->hash == h32 && keys_match(t, s, key, key_len)) {
                memmove(&t->spill[i], &t->spill[i + 1],
                        (t->spill_len - i - 1) * sizeof(ht_slot_t));
                t->spill_len--;
                t->spill[t->spill_len] = (ht_slot_t){0};
                t->size--;
                removed++;
            } else {
                i++;
            }
        }
        return removed;
    }

    // Main table: walk full probe chain, tombstone ALL matches
    size_t cap_mask = t->capacity - 1;
    size_t idx = h32 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        ht_slot_t *s = &t->slots[idx];

        if (slot_empty(s)) break;

        if (slot_is_tomb(s)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (s->probe_dist < dist) break;

        if (s->hash == h32 && keys_match(t, s, key, key_len)) {
            t->size--;
            s->hash = HASH_TOMB;
            s->key_len = 0;
            s->val_len = 0;
            s->offset = 0;
            t->tombstone_cnt++;
            removed++;
            delete_compact(t, idx);
            // After compact, re-scan from same position
            // (compact may have shifted entries)
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

// ht_remove_kv: remove ALL entries matching both key and value, return count.
size_t ht_remove_kv_with_hash(ht_table_t *t, uint64_t hash,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len) {
    if (!t || !key || !value) return 0;

    uint32_t h32 = (uint32_t)hash;
    size_t removed = 0;

    // Spill lane
    if (!hash_is_live(h32)) {
        for (size_t i = 0; i < t->spill_len; ) {
            ht_slot_t *s = &t->spill[i];
            if (s->hash == h32 && keys_match(t, s, key, key_len) &&
                vals_match(t, s, value, value_len)) {
                memmove(&t->spill[i], &t->spill[i + 1],
                        (t->spill_len - i - 1) * sizeof(ht_slot_t));
                t->spill_len--;
                t->spill[t->spill_len] = (ht_slot_t){0};
                t->size--;
                removed++;
            } else {
                i++;
            }
        }
        return removed;
    }

    // Main table
    size_t cap_mask = t->capacity - 1;
    size_t idx = h32 & cap_mask;
    uint16_t dist = 0;

    for (size_t steps = 0; steps <= t->capacity; steps++) {
        ht_slot_t *s = &t->slots[idx];

        if (slot_empty(s)) break;

        if (slot_is_tomb(s)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (s->probe_dist < dist) break;

        if (s->hash == h32 && keys_match(t, s, key, key_len) &&
            vals_match(t, s, value, value_len)) {
            t->size--;
            s->hash = HASH_TOMB;
            s->key_len = 0;
            s->val_len = 0;
            s->offset = 0;
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

// ht_remove_kv_one: remove FIRST entry matching both key and value.
bool ht_remove_kv_one_with_hash(ht_table_t *t, uint64_t hash,
                                const void *key, size_t key_len,
                                const void *value, size_t value_len) {
    if (!t || !key || !value) return false;

    uint32_t h32 = (uint32_t)hash;

    // Spill lane
    if (!hash_is_live(h32)) {
        for (size_t i = 0; i < t->spill_len; i++) {
            ht_slot_t *s = &t->spill[i];
            if (s->hash == h32 && keys_match(t, s, key, key_len) &&
                vals_match(t, s, value, value_len)) {
                memmove(&t->spill[i], &t->spill[i + 1],
                        (t->spill_len - i - 1) * sizeof(ht_slot_t));
                t->spill_len--;
                t->spill[t->spill_len] = (ht_slot_t){0};
                t->size--;
                return true;
            }
        }
        return false;
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

        if (s->probe_dist < dist) return false;

        if (s->hash == h32 && keys_match(t, s, key, key_len) &&
            vals_match(t, s, value, value_len)) {
            t->size--;
            s->hash = HASH_TOMB;
            s->key_len = 0;
            s->val_len = 0;
            s->offset = 0;
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
        printf("  [%4zu]: hash=0x%08x dist=%3u [%s] klen=%3u vlen=%3u off=%5" PRIu64 "\n",
               idx, s->hash, s->probe_dist, tag,
               s->key_len, s->val_len, s->offset);
    }
    // Also dump spill lane
    if (t->spill_len > 0) {
        printf("  Spill lane (%zu entries):\n", t->spill_len);
        for (size_t i = 0; i < t->spill_len; i++) {
            ht_slot_t *s = &t->spill[i];
            printf("  spill[%zu]: hash=0x%08x klen=%3u vlen=%3u off=%5" PRIu64 "\n",
                   i, s->hash, s->key_len, s->val_len, s->offset);
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
        const ht_slot_t *s = &t->slots[i];
        if (slot_empty(s)) continue;
        if (slot_is_tomb(s)) {
            tomb_count++;
            continue;
        }
        live_count++;

        size_t ideal = s->hash & cap_mask;
        size_t expected_dist = (i >= ideal) ? (i - ideal) : (t->capacity - ideal + i);
        if (s->probe_dist != expected_dist) {
            static char buf[256];
            snprintf(buf, sizeof(buf),
                     "slot[%zu]: probe_dist=%u but expected %zu (hash=0x%x ideal=%zu)",
                     i, s->probe_dist, expected_dist, s->hash, ideal);
            return buf;
        }
    }

    // Count spill lane
    for (size_t i = 0; i < t->spill_len; i++) {
        if (t->spill[i].key_len > 0) spill_live++;
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
    // For every live entry at position p, verify that searching from its
    // ideal position would NOT be stopped by early termination before
    // reaching p.
    {
        for (size_t i = 0; i < t->capacity; i++) {
            const ht_slot_t *s = &t->slots[i];
            if (!hash_is_live(s->hash)) continue;

            size_t ideal = s->hash & cap_mask;
            uint16_t dist = 0;
            for (size_t steps = 0; steps <= t->capacity; steps++) {
                size_t pos = (ideal + dist) & cap_mask;
                if (pos == i) break; // reached our entry — OK

                const ht_slot_t *scan = &t->slots[pos];
                if (slot_empty(scan)) {
                    static char buf[256];
                    snprintf(buf, sizeof(buf),
                             "slot[%zu] (hash=0x%x ideal=%zu dist=%u) unreachable: "
                             "hit EMPTY at [%zu] while probing from ideal",
                             i, s->hash, ideal, s->probe_dist, pos);
                    return buf;
                }
                if (slot_is_tomb(scan)) {
                    dist++;
                    continue;
                }
                if (scan->probe_dist < dist) {
                    static char buf[256];
                    snprintf(buf, sizeof(buf),
                             "slot[%zu] (hash=0x%x ideal=%zu dist=%u) unreachable: "
                             "early termination at [%zu] (dist=%u < %u)",
                             i, s->hash, ideal, s->probe_dist,
                             pos, scan->probe_dist, dist);
                    return buf;
                }
                dist++;
            }
        }
    }

    return NULL; // all good
}
