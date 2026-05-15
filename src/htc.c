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
#include <limits.h>
#include <assert.h>

/* Per-thread front cache — thread-local to avoid cache-line bouncing */
_Thread_local htc_front_cache_t htc_thread_cache = {0};

/* =========================================================================
 * Arena — flat array of records + free-list
 * ========================================================================= */

uint32_t htc_arena_alloc(htc_arena_t *a, uint64_t hash, uint64_t value)
{
    htc_spin_lock(&a->lock);
    if (a->free_count > 0) {
        a->free_count--;
        uint32_t idx = a->free_idx[a->free_count];
        a->records[idx].generation++;
        a->records[idx].full_hash  = hash;
        __atomic_store_n(&a->records[idx].flags, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&a->records[idx].user_value, value, __ATOMIC_RELAXED);
        htc_spin_unlock(&a->lock);
        return idx;
    }
    if (a->count >= a->capacity) {
        uint32_t new_cap = a->capacity ? a->capacity * 2 : 64;
        htc_record_t *nr = (htc_record_t *)realloc(
            a->records, (size_t)new_cap * sizeof(htc_record_t));
        if (!nr) { htc_spin_unlock(&a->lock); return UINT32_MAX; }
        a->records  = nr;
        a->capacity = new_cap;
    }
    uint32_t idx = a->count++;
    a->records[idx].full_hash  = hash;
    __atomic_store_n(&a->records[idx].flags, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&a->records[idx].user_value, value, __ATOMIC_RELAXED);
    htc_spin_unlock(&a->lock);
    return idx;
}

void htc_arena_free(htc_arena_t *a, uint32_t idx)
{
    htc_spin_lock(&a->lock);
    a->records[idx].full_hash  = 0;
    a->records[idx].generation++;
    __atomic_store_n(&a->records[idx].user_value, 0, __ATOMIC_RELAXED);
    if (a->free_count >= a->free_cap) {
        uint32_t new_cap = a->free_cap ? a->free_cap * 2 : 64;
        uint32_t *nf = (uint32_t *)realloc(
            a->free_idx, (size_t)new_cap * sizeof(uint32_t));
        if (!nf) { htc_spin_unlock(&a->lock); return; }
        a->free_idx = nf;
        a->free_cap = new_cap;
    }
    a->free_idx[a->free_count++] = idx;
    htc_spin_unlock(&a->lock);
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
        htc_spin_lock(&s->lock);
        /* Double-check after acquiring lock (another thread might have grown) */
        if (s->size >= s->capacity) {
            if (s->capacity >= HTC_STASH_MAX) {
                htc_spin_unlock(&s->lock);
                return -1;
            }
            uint32_t new_cap = s->capacity ? s->capacity * 2 : HTC_STASH_GROW;
            if (new_cap > HTC_STASH_MAX) new_cap = HTC_STASH_MAX;
            s->full_events++;
            _Atomic uint64_t *ns = (_Atomic uint64_t *)realloc(
                s->slots, (size_t)new_cap * sizeof(uint64_t));
            if (!ns) { htc_spin_unlock(&s->lock); return -1; }
            s->slots    = ns;
            s->capacity = new_cap;
        }
        htc_spin_unlock(&s->lock);
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

/* Try to shrink the stash if it has been empty for many maintenance epochs. */
void htc_stash_maintain(htc_stash_t *s)
{
    if (s->size == 0) {
        s->empty_epochs++;
        if (s->empty_epochs >= 4 && s->capacity > HTC_STASH_MIN) {
            uint32_t new_cap = s->capacity / 2;
            if (new_cap < HTC_STASH_MIN) new_cap = HTC_STASH_MIN;
            _Atomic uint64_t *ns = (_Atomic uint64_t *)realloc(
                s->slots, (size_t)new_cap * sizeof(uint64_t));
            if (ns) {
                s->slots    = ns;
                s->capacity = new_cap;
                s->full_events = 0;
            }
            s->empty_epochs = 0;
        }
    } else {
        s->empty_epochs = 0;
    }
}

/* =========================================================================
 * Epoch-based record retirement
 * ========================================================================= */

void htc_epoch_retire(htc_epoch_ctl_t *ep, htc_arena_t *a, uint32_t arena_idx)
{
    if (!ep) { htc_arena_free(a, arena_idx); return; }
    uint64_t cur = __atomic_load_n(&ep->global_epoch, __ATOMIC_RELAXED);

    htc_retire_node_t *n = (htc_retire_node_t *)malloc(sizeof(htc_retire_node_t));
    if (!n) { htc_arena_free(a, arena_idx); return; }
    n->arena_idx    = arena_idx;
    n->retire_epoch = cur + 2;
    n->next         = NULL;

    /* Push to retire list (lock-free stack via CAS) */
    htc_retire_node_t *old = __atomic_load_n(&ep->retire_head, __ATOMIC_RELAXED);
    do {
        n->next = old;
    } while (!__atomic_compare_exchange_n(&ep->retire_head, &old, n,
                                          false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

void htc_epoch_collect(htc_epoch_ctl_t *ep, htc_arena_t *a)
{
    if (!ep || !a) return;

    /* Compute the minimum non-zero thread epoch */
    uint64_t min_ep = UINT64_MAX;
    for (int i = 0; i < HTC_EPOCH_MAX_THREADS; i++) {
        uint64_t te = __atomic_load_n(&ep->thread_epoch[i], __ATOMIC_ACQUIRE);
        if (te != 0 && te < min_ep) min_ep = te;
    }

    /* Pop entire retire list */
    htc_retire_node_t *head = __atomic_exchange_n(&ep->retire_head, NULL,
                                                   __ATOMIC_ACQUIRE);

    /* Split into safe-to-free (retire_epoch < min_ep) and keep */
    htc_retire_node_t *keep = NULL, *keep_tail = NULL;
    htc_retire_node_t *free_list = NULL;

    while (head) {
        htc_retire_node_t *next = head->next;
        if (head->retire_epoch < min_ep) {
            head->next = free_list;
            free_list = head;
        } else {
            head->next = NULL;
            if (keep_tail) keep_tail->next = head;
            else           keep = head;
            keep_tail = head;
        }
        head = next;
    }

    /* Free safe items */
    while (free_list) {
        htc_retire_node_t *n = free_list;
        free_list = n->next;
        htc_arena_free(a, n->arena_idx);
        free(n);
    }

    /* Re-pend keep list */
    if (keep) {
        htc_retire_node_t *old = NULL;
        do {
            keep_tail->next = old;
        } while (!__atomic_compare_exchange_n(&ep->retire_head, &old, keep,
                                              false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    }
}

void htc_epoch_advance(htc_epoch_ctl_t *ep)
{
    if (!ep) return;
    __atomic_fetch_add(&ep->global_epoch, 1, __ATOMIC_RELEASE);
}

/* Thread-local epoch slot. UINT_MAX = unregistered.
 * Registered on first call to htc_epoch_pin(). */
static _Thread_local unsigned htc_epoch_tid = UINT_MAX;

static void htc_epoch_register(htc_epoch_ctl_t *ep)
{
    for (unsigned i = 0; i < HTC_EPOCH_MAX_THREADS; i++) {
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(&ep->thread_epoch[i], &zero, 1,
                                        false, __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            htc_epoch_tid = i;
            return;
        }
    }
}

uint64_t htc_epoch_pin(htc_epoch_ctl_t *ep)
{
    if (htc_epoch_tid >= HTC_EPOCH_MAX_THREADS)
        htc_epoch_register(ep);
    if (htc_epoch_tid >= HTC_EPOCH_MAX_THREADS)
        return 0;
    uint64_t e = __atomic_load_n(&ep->global_epoch, __ATOMIC_ACQUIRE);
    __atomic_store_n(&ep->thread_epoch[htc_epoch_tid], e, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    return e;
}

void htc_epoch_unpin(htc_epoch_ctl_t *ep)
{
    if (htc_epoch_tid < HTC_EPOCH_MAX_THREADS)
        __atomic_store_n(&ep->thread_epoch[htc_epoch_tid], 0, __ATOMIC_RELEASE);
}

/* =========================================================================
 * Lazy chunk migration (Phase 6)
 * ========================================================================= */

/* Start a resize: allocate a new generation, link the old one, publish. */
bool htc_resize_start(htc_table_t *t, uint32_t new_num_buckets)
{
    htc_table_gen_t *old_gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!old_gen) return false;

    if (new_num_buckets < 4) new_num_buckets = 4;
    uint32_t nb = 1;
    while (nb < new_num_buckets) nb <<= 1;

    htc_table_gen_t *gen = (htc_table_gen_t *)calloc(1, sizeof(htc_table_gen_t));
    if (!gen) return false;

    gen->buckets = (htc_bucket_t *)calloc((size_t)nb, sizeof(htc_bucket_t));
    gen->meta    = (htc_bucket_meta_t *)calloc((size_t)nb, sizeof(htc_bucket_meta_t));
    if (!gen->buckets || !gen->meta) {
        free(gen->buckets); free(gen->meta); free(gen);
        return false;
    }

    /* Share the arena and shards from the old generation */
    gen->arena        = old_gen->arena;
    gen->shards       = old_gen->shards;
    gen->shard_count  = old_gen->shard_count;
    gen->bucket_mask  = nb - 1;
    gen->num_buckets  = nb;
    gen->chunk_count  = (nb + HTC_CHUNK_SIZE - 1) / HTC_CHUNK_SIZE;
    gen->old          = old_gen;

    /* Freeze old generation — writers must now target the new gen */
    __atomic_store_n(&old_gen->state, HTC_GEN_OLD, __ATOMIC_RELEASE);

    /* Allocate migrated bitmap (one bit per chunk) */
    size_t bm_words = (gen->chunk_count + 63) / 64;
    gen->migrated_bitmap = (uint64_t *)calloc(bm_words, sizeof(uint64_t));
    if (!gen->migrated_bitmap) {
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELEASE);
        free(gen->meta); free(gen->buckets); free(gen);
        return false;
    }
    __atomic_store_n(&gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELEASE);

    /* Publish the new generation */
    __atomic_store_n(&t->current_gen, gen, __ATOMIC_RELEASE);
    return true;
}

/* Migrate a single chunk from the old generation to the new one. */
void htc_migrate_chunk(htc_table_t *t, uint32_t chunk_id)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    htc_table_gen_t *old = g->old;
    if (!old) return;

    uint32_t start = chunk_id * HTC_CHUNK_SIZE;
    uint32_t end   = start + HTC_CHUNK_SIZE;
    if (end > old->num_buckets) end = old->num_buckets;
    if (end > g->num_buckets) end = g->num_buckets;

    /* Migrate each bucket in the chunk */
    for (uint32_t bi = start; bi < end; bi++) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&old->buckets[bi].slot[si], __ATOMIC_ACQUIRE);
            if (!htc_slot_live(w)) continue;

            htc_record_t *rec = htc_arena_ptr(old->arena, htc_slot_index(w));
            uint64_t h = rec->full_hash;

            /* Find the correct bucket in the new generation */
            uint16_t tag = htc_tag16(h);
            uint32_t b1 = (uint32_t)(h & g->bucket_mask);
            uint32_t b2 = htc_alt_bucket(b1, h, tag) & g->bucket_mask;
            bool in_sec = htc_slot_in_secondary(w);

            uint32_t target = in_sec ? b2 : b1;
            uint64_t nw = htc_slot_pack(htc_slot_index(w), tag, HTC_STATE_LIVE, in_sec);

            /* Suppress duplicates: concurrent writer may have already placed
             * this entry in the new gen — skip copy, just clear old slot. */
            if (htc_bucket_scan(&g->buckets[b1], &g->meta[b1],
                                g->arena, h, HTC_SCAN_PRIMARY) >= 0) {
                __atomic_store_n(&old->buckets[bi].slot[si],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
                htc_ctrl_clear(&old->meta[bi], si);
                continue;
            }
            if (htc_must_check_secondary(&g->meta[b1], tag) &&
                htc_bucket_scan(&g->buckets[b2], &g->meta[b2],
                                g->arena, h, HTC_SCAN_SECONDARY) >= 0) {
                __atomic_store_n(&old->buckets[bi].slot[si],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
                htc_ctrl_clear(&old->meta[bi], si);
                continue;
            }

            htc_seq_guard_t sg = htc_bucket_seq_begin(&g->meta[target]);

            for (unsigned s = 0; s < HTC_BUCKET_SLOTS; s++) {
                uint64_t exp = htc_slot_empty_word();
                if (htc_atomic_cas(&g->buckets[target].slot[s], &exp, nw,
                                   __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    htc_ctrl_set(&g->meta[target], (unsigned)s, htc_partial8(tag));

                    if (in_sec)
                        htc_remap_inc(&g->meta[b1], tag);

                    __atomic_store_n(&old->buckets[bi].slot[si],
                                     htc_slot_empty_word(), __ATOMIC_RELEASE);
                    htc_ctrl_clear(&old->meta[bi], si);
                    break;
                }
            }

            htc_bucket_seq_end(&g->meta[target], sg);
        }
    }

    /* Mark chunk as migrated */
    unsigned word = chunk_id / 64;
    unsigned bit  = chunk_id % 64;
    __atomic_fetch_or(&g->migrated_bitmap[word], 1ULL << bit, __ATOMIC_RELEASE);
}

/* Called by writers to ensure a bucket's chunk has been migrated.
 * Uses atomic test-and-set on the migrated bitmap to prevent
 * double-migration races between concurrent callers. */
void htc_ensure_chunk_migrated(htc_table_t *t, uint32_t bucket_id)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!g || !g->old) return;

    uint32_t chunk = htc_chunk_of(bucket_id);
    if (chunk >= g->chunk_count) return;

    unsigned word = chunk / 64;
    unsigned bit  = chunk % 64;
    uint64_t mask = 1ULL << bit;

    /* Atomic test-and-set: only the caller that sets the bit proceeds.
     * If the bit was already set, another thread already claimed it. */
    uint64_t old = __atomic_fetch_or(&g->migrated_bitmap[word], mask, __ATOMIC_ACQ_REL);
    if (!(old & mask))
        htc_migrate_chunk(t, chunk);
}

/* Finish resize after all chunks migrated: free the old generation. */
void htc_resize_finish(htc_table_t *t)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!g || !g->old) return;

    /* Check if all chunks are migrated */
    bool all_done = true;
    for (uint32_t i = 0; i < g->chunk_count; i++) {
        unsigned word = i / 64;
        unsigned bit  = i % 64;
        if (!(__atomic_load_n(&g->migrated_bitmap[word], __ATOMIC_ACQUIRE) & (1ULL << bit))) {
            all_done = false;
            break;
        }
    }
    if (!all_done) return;

    /* All migrated — free old generation's buckets/meta, but keep arena
     * and shards (shared with the current generation). */
    htc_table_gen_t *old = g->old;
    g->old = NULL;
    htc_epoch_collect(t->epoch, old->arena);
    htc_epoch_advance(t->epoch);
    htc_epoch_collect(t->epoch, old->arena);
    free(old->meta);
    free(old->buckets);
    free(old->migrated_bitmap);
    free(old);
}

