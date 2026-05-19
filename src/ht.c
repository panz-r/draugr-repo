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

#include "draugr/ht_internal.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Constants
// ============================================================================

static const ht_config_t default_cfg = {
    .initial_capacity = 64,
    .max_load_factor = 0.75,
    .min_load_factor = 0.20,
    .tomb_threshold = 0.20,
    .zombie_window = 16,
    .max_probe_dist = 255,
};

#define INS_UPSERT 0
#define INS_ALWAYS 1
#define INS_UNIQUE 2

static size_t bare_main_block_size(size_t capacity) {
    size_t elem = sizeof(uint64_t) + sizeof(uint32_t);
    if (capacity > SIZE_MAX / elem) return 0;
    return capacity * elem;
}

static void bare_main_block_init(ht_bare_t *b, uint8_t *block, size_t capacity) {
    b->main_block = block;
    b->hash_pd = (uint64_t *)block;
    b->vals = (uint32_t *)(block + capacity * sizeof(uint64_t));
    b->capacity = capacity;
}

static uint8_t *bare_alloc_main_block(size_t capacity) {
    size_t sz = bare_main_block_size(capacity);
    if (sz == 0) return NULL;
    uint8_t *block = calloc(1, sz);
    if (!block) return NULL;
    memset(block + capacity * sizeof(uint64_t), 0xFF, capacity * sizeof(uint32_t));
    return block;
}

// ============================================================================
// Bare Internal: Compute X
// ============================================================================

double bare_compute_x(const ht_bare_t *t) {
    double lf = (double)t->size / (double)t->capacity;
    if (lf >= 1.0) return (double)t->capacity;
    if (lf < 0.01) return 1.0;
    return 1.0 / (1.0 - lf);
}

// ============================================================================
// Bare Internal: Spill Lane
// ============================================================================

bool bare_spill_grow(ht_bare_t *t) {
    size_t old_cap = t->spill_cap;
    size_t new_cap = old_cap ? old_cap * 2 : SPILL_INITIAL;

    size_t new_size = new_cap * (sizeof(uint64_t) + sizeof(uint32_t));
    if (new_cap > 0 && new_size / new_cap != (sizeof(uint64_t) + sizeof(uint32_t)))
        return false;
    uint8_t *new_block = malloc(new_size);
    if (!new_block) return false;

  if (old_cap > 0) {
    uint64_t *old_hash_pd = (uint64_t*)t->spill_block;
    uint32_t *old_vals = (uint32_t*)(t->spill_block + old_cap * sizeof(uint64_t));

    memcpy(new_block, old_hash_pd, old_cap * sizeof(uint64_t));
    memset(new_block + old_cap * sizeof(uint64_t), 0, (new_cap - old_cap) * sizeof(uint64_t));

    uint32_t *new_vals = (uint32_t*)(new_block + new_cap * sizeof(uint64_t));
    memcpy(new_vals, old_vals, old_cap * sizeof(uint32_t));
    memset(new_vals + old_cap, 0xFF, (new_cap - old_cap) * sizeof(uint32_t));

    if (!t->spill_in_block)
      free(t->spill_block);
  } else {
        memset(new_block, 0, new_cap * sizeof(uint64_t));
        memset(new_block + new_cap * sizeof(uint64_t), 0xFF, new_cap * sizeof(uint32_t));
    }

  t->spill_block = new_block;
  t->spill_hash_pd = (uint64_t*)new_block;
  t->spill_vals = (uint32_t*)(new_block + new_cap * sizeof(uint64_t));
  t->spill_cap = new_cap;
  t->spill_in_block = false;
  return true;
}

bool bare_spill_insert(ht_bare_t *t, uint64_t h48, uint32_t val) {
    if (t->spill_len >= t->spill_cap) {
        if (!bare_spill_grow(t)) return false;
    }
    t->spill_hash_pd[t->spill_len] = hpd_pack(h48, 0);
    t->spill_vals[t->spill_len] = val;
    t->spill_len++;
    t->size++;
    return true;
}

bool bare_spill_find(const ht_bare_t *t, uint64_t h48, uint32_t *out_val) {
    for (size_t i = 0; i < t->spill_len; i++) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48) {
            if (out_val) *out_val = t->spill_vals[i];
            return true;
        }
    }
    return false;
}

void bare_spill_find_all(const ht_bare_t *t, uint64_t h48,
                         ht_bare_callback cb, void *user_ctx) {
    for (size_t i = 0; i < t->spill_len; i++) {
        if (hpd_hash(t->spill_hash_pd[i]) == h48) {
            if (!cb(t->spill_vals[i], user_ctx))
                return;
        }
    }
}

size_t bare_spill_remove(ht_bare_t *t, uint64_t h48) {
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

bool bare_spill_remove_val(ht_bare_t *t, uint64_t h48, uint32_t val) {
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

static bool bare_rh_insert_unbounded(ht_bare_t *t, uint64_t h48, uint32_t val);

static bool overflow_stash_insert(ht_bare_t *t, uint64_t h48, uint32_t val) {
 if (t->overflow_len >= HT_OVERFLOW_STASH_CAP) return false;
 t->overflow_hash_pd[t->overflow_len] = hpd_pack(h48, 0);
 t->overflow_vals[t->overflow_len] = val;
 t->overflow_len++;
 t->size++;
 return true;
}

static bool overflow_stash_remove_val(ht_bare_t *t, uint64_t h48, uint32_t val) {
 for (size_t i = 0; i < t->overflow_len; i++) {
  if (hpd_hash(t->overflow_hash_pd[i]) == h48 && t->overflow_vals[i] == val) {
   t->overflow_len--;
   t->overflow_hash_pd[i] = t->overflow_hash_pd[t->overflow_len];
   t->overflow_vals[i] = t->overflow_vals[t->overflow_len];
   t->size--;
   return true;
  }
 }
 return false;
}

static size_t overflow_stash_remove(ht_bare_t *t, uint64_t h48) {
 size_t w = 0;
 for (size_t i = 0; i < t->overflow_len; i++) {
  if (hpd_hash(t->overflow_hash_pd[i]) == h48) {
   t->size--;
   continue;
  }
  if (i != w) {
   t->overflow_hash_pd[w] = t->overflow_hash_pd[i];
   t->overflow_vals[w] = t->overflow_vals[i];
  }
  w++;
 }
 size_t removed = t->overflow_len - w;
 t->overflow_len = w;
 return removed;
}

bool bare_rh_insert_bounded(ht_bare_t *t, uint64_t h48, uint32_t val) {
    if (t->capacity == 0) return bare_rh_insert_unbounded(t, h48, val);
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

  if (t->max_probe_dist > 0 && dist > t->max_probe_dist) {
   if (!overflow_stash_insert(t, h48, cur_val)) {
    t->overflow_violated = true;
    return bare_rh_insert_unbounded(t, h48, cur_val);
   }
   return true;
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

   if (t->max_probe_dist > 0 && dist > t->max_probe_dist) {
    if (!overflow_stash_insert(t, h48, cur_val)) {
     t->overflow_violated = true;
     return bare_rh_insert_unbounded(t, h48, cur_val);
    }
    return true;
   }

  idx = (idx + 1) & cap_mask;
  continue;
  }

  idx = (idx + 1) & cap_mask;
  dist++;

  if (t->max_probe_dist > 0 && dist > t->max_probe_dist) {
   if (!overflow_stash_insert(t, h48, cur_val)) {
    t->overflow_violated = true;
    return bare_rh_insert_unbounded(t, h48, cur_val);
   }
   return true;
  }

 if (dist > t->capacity) {
 if (!bare_resize_table(t)) return false;
 cap_mask = t->capacity - 1;
 ideal = h48 & cap_mask;
 idx = ideal;
 dist = 0;
 }
 }
}

bool bare_rh_insert_unbounded(ht_bare_t *t, uint64_t h48, uint32_t val) {
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
 cap_mask = t->capacity - 1;
 ideal = h48 & cap_mask;
 idx = ideal;
 dist = 0;
 }
 }
}

