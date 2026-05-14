/**
 * Draugr Concurrent Cuckoo Hash Table — Phase 1
 *
 * Flat-arena allocator, SWAR bucket-scan, simple cuckoo placement
 * (primary → secondary → stash), and grow-via-rehash.
 *
 * Uses raw malloc/calloc/realloc/free throughout (no DRAUGR macros)
 * so that ASan can track every allocation precisely.
 */

#include "draugr/htc_internal.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Arena — flat array of records + free-list
 * ========================================================================= */

uint32_t htc_arena_alloc(htc_arena_t *a, uint64_t hash, uint64_t value)
{
    if (a->free_count > 0) {
        a->free_count--;
        uint32_t idx = a->free_idx[a->free_count];
        a->records[idx].full_hash  = hash;
        a->records[idx].user_value = value;
        return idx;
    }
    if (a->count >= a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 64;
        htc_record_t *nr = (htc_record_t *)realloc(
            a->records, (size_t)new_cap * sizeof(htc_record_t));
        if (!nr) return UINT32_MAX;
        a->records  = nr;
        a->capacity = new_cap;
    }
    uint32_t idx = a->count++;
    a->records[idx].full_hash  = hash;
    a->records[idx].user_value = value;
    return idx;
}

void htc_arena_free(htc_arena_t *a, uint32_t idx)
{
    if (a->free_count >= a->free_cap) {
        uint32_t new_cap = a->free_cap ? a->free_cap * 2 : 64;
        uint32_t *nf = (uint32_t *)realloc(
            a->free_idx, (size_t)new_cap * sizeof(uint32_t));
        if (!nf) return;
        a->free_idx = nf;
        a->free_cap = new_cap;
    }
    a->free_idx[a->free_count++] = idx;
}

htc_record_t *htc_arena_ptr(htc_arena_t *a, uint32_t idx)
{
    return &a->records[idx];
}

void htc_arena_destroy(htc_arena_t *a)
{
    free(a->records);
    free(a->free_idx);
    memset(a, 0, sizeof(*a));
}

/* =========================================================================
 * Stash — linear overflow array
 * ========================================================================= */

int htc_stash_insert(htc_stash_t *s, uint64_t slot_word)
{
    if (s->size >= s->capacity) {
        uint32_t new_cap = s->capacity ? s->capacity * 2 : HTC_STASH_GROW;
        _Atomic uint64_t *ns = (_Atomic uint64_t *)realloc(
            s->slots, (size_t)new_cap * sizeof(uint64_t));
        if (!ns) return -1;
        s->slots    = ns;
        s->capacity = new_cap;
    }
    __atomic_store_n(&s->slots[s->size], slot_word, __ATOMIC_RELEASE);
    uint32_t ret = s->size;
    s->size++;
    return (int)ret;
}