/* Search recursively through old generations for a key. */
bool htc_find_in_old_gen(const htc_table_gen_t *g, htc_arena_t *arena,
                          uint64_t hash, uint64_t *out_value)
{
    if (!g) return false;
    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & g->bucket_mask);
    uint32_t  b2  = htc_alt_bucket(b1, hash, tag) & g->bucket_mask;

    /* Check primary bucket */
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si], __ATOMIC_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        if (htc_slot_in_secondary(w)) continue;
        htc_record_t *r = htc_arena_ptr(arena, htc_slot_index(w));
        if (r->full_hash == hash) {
            if (out_value) *out_value = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
            return true;
        }
    }

    /* Check secondary */
    if (htc_must_check_secondary(&g->meta[b1], tag)) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si], __ATOMIC_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            if (htc_slot_tag(w) != tag) continue;
            if (!htc_slot_in_secondary(w)) continue;
            htc_record_t *r = htc_arena_ptr(arena, htc_slot_index(w));
            if (r->full_hash == hash) {
                if (out_value) *out_value = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
                return true;
            }
        }
    }

    /* Recurse to older generation */
    return htc_find_in_old_gen(g->old, arena, hash, out_value);
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
 * Seq-validated bucket scan for lock-free reads (Phase 2)
 * Returns slot index on match, -1 on miss.
 * Retries if the bucket sequence changes during the scan.
 * ========================================================================= */