// ============================================================================
// Bare Internal: Delete Compact / Backward Shift
// ============================================================================

bool bare_verify_ideal_safe(const ht_bare_t *t, size_t idx, size_t len) {
    size_t cap_mask = t->capacity - 1;
    size_t write_offset = 0;
    for (size_t i = 0; i < len; i++) {
        size_t pos = (idx + 1 + i) & cap_mask;
        uint64_t hpd = t->hash_pd[pos];
        if (!hpd_live(hpd)) continue;

        size_t target = (idx + write_offset) & cap_mask;
        size_t ideal = hpd_hash(hpd) & cap_mask;
        if (ideal > target && (ideal - target) < t->capacity / 2)
            return false;

        size_t shift = (i + 1) - write_offset;
        if (shift > hpd_pd(hpd))
            return false;

        write_offset++;
    }
    return true;
}

void bare_commit_backward_shift(ht_bare_t *t, size_t idx, size_t len) {
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

void bare_delete_compact(ht_bare_t *t, size_t idx) {
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

// ============================================================================
// Bare Internal: Prophylactic Tombstones & Reinsert
// ============================================================================

void bare_place_prophylactic_tombstones(ht_bare_t *t) {
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

void bare_reinsert_main(ht_bare_t *t,
                               const uint64_t *old_hash_pd,
                               const uint32_t *old_vals,
                               size_t old_cap) {
    for (size_t i = 0; i < old_cap; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        bare_rh_insert_bounded(t, hpd_hash(hpd), old_vals[i]);
    }
}

void bare_reinsert_spill(ht_bare_t *t,
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

ht_bare_t *ht_bare_create(const ht_config_t *cfg, struct arena *arena) {
    (void)arena;
    ht_bare_t *t = calloc(1, sizeof(ht_bare_t));
    if (!t) return NULL;
    t->allocator = NULL;

    ht_config_t c = default_cfg;
    if (cfg) c = *cfg;
    if (c.initial_capacity < 4) c.initial_capacity = 4;

    size_t cap = next_pow2(c.initial_capacity);
    uint8_t *main_block = bare_alloc_main_block(cap);
    if (!main_block) { free(t); return NULL; }
    bare_main_block_init(t, main_block, cap);

  t->spill_cap = SPILL_INITIAL;
  size_t spill_size = t->spill_cap * (sizeof(uint64_t) + sizeof(uint32_t));
  t->spill_block = malloc(spill_size);
  if (!t->spill_block) { free(t->main_block); free(t); return NULL; }
  memset(t->spill_block, 0, t->spill_cap * sizeof(uint64_t));
  memset(t->spill_block + t->spill_cap * sizeof(uint64_t), 0xFF, t->spill_cap * sizeof(uint32_t));
  t->spill_hash_pd = (uint64_t*)t->spill_block;
  t->spill_vals = (uint32_t*)(t->spill_block + t->spill_cap * sizeof(uint64_t));
  t->spill_in_block = false;

    t->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
        (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
    t->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    t->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    t->zombie_window = c.zombie_window;
    t->max_probe_dist = (c.max_probe_dist == 0) ? 255 :
        (c.max_probe_dist > UINT16_MAX) ? UINT16_MAX : c.max_probe_dist;

    return t;
}

void ht_bare_destroy(ht_bare_t *t) {
    if (!t) return;
    if (!t->spill_in_block)
        free(t->spill_block);
    free(t->main_block);
    free(t);
}

void ht_bare_clear(ht_bare_t *t) {
    if (!t) return;
    memset(t->hash_pd, 0, t->capacity * sizeof(uint64_t));
    memset(t->vals, 0xFF, t->capacity * sizeof(uint32_t));
    memset(t->spill_block, 0, t->spill_cap * sizeof(uint64_t));
    memset(t->spill_block + t->spill_cap * sizeof(uint64_t), 0xFF, t->spill_cap * sizeof(uint32_t));
    t->size = 0;
    t->tombstone_cnt = 0;
    t->spill_len = 0;
    t->overflow_len = 0;
    t->overflow_violated = false;
}

// ============================================================================
// Bare Public API: Insert
// ============================================================================

bool ht_bare_insert(ht_bare_t *t, uint64_t hash, uint32_t val) {
    if (!t || val == VAL_NONE) return false;

    uint64_t h48 = hash & HASH_MASK;

    if (h48 < 2)
        return bare_spill_insert(t, h48, val);

    size_t main_live = t->size - t->spill_len - t->overflow_len;

    // Main-table load-factor resize uses main-table occupancy only
    if (!t->resizing && (double)(main_live + 1) / (double)t->capacity > t->max_load_factor) {
        if (t->capacity > SIZE_MAX / 2) return false;
        if (!ht_bare_resize(t, t->capacity * 2))
            return false;
        main_live = t->size - t->spill_len - t->overflow_len;
    }

    // Don't preemptively resize for stash-full here — let bare_rh_insert_bounded
    // handle it via Policy B fallback (unbounded insert). A proactive resize
    // would loop infinitely: reinserted overflow entries fill the stash again
    // under tight D_MAX with colliding hashes.

    // Reserve overflow capacity before mutating main table
    if (t->max_probe_dist > 0 && t->overflow_len == HT_OVERFLOW_STASH_CAP) {
        if (!ht_bare_resize(t, t->capacity * 2))
            return false;
        // Policy B: if still full after resize, bounded insert will
        // fall through to bare_rh_insert_unbounded as a soft fallback
    }

    return bare_rh_insert_bounded(t, h48, val);
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

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;
        if (!t->overflow_violated && t->max_probe_dist > 0 && dist > t->max_probe_dist) break;

        if (hpd_hash(slot_hpd) == h48) {
            if (out_val) *out_val = t->vals[idx];
            return true;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    for (size_t i = 0; i < t->overflow_len; i++) {
        if (hpd_hash(t->overflow_hash_pd[i]) == h48) {
            if (out_val) *out_val = t->overflow_vals[i];
            return true;
        }
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

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;
        if (!t->overflow_violated && t->max_probe_dist > 0 && dist > t->max_probe_dist) break;

        if (hpd_hash(slot_hpd) == h48) {
            if (!cb(t->vals[idx], user_ctx))
                return;
        }

        idx = (idx + 1) & cap_mask;
        dist++;
    }

    for (size_t i = 0; i < t->overflow_len; i++) {
        if (hpd_hash(t->overflow_hash_pd[i]) == h48) {
            if (!cb(t->overflow_vals[i], user_ctx))
                return;
        }
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

    removed += overflow_stash_remove(t, h48);

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

        if (hpd_empty(slot_hpd)) break;

        if (hpd_tomb(slot_hpd)) {
            idx = (idx + 1) & cap_mask;
            dist++;
            continue;
        }

        if (hpd_pd(slot_hpd) < dist) break;

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

    return overflow_stash_remove_val(t, h48, val);
}

// ============================================================================
// Bare Public API: Resize / Compact
// ============================================================================

bool bare_resize_table(ht_bare_t *t) {
  size_t old_cap = t->capacity;
  uint64_t *old_hash_pd = t->hash_pd;
  uint32_t *old_vals = t->vals;
  uint8_t *old_main_block = t->main_block;
  bool old_spill_in_block = t->spill_in_block;

  uint64_t *old_spill_hash_pd = t->spill_hash_pd;
  uint32_t *old_spill_vals = t->spill_vals;
  uint8_t *old_spill_block = t->spill_block;
  size_t old_spill_len = t->spill_len;

  uint64_t old_overflow_hash[HT_OVERFLOW_STASH_CAP];
  uint32_t old_overflow_vals[HT_OVERFLOW_STASH_CAP];
  size_t old_overflow_len = t->overflow_len;
  memcpy(old_overflow_hash, t->overflow_hash_pd, old_overflow_len * sizeof(uint64_t));
  memcpy(old_overflow_vals, t->overflow_vals, old_overflow_len * sizeof(uint32_t));

  size_t new_capacity = next_pow2(t->size * 2);
  if (new_capacity < 4) new_capacity = 4;
  uint8_t *new_main_block = bare_alloc_main_block(new_capacity);
  if (!new_main_block) return false;

  size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
  size_t new_spill_size = new_spill_cap * (sizeof(uint64_t) + sizeof(uint32_t));
  uint8_t *new_spill_block = malloc(new_spill_size);
  if (!new_spill_block) {
    free(new_main_block);
    return false;
  }
  memset(new_spill_block, 0, new_spill_cap * sizeof(uint64_t));
  memset(new_spill_block + new_spill_cap * sizeof(uint64_t), 0xFF, new_spill_cap * sizeof(uint32_t));
  uint64_t *new_spill_hash_pd = (uint64_t*)new_spill_block;
  uint32_t *new_spill_vals = (uint32_t*)(new_spill_block + new_spill_cap * sizeof(uint64_t));

  bare_main_block_init(t, new_main_block, new_capacity);
  t->size = 0;
  t->tombstone_cnt = 0;
  t->spill_block = new_spill_block;
  t->spill_hash_pd = new_spill_hash_pd;
  t->spill_vals = new_spill_vals;
  t->spill_cap = new_spill_cap;
  t->spill_len = 0;
  t->spill_in_block = false;
  t->overflow_len = 0;
  t->overflow_violated = false;

  bare_reinsert_main(t, old_hash_pd, old_vals, old_cap);
  bare_reinsert_spill(t, old_spill_hash_pd, old_spill_vals, old_spill_len);

  for (size_t i = 0; i < old_overflow_len; i++)
   bare_rh_insert_bounded(t, hpd_hash(old_overflow_hash[i]), old_overflow_vals[i]);

  bare_place_prophylactic_tombstones(t);

  free(old_main_block);
  if (!old_spill_in_block)
    free(old_spill_block);
  return true;
}

bool ht_bare_resize(ht_bare_t *t, size_t new_capacity) {
    if (!t) return false;
    size_t main_live = t->size - t->spill_len - t->overflow_len;
    if (new_capacity < main_live) return false;
    if (t->resizing) return true;
    t->resizing = true;
    bool ok = bare_resize_table(t);
    t->resizing = false;
    return ok;
}

void ht_bare_compact(ht_bare_t *t) {
  if (!t) return;

  uint8_t *old_main_block = t->main_block;
  uint64_t *old_hash_pd = t->hash_pd;
  uint32_t *old_vals = t->vals;
  size_t old_cap = t->capacity;
  bool old_spill_in_block = t->spill_in_block;

  uint64_t *old_spill_hash_pd = t->spill_hash_pd;
  uint32_t *old_spill_vals = t->spill_vals;
  uint8_t *old_spill_block = t->spill_block;
  size_t old_spill_len = t->spill_len;

  uint64_t old_overflow_hash[HT_OVERFLOW_STASH_CAP];
  uint32_t old_overflow_vals[HT_OVERFLOW_STASH_CAP];
  size_t old_overflow_len = t->overflow_len;
  memcpy(old_overflow_hash, t->overflow_hash_pd, old_overflow_len * sizeof(uint64_t));
  memcpy(old_overflow_vals, t->overflow_vals, old_overflow_len * sizeof(uint32_t));

  uint8_t *new_main_block = bare_alloc_main_block(old_cap);
  if (!new_main_block) return;

  size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
  size_t new_spill_size = new_spill_cap * (sizeof(uint64_t) + sizeof(uint32_t));
  uint8_t *new_spill_block = malloc(new_spill_size);
  if (!new_spill_block) {
    free(new_main_block);
    return;
  }
  memset(new_spill_block, 0, new_spill_cap * sizeof(uint64_t));
  memset(new_spill_block + new_spill_cap * sizeof(uint64_t), 0xFF, new_spill_cap * sizeof(uint32_t));
  uint64_t *new_spill_hash_pd = (uint64_t*)new_spill_block;
  uint32_t *new_spill_vals = (uint32_t*)(new_spill_block + new_spill_cap * sizeof(uint64_t));

  bare_main_block_init(t, new_main_block, old_cap);
  t->size = 0;
  t->tombstone_cnt = 0;
  t->spill_block = new_spill_block;
  t->spill_hash_pd = new_spill_hash_pd;
  t->spill_vals = new_spill_vals;
  t->spill_cap = new_spill_cap;
  t->spill_len = 0;
  t->spill_in_block = false;
  t->overflow_len = 0;
  t->overflow_violated = false;

  bare_reinsert_main(t, old_hash_pd, old_vals, old_cap);
  bare_reinsert_spill(t, old_spill_hash_pd, old_spill_vals, old_spill_len);

  for (size_t i = 0; i < old_overflow_len; i++)
   bare_rh_insert_bounded(t, hpd_hash(old_overflow_hash[i]), old_overflow_vals[i]);

  bare_place_prophylactic_tombstones(t);

  free(old_main_block);
  if (!old_spill_in_block)
    free(old_spill_block);
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

    size_t spill_off = t->capacity;
    size_t overflow_off = spill_off + t->spill_len;

    if (iter->idx < overflow_off) {
        size_t spill_idx = iter->idx - spill_off;
        while (spill_idx < t->spill_len) {
            uint32_t sval = t->spill_vals[spill_idx];
            uint64_t shpd = t->spill_hash_pd[spill_idx];
            spill_idx++;
            iter->idx = spill_off + spill_idx;
            if (sval != VAL_NONE) {
                if (out_hash) *out_hash = hpd_hash(shpd);
                if (out_val) *out_val = sval;
                return true;
            }
        }
    }

    size_t overflow_idx = iter->idx - overflow_off;
    while (overflow_idx < t->overflow_len) {
        uint64_t ohpd = t->overflow_hash_pd[overflow_idx];
        uint32_t oval = t->overflow_vals[overflow_idx];
        overflow_idx++;
        iter->idx = overflow_off + overflow_idx;
        if (oval != VAL_NONE) {
            if (out_hash) *out_hash = hpd_hash(ohpd);
            if (out_val) *out_val = oval;
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
    out_stats->load_factor = (double)t->size / (double)t->capacity;
    out_stats->tombstone_ratio = (t->size + t->tombstone_cnt > 0)
        ? (double)t->tombstone_cnt / (double)(t->size + t->tombstone_cnt)
        : 0.0;
}

const char *ht_bare_check_invariants(const ht_bare_t *t) {
    if (!t) return "table is NULL";
    size_t cap_mask = t->capacity - 1;

    size_t live_count = 0;
    size_t tomb_count = 0;
    size_t spill_live = 0;
    size_t overflow_live = 0;

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

    overflow_live = t->overflow_len;

    if (t->size != live_count + spill_live + overflow_live) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "size=%zu but found %zu live (%zu main + %zu spill + %zu overflow)",
                 t->size, live_count + spill_live + overflow_live,
                 live_count, spill_live, overflow_live);
        return buf;
    }

    if (t->overflow_len > HT_OVERFLOW_STASH_CAP) {
        static char buf[256];
        snprintf(buf, sizeof(buf),
                 "overflow_len=%zu exceeds HT_OVERFLOW_STASH_CAP=%d",
                 t->overflow_len, HT_OVERFLOW_STASH_CAP);
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
    if (t->overflow_len > 0) {
        printf("  Overflow stash (%zu entries):\n", t->overflow_len);
        for (size_t i = 0; i < t->overflow_len; i++) {
            printf("  overflow[%zu]: hash=0x%08" PRIx64 " val=%" PRIu32 "\n",
                   i, hpd_hash(t->overflow_hash_pd[i]), t->overflow_vals[i]);
        }
    }
}

// ============================================================================
// High-Level Internal: Arena / KV Allocation
// ============================================================================

static void *kv_alloc(ht_table_t *t, size_t n) {
#ifndef DRAUGR_USE_MALLOC
    if (t->allocator) {
        return arena_alloc(t->allocator, n);
    }
#endif
    (void)t;
    return malloc(n);
}

static void kv_free(ht_table_t *t, ht_entry_t *e) {
    if (!e->kv_ptr) return;
    (void)t;
#ifndef DRAUGR_USE_MALLOC
    if (t->allocator) {
        size_t total = ht_entry_key_storage(e->key_len) + e->val_len;
        arena_free(t->allocator, e->kv_ptr, total);
        e->kv_ptr = NULL;
        return;
    }
#endif
    free(e->kv_ptr);
    e->kv_ptr = NULL;
}

// ============================================================================
// High-Level Internal: Entry Management
// ============================================================================

static uint32_t alloc_entry(ht_table_t *t, uint16_t hash_hi,
    const void *key, size_t key_len,
    const void *value, size_t value_len) {
    if (key_len > UINT16_MAX) return VAL_NONE;

    if (t->entry_count >= t->entry_cap) {
        size_t new_cap;
        if (t->entry_cap == 0) new_cap = 64;
        else if (t->entry_cap > SIZE_MAX / 2) new_cap = SIZE_MAX;
        else new_cap = t->entry_cap * 2;

        ht_entry_t *ne;
        if (t->entries_in_block) {
            ne = calloc(new_cap, sizeof(ht_entry_t));
            if (!ne) return VAL_NONE;
            memcpy(ne, t->entries, t->entry_count * sizeof(ht_entry_t));
            t->entries_in_block = false;
        } else {
            ne = realloc(t->entries, new_cap * sizeof(ht_entry_t));
            if (!ne) return VAL_NONE;
            memset(ne + t->entry_cap, 0, (new_cap - t->entry_cap) * sizeof(ht_entry_t));
        }
        t->entries = ne;
        t->entry_cap = new_cap;
    }

    size_t klen_stor = ht_entry_key_storage(key_len);
    void *data = kv_alloc(t, klen_stor + value_len);
    if (!data) return VAL_NONE;
    memcpy(data, key, key_len);
    if (value_len > 0) {
        memcpy((uint8_t *)data + klen_stor, value, value_len);
    }

    uint32_t eidx = (uint32_t)t->entry_count++;
    t->entries[eidx].key_len = (uint16_t)key_len;
    t->entries[eidx].hash_hi = hash_hi;
    t->entries[eidx].val_len = (uint32_t)value_len;
    t->entries[eidx].kv_ptr = (uint8_t *)data;
    return eidx;
}

static bool update_entry_value(ht_table_t *t, uint32_t eidx,
                               const void *key, size_t key_len,
                               const void *value, size_t value_len) {
    ht_entry_t *e = &t->entries[eidx];
    size_t klen_stor = ht_entry_key_storage(key_len);
    if (value_len == e->val_len) {
        memcpy(e->kv_ptr + klen_stor, value, value_len);
        return true;
    }
    void *data = kv_alloc(t, klen_stor + value_len);
    if (!data) return false;
    memcpy(data, key, key_len);
    memcpy((uint8_t *)data + klen_stor, value, value_len);
    kv_free(t, e);
    e->val_len = (uint32_t)value_len;
    e->kv_ptr = (uint8_t *)data;
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
	const void *entry_key = ht_entry_key(e);
	if (t->eq_fn)
		return t->eq_fn(entry_key, e->key_len, key, key_len, t->user_ctx);
	return memcmp(entry_key, key, key_len) == 0;
}

static inline bool vals_match(const ht_table_t *t, uint32_t eidx,
		const void *val, size_t val_len) {
	(void)t;
	const ht_entry_t *e = &t->entries[eidx];
	if (e->val_len != val_len) return false;
	const void *entry_val = ht_entry_val(e);
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
    uint32_t stack_matches[64];
};

static bool hl_key_scan_cb_grow(struct hl_key_scan_ctx *ctx) {
    if (ctx->match_count < ctx->match_cap)
        return true;
    size_t new_cap = ctx->match_cap * 2;
    uint32_t *new_matches = realloc(
        ctx->matches == ctx->stack_matches ? NULL : ctx->matches,
        new_cap * sizeof(uint32_t));
    if (!new_matches) return false;
    if (ctx->matches == ctx->stack_matches)
        memcpy(new_matches, ctx->stack_matches, ctx->match_count * sizeof(uint32_t));
    ctx->matches = new_matches;
    ctx->match_cap = new_cap;
    return true;
}

static bool hl_key_scan_cb(uint32_t val, void *user_ctx) {
    struct hl_key_scan_ctx *ctx = user_ctx;
    if (keys_match(ctx->t, val, ctx->hash_hi, ctx->key, ctx->key_len)) {
        if (!hl_key_scan_cb_grow(ctx)) return false;
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
	return ctx->user_cb(ht_entry_key(e), e->key_len,
		ht_entry_val(e), e->val_len,
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
		return ctx->user_cb(ht_entry_key(e), e->key_len,
			ht_entry_val(e), e->val_len,
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
    return ht_create_with_arena(cfg, hash_fn, eq_fn, user_ctx, NULL);
}

ht_table_t *ht_create_with_arena(const ht_config_t *cfg,
 ht_hash_fn hash_fn, ht_eq_fn eq_fn,
 void *user_ctx, struct arena *arena) {
    if (!hash_fn) return NULL;

    ht_config_t c = default_cfg;
    if (cfg) c = *cfg;
    if (c.initial_capacity < 4) c.initial_capacity = 4;

    size_t cap = next_pow2(c.initial_capacity);
    size_t main_sz = bare_main_block_size(cap);
    size_t spill_sz = SPILL_INITIAL * (sizeof(uint64_t) + sizeof(uint32_t));
    size_t entry_cap = 64;
    size_t table_block_sz = main_sz + spill_sz + entry_cap * sizeof(ht_entry_t);

    uint8_t *table_block = calloc(1, table_block_sz);
    if (!table_block) return NULL;

    ht_table_t *t = calloc(1, sizeof(ht_table_t));
    if (!t) { free(table_block); return NULL; }

    t->table_block = table_block;

    ht_bare_t *b = &t->bare;
    bare_main_block_init(b, table_block, cap);

    b->spill_block = table_block + main_sz;
    b->spill_hash_pd = (uint64_t*)b->spill_block;
    b->spill_vals = (uint32_t*)(b->spill_block + SPILL_INITIAL * sizeof(uint64_t));
    b->spill_cap = SPILL_INITIAL;
    b->spill_in_block = true;
    memset(b->spill_vals, 0xFF, SPILL_INITIAL * sizeof(uint32_t));

    t->entries = (ht_entry_t *)(table_block + main_sz + spill_sz);
    t->entry_cap = entry_cap;
    t->entries_in_block = true;

    b->max_load_factor = (c.max_load_factor <= 0) ? 0.75 :
        (c.max_load_factor > 0.97) ? 0.97 : c.max_load_factor;
    b->min_load_factor = (c.min_load_factor >= 0) ? c.min_load_factor : 0.20;
    b->tomb_threshold = (c.tomb_threshold > 0) ? c.tomb_threshold : 0.20;
    b->zombie_window = c.zombie_window;
    b->max_probe_dist = (c.max_probe_dist > UINT16_MAX) ? UINT16_MAX : c.max_probe_dist;

#ifndef DRAUGR_USE_MALLOC
    if (arena) {
        t->allocator = arena;
    }
#endif
#ifdef DRAUGR_USE_MALLOC
    (void)arena;
#endif

    t->hash_fn = hash_fn;
    t->eq_fn = eq_fn;
    t->user_ctx = user_ctx;

    return t;
}

void ht_destroy(ht_table_t *t) {
  if (!t) return;
  for (size_t i = 0; i < t->entry_count; i++) {
    kv_free(t, &t->entries[i]);
  }
  if (!t->entries_in_block)
    free(t->entries);
  if (!t->bare.spill_in_block)
    free(t->bare.spill_block);
  free(t->table_block);
  free(t);
}

void ht_clear(ht_table_t *t) {
  if (!t) return;
  ht_bare_clear(&t->bare);
#ifndef DRAUGR_USE_MALLOC
  if (t->allocator) {
    arena_clear(t->allocator);
  } else
#endif
  {
    for (size_t i = 0; i < t->entry_count; i++) {
      free(t->entries[i].kv_ptr);
    }
  }
  if (!t->bare.spill_in_block) {
    free(t->bare.spill_block);
    size_t main_sz = bare_main_block_size(t->bare.capacity);
    t->bare.spill_block = t->table_block + main_sz;
    t->bare.spill_hash_pd = (uint64_t*)t->bare.spill_block;
    t->bare.spill_vals = (uint32_t*)(t->bare.spill_block + t->bare.spill_cap * sizeof(uint64_t));
    t->bare.spill_cap = SPILL_INITIAL;
    t->bare.spill_in_block = true;
    memset(t->bare.spill_hash_pd, 0, t->bare.spill_cap * sizeof(uint64_t));
    memset(t->bare.spill_vals, 0xFF, t->bare.spill_cap * sizeof(uint32_t));
  }
  if (!t->entries_in_block) {
    free(t->entries);
    size_t main_sz = bare_main_block_size(t->bare.capacity);
    size_t spill_sz = t->bare.spill_cap * (sizeof(uint64_t) + sizeof(uint32_t));
    size_t off_entries = (main_sz + spill_sz + 7) & ~(size_t)7;
    t->entries = (ht_entry_t *)(t->table_block + off_entries);
    t->entry_cap = 64;
    t->entries_in_block = true;
  }
  memset(t->entries, 0, t->entry_cap * sizeof(ht_entry_t));
  t->entry_count = 0;
}

// ============================================================================
// High-Level Public API: Insert / Upsert / Unsert
// ============================================================================

static ht_insert_result_t do_insert_with_hash(ht_table_t *t, uint64_t hash,
                                              const void *key, size_t key_len,
                                              const void *value, size_t value_len,
                                              int mode) {
    if (!t || !key) return HT_INSERT_FAILED;
    if (!value && value_len > 0) value_len = 0;

    uint64_t h48 = hash & HASH_MASK;
    uint16_t hash_hi = (uint16_t)(hash >> 48);

    // Phase 1: Scan for existing entries (UPSERT/UNIQUE only)
    if (mode == INS_UPSERT) {
        struct hl_key_scan_ctx ctx = {
            .t = t, .hash_hi = hash_hi, .key = key, .key_len = key_len,
            .matches = ctx.stack_matches, .match_count = 0, .match_cap = 64,
        };
        ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

        if (ctx.match_count > 0) {
            bool ok = update_entry_value(t, ctx.matches[0], key, key_len, value, value_len);
            if (ctx.matches != ctx.stack_matches)
                free(ctx.matches);
            if (!ok) return HT_INSERT_FAILED;
            for (size_t i = 1; i < ctx.match_count; i++) {
                kv_free(t, &t->entries[ctx.matches[i]]);
                ht_bare_remove_val(&t->bare, hash, ctx.matches[i]);
            }
            return HT_INSERT_UPDATE;
        }
        if (ctx.matches != ctx.stack_matches)
            free(ctx.matches);
    } else if (mode == INS_UNIQUE) {
        struct hl_kv_scan_ctx ctx = {
            .t = t, .hash_hi = hash_hi, .key = key, .key_len = key_len,
            .value = value, .value_len = value_len,
        };
        ht_bare_find_all(&t->bare, hash, hl_kv_scan_cb, &ctx);
        if (ctx.kv_found) return HT_INSERT_UPDATE;
    }

    // Phase 2: Insert new entry
    ht_bare_t *b = &t->bare;
    size_t main_live = b->size - b->spill_len - b->overflow_len;
    if (!b->resizing && (double)(main_live + 1) / (double)b->capacity > b->max_load_factor) {
        if (b->capacity > SIZE_MAX / 2 || !ht_resize(t, b->capacity * 2))
            return HT_INSERT_FAILED;
        if (b->size == 0) {
            b->resizing = false;
            b->size = 0;
            b->tombstone_cnt = 0;
            return HT_INSERT_FAILED;
        }
    }

    uint32_t eidx = alloc_entry(t, hash_hi, key, key_len, value, value_len);
    if (eidx == VAL_NONE) return HT_INSERT_FAILED;

    bool result;
    if (h48 < 2)
        result = bare_spill_insert(b, h48, eidx);
    else
        result = bare_rh_insert_bounded(b, h48, eidx);

    return result ? HT_INSERT_OK : HT_INSERT_FAILED;
}

ht_insert_result_t ht_insert_with_hash(ht_table_t *t, uint64_t hash,
                                       const void *key, size_t key_len,
                                       const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_ALWAYS);
}

ht_insert_result_t ht_insert(ht_table_t *t, const void *key, size_t key_len,
                             const void *value, size_t value_len) {
    if (!t || !key) return HT_INSERT_FAILED;
    if (!value && value_len > 0) value_len = 0;
    return ht_insert_with_hash(t, t->hash_fn(key, key_len, t->user_ctx),
                               key, key_len, value, value_len);
}

ht_insert_result_t ht_upsert_with_hash(ht_table_t *t, uint64_t hash,
                                       const void *key, size_t key_len,
                                       const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_UPSERT);
}

ht_insert_result_t ht_upsert(ht_table_t *t, const void *key, size_t key_len,
                             const void *value, size_t value_len) {
    if (!t || !key) return HT_INSERT_FAILED;
    if (!value && value_len > 0) value_len = 0;
    return ht_upsert_with_hash(t, t->hash_fn(key, key_len, t->user_ctx),
                               key, key_len, value, value_len);
}

ht_insert_result_t ht_unsert_with_hash(ht_table_t *t, uint64_t hash,
                                        const void *key, size_t key_len,
                                        const void *value, size_t value_len) {
    return do_insert_with_hash(t, hash, key, key_len, value, value_len, INS_UNIQUE);
}

ht_insert_result_t ht_unsert(ht_table_t *t, const void *key, size_t key_len,
                              const void *value, size_t value_len) {
    if (!t || !key) return HT_INSERT_FAILED;
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
	return ht_entry_val(e);
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
	return ht_entry_val(e);
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
        memcpy(&new_val, found, sizeof(new_val));
        if (delta > 0 && new_val > INT64_MAX - delta)
            new_val = INT64_MAX;
        else if (delta < 0 && new_val < INT64_MIN - delta)
            new_val = INT64_MIN;
        else
            new_val += delta;
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
        memcpy(&new_val, found, sizeof(new_val));
        if (delta > 0 && new_val > INT64_MAX - delta)
            new_val = INT64_MAX;
        else if (delta < 0 && new_val < INT64_MIN - delta)
            new_val = INT64_MIN;
        else
            new_val += delta;
    } else {
        new_val = delta;
    }
    ht_insert_result_t result = ht_upsert_with_hash(t, hash, key, key_len, &new_val, sizeof(new_val));
    if (result == HT_INSERT_FAILED) {
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

    struct hl_key_scan_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = ctx.stack_matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

    size_t removed = ctx.match_count;
    for (size_t i = 0; i < ctx.match_count; i++) {
        kv_free(t, &t->entries[ctx.matches[i]]);
        ht_bare_remove_val(&t->bare, hash, ctx.matches[i]);
    }

    if (ctx.matches != ctx.stack_matches)
        free(ctx.matches);

    if (removed > 0 && t->bare.min_load_factor > 0 && t->bare.size > 0 &&
        (double)t->bare.size / (double)t->bare.capacity < t->bare.min_load_factor &&
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
    struct hl_key_scan_ctx kctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = kctx.stack_matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &kctx);

    // Filter by value match and remove
    size_t removed = 0;
    for (size_t i = 0; i < kctx.match_count; i++) {
        if (vals_match(t, kctx.matches[i], value, value_len)) {
            kv_free(t, &t->entries[kctx.matches[i]]);
            ht_bare_remove_val(&t->bare, hash, kctx.matches[i]);
            removed++;
        }
    }

    if (kctx.matches != kctx.stack_matches)
        free(kctx.matches);

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

    struct hl_key_scan_ctx ctx = {
        .t = t, .hash_hi = (uint16_t)(hash >> 48),
        .key = key, .key_len = key_len,
        .matches = ctx.stack_matches, .match_count = 0, .match_cap = 64,
    };
    ht_bare_find_all(&t->bare, hash, hl_key_scan_cb, &ctx);

    for (size_t i = 0; i < ctx.match_count; i++) {
        if (vals_match(t, ctx.matches[i], value, value_len)) {
            kv_free(t, &t->entries[ctx.matches[i]]);
            ht_bare_remove_val(&t->bare, hash, ctx.matches[i]);

            if (t->bare.min_load_factor > 0 && t->bare.size > 0 &&
                (double)t->bare.size / (double)t->bare.capacity < t->bare.min_load_factor &&
                t->bare.capacity > 64) {
                size_t new_cap = t->bare.capacity / 2;
                if (new_cap >= 64 && new_cap >= t->bare.size * 2)
                    ht_resize(t, new_cap);
            }

            if (ctx.matches != ctx.stack_matches)
                free(ctx.matches);
            return true;
        }
    }

    if (ctx.matches != ctx.stack_matches)
        free(ctx.matches);
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

static bool ht_rebuild(ht_table_t *t, size_t new_capacity) {
  ht_bare_t *b = &t->bare;

  uint8_t *old_main_block = b->main_block;
  uint64_t *old_hash_pd = b->hash_pd;
  uint32_t *old_vals = b->vals;
  ht_entry_t *old_entries = t->entries;
  size_t old_cap = b->capacity;
  size_t old_entry_count = t->entry_count;
  size_t old_entry_cap = t->entry_cap;
  bool old_entries_in_block = t->entries_in_block;
  bool old_spill_in_block = b->spill_in_block;
  uint8_t *old_spill_block = b->spill_block;

  uint64_t *old_spill_hash_pd = b->spill_hash_pd;
  uint32_t *old_spill_vals = b->spill_vals;
  size_t old_spill_len = b->spill_len;

  uint64_t old_overflow_hash[HT_OVERFLOW_STASH_CAP];
  uint32_t old_overflow_vals[HT_OVERFLOW_STASH_CAP];
  size_t old_overflow_len = b->overflow_len;
  memcpy(old_overflow_hash, b->overflow_hash_pd, old_overflow_len * sizeof(uint64_t));
  memcpy(old_overflow_vals, b->overflow_vals, old_overflow_len * sizeof(uint32_t));

    size_t new_entry_cap = old_entry_cap;
    size_t new_main_sz = bare_main_block_size(new_capacity);
    size_t new_spill_cap = old_spill_len > SPILL_INITIAL ? old_spill_len : SPILL_INITIAL;
    size_t new_spill_sz = new_spill_cap * (sizeof(uint64_t) + sizeof(uint32_t));
    size_t off_entries = new_main_sz + new_spill_sz;
    off_entries = (off_entries + 7) & ~(size_t)7;
    size_t new_block_sz = off_entries + new_entry_cap * sizeof(ht_entry_t);

    uint8_t *new_block = calloc(1, new_block_sz);
    if (!new_block) return false;

    memset(new_block + new_capacity * sizeof(uint64_t), 0xFF,
           new_capacity * sizeof(uint32_t));

    uint8_t *new_spill_block = new_block + new_main_sz;
    uint64_t *new_spill_hash_pd = (uint64_t*)new_spill_block;
    uint32_t *new_spill_vals = (uint32_t*)(new_spill_block + new_spill_cap * sizeof(uint64_t));
    memset(new_spill_vals, 0xFF, new_spill_cap * sizeof(uint32_t));

	ht_entry_t *tmp_entries = (ht_entry_t *)(new_block + off_entries);
	ht_entry_t *tmp_entries_block_start = tmp_entries;
	size_t tmp_entry_count = 0;
	size_t tmp_entry_cap = new_entry_cap;

    ht_bare_t tmp_b = *b;
    bare_main_block_init(&tmp_b, new_block, new_capacity);
    tmp_b.size = 0;
    tmp_b.tombstone_cnt = 0;
    tmp_b.spill_block = new_spill_block;
    tmp_b.spill_hash_pd = new_spill_hash_pd;
    tmp_b.spill_vals = new_spill_vals;
    tmp_b.spill_cap = new_spill_cap;
    tmp_b.spill_len = 0;
    tmp_b.overflow_len = 0;
    tmp_b.overflow_violated = false;

    bool ok = true;
    for (size_t i = 0; i < old_cap && ok; i++) {
        uint64_t hpd = old_hash_pd[i];
        if (!hpd_live(hpd)) continue;
        uint32_t old_eidx = old_vals[i];
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;

        if (tmp_entry_count >= tmp_entry_cap) {
            size_t grow = tmp_entry_cap > SIZE_MAX / 2 ? SIZE_MAX : tmp_entry_cap * 2;
            ht_entry_t *ne = realloc(tmp_entries, grow * sizeof(ht_entry_t));
            if (!ne) { ok = false; break; }
            memset(ne + tmp_entry_cap, 0, (grow - tmp_entry_cap) * sizeof(ht_entry_t));
            tmp_entries = ne;
            tmp_entry_cap = grow;
        }

        size_t klen_stor = ht_entry_key_storage(e->key_len);
        void *data = kv_alloc(t, klen_stor + e->val_len);
        if (!data) { ok = false; break; }
        memcpy(data, ht_entry_key(e), e->key_len);
        if (e->val_len > 0) memcpy((uint8_t *)data + klen_stor, ht_entry_val(e), e->val_len);

        uint32_t new_eidx = (uint32_t)tmp_entry_count++;
        tmp_entries[new_eidx] = *e;
        tmp_entries[new_eidx].kv_ptr = (uint8_t *)data;

        uint64_t h48 = hpd_hash(hpd);
        if (h48 < 2)
            ok = bare_spill_insert(&tmp_b, h48, new_eidx);
        else
            ok = bare_rh_insert_bounded(&tmp_b, h48, new_eidx);
    }

    for (size_t i = 0; i < old_spill_len && ok; i++) {
        uint32_t old_eidx = old_spill_vals[i];
        if (old_eidx == VAL_NONE) continue;
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;

        if (tmp_entry_count >= tmp_entry_cap) {
            size_t grow = tmp_entry_cap > SIZE_MAX / 2 ? SIZE_MAX : tmp_entry_cap * 2;
            ht_entry_t *ne = realloc(tmp_entries, grow * sizeof(ht_entry_t));
            if (!ne) { ok = false; break; }
            memset(ne + tmp_entry_cap, 0, (grow - tmp_entry_cap) * sizeof(ht_entry_t));
            tmp_entries = ne;
            tmp_entry_cap = grow;
        }

        size_t klen_stor = ht_entry_key_storage(e->key_len);
        void *data = kv_alloc(t, klen_stor + e->val_len);
        if (!data) { ok = false; break; }
        memcpy(data, ht_entry_key(e), e->key_len);
        if (e->val_len > 0) memcpy((uint8_t *)data + klen_stor, ht_entry_val(e), e->val_len);

        uint32_t new_eidx = (uint32_t)tmp_entry_count++;
        tmp_entries[new_eidx] = *e;
        tmp_entries[new_eidx].kv_ptr = (uint8_t *)data;

        ok = bare_spill_insert(&tmp_b, hpd_hash(old_spill_hash_pd[i]), new_eidx);
    }

    for (size_t i = 0; i < old_overflow_len && ok; i++) {
        uint32_t old_eidx = old_overflow_vals[i];
        const ht_entry_t *e = &old_entries[old_eidx];
        if (e->key_len == 0) continue;

        if (tmp_entry_count >= tmp_entry_cap) {
            size_t grow = tmp_entry_cap > SIZE_MAX / 2 ? SIZE_MAX : tmp_entry_cap * 2;
            ht_entry_t *ne = realloc(tmp_entries, grow * sizeof(ht_entry_t));
            if (!ne) { ok = false; break; }
            memset(ne + tmp_entry_cap, 0, (grow - tmp_entry_cap) * sizeof(ht_entry_t));
            tmp_entries = ne;
            tmp_entry_cap = grow;
        }

        uint64_t oh48 = hpd_hash(old_overflow_hash[i]);

        size_t klen_stor = ht_entry_key_storage(e->key_len);
        void *data = kv_alloc(t, klen_stor + e->val_len);
        if (!data) { ok = false; break; }
        memcpy(data, ht_entry_key(e), e->key_len);
        if (e->val_len > 0) memcpy((uint8_t *)data + klen_stor, ht_entry_val(e), e->val_len);

        uint32_t new_eidx = (uint32_t)tmp_entry_count++;
        tmp_entries[new_eidx] = *e;
        tmp_entries[new_eidx].kv_ptr = (uint8_t *)data;

        if (oh48 < 2)
            ok = bare_spill_insert(&tmp_b, oh48, new_eidx);
        else
            ok = bare_rh_insert_bounded(&tmp_b, oh48, new_eidx);
    }

	if (!ok) {
		for (size_t i = 0; i < tmp_entry_count; i++) {
			kv_free(t, &tmp_entries[i]);
		}
		if (tmp_entries != tmp_entries_block_start)
			free(tmp_entries);
		free(new_block);
		return false;
	}

    bare_place_prophylactic_tombstones(&tmp_b);

    for (size_t i = 0; i < old_entry_count; i++) {
        kv_free(t, &old_entries[i]);
    }
  if (!old_entries_in_block)
    free(old_entries);
  if (!old_spill_in_block)
    free(old_spill_block);
  free(old_main_block);

  *b = tmp_b;
  t->table_block = new_block;
  t->entries = tmp_entries;
  t->entry_count = tmp_entry_count;
  t->entry_cap = tmp_entry_cap;
  t->entries_in_block = (tmp_entries == (ht_entry_t *)(new_block + off_entries));
  b->spill_in_block = true;
  return true;
}

bool ht_resize(ht_table_t *t, size_t new_capacity) {
    if (!t) return false;
    size_t main_live = t->bare.size - t->bare.spill_len - t->bare.overflow_len;
    if (new_capacity < main_live) return false;
    if (t->bare.resizing) return true;

    t->bare.resizing = true;
    new_capacity = next_pow2(new_capacity);

    if (new_capacity == t->bare.capacity) {
        t->bare.resizing = false;
        return true;
    }

    bool result = ht_rebuild(t, new_capacity);
    t->bare.resizing = false;
    return result;
}


bool ht_compact(ht_table_t *t) {
    if (!t) return false;
    return ht_rebuild(t, t->bare.capacity);
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
		if (out_key) *out_key = ht_entry_key(e);
		if (out_key_len) *out_key_len = e->key_len;
		if (out_value) *out_value = ht_entry_val(e);
		if (out_value_len) *out_value_len = e->val_len;
		return true;
	}

    return false;
}

// ============================================================================
// High-Level Public API: Statistics
// ============================================================================

void ht_stats(const ht_table_t *t, ht_stats_t *out_stats) {
    if (!t || !out_stats) return;
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
            if (eidx == VAL_NONE) {
                printf("  [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s] eidx=VAL_NONE\n",
                       idx, hpd_hash(hpd), hpd_pd(hpd), tag);
	} else {
		const ht_entry_t *e = &t->entries[eidx];
		printf(" [%4zu]: hash=0x%08" PRIx64 " dist=%3u [%s] klen=%3u vlen=%3u kv=%p\n",
			idx, hpd_hash(hpd), hpd_pd(hpd), tag,
			e->key_len, e->val_len, (void *)e->kv_ptr);
	}
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
            if (eidx == VAL_NONE) {
                printf("  spill[%zu]: hash=0x%08" PRIx64 " eidx=VAL_NONE\n",
                       i, hpd_hash(shpd));
	} else {
		const ht_entry_t *e = &t->entries[eidx];
		printf(" spill[%zu]: hash=0x%08" PRIx64 " klen=%3u vlen=%3u kv=%p\n",
			i, hpd_hash(shpd), e->key_len, e->val_len, (void *)e->kv_ptr);
	}
        }
    }
}

// ============================================================================
// High-Level Public API: Invariant Checker
// ============================================================================

const char *ht_check_invariants(const ht_table_t *t) {
    if (!t) return "table is NULL";
    return ht_bare_check_invariants(&t->bare);
}