int htc_stash_find(const htc_stash_t *s, htc_arena_t *a, uint64_t h)
{
    uint16_t tag = htc_tag16(h);
    for (uint32_t i = 0; i < s->size; i++) {
        uint64_t w = __atomic_load_n(&s->slots[i], __ATOMIC_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        htc_record_t *rec = htc_arena_ptr(a, htc_slot_index(w));
        if (rec->full_hash == h) return (int)i;
    }
    return -1;
}

void htc_stash_remove_at(htc_stash_t *s, unsigned idx)
{
    s->size--;
    if ((uint32_t)idx < s->size) {
        uint64_t w = __atomic_load_n(&s->slots[s->size], __ATOMIC_RELAXED);
        __atomic_store_n(&s->slots[idx], w, __ATOMIC_RELAXED);
    }
}

void htc_stash_destroy(htc_stash_t *s)
{
    free(s->slots);
    memset(s, 0, sizeof(*s));
}

/* =========================================================================
 * Bucket scan — SWAR match8
 * ========================================================================= */

int htc_bucket_scan(htc_bucket_t *b, htc_bucket_meta_t *m,
                    htc_arena_t *a, uint64_t h, htc_scan_mode_t mode)
{
    uint16_t tag     = htc_tag16(h);
    uint8_t  partial = htc_partial8(tag);
    uint64_t ctrl    = htc_ctrl_load(m);
    uint64_t mask    = htc_match8(ctrl, partial);

    while (mask) {
        unsigned i = htc_ctz_candidate(mask);
        mask = htc_clear_candidate(mask, i);

        uint64_t w = __atomic_load_n(&b->slot[i], __ATOMIC_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_in_secondary(w) != (mode == HTC_SCAN_SECONDARY))
            continue;
        if (htc_slot_tag(w) != tag) continue;

        htc_record_t *rec = htc_arena_ptr(a, htc_slot_index(w));
        if (rec->full_hash != h) continue;

        return (int)i;
    }
    return -1;
}

/* =========================================================================
 * Internal helpers — placement & rehash
 * ========================================================================= */

static bool htc_place_entry(htc_table_t *t, uint64_t h, uint64_t slot_word)
{
    uint16_t tag     = htc_tag16(h);
    uint8_t  partial = htc_partial8(tag);
    uint32_t b1      = (uint32_t)(h & t->bucket_mask);
    uint32_t b2      = htc_alt_bucket(b1, h, tag) & t->bucket_mask;

    /* try primary */
    {
        htc_bucket_meta_t *m = &t->meta[b1];
        uint64_t ctrl = htc_ctrl_load(m);
        uint64_t em   = htc_match8(ctrl, 0);
        if (em) {
            unsigned si = htc_ctz_candidate(em);
            __atomic_store_n(&t->buckets[b1].slot[si], slot_word,
                             __ATOMIC_RELEASE);
            htc_ctrl_set(m, si, partial);
            return true;
        }
    }

    /* try secondary */
    {
        htc_bucket_meta_t *m = &t->meta[b2];
        uint64_t ctrl = htc_ctrl_load(m);
        uint64_t em   = htc_match8(ctrl, 0);
        if (em) {
            unsigned si = htc_ctz_candidate(em);
            uint64_t sw = slot_word | HTC_SLOT_SEC_MASK;
            __atomic_store_n(&t->buckets[b2].slot[si], sw,
                             __ATOMIC_RELEASE);
            htc_ctrl_set(m, si, partial);
            htc_remap_inc(&t->meta[b1], tag);
            return true;
        }
    }

    /* stash */
    return htc_stash_insert(&t->stash, slot_word) >= 0;
}

static bool htc_rehash_place(htc_table_t *t, uint64_t h, uint64_t slot_word)
{
    return htc_place_entry(t, h, slot_word);
}

bool htc_grow(htc_table_t *t)
{
    uint32_t new_count = t->num_buckets * 2;
    size_t   bsz       = (size_t)new_count * sizeof(htc_bucket_t);
    size_t   msz       = (size_t)new_count * sizeof(htc_bucket_meta_t);

    htc_bucket_t      *new_buckets = (htc_bucket_t *)calloc(1, bsz);
    htc_bucket_meta_t *new_meta    = (htc_bucket_meta_t *)calloc(1, msz);
    if (!new_buckets || !new_meta) {
        free(new_buckets);
        free(new_meta);
        return false;
    }

    uint32_t      old_bucket_count = t->num_buckets;
    htc_bucket_t *old_buckets      = t->buckets;
    htc_bucket_meta_t *old_meta    = t->meta;
    htc_stash_t        old_stash   = t->stash;

    t->buckets     = new_buckets;
    t->meta        = new_meta;
    t->num_buckets = new_count;
    t->bucket_mask = new_count - 1;
    memset(&t->stash, 0, sizeof(t->stash));

    bool ok     = true;
    uint32_t bi;

    /* rehash old bucket slots */
    for (bi = 0; bi < old_bucket_count && ok; bi++) {
        unsigned si;
        for (si = 0; si < HTC_BUCKET_SLOTS && ok; si++) {
            uint64_t w = __atomic_load_n(&old_buckets[bi].slot[si],
                                         __ATOMIC_RELAXED);
            if (!htc_slot_live(w)) continue;
            htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
            ok = htc_rehash_place(t, rec->full_hash, w);
        }
    }

    /* rehash old stash */
    if (ok) {
        uint32_t i;
        for (i = 0; i < old_stash.size && ok; i++) {
            uint64_t w = __atomic_load_n(&old_stash.slots[i],
                                         __ATOMIC_RELAXED);
            if (!htc_slot_live(w)) continue;
            htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
            ok = htc_rehash_place(t, rec->full_hash, w);
        }
    }

    if (!ok) {
        t->buckets     = old_buckets;
        t->meta        = old_meta;
        t->num_buckets = old_bucket_count;
        t->bucket_mask = old_bucket_count - 1;
        t->stash       = old_stash;
        free(new_buckets);
        free(new_meta);
        return false;
    }

    free(old_buckets);
    free(old_meta);
    htc_stash_destroy(&old_stash);
    return true;
}

/* =========================================================================
 * Table lifecycle
 * ========================================================================= */

htc_table_t *htc_create(const htc_config_t *cfg)
{
    return htc_create_with_arena(cfg, NULL);
}

htc_table_t *htc_create_with_arena(const htc_config_t *cfg,
                                   struct arena *arena)
{
    (void)arena;

    htc_config_t c;
    if (cfg) {
        c = *cfg;
    } else {
        c.initial_buckets  = 4;
        c.max_load_factor  = 0.75;
    }
    if (c.initial_buckets < 4) c.initial_buckets = 4;

    uint32_t nb = 1;
    while (nb < c.initial_buckets) nb <<= 1;

    htc_table_t *t = (htc_table_t *)calloc(1, sizeof(htc_table_t));
    if (!t) return NULL;

    t->buckets = (htc_bucket_t *)calloc((size_t)nb, sizeof(htc_bucket_t));
    if (!t->buckets) { free(t); return NULL; }

    t->meta = (htc_bucket_meta_t *)calloc((size_t)nb,
                                          sizeof(htc_bucket_meta_t));
    if (!t->meta) { free(t->buckets); free(t); return NULL; }

    t->arena = (htc_arena_t *)calloc(1, sizeof(htc_arena_t));
    if (!t->arena) { free(t->meta); free(t->buckets); free(t); return NULL; }

    t->num_buckets    = nb;
    t->bucket_mask    = nb - 1;
    t->max_load_factor = c.max_load_factor;
    t->size           = 0;
    t->allocator      = NULL;
    return t;
}

void htc_destroy(htc_table_t *t)
{
    if (!t) return;
    htc_arena_destroy(t->arena);
    htc_stash_destroy(&t->stash);
    free(t->buckets);
    free(t->meta);
    free(t->arena);
    free(t);
}

void htc_clear(htc_table_t *t)
{
    if (!t) return;
    htc_arena_destroy(t->arena);
    htc_stash_destroy(&t->stash);
    memset(t->buckets, 0, (size_t)t->num_buckets * sizeof(htc_bucket_t));
    memset(t->meta, 0, (size_t)t->num_buckets * sizeof(htc_bucket_meta_t));
    t->size = 0;
}

/* =========================================================================
 * Public API — insert / upsert / update / find / remove / size
 * ========================================================================= */

bool htc_insert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return false;
    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & t->bucket_mask);

    /* duplicate check */
    if (htc_bucket_scan(&t->buckets[b1], &t->meta[b1],
                        t->arena, hash, HTC_SCAN_PRIMARY) >= 0)
        return false;
    if (htc_must_check_secondary(&t->meta[b1], tag)) {
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & t->bucket_mask;
        if (htc_bucket_scan(&t->buckets[b2], &t->meta[b2],
                            t->arena, hash, HTC_SCAN_SECONDARY) >= 0)
            return false;
    }
    if (htc_stash_find(&t->stash, t->arena, hash) >= 0)
        return false;

    /* allocate arena record */
    uint32_t idx = htc_arena_alloc(t->arena, hash, value);
    if (idx == UINT32_MAX) return false;

    uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

    if (!htc_place_entry(t, hash, slot_word)) {
        htc_arena_free(t->arena, idx);
        return false;
    }

    t->size++;

    double lf = (double)t->size / (double)t->num_buckets;
    if (lf > t->max_load_factor) htc_grow(t);

    return true;
}