static int htc_bucket_scan_seq(htc_bucket_t *b, htc_bucket_meta_t *m,
                                htc_arena_t *a, uint64_t h,
                                htc_scan_mode_t mode)
{
    uint16_t want_tag      = htc_tag16(h);
    uint8_t  want_p8       = htc_partial8(want_tag);
    bool     want_secondary = (mode == HTC_SCAN_SECONDARY);

retry:
    {
        uint32_t s0 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (s0 & HTC_SEQ_BUSY) goto retry;

        uint64_t ctrl       = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
        uint64_t candidates = htc_match8(ctrl, want_p8);

        while (candidates) {
            unsigned i = htc_ctz_candidate(candidates);
            candidates = htc_clear_candidate(candidates, i);

            uint64_t w = __atomic_load_n(&b->slot[i], __ATOMIC_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            if (htc_slot_tag(w) != want_tag) continue;
            if (htc_slot_in_secondary(w) != want_secondary) continue;

            htc_record_t *r = htc_arena_ptr(a, htc_slot_index(w));
            if (r->full_hash != h) continue;

            uint32_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
            if (s0 != s1 || (s1 & HTC_SEQ_BUSY)) goto retry;

            return (int)i;
        }

        uint32_t s1 = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
        if (s0 != s1 || (s1 & HTC_SEQ_BUSY)) goto retry;
    }
    return -1;
}

/* =========================================================================
 * BFS displacement (Phase 4)
 *
 * Three-phase protocol:
 *   1. Search:  find candidate path without mutating.
 *   2. Validate: lock touched shards, prove the path is still valid.
 *   3. Commit:   move the vacancy backward under seq protection.
 * ========================================================================= */

/* BFS queue entry for the displacement search */
typedef struct {
    uint32_t bucket;
    int16_t  parent;       /* index into queue of parent, -1 for root */
    uint8_t  parent_slot;  /* slot in parent bucket whose occupant moves here */
    uint8_t  depth;
} htc_bfs_node_t;

/* A validated move: victim at (from_bucket, from_slot) goes to (to_bucket, to_slot) */
typedef struct {
    uint32_t from_bucket;
    uint8_t  from_slot;
    uint32_t to_bucket;
    uint8_t  to_slot;
} ht_move_t;

/* A complete displacement path */
typedef struct {
    ht_move_t  moves[HTC_BFS_MAX_PATH];
    uint8_t    move_count;
    uint32_t   insert_bucket;
    uint8_t    insert_slot;
} ht_cuckoo_path_t;

/* Build the path in vacancy-backward commit order.
 * moves[0] goes to the empty slot; each subsequent move goes to the
 * slot freed by the previous move. */
static bool build_path(htc_bfs_node_t *nodes, int node_i,
                       uint32_t empty_bucket, uint8_t empty_slot,
                       ht_cuckoo_path_t *out)
{
    ht_move_t tmp[HTC_BFS_MAX_PATH];
    uint8_t n = 0;
    uint32_t to_bucket = empty_bucket;
    uint8_t  to_slot   = empty_slot;

    while (nodes[node_i].parent >= 0) {
        htc_bfs_node_t *child  = &nodes[node_i];
        htc_bfs_node_t *parent = &nodes[child->parent];

        if (n >= HTC_BFS_MAX_PATH) return false;

        tmp[n].from_bucket = parent->bucket;
        tmp[n].from_slot   = child->parent_slot;
        tmp[n].to_bucket   = to_bucket;
        tmp[n].to_slot     = to_slot;
        n++;

        to_bucket = parent->bucket;
        to_slot   = child->parent_slot;
        node_i    = child->parent;
    }

    /* tmp[] is already in commit order: moves[0] writes to the empty slot. */
    memcpy(out->moves, tmp, n * sizeof(tmp[0]));
    out->move_count   = n;
    out->insert_bucket = to_bucket;
    out->insert_slot   = to_slot;
    return true;
}

/* BFS search: find a candidate displacement path without mutating.
 * Uses acquire loads to observe published slot state. */
static bool bfs_find_path(htc_table_t *t,
                          uint32_t b1, uint32_t b2,
                          ht_cuckoo_path_t *out)
{
    htc_bfs_node_t nodes[HTC_BFS_BUCKET_BUDGET];
    uint32_t visited[HTC_BFS_BUCKET_BUDGET];
    uint16_t visited_len = 0;
    uint16_t head = 0, tail = 0;

    /* Roots: the two candidate buckets for the new key */
    nodes[tail].bucket = b1;
    nodes[tail].parent = -1;
    nodes[tail].parent_slot = 0;
    nodes[tail].depth = 0;
    tail++;

    /* Mark b1 as visited */
    visited[visited_len++] = b1;

    if (b2 != b1) {
        nodes[tail].bucket = b2;
        nodes[tail].parent = -1;
        nodes[tail].parent_slot = 0;
        nodes[tail].depth = 0;
        tail++;
        visited[visited_len++] = b2;
    }

    while (head < tail && tail < HTC_BFS_BUCKET_BUDGET) {
        htc_bfs_node_t *n = &nodes[head++];
        htc_bucket_t *b = &t->buckets[n->bucket];

        /* 1. Check for an empty slot in this bucket */
        for (uint8_t s = 0; s < HTC_BUCKET_SLOTS; s++) {
            uint64_t w = __atomic_load_n(&b->slot[s], __ATOMIC_ACQUIRE);
            if (htc_slot_empty(w)) {
                /* Found empty slot — reconstruct path to here */
                return build_path(nodes, (int)(head - 1), n->bucket, s, out);
            }
        }

        if (n->depth >= HTC_BFS_MAX_DEPTH) continue;

        /* 2. Expand by considering evicting each live slot.
         * Single pass: collect live words + slot indices, check for cold. */
        uint64_t live_w[HTC_BUCKET_SLOTS];
        uint8_t  live_s[HTC_BUCKET_SLOTS]; /* original slot indices */
        uint8_t  live_n = 0;
        bool     has_cold = false;
        for (uint8_t s = 0; s < HTC_BUCKET_SLOTS; s++) {
            uint64_t w = __atomic_load_n(&b->slot[s], __ATOMIC_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            live_w[live_n] = w;
            live_s[live_n] = s;
            live_n++;
            if (!(w & HTC_SLOT_HOT_MASK)) has_cold = true;
        }

        for (uint8_t li = 0; li < live_n && tail < HTC_BFS_BUCKET_BUDGET; li++) {
            uint64_t w = live_w[li];
            if ((w & HTC_SLOT_HOT_MASK) && has_cold) continue;

            htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(w));
            uint64_t vh = r->full_hash;
            uint16_t vtag = htc_tag16(vh);
            uint32_t vp = (uint32_t)(vh & t->bucket_mask);
            uint32_t vs = htc_alt_bucket(vp, vh, vtag) & t->bucket_mask;

            uint32_t alt;
            if (n->bucket == vp)       alt = vs;
            else if (n->bucket == vs)  alt = vp;
            else                       continue;

            if (alt == n->bucket) continue;

            bool seen = false;
            for (uint16_t vi = 0; vi < visited_len; vi++)
                if (visited[vi] == alt) { seen = true; break; }
            if (seen) continue;

            visited[visited_len++] = alt;
            nodes[tail].bucket = alt;
            nodes[tail].parent = (int16_t)(head - 1);
            nodes[tail].parent_slot = live_s[li];
            nodes[tail].depth = n->depth + 1;
            tail++;

            /* Prefetch the alternate bucket's meta and first slot cache line */
            __builtin_prefetch(&t->meta[alt], 0, 1);
            __builtin_prefetch(&t->buckets[alt], 0, 1);
        }
    }

    return false;
}

/* Validate a candidate path after locking.
 * Returns true only if every move is still legal. */
static bool validate_path_locked(htc_table_t *t, ht_cuckoo_path_t *path,
                                  uint64_t *expected_src)
{
    /* First destination must be empty */
    if (path->move_count > 0) {
        uint64_t dst = __atomic_load_n(
            &t->buckets[path->moves[0].to_bucket].slot[path->moves[0].to_slot],
            __ATOMIC_ACQUIRE);
        if (!htc_slot_empty(dst)) return false;
    } else {
        uint64_t dst = __atomic_load_n(
            &t->buckets[path->insert_bucket].slot[path->insert_slot],
            __ATOMIC_ACQUIRE);
        return htc_slot_empty(dst);
    }

    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src = __atomic_load_n(
            &t->buckets[m->from_bucket].slot[m->from_slot], __ATOMIC_ACQUIRE);

        if (!htc_slot_live(src)) return false;

        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src));
        uint64_t h = r->full_hash;
        uint16_t tag = htc_tag16(h);
        uint32_t p = (uint32_t)(h & t->bucket_mask);
        uint32_t s = htc_alt_bucket(p, h, tag) & t->bucket_mask;

        /* Source must be in one of its two legal buckets */
        if (!(m->from_bucket == p || m->from_bucket == s)) return false;
        /* Destination must be the other legal bucket */
        if (!(m->to_bucket == p || m->to_bucket == s)) return false;
        /* Cannot move within same bucket */
        if (m->from_bucket == m->to_bucket) return false;

        expected_src[i] = src;
    }
    return true;
}

/* Commit a validated path under locks.
 * Applies moves in vacancy-backward order:
 *   moves[0] → first destination (originally empty)
 *   moves[1] → slot freed by moves[0]
 *   ...
 *   new entry → slot freed by last move */
static bool commit_path_locked(htc_table_t *t, ht_cuckoo_path_t *path,
                               uint64_t *expected_src,
                               uint64_t new_slot_word)
{
    /* Collect unique touched buckets for seq guards */
    uint32_t touched[HTC_BFS_MAX_PATH * 2 + 2];
    uint8_t tn = 0;
    if (path->move_count > 0) {
        for (uint8_t i = 0; i < path->move_count; i++) {
            uint32_t buckets[2] = {path->moves[i].from_bucket, path->moves[i].to_bucket};
            for (int b = 0; b < 2; b++) {
                bool dup = false;
                for (uint8_t j = 0; j < tn; j++)
                    if (touched[j] == buckets[b]) { dup = true; break; }
                if (!dup) touched[tn++] = buckets[b];
            }
        }
    }
    /* Also include insert bucket */
    bool dup = false;
    for (uint8_t j = 0; j < tn; j++)
        if (touched[j] == path->insert_bucket) { dup = true; break; }
    if (!dup) touched[tn++] = path->insert_bucket;

    /* Mark all touched buckets seq busy */
    htc_seq_guard_t guards[HTC_BFS_MAX_PATH * 2 + 2];
    for (uint8_t i = 0; i < tn; i++)
        guards[i] = htc_bucket_seq_begin(&t->meta[touched[i]]);

    /* Phase 1: all slot mutations (batch for cache locality) */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];

        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint32_t p = (uint32_t)(r->full_hash & t->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        uint16_t etag = htc_tag16(r->full_hash);

        uint64_t dst_word = htc_slot_pack(htc_slot_index(src_word), etag,
                                           HTC_STATE_LIVE, in_sec);
        __atomic_store_n(&t->buckets[m->to_bucket].slot[m->to_slot],
                         dst_word, __ATOMIC_RELEASE);
        __atomic_store_n(&t->buckets[m->from_bucket].slot[m->from_slot],
                         htc_slot_empty_word(), __ATOMIC_RELEASE);
    }
    {
        htc_record_t *nr = htc_arena_ptr(t->arena, htc_slot_index(new_slot_word));
        uint16_t ntag = htc_tag16(nr->full_hash);
        bool nin_sec = (path->insert_bucket != (uint32_t)(nr->full_hash & t->bucket_mask));
        uint64_t nw = htc_slot_pack(htc_slot_index(new_slot_word), ntag,
                                     HTC_STATE_LIVE, nin_sec);
        __atomic_store_n(&t->buckets[path->insert_bucket].slot[path->insert_slot],
                         nw, __ATOMIC_RELEASE);
    }

    /* Phase 2: all ctrl updates (separate sweep) */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint16_t etag = htc_tag16(r->full_hash);
        uint8_t ep8 = htc_partial8(etag);

        htc_ctrl_set(&t->meta[m->to_bucket], m->to_slot, ep8);
        htc_ctrl_clear(&t->meta[m->from_bucket], m->from_slot);
    }
    {
        htc_record_t *nr = htc_arena_ptr(t->arena, htc_slot_index(new_slot_word));
        uint8_t np8 = htc_partial8(htc_tag16(nr->full_hash));
        htc_ctrl_set(&t->meta[path->insert_bucket], path->insert_slot, np8);
    }

    /* Phase 3: remap updates (deferred, not on critical path) */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint32_t p = (uint32_t)(r->full_hash & t->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        bool was_in_sec = htc_slot_in_secondary(src_word);
        uint16_t etag = htc_tag16(r->full_hash);

        if (was_in_sec && !in_sec)
            htc_remap_dec(&t->meta[p]);
        else if (!was_in_sec && in_sec)
            htc_remap_inc(&t->meta[p], etag);
    }
    {
        htc_record_t *nr = htc_arena_ptr(t->arena, htc_slot_index(new_slot_word));
        uint16_t ntag = htc_tag16(nr->full_hash);
        bool nin_sec = (path->insert_bucket != (uint32_t)(nr->full_hash & t->bucket_mask));
        if (nin_sec)
            htc_remap_inc(&t->meta[(uint32_t)(nr->full_hash & t->bucket_mask)], ntag);
    }

    /* End seq busy */
    for (uint8_t i = 0; i < tn; i++)
        htc_bucket_seq_end(&t->meta[touched[i]], guards[i]);

    return true;
}