bool htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & t->bucket_mask);

    /* try to find existing */
    {
        int si = htc_bucket_scan(&t->buckets[b1], &t->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }
    if (htc_must_check_secondary(&t->meta[b1], tag)) {
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & t->bucket_mask;
        int si = htc_bucket_scan(&t->buckets[b2], &t->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }
    {
        int si = htc_stash_find(&t->stash, t->arena, hash);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->stash.slots[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }

    /* not found — insert */
    uint32_t idx = htc_arena_alloc(t->arena, hash, value);
    if (idx == UINT32_MAX) return false;

    uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

    if (!htc_place_entry(t, hash, slot_word)) {
        htc_arena_free(t->arena, idx);
        return false;
    }

    t->size++;

    double lf = (double)t->size / (double)t->num_buckets;
    if (lf > t->max_load_factor) htc_grow(t);

    return true;
}

bool htc_update(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return false;
    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & t->bucket_mask);

    {
        int si = htc_bucket_scan(&t->buckets[b1], &t->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }
    if (htc_must_check_secondary(&t->meta[b1], tag)) {
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & t->bucket_mask;
        int si = htc_bucket_scan(&t->buckets[b2], &t->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }
    {
        int si = htc_stash_find(&t->stash, t->arena, hash);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->stash.slots[si],
                                         __ATOMIC_ACQUIRE);
            htc_arena_ptr(t->arena, htc_slot_index(w))->user_value = value;
            return true;
        }
    }
    return false;
}

bool htc_find(const htc_table_t *t_, uint64_t hash, uint64_t *out_value)
{
    if (!t_) return false;
    htc_table_t *t = (htc_table_t *)t_;
    uint16_t    tag  = htc_tag16(hash);
    uint32_t     b1  = (uint32_t)(hash & t->bucket_mask);

    {
        int si = htc_bucket_scan(&t->buckets[b1], &t->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            if (out_value)
                *out_value = htc_arena_ptr(t->arena,
                                           htc_slot_index(w))->user_value;
            return true;
        }
    }
    if (htc_must_check_secondary(&t->meta[b1], tag)) {
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & t->bucket_mask;
        int si = htc_bucket_scan(&t->buckets[b2], &t->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            if (out_value)
                *out_value = htc_arena_ptr(t->arena,
                                           htc_slot_index(w))->user_value;
            return true;
        }
    }
    {
        int si = htc_stash_find(&t->stash, t->arena, hash);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->stash.slots[si],
                                         __ATOMIC_ACQUIRE);
            if (out_value)
                *out_value = htc_arena_ptr(t->arena,
                                           htc_slot_index(w))->user_value;
            return true;
        }
    }

    if (out_value) *out_value = 0;
    return false;
}