/* ─── LFBCH-style lock-free BFS commit (§23) ─────────────────
 * Commits a BFS displacement path without holding shard locks.
 * Uses per-bucket seq guards + CAS on destination slots for
 * mutual exclusion. Falls back to locked commit on CAS failure.
 *
 * Returns true on success, false if rollback needed (caller must
 * re-acquire shard locks and use commit_path_locked). */
static bool commit_path_lfbch(htc_table_t *t, ht_cuckoo_path_t *path,
                              uint64_t *expected_src,
                              uint64_t new_slot_word)
{
    /* Vacancy-backward: moves[0] writes to the originally empty slot.
     * Each subsequent move writes to the slot freed by the previous move.
     * The new entry writes to the slot freed by the last move. */

    /* Phase 1: CAS-claim all destination slots and clear all source slots.
     * If any CAS fails, roll back completed moves and return false. */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        /* Load current source slot word — expected_src is not yet populated
         * (it gets filled by validate_path_locked in the locked fallback). */
        uint64_t src_word = __atomic_load_n(
            &t->buckets[m->from_bucket].slot[m->from_slot], __ATOMIC_ACQUIRE);
        expected_src[i] = src_word;

        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint32_t p = (uint32_t)(r->full_hash & t->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        uint16_t etag = htc_tag16(r->full_hash);
        uint64_t dst_word = htc_slot_pack(htc_slot_index(src_word), etag,
                                           HTC_STATE_LIVE, in_sec);

        /* Claim destination slot with CAS (fails if another writer took it) */
        uint64_t exp = htc_slot_empty_word();
        if (!htc_atomic_cas(&t->buckets[m->to_bucket].slot[m->to_slot],
                            &exp, dst_word,
                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            /* Rollback: reverse all completed moves */
            for (int8_t j = (int8_t)i - 1; j >= 0; j--) {
                ht_move_t *rj = &path->moves[j];
                uint64_t rj_src = expected_src[j];
                /* Restore source (clear destination, restore source) */
                __atomic_store_n(&t->buckets[rj->from_bucket].slot[rj->from_slot],
                                 rj_src, __ATOMIC_RELEASE);
                __atomic_store_n(&t->buckets[rj->to_bucket].slot[rj->to_slot],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
            }
            return false;
        }

        /* Seq-guard source clear so readers don't see the entry in both places */
        htc_seq_guard_t src_guard = htc_bucket_seq_begin(&t->meta[m->from_bucket]);
        __atomic_store_n(&t->buckets[m->from_bucket].slot[m->from_slot],
                         htc_slot_empty_word(), __ATOMIC_RELEASE);
        htc_bucket_seq_end(&t->meta[m->from_bucket], src_guard);
    }

    /* Place the new entry into the final vacancy slot */
    {
        uint64_t exp = htc_slot_empty_word();
        if (!htc_atomic_cas(&t->buckets[path->insert_bucket].slot[path->insert_slot],
                            &exp, new_slot_word,
                            __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
            /* Rollback all moves */
            for (int8_t j = (int8_t)path->move_count - 1; j >= 0; j--) {
                ht_move_t *rj = &path->moves[j];
                uint64_t rj_src = expected_src[j];
                __atomic_store_n(&t->buckets[rj->from_bucket].slot[rj->from_slot],
                                 rj_src, __ATOMIC_RELEASE);
                __atomic_store_n(&t->buckets[rj->to_bucket].slot[rj->to_slot],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
            }
            return false;
        }
    }

    /* Phase 2: ctrl updates (no rollback needed — slots are committed) */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint16_t etag = htc_tag16(r->full_hash);
        uint8_t ep8 = htc_partial8(etag);
        htc_ctrl_set(&t->meta[m->to_bucket], m->to_slot, ep8);
        htc_ctrl_clear(&t->meta[m->from_bucket], m->from_slot);
    }
    {
        htc_record_t *nr = htc_arena_ptr(t->arena, htc_slot_index(new_slot_word));
        uint8_t np8 = htc_partial8(htc_tag16(nr->full_hash));
        htc_ctrl_set(&t->meta[path->insert_bucket], path->insert_slot, np8);
    }

    /* Phase 3: remap updates */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(src_word));
        uint32_t p = (uint32_t)(r->full_hash & t->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        bool was_in_sec = htc_slot_in_secondary(src_word);
        uint16_t etag = htc_tag16(r->full_hash);
        if (was_in_sec && !in_sec)
            htc_remap_dec(&t->meta[p]);
        else if (!was_in_sec && in_sec)
            htc_remap_inc(&t->meta[p], etag);
    }
    {
        htc_record_t *nr = htc_arena_ptr(t->arena, htc_slot_index(new_slot_word));
        uint16_t ntag = htc_tag16(nr->full_hash);
        bool nin_sec = (path->insert_bucket != (uint32_t)(nr->full_hash & t->bucket_mask));
        if (nin_sec)
            htc_remap_inc(&t->meta[(uint32_t)(nr->full_hash & t->bucket_mask)], ntag);
    }
    return true;
}

/* =========================================================================
 * Internal helpers — placement & rehash
 * ========================================================================= */

static bool htc_place_entry(htc_table_t *t, htc_table_gen_t *gen,
                            uint64_t h, uint64_t slot_word,
                            htc_stash_t *stash_override)
{
    uint16_t tag     = htc_tag16(h);
    uint8_t  partial = htc_partial8(tag);
    uint32_t b1      = (uint32_t)(h & (gen ? gen->bucket_mask : t->bucket_mask));
    uint32_t b2      = htc_alt_bucket(b1, h, tag) & (gen ? gen->bucket_mask : t->bucket_mask);

    htc_bucket_t      *buckets = gen ? gen->buckets : t->buckets;
    htc_bucket_meta_t *meta    = gen ? gen->meta : t->meta;

    /* try primary — CAS-based slot claim */
    {
        htc_bucket_t *bk = &buckets[b1];
        htc_bucket_meta_t *m = &meta[b1];
        uint64_t ctrl = htc_ctrl_load(m);
        uint64_t em   = htc_match8(ctrl, 0);
        while (em) {
            unsigned si = htc_ctz_candidate(em);
            uint64_t exp = htc_slot_empty_word();
            uint64_t nw = slot_word & ~HTC_SLOT_SEC_MASK;
            if (htc_atomic_cas(&bk->slot[si], &exp, nw,
                               __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                htc_ctrl_set(m, si, partial);
                return true;
            }
            em = htc_clear_candidate(em, si);
        }
    }

    /* try secondary — CAS-based */
    {
        htc_bucket_t *bk = &buckets[b2];
        htc_bucket_meta_t *m = &meta[b2];
        uint64_t ctrl = htc_ctrl_load(m);
        uint64_t em   = htc_match8(ctrl, 0);
        while (em) {
            unsigned si = htc_ctz_candidate(em);
            uint64_t exp = htc_slot_empty_word();
            uint64_t nw = slot_word | HTC_SLOT_SEC_MASK;
            if (htc_atomic_cas(&bk->slot[si], &exp, nw,
                               __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                htc_ctrl_set(m, si, partial);
                htc_remap_inc(&meta[b1], tag);
                return true;
            }
            em = htc_clear_candidate(em, si);
        }
    }

    /* stash */
    htc_stash_t *s = stash_override ? stash_override : &t->stash;
    return htc_stash_insert(s, slot_word) >= 0;
}

static bool htc_rehash_place(htc_table_t *t, uint64_t h, uint64_t slot_word)
{
    return htc_place_entry(t, NULL, h, slot_word, NULL);
}

bool htc_grow(htc_table_t *t)
{
    htc_spin_lock(&t->grow_lock);

    /* Freeze the current generation: set state to FREEZING.
     * This prevents new writers from committing into it. */
    htc_table_gen_t *old_gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!old_gen) { htc_spin_unlock(&t->grow_lock); return false; }

    HTC_STAT_INC(t->stats.grow_started);

    uint32_t exp_state = HTC_GEN_ACTIVE;
    if (!__atomic_compare_exchange_n(&old_gen->state, &exp_state, HTC_GEN_FREEZING,
                                     false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        /* Another thread is already growing — wait and retry */
        while (__atomic_load_n(&t->current_gen->state, __ATOMIC_ACQUIRE) != HTC_GEN_ACTIVE)
            ;
        htc_spin_unlock(&t->grow_lock);
        return false;
    }

    /* Lock ALL shards in ascending order. This waits for in-flight writers
     * that loaded old_gen before the FREEZE but haven't committed yet.
     * After this, no writer can mutate old_gen's buckets. */
    for (uint32_t si = 0; si < t->shard_count; si++)
        htc_spin_lock(&t->shards[si].lock);

    uint32_t new_count = t->num_buckets * 2;
    size_t   bsz       = (size_t)new_count * sizeof(htc_bucket_t);
    size_t   msz       = (size_t)new_count * sizeof(htc_bucket_meta_t);

    htc_bucket_t      *new_buckets = (htc_bucket_t *)calloc(1, bsz);
    htc_bucket_meta_t *new_meta    = (htc_bucket_meta_t *)calloc(1, msz);
    if (!new_buckets || !new_meta) {
        free(new_buckets); free(new_meta);
        for (uint32_t si = t->shard_count; si > 0; si--)
            htc_spin_unlock(&t->shards[si - 1].lock);
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELEASE);
        htc_spin_unlock(&t->grow_lock);
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

    /* Rehash under full protection (all shards locked, old gen frozen) */
    htc_spin_lock(&t->arena->lock);

    bool ok = true;
    for (uint32_t bi = 0; bi < old_bucket_count && ok; bi++) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS && ok; si++) {
            uint64_t w = __atomic_load_n(&old_buckets[bi].slot[si], __ATOMIC_RELAXED);
            if (!htc_slot_live(w)) continue;
            htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
            ok = htc_rehash_place(t, rec->full_hash, w);
            if (ok) {
                HTC_STAT_INC(t->stats.grow_copied_bucket_entries);
                __atomic_store_n(&old_buckets[bi].slot[si],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
                htc_ctrl_clear(&old_meta[bi], si);
            }
        }
    }

    /* rehash old stash */
    if (ok) {
        for (uint32_t i = 0; i < old_stash.size && ok; i++) {
            uint64_t w = __atomic_load_n(&old_stash.slots[i],
                                         __ATOMIC_RELAXED);
            if (!htc_slot_live(w)) continue;
            htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
            ok = htc_rehash_place(t, rec->full_hash, w);
            if (ok) {
                HTC_STAT_INC(t->stats.grow_copied_stash_entries);
                __atomic_store_n(&old_stash.slots[i],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
            }
        }
    }

    /* rehash per-shard stashes */
    if (ok && t->shards) {
        for (uint32_t si = 0; si < t->shard_count && ok; si++) {
            htc_stash_t *ss = &t->shards[si].stash;
            for (uint32_t ei = 0; ei < ss->size && ok; ei++) {
                uint64_t w = __atomic_load_n(&ss->slots[ei], __ATOMIC_RELAXED);
                if (!htc_slot_live(w)) continue;
                htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
                ok = htc_rehash_place(t, rec->full_hash, w);
                if (ok) {
                    HTC_STAT_INC(t->stats.grow_copied_stash_entries);
                    __atomic_store_n(&ss->slots[ei],
                                     htc_slot_empty_word(), __ATOMIC_RELEASE);
                }
            }
            ss->size = 0;
            ss->full_events = 0;
            ss->empty_epochs = 0;
        }
    }

    htc_spin_unlock(&t->arena->lock);

    if (!ok) {
        htc_stash_destroy(&t->stash);
        t->buckets     = old_buckets;
        t->meta        = old_meta;
        t->num_buckets = old_bucket_count;
        t->bucket_mask = old_bucket_count - 1;
        t->stash       = old_stash;
        free(new_buckets);
        free(new_meta);
        for (uint32_t si = t->shard_count; si > 0; si--)
            htc_spin_unlock(&t->shards[si - 1].lock);
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELEASE);
        htc_spin_unlock(&t->grow_lock);
        return false;
    }

    htc_stash_destroy(&old_stash);

    /* Create the new generation — set its state to ACTIVE */
    htc_table_gen_t *new_gen = (htc_table_gen_t *)calloc(1, sizeof(htc_table_gen_t));
    if (!new_gen) {
        htc_stash_destroy(&t->stash);
        t->buckets     = old_buckets;
        t->meta        = old_meta;
        t->num_buckets = old_bucket_count;
        t->bucket_mask = old_bucket_count - 1;
        t->stash       = old_stash;
        free(new_buckets);
        free(new_meta);
        for (uint32_t si = t->shard_count; si > 0; si--)
            htc_spin_unlock(&t->shards[si - 1].lock);
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELEASE);
        htc_spin_unlock(&t->grow_lock);
        return false;
    }

    new_gen->state        = HTC_GEN_ACTIVE;
    new_gen->buckets      = new_buckets;
    new_gen->meta         = new_meta;
    new_gen->arena        = old_gen->arena;
    new_gen->shards       = t->shards;
    new_gen->bucket_mask  = new_count - 1;
    new_gen->num_buckets  = new_count;
    new_gen->shard_count  = old_gen->shard_count;
    new_gen->chunk_count  = (new_count + HTC_CHUNK_SIZE - 1) / HTC_CHUNK_SIZE;
    size_t bm_words = (new_gen->chunk_count + 63) / 64;
    new_gen->migrated_bitmap = (uint64_t *)calloc(bm_words, sizeof(uint64_t));
    if (new_gen->migrated_bitmap) {
        for (size_t wi = 0; wi < bm_words; wi++)
            new_gen->migrated_bitmap[wi] = ~0ULL;
    }
    new_gen->old          = old_gen;

    /* Mark old generation as read-only and publish new gen */
    __atomic_store_n(&old_gen->state, HTC_GEN_OLD, __ATOMIC_RELEASE);
    __atomic_store_n(&t->current_gen, new_gen, __ATOMIC_RELEASE);

    /* Release all shards — writers can now target the ACTIVE new gen */
    for (uint32_t si = t->shard_count; si > 0; si--)
        htc_spin_unlock(&t->shards[si - 1].lock);

    htc_epoch_collect(t->epoch, t->arena);
    htc_epoch_advance(t->epoch);
    htc_spin_unlock(&t->grow_lock);
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
    /* Clear per-thread front cache: stale entries from a previous table
     * would reference arena indices from a destroyed arena. */
    memset(&htc_thread_cache, 0, sizeof(htc_thread_cache));

    (void)arena;

    htc_config_t c;
    if (cfg) {
        c = *cfg;
    } else {
        c.initial_buckets  = 4;
        c.max_load_factor  = 0.75;
        c.shard_count      = 0;
    }
    if (c.initial_buckets < 4) c.initial_buckets = 4;

    uint32_t nb = 1;
    while (nb < c.initial_buckets) nb <<= 1;

    uint32_t sc = c.shard_count;
    if (sc == 0) sc = HTC_DEFAULT_SHARD_COUNT;
    if (sc > nb) sc = nb;
    if (sc < 1) sc = 1;

    htc_table_t *t = (htc_table_t *)calloc(1, sizeof(htc_table_t));
    if (!t) return NULL;

    t->buckets = (htc_bucket_t *)calloc((size_t)nb, sizeof(htc_bucket_t));
    if (!t->buckets) { free(t); return NULL; }

    t->meta = (htc_bucket_meta_t *)calloc((size_t)nb,
                                           sizeof(htc_bucket_meta_t));
    if (!t->meta) { free(t->buckets); free(t); return NULL; }

    t->arena = (htc_arena_t *)calloc(1, sizeof(htc_arena_t));
    if (!t->arena) { free(t->meta); free(t->buckets); free(t); return NULL; }

    /* Phase 2: shards and epoch control */
    t->shards = (htc_shard_t *)calloc((size_t)sc, sizeof(htc_shard_t));
    if (!t->shards) { free(t->arena); free(t->meta); free(t->buckets); free(t); return NULL; }
    for (uint32_t i = 0; i < sc; i++)
        t->shards[i].stash.allocator = t->allocator;

    t->epoch = (htc_epoch_ctl_t *)calloc(1, sizeof(htc_epoch_ctl_t));
    if (!t->epoch) { free(t->shards); free(t->arena); free(t->meta); free(t->buckets); free(t); return NULL; }
    __atomic_store_n(&t->epoch->global_epoch, 1, __ATOMIC_RELAXED);

    t->num_buckets    = nb;
    t->bucket_mask    = nb - 1;
    t->shard_count    = sc;
    t->flags          = c.flags;
    t->max_load_factor = c.max_load_factor;
    __atomic_store_n(&t->size, 0, __ATOMIC_RELAXED);
    t->allocator      = NULL;

    /* Create initial table generation */
    {
        htc_table_gen_t *gen = (htc_table_gen_t *)calloc(1, sizeof(htc_table_gen_t));
        if (!gen) { free(t->epoch); free(t->shards); free(t->arena); free(t->meta); free(t->buckets); free(t); return NULL; }
        __atomic_store_n(&gen->state, HTC_GEN_ACTIVE, __ATOMIC_RELAXED);
        gen->buckets      = t->buckets;
        gen->meta         = t->meta;
        gen->arena        = t->arena;
        gen->shards       = t->shards;
        gen->bucket_mask  = nb - 1;
        gen->num_buckets  = nb;
        gen->shard_count  = sc;
        gen->chunk_count  = (nb + HTC_CHUNK_SIZE - 1) / HTC_CHUNK_SIZE;
        gen->old          = NULL;
        __atomic_store_n(&t->current_gen, gen, __ATOMIC_RELEASE);
    }
    return t;
}

void htc_destroy(htc_table_t *t)
{
    if (!t) return;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);

    /* Drain any pending retirements */
    for (int i = 0; i < 8; i++) {
        htc_epoch_advance(t->epoch);
        htc_epoch_collect(t->epoch, g->arena);
    }
    /* Force-free any remaining retire nodes */
    {
        htc_retire_node_t *n = __atomic_exchange_n(&t->epoch->retire_head, NULL,
                                                    __ATOMIC_ACQUIRE);
        while (n) {
            htc_retire_node_t *next = n->next;
            free(n);
            n = next;
        }
    }

    /* Free through the generation (owns all data).
     * Arena and shards are shared across generations — free once. */
    void *arena_freed = NULL;
    void *shards_freed = NULL;
    while (g) {
        htc_table_gen_t *old = g->old;
        if (g->arena && g->arena != arena_freed) {
            htc_arena_destroy(g->arena);
            free(g->arena);
            arena_freed = g->arena;
        }
        free(g->meta);
        free(g->buckets);
        if (g->shards && g->shards != shards_freed) {
            for (uint32_t i = 0; i < g->shard_count; i++)
                htc_stash_destroy(&g->shards[i].stash);
            free(g->shards);
            shards_freed = g->shards;
        }
        free(g->migrated_bitmap);
        free(g);
        g = old;
    }

    htc_stash_destroy(&t->stash);
    free(t->epoch);
    free(t);
}

void htc_clear(htc_table_t *t)
{
    if (!t) return;
    htc_arena_destroy(t->arena);
    htc_stash_destroy(&t->stash);
    memset(t->buckets, 0, (size_t)t->num_buckets * sizeof(htc_bucket_t));
    memset(t->meta, 0, (size_t)t->num_buckets * sizeof(htc_bucket_meta_t));
    __atomic_store_n(&t->size, 0, __ATOMIC_RELAXED);
    if (t->filter) cuckoo_filter_reset(t->filter);
}

/* ─── AMQ filter (spec §25) ────────────────────────────────── */
void htc_set_filter(htc_table_t *t, cuckoo_filter_t *cf) {
    if (t) t->filter = cf;
}

cuckoo_filter_t *htc_get_filter(const htc_table_t *t) {
    return t ? t->filter : NULL;
}

#ifdef HTC_STATS
void htc_stats_print(const htc_table_t *t) {
    if (!t) return;
    const htc_stats_t *s = &t->stats;
    printf("htc stats:\n");
    printf("  find_primary_hit    %lu\n", __atomic_load_n(&s->find_primary_hit, __ATOMIC_RELAXED));
    printf("  find_secondary_hit  %lu\n", __atomic_load_n(&s->find_secondary_hit, __ATOMIC_RELAXED));
    printf("  find_stash_hit      %lu\n", __atomic_load_n(&s->find_stash_hit, __ATOMIC_RELAXED));
    printf("  find_oldgen_hit     %lu\n", __atomic_load_n(&s->find_oldgen_hit, __ATOMIC_RELAXED));
    printf("  find_negative       %lu\n", __atomic_load_n(&s->find_negative, __ATOMIC_RELAXED));
    printf("  secondary_skipped   %lu\n", __atomic_load_n(&s->secondary_skipped, __ATOMIC_RELAXED));
    printf("  secondary_checked   %lu\n", __atomic_load_n(&s->secondary_checked, __ATOMIC_RELAXED));
    printf("  seq_retries         %lu\n", __atomic_load_n(&s->seq_retries, __ATOMIC_RELAXED));
    printf("  bfs_attempts        %lu\n", __atomic_load_n(&s->bfs_attempts, __ATOMIC_RELAXED));
    printf("  bfs_success         %lu\n", __atomic_load_n(&s->bfs_success, __ATOMIC_RELAXED));
    printf("  bfs_no_path         %lu\n", __atomic_load_n(&s->bfs_no_path, __ATOMIC_RELAXED));
    printf("  stash_insert        %lu\n", __atomic_load_n(&s->stash_insert, __ATOMIC_RELAXED));
    printf("  stash_grow          %lu\n", __atomic_load_n(&s->stash_grow, __ATOMIC_RELAXED));
    printf("  stash_full          %lu\n", __atomic_load_n(&s->stash_full, __ATOMIC_RELAXED));
    printf("  front_cache_hit     %lu\n", __atomic_load_n(&s->front_cache_hit, __ATOMIC_RELAXED));
    printf("  front_cache_miss    %lu\n", __atomic_load_n(&s->front_cache_miss, __ATOMIC_RELAXED));
    printf("  grow_started        %lu\n", __atomic_load_n(&s->grow_started, __ATOMIC_RELAXED));
    printf("  grow_copied_buckets %lu\n", __atomic_load_n(&s->grow_copied_bucket_entries, __ATOMIC_RELAXED));
    printf("  grow_copied_stashes %lu\n", __atomic_load_n(&s->grow_copied_stash_entries, __ATOMIC_RELAXED));
    printf("  writer_retry_chg    %lu\n", __atomic_load_n(&s->writer_retry_gen_changed, __ATOMIC_RELAXED));
    printf("  writer_retry_frz    %lu\n", __atomic_load_n(&s->writer_retry_gen_frozen, __ATOMIC_RELAXED));
}
#endif

/* =========================================================================
 * Public API — insert / upsert / update / find / remove / size
 * ========================================================================= */

bool htc_insert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return false;

    uint32_t s1 = 0, s2 = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
        if (!gen) return false;
        htc_bucket_t      *buckets = gen->buckets;
        htc_bucket_meta_t *meta    = gen->meta;
        uint32_t           mask    = gen->bucket_mask;

        uint16_t tag  = htc_tag16(hash);
        uint8_t  p8   = htc_partial8(tag);
        uint32_t  b1  = (uint32_t)(hash & mask);
        uint32_t  b2  = htc_alt_bucket(b1, hash, tag) & mask;
        s1 = htc_shard_of(b1, t->shard_count);
        s2 = htc_shard_of(b2, t->shard_count);

        if (t->shards) htc_lock_shards(t->shards, s1, s2);

        /* Revalidate after acquiring locks: verify the generation loaded at
         * attempt start is still current and ACTIVE. A concurrent grow may
         * have frozen this gen while we waited on shard locks. */
        {
            htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
            if (cur != gen) {
                HTC_STAT_INC(t->stats.writer_retry_gen_changed);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
            if (__atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) != HTC_GEN_ACTIVE) {
                HTC_STAT_INC(t->stats.writer_retry_gen_frozen);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
        }

        /* Ensure the chunks for both candidate buckets are migrated */
        htc_ensure_chunk_migrated(t, b1);
        if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

        /* duplicate check (under shard lock) */
        if (htc_bucket_scan(buckets + b1, meta + b1,
                            t->arena, hash, HTC_SCAN_PRIMARY) >= 0) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return false;
        }
        if (htc_must_check_secondary(meta + b1, tag)) {
            if (htc_bucket_scan(buckets + b2, meta + b2,
                                t->arena, hash, HTC_SCAN_SECONDARY) >= 0) {
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                return false;
            }
        }
        htc_stash_t *stash_ptr = t->shards ? &t->shards[htc_shard_of(b1, t->shard_count)].stash : &t->stash;
        if (htc_stash_find(stash_ptr, t->arena, hash) >= 0) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return false;
        }

        /* allocate arena record (before mutation, under lock) */
        uint32_t idx = htc_arena_alloc(t->arena, hash, value);
        if (idx == UINT32_MAX) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return false;
        }

        uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

        htc_seq_guard_t g1 = {0}, g2 = {0};
        g1 = htc_bucket_seq_begin(meta + b1);
        if (b2 != b1)
            g2 = htc_bucket_seq_begin(meta + b2);

        /* Try direct primary insert */
        {
            uint64_t ctrl = htc_ctrl_load(meta + b1);
            uint64_t em   = htc_match8(ctrl, 0);
            while (em) {
                unsigned si = htc_ctz_candidate(em);
                uint64_t exp = htc_slot_empty_word();
                if (htc_atomic_cas(&(buckets + b1)->slot[si], &exp, slot_word,
                                   __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    htc_ctrl_set(meta + b1, si, p8);
                    htc_bucket_seq_end(meta + b1, g1);
                    if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);
                    goto insert_done;
                }
                em = htc_clear_candidate(em, si);
            }
        }

        /* Try direct secondary insert */
        {
            uint64_t ctrl = htc_ctrl_load(meta + b2);
            uint64_t em   = htc_match8(ctrl, 0);
            while (em) {
                unsigned si = htc_ctz_candidate(em);
                uint64_t exp = htc_slot_empty_word();
                uint64_t sw = slot_word | HTC_SLOT_SEC_MASK;
                if (htc_atomic_cas(&(buckets + b2)->slot[si], &exp, sw,
                                   __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
                    htc_ctrl_set(meta + b2, si, p8);
                    htc_remap_inc(meta + b1, tag);
                    htc_bucket_seq_end(meta + b1, g1);
                    if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);
                    goto insert_done;
                }
                em = htc_clear_candidate(em, si);
            }
        }

        /* Both buckets full — try BFS displacement */
        {
            ht_cuckoo_path_t bfs_path = {0};
            htc_bucket_seq_end(meta + b1, g1);
            if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);

            if (bfs_find_path(t, b1, b2, &bfs_path)) {
                HTC_STAT_INC(t->stats.bfs_attempts);
                /* Collect shards from b1, b2, and all path buckets */
                uint32_t all_sids[HTC_BFS_MAX_PATH + 4];
                uint8_t  all_n = 0;
                uint32_t tmp_buckets[HTC_BFS_MAX_PATH * 2 + 4];
                uint8_t  tmp_n = 0;
                tmp_buckets[tmp_n++] = b1;
                if (b2 != b1) tmp_buckets[tmp_n++] = b2;
                for (uint8_t mi = 0; mi < bfs_path.move_count; mi++) {
                    tmp_buckets[tmp_n++] = bfs_path.moves[mi].from_bucket;
                    tmp_buckets[tmp_n++] = bfs_path.moves[mi].to_bucket;
                }
                tmp_buckets[tmp_n++] = bfs_path.insert_bucket;

                for (uint8_t ti = 0; ti < tmp_n; ti++) {
                    uint32_t sid = htc_shard_of(tmp_buckets[ti], t->shard_count);
                    bool dup = false;
                    for (uint8_t sj = 0; sj < all_n; sj++)
                        if (all_sids[sj] == sid) { dup = true; break; }
                    if (!dup) all_sids[all_n++] = sid;
                }

                /* If path touches too many shards, skip BFS and use stash.
                 * s1/s2 are still locked — fall through to revalidation + stash. */
                uint8_t bfs_max_shards = HTC_BFS_MAX_SHARDS;
                if (t->shard_count < bfs_max_shards * 2)
                    bfs_max_shards = t->shard_count;
                if (all_n <= bfs_max_shards) {
                    uint64_t expected_src[HTC_BFS_MAX_PATH] = {0};

                    /* LFBCH commit (§23): per-move CAS + seq guards.
                     * If it fails (CAS race), fall back to locked protocol. */
                    if (commit_path_lfbch(t, &bfs_path, expected_src, slot_word)) {
                        HTC_STAT_INC(t->stats.bfs_success);
                        goto insert_done;
                    }

                    /* Locked protocol */
                    if (t->shards) {
                        htc_unlock_shards(t->shards, s1, s2);
                        for (uint8_t i = 0; i < all_n; i++)
                            for (uint8_t j = i + 1; j < all_n; j++)
                                if (all_sids[j] < all_sids[i]) {
                                    uint32_t tmp = all_sids[i];
                                    all_sids[i] = all_sids[j];
                                    all_sids[j] = tmp;
                                }
                        for (uint8_t i = 0; i < all_n; i++)
                            htc_spin_lock(&t->shards[all_sids[i]].lock);
                    }

                    if (validate_path_locked(t, &bfs_path, expected_src)) {
                        HTC_STAT_INC(t->stats.bfs_success);
                        commit_path_locked(t, &bfs_path, expected_src, slot_word);
                        if (t->shards)
                            for (uint8_t i = all_n; i > 0; i--)
                                htc_spin_unlock(&t->shards[all_sids[i - 1]].lock);
                        goto insert_done;
                    }

                    if (t->shards)
                        for (uint8_t i = all_n; i > 0; i--)
                            htc_spin_unlock(&t->shards[all_sids[i - 1]].lock);
                    if (t->shards) htc_lock_shards(t->shards, s1, s2);
                }
            } else {
                HTC_STAT_INC(t->stats.bfs_no_path);
            }
        }

        /* Revalidate before stash mutation. If the gen was frozen while we
         * were in the BFS scope (which may have unlocked/re-locked shards),
         * the arena record allocated at line 1333 must be freed on retry. */
        {
            htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
            if (cur != gen) {
                HTC_STAT_INC(t->stats.writer_retry_gen_changed);
                htc_arena_free(t->arena, idx);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
            if (__atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) != HTC_GEN_ACTIVE) {
                HTC_STAT_INC(t->stats.writer_retry_gen_frozen);
                htc_arena_free(t->arena, idx);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
        }

        /* BFS failed — use stash (g1/g2 already ended above) */
        if (htc_stash_insert(stash_ptr, slot_word) >= 0) {
            HTC_STAT_INC(t->stats.stash_insert);
            htc_stash_maintain(stash_ptr);
            goto insert_done;
        }
        HTC_STAT_INC(t->stats.stash_full);

        /* Stash full — free record, grow table, retry */
        htc_arena_free(t->arena, idx);
        if (t->shards) htc_unlock_shards(t->shards, s1, s2);
        htc_epoch_collect(t->epoch, t->arena);
        htc_epoch_advance(t->epoch);
        if (!htc_grow(t)) return false;
        continue;

retry_nowait:
        /* Generation state changed — just retry. The next iteration
         * loads the current generation and re-validates. */
        continue;
    }

    return false;

insert_done:
    __atomic_fetch_add(&t->size, 1, __ATOMIC_RELAXED);
    if (t->filter) cuckoo_filter_insert(t->filter, hash);

    if (t->shards) htc_unlock_shards(t->shards, s1, s2);

    htc_epoch_collect(t->epoch, t->arena);
    htc_epoch_advance(t->epoch);

    double lf = (double)__atomic_load_n(&t->size, __ATOMIC_RELAXED) / (double)t->num_buckets;
    if (lf > t->max_load_factor) htc_grow(t);

    return true;
}

bool htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return false;

    for (int attempt = 0; attempt < 4; attempt++) {
        htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
        if (!gen) return false;
        uint32_t mask = gen->bucket_mask;

        uint16_t tag  = htc_tag16(hash);
        uint32_t  b1  = (uint32_t)(hash & mask);
        uint32_t  b2  = htc_alt_bucket(b1, hash, tag) & mask;

        /* Ensure chunk is migrated so we operate on the current gen */
        htc_ensure_chunk_migrated(t, b1);
        if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

        /* Search current gen buckets and stash, but don't update yet */
        uint64_t found_word = 0;
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            found_word = __atomic_load_n(&gen->buckets[b1].slot[si], __ATOMIC_ACQUIRE);
        } else if (htc_must_check_secondary(&gen->meta[b1], tag)) {
            si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
            if (si >= 0)
                found_word = __atomic_load_n(&gen->buckets[b2].slot[si], __ATOMIC_ACQUIRE);
        }
        if (!found_word) {
            htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
            int sti = htc_stash_find(ss, t->arena, hash);
            htc_stash_t *found_ss = ss;
            if (sti < 0 && ss != &t->stash) {
                sti = htc_stash_find(&t->stash, t->arena, hash);
                if (sti >= 0) found_ss = &t->stash;
            }
            if (sti >= 0)
                found_word = __atomic_load_n(&found_ss->slots[sti], __ATOMIC_ACQUIRE);
        }
        if (found_word) {
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(found_word))->user_value,
                             value, __ATOMIC_RELAXED);
            return true;
        }

        /* Check old gen chain for stale copies and clear them */
        if (gen->old) {
            uint64_t old_val = 0;
            if (htc_find_in_old_gen(gen->old, t->arena, hash, &old_val)) {
                /* Entry exists only in old gen — force migration then retry */
                htc_ensure_chunk_migrated(t, b1);
                if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
                continue;
            }
        }

        /* not found — insert */
        uint32_t idx = htc_arena_alloc(t->arena, hash, value);
        if (idx == UINT32_MAX) return false;

        uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

        if (!htc_place_entry(t, gen, hash, slot_word, NULL)) {
            htc_arena_free(t->arena, idx);
            return false;
        }

        __atomic_fetch_add(&t->size, 1, __ATOMIC_RELAXED);
        htc_epoch_collect(t->epoch, t->arena);
        htc_epoch_advance(t->epoch);

        double lf = (double)__atomic_load_n(&t->size, __ATOMIC_RELAXED) / (double)t->num_buckets;
        if (lf > t->max_load_factor) htc_grow(t);

        return true;
    }
    return false;
}