bool htc_remove(htc_table_t *t, uint64_t hash)
{
    if (!t) return false;
    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & t->bucket_mask);

    {
        int si = htc_bucket_scan(&t->buckets[b1], &t->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            __atomic_store_n(&t->buckets[b1].slot[si],
                             htc_slot_empty_word(), __ATOMIC_RELEASE);
            htc_ctrl_clear(&t->meta[b1], (unsigned)si);
            htc_remap_dec(&t->meta[b1]);
            htc_arena_free(t->arena, idx);
            t->size--;
            return true;
        }
    }
    if (htc_must_check_secondary(&t->meta[b1], tag)) {
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & t->bucket_mask;
        int si = htc_bucket_scan(&t->buckets[b2], &t->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            __atomic_store_n(&t->buckets[b2].slot[si],
                             htc_slot_empty_word(), __ATOMIC_RELEASE);
            htc_ctrl_clear(&t->meta[b2], (unsigned)si);
            htc_remap_dec(&t->meta[b2]);
            htc_arena_free(t->arena, idx);
            t->size--;
            return true;
        }
    }
    {
        int si = htc_stash_find(&t->stash, t->arena, hash);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&t->stash.slots[si],
                                         __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_stash_remove_at(&t->stash, (unsigned)si);
            htc_arena_free(t->arena, idx);
            t->size--;
            return true;
        }
    }
    return false;
}

size_t htc_size(const htc_table_t *t)
{
    if (!t) return 0;
    return t->size;
}