bool htc_update(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return false;

    htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!gen) return false;
    uint32_t mask = gen->bucket_mask;

    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & mask);
    uint32_t  b2  = htc_alt_bucket(b1, hash, tag) & mask;

    htc_ensure_chunk_migrated(t, b1);
    if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

    {
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si], __ATOMIC_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, __ATOMIC_RELAXED);
            return true;
        }
    }
    if (htc_must_check_secondary(&gen->meta[b1], tag)) {
        int si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&gen->buckets[b2].slot[si], __ATOMIC_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, __ATOMIC_RELAXED);
            return true;
        }
    }
    {
        htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
        int si = htc_stash_find(ss, t->arena, hash);
        if (si < 0 && ss != &t->stash)
            si = htc_stash_find(&t->stash, t->arena, hash);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&ss->slots[si], __ATOMIC_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, __ATOMIC_RELAXED);
            return true;
        }
    }

    /* Not found in current gen — check old gen */
    if (gen->old) {
        uint64_t old_val = 0;
        if (htc_find_in_old_gen(gen->old, t->arena, hash, &old_val)) {
            /* Force migration so next call finds it in current gen */
            htc_ensure_chunk_migrated(t, b1);
            if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
        }
    }
    return false;
}

bool htc_find(const htc_table_t *t_, uint64_t hash, uint64_t *out_value)
{
    if (!t_) return false;
    htc_table_t *t = (htc_table_t *)t_;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!g) return false;

    /* Pin epoch before any arena record access (including front cache) */
    htc_epoch_pin(t->epoch);

    /* AMQ filter (spec §25): if the filter says not present, skip lookup */
    if (t->filter && !cuckoo_filter_lookup(t->filter, hash)) {
        htc_epoch_unpin(t->epoch);
        HTC_STAT_INC(t->stats.find_negative);
        return false;
    }

    /* Phase 8: check front cache under epoch protection (unless disabled) */
    if (out_value && !(t->flags & HTC_CFG_DISABLE_FRONT_CACHE)) {
        uint64_t cv = 0;
        if (htc_front_cache_lookup(&htc_thread_cache, g->arena, hash, &cv)) {
            HTC_STAT_INC(t->stats.front_cache_hit);
            *out_value = cv;
            htc_epoch_unpin(t->epoch);
            return true;
        } else {
            HTC_STAT_INC(t->stats.front_cache_miss);
        }
    }

    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & g->bucket_mask);
    uint32_t found_idx = UINT32_MAX; /* arena idx for cache insert */

    {
        int si = htc_bucket_scan_seq(&g->buckets[b1], &g->meta[b1],
                                     g->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            found_idx = (uint32_t)htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
            uint32_t gen = rec->generation;
            if (out_value) *out_value = __atomic_load_n(&rec->user_value, __ATOMIC_RELAXED);
            HTC_STAT_INC(t->stats.find_primary_hit);
            htc_epoch_unpin(t->epoch);
            htc_front_cache_insert(&htc_thread_cache, hash, found_idx, gen);
            return true;
        }
    }
    if (htc_must_check_secondary(&g->meta[b1], tag)) {
        HTC_STAT_INC(t->stats.secondary_checked);
        uint32_t b2 = htc_alt_bucket(b1, hash, tag) & g->bucket_mask;
        int si = htc_bucket_scan_seq(&g->buckets[b2], &g->meta[b2],
                                     g->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            found_idx = (uint32_t)htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
            uint32_t gen = rec->generation;
            if (out_value) *out_value = __atomic_load_n(&rec->user_value, __ATOMIC_RELAXED);
            HTC_STAT_INC(t->stats.find_secondary_hit);
            htc_epoch_unpin(t->epoch);
            htc_front_cache_insert(&htc_thread_cache, hash, found_idx, gen);
            return true;
        }
    } else {
        HTC_STAT_INC(t->stats.secondary_skipped);
    }
    {
        htc_stash_t *ss = g->shards ? &g->shards[htc_shard_of(b1, g->shard_count)].stash : &t->stash;
        int si = htc_stash_find(ss, g->arena, hash);
        htc_stash_t *found = ss;
        if (si < 0 && ss != &t->stash) {
            si = htc_stash_find(&t->stash, g->arena, hash);
            if (si >= 0) found = &t->stash;
        }
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&found->slots[si], __ATOMIC_ACQUIRE);
            found_idx = (uint32_t)htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
            uint32_t gen = rec->generation;
            if (out_value) *out_value = __atomic_load_n(&rec->user_value, __ATOMIC_RELAXED);
            HTC_STAT_INC(t->stats.find_stash_hit);
            htc_epoch_unpin(t->epoch);
            htc_front_cache_insert(&htc_thread_cache, hash, found_idx, gen);
            return true;
        }
    }

    /* Phase 6: check old generation if migration is active */
    if (g->old) {
        if (htc_find_in_old_gen(g->old, g->arena, hash, out_value)) {
            HTC_STAT_INC(t->stats.find_oldgen_hit);
            htc_epoch_unpin(t->epoch);
            return true;
        }
    }

    HTC_STAT_INC(t->stats.find_negative);
    htc_epoch_unpin(t->epoch);
    if (out_value) *out_value = 0;
    return false;
}

bool htc_remove(htc_table_t *t, uint64_t hash)
{
    if (!t) return false;

    /* Load current gen for consistent buckets/meta/mask */
    htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (!gen) return false;

    uint16_t tag  = htc_tag16(hash);
    uint32_t  b1  = (uint32_t)(hash & gen->bucket_mask);
    uint32_t  b2  = htc_alt_bucket(b1, hash, tag) & gen->bucket_mask;
    uint32_t  s1  = htc_shard_of(b1, t->shard_count);
    uint32_t  s2  = htc_shard_of(b2, t->shard_count);

    if (t->shards) htc_lock_shards(t->shards, s1, s2);

    /* Revalidate: after acquiring locks, reload current_gen and verify
     * it is still ACTIVE. Prevents committing into a frozen generation. */
    htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    if (cur != gen || __atomic_load_n(&cur->state, __ATOMIC_ACQUIRE) != HTC_GEN_ACTIVE) {
        if (t->shards) htc_unlock_shards(t->shards, s1, s2);
        return false;
    }

    /* Ensure chunks for candidate buckets are migrated */
    htc_ensure_chunk_migrated(t, b1);
    if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

    {
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            htc_seq_guard_t gs = htc_bucket_seq_begin(&gen->meta[b1]);
            uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si],
                                         __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&gen->buckets[b1].slot[si],
                             htc_slot_empty_word(), __ATOMIC_RELEASE);
            htc_ctrl_clear(&gen->meta[b1], (unsigned)si);
            htc_bucket_seq_end(&gen->meta[b1], gs);
            htc_remap_dec(&gen->meta[b1]);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->filter) cuckoo_filter_delete(t->filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            htc_epoch_collect(t->epoch, t->arena);
            htc_epoch_advance(t->epoch);
            return true;
        }
    }
    if (htc_must_check_secondary(&gen->meta[b1], tag)) {
        int si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            htc_seq_guard_t gs = htc_bucket_seq_begin(&gen->meta[b2]);
            uint64_t w = __atomic_load_n(&gen->buckets[b2].slot[si],
                                         __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, __ATOMIC_RELAXED);
            __atomic_store_n(&gen->buckets[b2].slot[si],
                             htc_slot_empty_word(), __ATOMIC_RELEASE);
            htc_ctrl_clear(&gen->meta[b2], (unsigned)si);
            htc_bucket_seq_end(&gen->meta[b2], gs);
            htc_remap_dec(&gen->meta[b1]);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->filter) cuckoo_filter_delete(t->filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            htc_epoch_collect(t->epoch, t->arena);
            htc_epoch_advance(t->epoch);
            return true;
        }
    }
    {
        htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
        int si = htc_stash_find(ss, t->arena, hash);
        htc_stash_t *found = ss;
        if (si < 0 && ss != &t->stash) {
            si = htc_stash_find(&t->stash, t->arena, hash);
            if (si >= 0) found = &t->stash;
        }
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&found->slots[si], __ATOMIC_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, __ATOMIC_RELAXED);
            htc_stash_remove_at(found, (unsigned)si);
            htc_stash_maintain(found);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->filter) cuckoo_filter_delete(t->filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            htc_epoch_collect(t->epoch, t->arena);
            htc_epoch_advance(t->epoch);
            return true;
        }
    }

    /* Not found in current gen — try old generations (post-migration) */
    if (gen->old) {
        uint64_t old_val = 0;
        if (htc_find_in_old_gen(gen->old, t->arena, hash, &old_val)) {
            /* Entry exists in old gen but was not yet migrated to current gen.
             * Force its chunk migration, then retry removal from current gen. */
            htc_ensure_chunk_migrated(t, b1);
            if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
            /* After forced migration, try one more time on current gen */
            int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                     t->arena, hash, HTC_SCAN_PRIMARY);
            if (si >= 0) {
                htc_seq_guard_t gs = htc_bucket_seq_begin(&gen->meta[b1]);
                uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si],
                                             __ATOMIC_ACQUIRE);
                uint32_t idx = htc_slot_index(w);
                htc_record_t *rec = htc_arena_ptr(t->arena, idx);
                __atomic_store_n(&rec->flags, 1, __ATOMIC_RELAXED);
                __atomic_store_n(&gen->buckets[b1].slot[si],
                                 htc_slot_empty_word(), __ATOMIC_RELEASE);
                htc_ctrl_clear(&gen->meta[b1], (unsigned)si);
                htc_bucket_seq_end(&gen->meta[b1], gs);
                htc_front_cache_remove(&htc_thread_cache, hash);
                htc_epoch_retire(t->epoch, t->arena, idx);
                __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                htc_epoch_collect(t->epoch, t->arena);
                htc_epoch_advance(t->epoch);
                return true;
            }
        }
    }

    if (t->shards) htc_unlock_shards(t->shards, s1, s2);
    htc_epoch_collect(t->epoch, t->arena);
    htc_epoch_advance(t->epoch);
    return false;
}

size_t htc_size(const htc_table_t *t)
{
    if (!t) return 0;
    return __atomic_load_n(&((htc_table_t *)t)->size, __ATOMIC_RELAXED);
}
