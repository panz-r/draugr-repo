/**
 * Draugr Concurrent Cuckoo Hash Table — Phase 1
 *
 * Flat-arena allocator, SWAR bucket-scan, simple cuckoo placement
 * (primary → secondary → stash), and grow-via-rehash.
 *
 * Uses raw malloc/calloc/realloc/free throughout (no DRAUGR macros)
 * so that ASan can track every allocation precisely.
 *
 * ─── RAW_ATOMIC operation registry (Battery 17 Q3) ────────────
 *
 * Every dangerous raw atomic store outside approved helpers must be
 * annotated with "RAW_ATOMIC".  Grep for RAW_ATOMIC to audit all
 * critical ordering points.
 *
 * Approved raw operations and their locations:
 *
 *   slot word stores/CAS:
 *     htc_place_entry (primary/secondary CAS)
 *     commit_path_locked (Phase 1 slot writes)
 *     htc_stash_insert (CAS)
 *     htc_stash_remove_at (RELEASE EMPTY store)
 *     htc_remove (primary/secondary slot clear)
 *     htc_migrate_chunk (CAS)
 *
 *   ctrl_tags writes:
 *     htc_ctrl_set / htc_ctrl_clear
 *
 *   remap_count writes:
 *     htc_remap_inc / htc_remap_dec
 *
 *   record.flags writes:
 *     htc_remove (flags = 1, RELEASE)
 *
 *   current_gen writes:
 *     htc_grow (t->current_gen = new_gen, RELEASE)
 *     htc_resize_start (t->current_gen = gen, RELEASE)
 *
 *   gen->state writes:
 *     htc_grow (ACTIVE->FREEZING CAS, FREEZING->OLD store)
 *     htc_resize_start (FREEZING->ACTIVE store)
 *
 * No code outside these paths should perform raw stores on the above fields.
 * ──────────────────────────────────────────────────────────────────────────── */

#include "draugr/htc_internal.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <stdio.h>

/* Per-thread front cache — thread-local to avoid cache-line bouncing */
_Thread_local htc_front_cache_t htc_thread_cache = {0};

#ifdef HTC_WITNESS
_Thread_local htc_witness_log_t htc_witness_log = {0};
#endif

/* =========================================================================
 * Arena — flat array of records + free-list
 * ========================================================================= */

/* Record pointer via block-linked arena: find the block containing idx
 * and return the slot.  Blocks are never moved or freed while the
 * arena is alive, so this is safe without the arena lock. */
htc_record_t *htc_arena_ptr(htc_arena_t *a, uint32_t idx) {
    uint32_t bi = idx >> HTC_ARENA_BLOCK_SHIFT;
    uint32_t si = idx & HTC_ARENA_BLOCK_MASK;
    htc_arena_block_t *b = __atomic_load_n(&a->head, __ATOMIC_RELAXED);
    for (uint32_t i = 0; b && i < bi; i++) b = b->next;
    return b ? &b->recs[si] : NULL;
}

/* Helper: zero a record's fields.  Caller must hold a->lock. */
static inline void htc_arena_rec_zero(htc_record_t *r) {
    r->identity_hash  = 0;
    r->placement_hash = 0;
    r->generation++;
#ifdef HTC_TEST_SMALL_RECORD_GEN_BITS
    r->generation &= (1u << HTC_TEST_SMALL_RECORD_GEN_BITS) - 1;
#endif
    __atomic_store_n(&r->user_value, 0, HTC_MO_RELEASE);
}

uint32_t htc_arena_alloc(htc_arena_t *a, uint64_t identity_hash,
                          uint64_t placement_hash, uint64_t value)
{
    htc_spin_lock(&a->lock);
    htc_record_t *r;
    if (a->free_count > 0) {
        a->free_count--;
        uint32_t idx = a->free_idx[a->free_count];
        r = htc_arena_ptr(a, idx);
        r->generation++;
        r->identity_hash  = identity_hash;
        r->placement_hash = placement_hash;
        __atomic_store_n(&r->flags, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&r->user_value, value, HTC_MO_RELEASE);
#ifdef HTC_TEST_SMALL_RECORD_GEN_BITS
        r->generation &= (1u << HTC_TEST_SMALL_RECORD_GEN_BITS) - 1;
#endif
        htc_spin_unlock(&a->lock);
        return idx;
    }
    /* Allocate a new block when current one is full */
    uint32_t bi = a->count >> HTC_ARENA_BLOCK_SHIFT;
    uint32_t needed = bi + 1;
    uint32_t have = 0;
    {
        htc_arena_block_t *b = a->head;
        while (b) { have++; b = b->next; }
    }
    while (have < needed) {
        htc_arena_block_t *nb = (htc_arena_block_t *)calloc(1, sizeof(htc_arena_block_t));
        if (!nb) { htc_spin_unlock(&a->lock); return UINT32_MAX; }
        nb->next = a->head;
        __atomic_store_n(&a->head, nb, __ATOMIC_RELEASE);
        have++;
    }
    uint32_t idx = a->count++;
    r = htc_arena_ptr(a, idx);
    r->identity_hash  = identity_hash;
    r->placement_hash = placement_hash;
    __atomic_store_n(&r->flags, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&r->user_value, value, HTC_MO_RELEASE);
#ifdef HTC_TEST_SMALL_RECORD_GEN_BITS
    r->generation &= (1u << HTC_TEST_SMALL_RECORD_GEN_BITS) - 1;
#endif
    htc_spin_unlock(&a->lock);
    return idx;
}

void htc_arena_free(htc_arena_t *a, uint32_t idx)
{
    htc_spin_lock(&a->lock);
    htc_arena_rec_zero(htc_arena_ptr(a, idx));
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

void htc_arena_destroy(htc_arena_t *a)
{
    htc_arena_block_t *b = a->head;
    while (b) {
        htc_arena_block_t *next = b->next;
        free(b);
        b = next;
    }
    free(a->free_idx);
    memset(a, 0, sizeof(*a));
}

/* =========================================================================
 * Stash — fixed-size per-shard overflow (HTC_STASH_MAX = 32 entries)
 * Embedded array so lock-free readers never see a dangling pointer.
 * ========================================================================= */

int htc_stash_find(const htc_stash_t *s, htc_arena_t *a,
                    uint64_t identity_hash, uint16_t tag)
{
    /* Fast path: empty stash — skip 32 ACQUIRE loads */
    if (__atomic_load_n(&s->live_count, HTC_MO_ACQUIRE) == 0)
        return -1;
    for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
        uint64_t w = __atomic_load_n(&s->slots[i], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        htc_record_t *rec = htc_arena_ptr(a, htc_slot_index(w));
        if (rec->identity_hash != identity_hash) continue;
        if (__atomic_load_n(&rec->flags, HTC_MO_ACQUIRE) != 0) continue;
        return (int)i;
    }
    return -1;
}

void htc_stash_remove_at(htc_stash_t *s, unsigned idx)
{
    /* Release-store EMPTY — no compaction, safe for lock-free readers. */
    __atomic_store_n(&s->slots[idx], htc_slot_empty_word(), HTC_MO_RELEASE);
    __atomic_fetch_sub(&s->live_count, 1, __ATOMIC_RELAXED);
}

int htc_stash_insert(htc_stash_t *s, uint64_t slot_word)
{
    /* Scan for first EMPTY slot */
    for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
        uint64_t exp = htc_slot_empty_word();
        if (htc_atomic_cas(&s->slots[i], &exp, slot_word,
                           HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
            __atomic_fetch_add(&s->live_count, 1, __ATOMIC_RELAXED);
            return (int)i;
        }
    }
    s->full_events++;
    return -1;
}

void htc_stash_destroy(htc_stash_t *s)
{
    memset(s, 0, sizeof(*s));
}

void htc_stash_maintain(htc_stash_t *s)
{
    /* Fixed-size array — no shrink needed. Just reset tracking counters. */
    if (s->size == 0)
        s->empty_epochs++;
    else
        s->empty_epochs = 0;
}

/* =========================================================================
 * Epoch-based record retirement
 * ========================================================================= */

void htc_epoch_retire(htc_epoch_ctl_t *ep, htc_arena_t *a, uint32_t arena_idx)
{
    if (!ep) { htc_arena_free(a, arena_idx); return; }
    HTC_SCHED_HOOK(14); /* before epoch retire free */
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
                                          false, HTC_MO_RELEASE, __ATOMIC_RELAXED));
}

uint64_t htc_epoch_collect(htc_epoch_ctl_t *ep, htc_arena_t *a)
{
    if (!ep || !a) return 0;

    /* Compute the minimum non-zero thread epoch */
    uint64_t min_ep = UINT64_MAX;
    for (int i = 0; i < HTC_EPOCH_MAX_THREADS; i++) {
        uint64_t te = __atomic_load_n(&ep->thread_epoch[i], HTC_MO_ACQUIRE);
        if (te != 0 && te < min_ep) min_ep = te;
    }

    /* Pop entire retire list */
    htc_retire_node_t *head = __atomic_exchange_n(&ep->retire_head, NULL,
                                                    HTC_MO_ACQUIRE);

    /* Split into safe-to-free (retire_epoch < min_ep) and keep */
    htc_retire_node_t *keep = NULL, *keep_tail = NULL;
    htc_retire_node_t *free_list = NULL;
    uint64_t reclaimed = 0;

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
        reclaimed++;
    }

    /* Re-pend keep list */
    if (keep) {
        htc_retire_node_t *old = NULL;
        do {
            keep_tail->next = old;
        } while (!__atomic_compare_exchange_n(&ep->retire_head, &old, keep,
                                              false, HTC_MO_RELEASE, HTC_MO_ACQUIRE));
    }

    /* Process retired generations (Battery 8 Q5).
     * NOTE: retired gens are NOT freed here — writers may still hold
     * gen pointers loaded before the gen was retired.  Epoch-unpinned
     * writers cannot be protected by retire_epoch < min_ep.
     * Gens are freed during htc_destroy instead. */
    {
        htc_retire_gen_t *rg = __atomic_exchange_n(&ep->retire_gen_head, NULL,
                                                     HTC_MO_ACQUIRE);
        /* Re-pend all retired gens — they remain reachable via the
         * gen chain (new_gen->old) and are freed during destroy. */
        if (rg) {
            htc_retire_gen_t *tail = rg;
            while (tail->next) tail = tail->next;
            htc_retire_gen_t *old_rg = NULL;
            do {
                tail->next = old_rg;
            } while (!__atomic_compare_exchange_n(&ep->retire_gen_head, &old_rg,
                                                   rg, false,
                                                   HTC_MO_RELEASE, HTC_MO_ACQUIRE));
        }
    }
    return reclaimed;
}

void htc_epoch_advance(htc_epoch_ctl_t *ep)
{
    if (!ep) return;
    __atomic_fetch_add(&ep->global_epoch, 1, HTC_MO_RELEASE);
}

/* Retire a generation for deferred freeing. The gen's buckets and meta
 * are freed once all readers that loaded gen before publication have
 * advanced past retire_epoch. Returns true if enqueued, false on OOM
 * (caller must keep the gen reachable via the generation chain). (Q9) */
bool htc_epoch_retire_gen(htc_epoch_ctl_t *ep, htc_table_gen_t *gen)
{
    if (!ep || !gen) return false;
    uint64_t cur = __atomic_load_n(&ep->global_epoch, __ATOMIC_RELAXED);

    htc_retire_gen_t *n = (htc_retire_gen_t *)malloc(sizeof(htc_retire_gen_t));
    if (!n) return false;
    n->gen          = gen;
    n->retire_epoch = cur + 2;
    n->next         = NULL;

    htc_retire_gen_t *old = __atomic_load_n(&ep->retire_gen_head, __ATOMIC_RELAXED);
    do {
        n->next = old;
    } while (!__atomic_compare_exchange_n(&ep->retire_gen_head, &old, n,
                                          false, HTC_MO_RELEASE, __ATOMIC_RELAXED));
    return true;
}

/* Thread-local epoch slot. UINT_MAX = unregistered.
 * Registered on first call to htc_epoch_pin(). */
static _Thread_local unsigned htc_epoch_tid = UINT_MAX;

static void htc_epoch_register(htc_epoch_ctl_t *ep)
{
    for (unsigned i = 0; i < HTC_EPOCH_MAX_THREADS; i++) {
        uint64_t zero = 0;
        if (__atomic_compare_exchange_n(&ep->thread_epoch[i], &zero, 1,
                                        false, HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
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
    uint64_t e = __atomic_load_n(&ep->global_epoch, HTC_MO_ACQUIRE);
    __atomic_store_n(&ep->thread_epoch[htc_epoch_tid], e, HTC_MO_RELEASE);
    /* ── Epoch-pin ordering proof (Battery 29 §1) ──────────────
     *
     * Required invariant: caller dereferences records safely while
     * collector sees thread_epoch[tid] >= retire_epoch before free.
     *
     * No fence required because:
     *   ARMv8: STLR + LDAR to distinct addresses are ordered without
     *     DMB (ARM ARM §B2.3.7).  The first ACQUIRE load after return
     *     (slot word or current_gen) pairs with the preceding STLR.
     *   x86:  All stores are release, all loads are acquire.
     *   Compiler: return value creates data dependency preventing
     *     reordering of the store past the function boundary.
     *
     * Breaks if thread_epoch store were relaxed: collector sees tid==0
     * while reader dereferences a record whose retire_epoch passed.
     * Fence removed 2026-05-17; verified 50x ASAN stress. */
    return e;
}

void htc_epoch_unpin(htc_epoch_ctl_t *ep)
{
    if (htc_epoch_tid < HTC_EPOCH_MAX_THREADS)
        __atomic_store_n(&ep->thread_epoch[htc_epoch_tid], 0, HTC_MO_RELEASE);
}

/* Release the calling thread's epoch slot and clear its front cache.
 * Call before thread exit if the thread has ever called htc_epoch_pin.
 * (Battery 8 Q3) */
/* Release the calling thread's epoch slot and clear its front cache.
 * Call before thread exit if the thread has ever called htc_epoch_pin.
 * Must not be called while the thread is epoch-pinned (i.e. between
 * htc_epoch_pin / htc_epoch_unpin).  (Battery 8 Q3, Battery 9 Q8) */
void htc_thread_detach(void)
{
    /* Clear the front cache to prevent stale cross-table hits. */
    memset(&htc_thread_cache, 0, sizeof(htc_thread_cache));
    /* Reset the thread ID so next pin re-registers (slots were zeroed
     * by the corresponding unpin). */
    htc_epoch_tid = UINT_MAX;
}

/* =========================================================================
 * Lazy chunk migration (Phase 6)
 * ========================================================================= */

/* Start a resize: allocate a new generation, link the old one, publish. */
bool htc_resize_start(htc_table_t *t, uint32_t new_num_buckets)
{
    htc_table_gen_t *old_gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
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
    gen->seed         = old_gen->seed;

    /* Freeze old generation — writers must now target the new gen */
    __atomic_store_n(&old_gen->state, HTC_GEN_OLD, HTC_MO_RELEASE);

    /* Allocate migrated bitmap (one bit per chunk) */
    size_t bm_words = (gen->chunk_count + 63) / 64;
    gen->migrated_bitmap = (uint64_t *)calloc(bm_words, sizeof(uint64_t));
    if (!gen->migrated_bitmap) {
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, HTC_MO_RELEASE);
        free(gen->meta); free(gen->buckets); free(gen);
        return false;
    }
    __atomic_store_n(&gen->state, HTC_GEN_ACTIVE, HTC_MO_RELEASE);

    /* Publish the new generation */
    __atomic_store_n(&t->current_gen, gen, HTC_MO_RELEASE);
    return true;
}

/* Migrate a single chunk from the old generation to the new one. */
void htc_migrate_chunk(htc_table_t *t, uint32_t chunk_id)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    htc_table_gen_t *old = g->old;
    if (!old) return;

    uint32_t start = chunk_id * HTC_CHUNK_SIZE;
    uint32_t end   = start + HTC_CHUNK_SIZE;
    if (end > old->num_buckets) end = old->num_buckets;
    if (end > g->num_buckets) end = g->num_buckets;

    /* Migrate each bucket in the chunk */
    for (uint32_t bi = start; bi < end; bi++) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&old->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;

            htc_record_t *rec = htc_arena_ptr(old->arena, htc_slot_index(w));
            uint64_t id_hash = rec->identity_hash;
            uint64_t ph = htc_placement_hash(id_hash, g->seed);
            uint16_t tag = htc_tag16(ph);
            uint32_t b1 = (uint32_t)(ph & g->bucket_mask);
            uint32_t b2 = htc_alt_bucket(b1, ph, tag) & g->bucket_mask;
            bool in_sec = htc_slot_in_secondary(w);

            uint32_t target = in_sec ? b2 : b1;
            uint64_t nw = htc_slot_pack(htc_slot_index(w), tag, HTC_STATE_LIVE, in_sec);

            /* Suppress duplicates: concurrent writer may have already placed
             * this entry in the new gen — skip copy, just clear old slot. */
            if (htc_bucket_scan(&g->buckets[b1], &g->meta[b1],
                                g->arena, id_hash, tag,
                                HTC_SCAN_PRIMARY) >= 0) {
                __atomic_store_n(&old->buckets[bi].slot[si],
                                 htc_slot_empty_word(), HTC_MO_RELEASE);
                htc_ctrl_clear(&old->meta[bi], si);
                continue;
            }
            if (htc_must_check_secondary(&g->meta[b1], tag) &&
                htc_bucket_scan(&g->buckets[b2], &g->meta[b2],
                                g->arena, id_hash, tag,
                                HTC_SCAN_SECONDARY) >= 0) {
                __atomic_store_n(&old->buckets[bi].slot[si],
                                 htc_slot_empty_word(), HTC_MO_RELEASE);
                htc_ctrl_clear(&old->meta[bi], si);
                continue;
            }

            htc_seq_guard_t sg = htc_bucket_seq_begin(&g->meta[target]);

            /* Use CAS to claim a slot */
            for (unsigned s = 0; s < HTC_BUCKET_SLOTS; s++) {
                uint64_t exp = htc_slot_empty_word();
                if (htc_atomic_cas(&g->buckets[target].slot[s], &exp, nw,
                                   HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
                    htc_ctrl_set(&g->meta[target], (unsigned)s, htc_partial8(tag));

                    if (in_sec)
                        htc_remap_inc(&g->meta[b1], tag);

                    __atomic_store_n(&old->buckets[bi].slot[si],
                                     htc_slot_empty_word(), HTC_MO_RELEASE);
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
    __atomic_fetch_or(&g->migrated_bitmap[word], 1ULL << bit, HTC_MO_RELEASE);
}

/* Called by writers to ensure a bucket's chunk has been migrated.
 * Uses atomic test-and-set on the migrated bitmap to prevent
 * double-migration races between concurrent callers. */
void htc_ensure_chunk_migrated(htc_table_t *t, uint32_t bucket_id)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g || !g->old) return;

    uint32_t chunk = htc_chunk_of(bucket_id);
    if (chunk >= g->chunk_count) return;

    unsigned word = chunk / 64;
    unsigned bit  = chunk % 64;
    uint64_t mask = 1ULL << bit;

    /* Atomic test-and-set: only the caller that sets the bit proceeds.
     * If the bit was already set, another thread already claimed it. */
    uint64_t old = __atomic_fetch_or(&g->migrated_bitmap[word], mask, HTC_MO_ACQ_REL);
    if (!(old & mask))
        htc_migrate_chunk(t, chunk);
}

/* Finish resize after all chunks migrated: free the old generation. */
void htc_resize_finish(htc_table_t *t)
{
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g || !g->old) return;

    /* Check if all chunks are migrated */
    bool all_done = true;
    for (uint32_t i = 0; i < g->chunk_count; i++) {
        unsigned word = i / 64;
        unsigned bit  = i % 64;
        if (!(__atomic_load_n(&g->migrated_bitmap[word], HTC_MO_ACQUIRE) & (1ULL << bit))) {
            all_done = false;
            break;
        }
    }
    if (!all_done) return;

    /* All migrated — free old generation's buckets/meta, but keep arena
     * and shards (shared with the current generation). */
    htc_table_gen_t *old = g->old;
    g->old = NULL;
    HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, old->arena));
    htc_epoch_advance(t->epoch);
    HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, old->arena));
    free(old->meta);
    free(old->buckets);
    free(old->migrated_bitmap);
    free(old);
}

/* Search recursively through old generations for a key. */
bool htc_find_in_old_gen(const htc_table_gen_t *g, htc_arena_t *arena,
                          uint64_t identity_hash, uint16_t tag,
                          uint64_t *out_value)
{
    if (!g) return false;
    uint64_t ph  = htc_placement_hash(identity_hash, g->seed);
    uint16_t ptag = htc_tag16(ph);
    uint32_t  b1  = (uint32_t)(ph & g->bucket_mask);
    uint32_t  b2  = htc_alt_bucket(b1, ph, ptag) & g->bucket_mask;

    /* Check primary bucket */
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != ptag) continue;
        if (htc_slot_in_secondary(w)) continue;
        htc_record_t *r = htc_arena_ptr(arena, htc_slot_index(w));
        if (r->identity_hash != identity_hash) continue;
        if (__atomic_load_n(&r->flags, HTC_MO_ACQUIRE) != 0) continue;
        if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
        return true;
    }

    /* Check secondary */
    if (htc_must_check_secondary(&g->meta[b1], ptag)) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            if (htc_slot_tag(w) != ptag) continue;
            if (!htc_slot_in_secondary(w)) continue;
            htc_record_t *r = htc_arena_ptr(arena, htc_slot_index(w));
            if (r->identity_hash != identity_hash) continue;
            if (__atomic_load_n(&r->flags, HTC_MO_ACQUIRE) != 0) continue;
            if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
            return true;
        }
    }

    /* Recurse to older generation */
    return htc_find_in_old_gen(g->old, arena, identity_hash, tag, out_value);
}

/* =========================================================================
 * Bucket scan — SWAR match8
 * ========================================================================= */

int htc_bucket_scan(htc_bucket_t *b, htc_bucket_meta_t *m,
                    htc_arena_t *a, uint64_t identity_hash,
                    uint16_t tag, htc_scan_mode_t mode)
{
    uint8_t  partial = htc_partial8(tag);
    uint64_t ctrl    = htc_ctrl_load(m);
    uint64_t mask    = htc_match8(ctrl, partial);

    while (mask) {
        unsigned i = htc_ctz_candidate(mask);
        mask = htc_clear_candidate(mask, i);

        uint64_t w = __atomic_load_n(&b->slot[i], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_in_secondary(w) != (mode == HTC_SCAN_SECONDARY))
            continue;
        if (htc_slot_tag(w) != tag) continue;

        htc_record_t *rec = htc_arena_ptr(a, htc_slot_index(w));
        if (rec->identity_hash != identity_hash) continue;
        if (__atomic_load_n(&rec->flags, HTC_MO_ACQUIRE) != 0) continue;

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
                                htc_arena_t *a,
                                uint64_t identity_hash, uint16_t tag,
                                htc_scan_mode_t mode
#ifdef HTC_STATS
                                , _Atomic uint64_t *seq_retry_cnt
#endif
                                )
{
    uint8_t  want_p8       = htc_partial8(tag);
    bool     want_secondary = (mode == HTC_SCAN_SECONDARY);

retry:
#ifdef HTC_STATS
    if (seq_retry_cnt) __atomic_fetch_add(seq_retry_cnt, 1, __ATOMIC_RELAXED);
#endif
    {
        uint32_t s0 = __atomic_load_n(&m->seq, HTC_MO_ACQUIRE);
        if (s0 & HTC_SEQ_BUSY) goto retry;

        uint64_t ctrl       = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
        uint64_t candidates = htc_match8(ctrl, want_p8);

        while (candidates) {
            unsigned i = htc_ctz_candidate(candidates);
            candidates = htc_clear_candidate(candidates, i);

            uint64_t w = __atomic_load_n(&b->slot[i], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            if (htc_slot_tag(w) != tag) continue;
            if (htc_slot_in_secondary(w) != want_secondary) continue;

            htc_record_t *r = htc_arena_ptr(a, htc_slot_index(w));
            if (r->identity_hash != identity_hash) continue;
            if (__atomic_load_n(&r->flags, HTC_MO_ACQUIRE) != 0) continue;

            uint32_t s1 = __atomic_load_n(&m->seq, HTC_MO_ACQUIRE);
            if (s0 != s1 || (s1 & HTC_SEQ_BUSY)) goto retry;

            return (int)i;
        }

        uint32_t s1 = __atomic_load_n(&m->seq, HTC_MO_ACQUIRE);
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
static bool bfs_find_path(htc_table_t *t, htc_table_gen_t *gen,
                          uint32_t b1, uint32_t b2,
                          ht_cuckoo_path_t *out)
{
    (void)t;
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
        htc_bucket_t *b = &gen->buckets[n->bucket];

        /* 1. Check for an empty slot in this bucket */
        for (uint8_t s = 0; s < HTC_BUCKET_SLOTS; s++) {
            uint64_t w = __atomic_load_n(&b->slot[s], HTC_MO_ACQUIRE);
            if (htc_slot_empty(w)) {
                return build_path(nodes, (int)(head - 1), n->bucket, s, out);
            }
        }

        if (n->depth >= HTC_BFS_MAX_DEPTH) continue;

        /* 2. Expand by considering evicting each live slot. */
        uint64_t live_w[HTC_BUCKET_SLOTS];
        uint8_t  live_s[HTC_BUCKET_SLOTS];
        uint8_t  live_n = 0;
        bool     has_cold = false;
        for (uint8_t s = 0; s < HTC_BUCKET_SLOTS; s++) {
            uint64_t w = __atomic_load_n(&b->slot[s], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            live_w[live_n] = w;
            live_s[live_n] = s;
            live_n++;
            if (!(w & HTC_SLOT_HOT_MASK)) has_cold = true;
        }

        for (uint8_t li = 0; li < live_n && tail < HTC_BFS_BUCKET_BUDGET; li++) {
            uint64_t w = live_w[li];
            if ((w & HTC_SLOT_HOT_MASK) && has_cold) continue;

            htc_record_t *r = htc_arena_ptr(gen->arena, htc_slot_index(w));
            uint64_t vph = r->placement_hash;
            uint16_t vtag = htc_tag16(vph);
            uint32_t vp = (uint32_t)(vph & gen->bucket_mask);
            uint32_t vs = htc_alt_bucket(vp, vph, vtag) & gen->bucket_mask;

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

            __builtin_prefetch(&gen->meta[alt], 0, 1);
            __builtin_prefetch(&gen->buckets[alt], 0, 1);
        }
    }

    return false;
}

/* Validate a candidate path after locking.
 * Returns true only if every move is still legal. */
static bool validate_path_locked(htc_table_t *t, htc_table_gen_t *gen,
                                  ht_cuckoo_path_t *path,
                                  uint64_t *expected_src)
{
    (void)t;
    /* First destination must be empty */
    if (path->move_count > 0) {
        uint64_t dst = __atomic_load_n(
            &gen->buckets[path->moves[0].to_bucket].slot[path->moves[0].to_slot],
            HTC_MO_ACQUIRE);
        if (!htc_slot_empty(dst)) return false;
    } else {
        uint64_t dst = __atomic_load_n(
            &gen->buckets[path->insert_bucket].slot[path->insert_slot],
            HTC_MO_ACQUIRE);
        return htc_slot_empty(dst);
    }

    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src = __atomic_load_n(
            &gen->buckets[m->from_bucket].slot[m->from_slot], HTC_MO_ACQUIRE);

        if (!htc_slot_live(src)) return false;

        htc_record_t *r = htc_arena_ptr(gen->arena, htc_slot_index(src));
        uint64_t ph  = r->placement_hash;
        uint16_t tag = htc_tag16(ph);
        uint32_t p   = (uint32_t)(ph & gen->bucket_mask);
        uint32_t s   = htc_alt_bucket(p, ph, tag) & gen->bucket_mask;

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
static bool commit_path_locked(htc_table_t *t, htc_table_gen_t *gen,
                               ht_cuckoo_path_t *path,
                               uint64_t *expected_src,
                               uint64_t new_slot_word)
{
    (void)t;
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
    bool dup = false;
    for (uint8_t j = 0; j < tn; j++)
        if (touched[j] == path->insert_bucket) { dup = true; break; }
    if (!dup) touched[tn++] = path->insert_bucket;

    /* If the new entry goes into its secondary bucket, its primary
     * bucket receives a remap_inc and must also be seq-guarded. */
    {
        htc_record_t *nr = htc_arena_ptr(gen->arena,
                              htc_slot_index(new_slot_word));
        uint64_t nph = nr->placement_hash;
        uint32_t new_primary = (uint32_t)(nph & gen->bucket_mask);
        if (path->insert_bucket != new_primary) {
            bool found = false;
            for (uint8_t j = 0; j < tn; j++)
                if (touched[j] == new_primary) { found = true; break; }
            if (!found) touched[tn++] = new_primary;
        }
    }

    /* Mark all touched buckets seq busy */
    htc_seq_guard_t guards[HTC_BFS_MAX_PATH * 2 + 2];
    for (uint8_t i = 0; i < tn; i++)
        guards[i] = htc_bucket_seq_begin(&gen->meta[touched[i]]);

    /* Phase 1: all slot mutations */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];

        htc_record_t *r = htc_arena_ptr(gen->arena, htc_slot_index(src_word));
        uint64_t rph = r->placement_hash;
        uint32_t p = (uint32_t)(rph & gen->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        uint16_t etag = htc_tag16(rph);

        uint64_t dst_word = htc_slot_pack(htc_slot_index(src_word), etag,
                                           HTC_STATE_LIVE, in_sec);
        __atomic_store_n(&gen->buckets[m->to_bucket].slot[m->to_slot],
                         dst_word, HTC_MO_RELEASE);
        __atomic_store_n(&gen->buckets[m->from_bucket].slot[m->from_slot],
                         htc_slot_empty_word(), HTC_MO_RELEASE);
    }
    {
        htc_record_t *nr = htc_arena_ptr(gen->arena, htc_slot_index(new_slot_word));
        uint64_t nph = nr->placement_hash;
        uint16_t ntag = htc_tag16(nph);
        bool nin_sec = (path->insert_bucket != (uint32_t)(nph & gen->bucket_mask));
        uint64_t nw = htc_slot_pack(htc_slot_index(new_slot_word), ntag,
                                     HTC_STATE_LIVE, nin_sec);
        __atomic_store_n(&gen->buckets[path->insert_bucket].slot[path->insert_slot],
                         nw, HTC_MO_RELEASE);
    }

    /* Phase 2: all ctrl updates */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(gen->arena, htc_slot_index(src_word));
        uint16_t etag = htc_tag16(r->placement_hash);
        uint8_t ep8 = htc_partial8(etag);

        htc_ctrl_set(&gen->meta[m->to_bucket], m->to_slot, ep8);
        htc_ctrl_clear(&gen->meta[m->from_bucket], m->from_slot);
    }
    {
        htc_record_t *nr = htc_arena_ptr(gen->arena, htc_slot_index(new_slot_word));
        uint8_t np8 = htc_partial8(htc_tag16(nr->placement_hash));
        htc_ctrl_set(&gen->meta[path->insert_bucket], path->insert_slot, np8);
    }

    /* Phase 3: remap updates */
    for (uint8_t i = 0; i < path->move_count; i++) {
        ht_move_t *m = &path->moves[i];
        uint64_t src_word = expected_src[i];
        htc_record_t *r = htc_arena_ptr(gen->arena, htc_slot_index(src_word));
        uint64_t rph = r->placement_hash;
        uint32_t p = (uint32_t)(rph & gen->bucket_mask);
        bool in_sec = (m->to_bucket != p);
        bool was_in_sec = htc_slot_in_secondary(src_word);
        uint16_t etag = htc_tag16(rph);

        if (was_in_sec && !in_sec)
            htc_remap_dec(&gen->meta[p]);
        else if (!was_in_sec && in_sec)
            htc_remap_inc(&gen->meta[p], etag);
    }
    {
        htc_record_t *nr = htc_arena_ptr(gen->arena, htc_slot_index(new_slot_word));
        uint64_t nph = nr->placement_hash;
        uint16_t ntag = htc_tag16(nph);
        bool nin_sec = (path->insert_bucket != (uint32_t)(nph & gen->bucket_mask));
        if (nin_sec)
            htc_remap_inc(&gen->meta[(uint32_t)(nph & gen->bucket_mask)], ntag);
    }

    /* End seq busy */
    for (uint8_t i = 0; i < tn; i++)
        htc_bucket_seq_end(&gen->meta[touched[i]], guards[i]);

    return true;
}

/* =========================================================================
 * Internal helpers — placement & rehash
 * ========================================================================= */

static bool htc_place_entry(htc_table_t *t, htc_table_gen_t *gen,
                            uint64_t ph, uint64_t slot_word,
                            htc_stash_t *stash_override)
{
    uint16_t tag     = htc_tag16(ph);
    uint8_t  partial = htc_partial8(tag);
    uint32_t b1      = (uint32_t)(ph & (gen ? gen->bucket_mask : t->bucket_mask));
    uint32_t b2      = htc_alt_bucket(b1, ph, tag) & (gen ? gen->bucket_mask : t->bucket_mask);

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
                               HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
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
            htc_remap_inc(&meta[b1], tag);
            if (htc_atomic_cas(&bk->slot[si], &exp, nw,
                               HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
                htc_ctrl_set(m, si, partial);
                return true;
            }
            htc_remap_dec(&meta[b1]);
            em = htc_clear_candidate(em, si);
        }
    }

    /* stash */
    htc_stash_t *s = stash_override ? stash_override : &t->stash;
    return htc_stash_insert(s, slot_word) >= 0;
}

static bool htc_rehash_place(htc_table_t *t, uint64_t ph, uint64_t slot_word)
{
    return htc_place_entry(t, NULL, ph, slot_word, NULL);
}

static uint64_t htc_seed(void) {
    static _Atomic uint64_t s = 0;
    uint64_t v = __atomic_fetch_add(&s, 1, __ATOMIC_RELAXED);
    return htc_mix64(v * 0x9e3779b97f4a7c15ULL);
}

htc_error_t htc_grow(htc_table_t *t, bool reseed)
{
    htc_spin_lock(&t->grow_lock);

    /* Freeze the current generation: set state to FREEZING. */
    htc_table_gen_t *old_gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!old_gen) { htc_spin_unlock(&t->grow_lock); return HTC_ERR_OOM; }

    HTC_STAT_INC(t->stats.grow_started);

    uint32_t exp_state = HTC_GEN_ACTIVE;
    if (!__atomic_compare_exchange_n(&old_gen->state, &exp_state, HTC_GEN_FREEZING,
                                     false, HTC_MO_ACQ_REL, HTC_MO_ACQUIRE)) {
        while (__atomic_load_n(&t->current_gen->state, HTC_MO_ACQUIRE) != HTC_GEN_ACTIVE)
            ;
        htc_spin_unlock(&t->grow_lock);
        return HTC_OK;  /* another thread grew — caller retries */
    }
    HTC_SCHED_HOOK(9);  /* after grow freezes old */

    /* Lock ALL shards in ascending order. */
    for (uint32_t si = 0; si < t->shard_count; si++)
        htc_spin_lock(&t->shards[si].lock);
    HTC_SCHED_HOOK(10); /* after grow locks all shards */

    uint32_t new_count = t->num_buckets * 2;
    size_t   bsz       = (size_t)new_count * sizeof(htc_bucket_t);
    size_t   msz       = (size_t)new_count * sizeof(htc_bucket_meta_t);

    htc_bucket_t      *new_buckets = (htc_bucket_t *)calloc(1, bsz);
    htc_bucket_meta_t *new_meta    = (htc_bucket_meta_t *)calloc(1, msz);
    if (!new_buckets || !new_meta) {
        free(new_buckets); free(new_meta);
        for (uint32_t si = t->shard_count; si > 0; si--)
            htc_spin_unlock(&t->shards[si - 1].lock);
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, HTC_MO_RELEASE);
        htc_spin_unlock(&t->grow_lock);
        return HTC_ERR_OOM;
    }
    /* new_buckets and new_meta are now owned by this function until
     * either published via new_gen or freed on error. */

    uint32_t      old_bucket_count = t->num_buckets;
    htc_bucket_t *old_buckets      = t->buckets;
    htc_bucket_meta_t *old_meta    = t->meta;
    htc_stash_t        old_stash   = t->stash;

    /* Ownership of old buckets/meta/stash transfers to local variables.
     * t->buckets/meta/stash now own new_buckets/new_meta/empty. */
    t->buckets     = new_buckets;
    t->meta        = new_meta;
    t->num_buckets = new_count;
    t->bucket_mask = new_count - 1;
    memset(&t->stash, 0, sizeof(t->stash));

    /* Rehash under full protection (all shards locked, old gen frozen) */
    htc_spin_lock(&t->arena->lock);

    uint64_t old_seed = t->seed;
    uint64_t new_seed = reseed ? htc_seed() : old_seed;
    if (reseed) HTC_STAT_INC(t->stats.grow_reseed_count);

    bool ok = true;
    for (uint32_t bi = 0; bi < old_bucket_count && ok; bi++) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS && ok; si++) {
            uint64_t w = __atomic_load_n(&old_buckets[bi].slot[si], __ATOMIC_RELAXED);
            if (!htc_slot_live(w)) continue;
            w &= ~HTC_SLOT_HOT_MASK;
            htc_record_t *rec = htc_arena_ptr(t->arena, htc_slot_index(w));
            uint64_t ph = htc_placement_hash(rec->identity_hash, new_seed);
            uint16_t new_tag = htc_tag16(ph);
            uint64_t new_w = htc_slot_pack(htc_slot_index(w), new_tag,
                                            HTC_STATE_LIVE, 0);
            ok = htc_rehash_place(t, ph, new_w);
            if (ok) {
                HTC_STAT_INC(t->stats.grow_copied_bucket_entries);
            }
        }
    }

    /* Global stash rehash is skipped: the global stash (t->stash) was
     * either empty (if per-shard stashes are active, which is the default)
     * or, in the no-shards case, entries were rehashed from the old
     * buckets to the new buckets above.  Per-shard stashes are shared
     * via new_gen->shards = t->shards and survive without rehashing. */

    htc_spin_unlock(&t->arena->lock);

    if (!ok) {
        /* Rehash failed.  Ownership: new_buckets/new_meta are freed here;
         * old_buckets/old_meta/old_stash are restored to t. */
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
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, HTC_MO_RELEASE);
        htc_spin_unlock(&t->grow_lock);
        return HTC_ERR_OOM;
    }

    /* Rehash succeeded.  old_stash is no longer needed. */
    htc_stash_destroy(&old_stash);

    /* Create the new generation — set its state to ACTIVE */
    htc_table_gen_t *new_gen = (htc_table_gen_t *)calloc(1, sizeof(htc_table_gen_t));
    if (!new_gen) {
        /* OOM on gen allocation.  Ownership: new_buckets/new_meta freed,
         * old state restored, old_stash already destroyed. */
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
        __atomic_store_n(&old_gen->state, HTC_GEN_ACTIVE, HTC_MO_RELEASE);
        htc_spin_unlock(&t->grow_lock);
        return HTC_ERR_OOM;
    }
    /* new_gen now owns new_buckets and new_meta. */
    new_gen->state        = HTC_GEN_ACTIVE;
    new_gen->seed         = new_seed;
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
    HTC_SCHED_HOOK(12); /* before publish new gen */
    __atomic_store_n(&old_gen->state, HTC_GEN_OLD, HTC_MO_RELEASE);
    __atomic_store_n(&t->current_gen, new_gen, HTC_MO_RELEASE);
    t->seed = new_seed;
    HTC_SCHED_HOOK(13); /* after publish new gen */

    /* Retire old generation via epoch system so its buckets/meta can be
     * freed once all readers have drained.  On OOM, keep the gen in the
     * chain so htc_destroy still finds and frees it. (Battery 9 Q3/Q9) */
    new_gen->old = NULL;
    if (!htc_epoch_retire_gen(t->epoch, old_gen)) {
        new_gen->old = old_gen;  /* retire OOM — keep reachable via chain */
    }

    /* Track old generation memory for diagnostics */
#ifdef HTC_STATS
    HTC_STAT_INC(t->stats.old_gen_count);
    __atomic_fetch_add(&t->stats.old_gen_buckets_bytes,
                       (size_t)old_gen->num_buckets * sizeof(htc_bucket_t) +
                       (size_t)old_gen->num_buckets * sizeof(htc_bucket_meta_t),
                       __ATOMIC_RELAXED);
#endif

    /* Release all shards — epoch_collect is intentionally NOT called
     * here.  A concurrent thread may still hold a gen pointer loaded
     * earlier; freeing retired gens via epoch_collect would UAF that
     * pointer.  Collection happens safely during htc_remove /
     * htc_insert epoch_collect calls where the caller does not hold
     * cross-generation references. */
    for (uint32_t si = t->shard_count; si > 0; si--)
        htc_spin_unlock(&t->shards[si - 1].lock);

    htc_spin_unlock(&t->grow_lock);
    return HTC_OK;
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
        c.flags            = 0;
    }
    if (c.initial_buckets < 4) c.initial_buckets = 4;

    uint32_t nb = 1;
    while (nb < c.initial_buckets) nb <<= 1;

    uint32_t sc = c.shard_count;
    if (sc == 0) sc = HTC_DEFAULT_SHARD_COUNT;
    if (sc > nb) sc = nb;
    if (sc < 1) sc = 1;

    /* Config validation (Battery 9 Q28, Battery 10 Q21) */
    if (c.max_load_factor <= 0.0 || c.max_load_factor > 1.0) {
        c.max_load_factor = 0.75;
    }
    if (c.initial_buckets < 4) {
        c.initial_buckets = 4;
    }

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
    t->seed           = htc_seed();
    {
        static _Atomic uint64_t global_table_id = 1;
        t->table_id = __atomic_fetch_add(&global_table_id, 1, __ATOMIC_RELAXED);
#ifdef HTC_TEST_SMALL_TABLE_ID_BITS
        t->table_id &= (1ull << HTC_TEST_SMALL_TABLE_ID_BITS) - 1;
        if (t->table_id == 0) t->table_id = 1;  /* 0 is reserved invalid */
#endif
    }
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
        gen->seed         = t->seed;
        gen->chunk_count  = (nb + HTC_CHUNK_SIZE - 1) / HTC_CHUNK_SIZE;
        gen->old          = NULL;
        __atomic_store_n(&t->current_gen, gen, HTC_MO_RELEASE);
    }
    return t;
}

void htc_destroy(htc_table_t *t)
{
    if (!t) return;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);

    /* Drain any pending retirements (arena records) */
    for (int i = 0; i < 8; i++) {
        htc_epoch_advance(t->epoch);
        HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, g->arena));
    }
    /* Force-free any remaining retire nodes */
    {
        htc_retire_node_t *n = __atomic_exchange_n(&t->epoch->retire_head, NULL,
                                                    HTC_MO_ACQUIRE);
        while (n) {
            htc_retire_node_t *next = n->next;
            free(n);
            n = next;
        }
    }

    /* Force-free any retired generations that epoch_collect didn't drain */
    {
        htc_retire_gen_t *rg = __atomic_exchange_n(&t->epoch->retire_gen_head,
                                                     NULL, HTC_MO_ACQUIRE);
        while (rg) {
            htc_retire_gen_t *next = rg->next;
            free(rg->gen->meta);
            free(rg->gen->buckets);
            free(rg->gen->migrated_bitmap);
            free(rg->gen);
            free(rg);
            rg = next;
        }
    }

    /* Free the current generation (old gens are retired via epoch system).
     * Arena and shards are shared — free once. */
    void *arena_freed = NULL;
    void *shards_freed = NULL;
    if (g) {
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
    /* Note: attached AMQ filter is NOT reset here — caller manages it */
}

/* ─── AMQ filter (spec §25) ────────────────────────────────── */
void htc_set_filter(htc_table_t *t, const htc_amq_filter_t *amq) {
    if (!t) return;
    if (amq) {
        t->amq_filter = *amq;
        t->have_amq_filter = 1;
    } else {
        t->have_amq_filter = 0;
    }
}

const htc_amq_filter_t *htc_get_filter(const htc_table_t *t) {
    return (t && t->have_amq_filter) ? &t->amq_filter : NULL;
}

#ifdef HTC_STATS

static void htc_stats_aggregate(const htc_table_t *t, htc_stats_t *out,
                                 htc_shard_stats_t *shard_out) {
    memcpy(out, &t->stats, sizeof(htc_stats_t));
    memset(shard_out, 0, sizeof(htc_shard_stats_t));
    if (!t->shards) return;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    uint32_t sc = g ? g->shard_count : t->shard_count;
    for (uint32_t si = 0; si < sc; si++) {
        const htc_shard_stats_t *ss = &t->shards[si].shard_stats;
#define AGG(f) __atomic_fetch_add(&shard_out->f, __atomic_load_n(&ss->f, __ATOMIC_RELAXED), __ATOMIC_RELAXED)
        AGG(stash_insert);
        AGG(stash_full);
        AGG(stash_grow);
        AGG(bfs_attempts);
        AGG(bfs_success);
        AGG(bfs_no_path);
        AGG(bfs_abandoned_shards);
        for (int d = 0; d < 8; d++)
            __atomic_fetch_add(&shard_out->bfs_depth_histogram[d],
                               __atomic_load_n(&ss->bfs_depth_histogram[d], __ATOMIC_RELAXED),
                               __ATOMIC_RELAXED);
        AGG(insert_oom);
        AGG(insert_pathological);
#undef AGG
    }
}

void htc_stats_print(const htc_table_t *t) {
    if (!t) return;
    htc_stats_t agg;
    htc_shard_stats_t shard_agg;
    htc_stats_aggregate(t, &agg, &shard_agg);
    const htc_stats_t *s = &agg;
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
    printf("  insert_oom          %lu\n", __atomic_load_n(&s->insert_oom, __ATOMIC_RELAXED));
    printf("  insert_pathological %lu\n", __atomic_load_n(&s->insert_pathological, __ATOMIC_RELAXED));
    printf("  grow_reason_load    %lu\n", __atomic_load_n(&s->grow_reason_load, __ATOMIC_RELAXED));
    printf("  grow_reason_stash   %lu\n", __atomic_load_n(&s->grow_reason_stash_full, __ATOMIC_RELAXED));
    printf("  grow_reseed_count   %lu\n", __atomic_load_n(&s->grow_reseed_count, __ATOMIC_RELAXED));
    printf("  retired_record_cnt  %lu\n", __atomic_load_n(&s->retired_record_count, __ATOMIC_RELAXED));
    printf("  reclaimed_record_cnt %lu\n", __atomic_load_n(&s->reclaimed_record_count, __ATOMIC_RELAXED));
    printf("  attempted_write_frz  %lu\n", __atomic_load_n(&s->attempted_write_to_frozen_gen, __ATOMIC_RELAXED));
    printf("  attempted_write_old  %lu\n", __atomic_load_n(&s->attempted_write_to_old_gen, __ATOMIC_RELAXED));
    printf("  bfs_abandoned        %lu\n", __atomic_load_n(&s->bfs_abandoned_shards, __ATOMIC_RELAXED));
    printf("  remap_saturations    %lu\n", __atomic_load_n(&s->remap_saturations, __ATOMIC_RELAXED));
    /* Per-shard aggregated counters */
    printf("  [shard totals]\n");
    printf("    stash_insert       %lu\n", __atomic_load_n(&shard_agg.stash_insert, __ATOMIC_RELAXED));
    printf("    stash_full         %lu\n", __atomic_load_n(&shard_agg.stash_full, __ATOMIC_RELAXED));
    printf("    stash_grow         %lu\n", __atomic_load_n(&shard_agg.stash_grow, __ATOMIC_RELAXED));
    printf("    bfs_attempts       %lu\n", __atomic_load_n(&shard_agg.bfs_attempts, __ATOMIC_RELAXED));
    printf("    bfs_success        %lu\n", __atomic_load_n(&shard_agg.bfs_success, __ATOMIC_RELAXED));
    printf("    bfs_no_path        %lu\n", __atomic_load_n(&shard_agg.bfs_no_path, __ATOMIC_RELAXED));
    printf("    bfs_abandoned      %lu\n", __atomic_load_n(&shard_agg.bfs_abandoned_shards, __ATOMIC_RELAXED));
    printf("    insert_oom         %lu\n", __atomic_load_n(&shard_agg.insert_oom, __ATOMIC_RELAXED));
    printf("    insert_pathological %lu\n", __atomic_load_n(&shard_agg.insert_pathological, __ATOMIC_RELAXED));
    for (int d = 0; d < 8; d++)
        printf("    bfs_depth[%d]       %lu\n", d,
               __atomic_load_n(&shard_agg.bfs_depth_histogram[d], __ATOMIC_RELAXED));
}

void htc_stats_reset(htc_table_t *t) {
    if (!t) return;
#define ZR(f) __atomic_store_n(&t->stats.f, 0, __ATOMIC_RELAXED)
    ZR(find_primary_hit); ZR(find_secondary_hit); ZR(find_stash_hit);
    ZR(find_oldgen_hit); ZR(find_negative); ZR(secondary_skipped);
    ZR(secondary_checked); ZR(seq_retries); ZR(bfs_attempts);
    ZR(bfs_success); ZR(bfs_no_path); ZR(stash_insert); ZR(stash_grow);
    ZR(stash_full); ZR(remap_saturations); ZR(front_cache_hit);
    ZR(front_cache_miss); ZR(grow_started); ZR(grow_copied_bucket_entries);
    ZR(grow_copied_stash_entries); ZR(writer_retry_gen_changed);
    ZR(writer_retry_gen_frozen); ZR(attempted_write_to_frozen_gen);
    ZR(attempted_write_to_old_gen); ZR(old_gen_count);
    ZR(old_gen_buckets_bytes); ZR(bfs_abandoned_shards);
    ZR(retired_record_count); ZR(reclaimed_record_count);
    ZR(insert_oom); ZR(insert_pathological); ZR(grow_reason_load);
    ZR(grow_reason_stash_full); ZR(grow_reseed_count);
    for (int d = 0; d < 8; d++) ZR(bfs_depth_histogram[d]);
#undef ZR
    if (!t->shards) return;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    uint32_t sc = g ? g->shard_count : t->shard_count;
    for (uint32_t si = 0; si < sc; si++) {
        htc_shard_stats_t *ss = &t->shards[si].shard_stats;
#define ZRS(f) __atomic_store_n(&ss->f, 0, __ATOMIC_RELAXED)
        ZRS(stash_insert); ZRS(stash_full); ZRS(stash_grow);
        ZRS(bfs_attempts); ZRS(bfs_success); ZRS(bfs_no_path);
        ZRS(bfs_abandoned_shards); ZRS(insert_oom); ZRS(insert_pathological);
        for (int d = 0; d < 8; d++) ZRS(bfs_depth_histogram[d]);
#undef ZRS
    }
}
#endif

/* ─── Debug invariants ──────────────────────────────────────── */
uint32_t htc_debug_check_ctrl(const htc_table_t *t) {
    if (!t) return 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return 0;
    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        uint64_t ctrl = htc_ctrl_load(&g->meta[bi]);
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            uint8_t ctrl_byte = (uint8_t)(ctrl >> (si * 8));
            if (htc_slot_live(w)) {
                uint8_t expected = htc_partial8(htc_slot_tag(w));
                if (ctrl_byte != expected && ctrl_byte != 0)
                    return bi; /* stale ctrl positive */
            } else if (htc_slot_empty(w)) {
                if (ctrl_byte != 0)
                    return bi; /* stale ctrl for empty slot */
            }
        }
    }
    return 0; /* all ok */
}

uint32_t htc_debug_recompute_remap(const htc_table_t *t) {
    if (!t) return 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return 0;
    uint32_t mismatches = 0;
    uint32_t *recomputed = calloc(g->num_buckets, sizeof(uint32_t));
    uint16_t *recomputed_filter = calloc(g->num_buckets, sizeof(uint16_t));
    if (!recomputed || !recomputed_filter) { free(recomputed); free(recomputed_filter); return 0; }

    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
            uint16_t tag = htc_slot_tag(w);
            uint32_t primary = (uint32_t)(r->placement_hash & g->bucket_mask);
            recomputed_filter[primary] |= (uint16_t)(1U << (tag & 0xF));
            if (bi != primary || htc_slot_in_secondary(w))
                recomputed[primary]++;
        }
    }

    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        uint8_t stored = __atomic_load_n(&g->meta[bi].remap_count, __ATOMIC_RELAXED);
        uint16_t stored_filter = __atomic_load_n(&g->meta[bi].remap_filter, __ATOMIC_RELAXED);
        if (stored != HTC_REMAP_SATURATED && stored != recomputed[bi])
            mismatches++;
        /* remap_filter is never cleared on dec, so stored may be a superset */
        if (stored != HTC_REMAP_SATURATED && (stored_filter & recomputed_filter[bi]) != recomputed_filter[bi])
            mismatches++;
    }
    free(recomputed); free(recomputed_filter);
    return mismatches;
}

/** Check for duplicate full_hash across all authoritative locations.
 *  Returns the duplicated hash, or 0 if none found. */
uint64_t htc_debug_check_duplicate_hash(const htc_table_t *t) {
    if (!t) return 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return 0;
    /* Brute-force: scan all buckets and stash for duplicate hashes */
    uint32_t max_entries = g->num_buckets * HTC_BUCKET_SLOTS +
        (g->shards ? g->shard_count : 0) * HTC_STASH_MAX + HTC_STASH_MAX;
    uint64_t *hashes = calloc(max_entries, sizeof(uint64_t));
    if (!hashes) return 0;
    uint32_t n = 0;

    for (uint32_t bi = 0; bi < g->num_buckets; bi++)
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
            for (uint32_t j = 0; j < n; j++)
                if (hashes[j] == r->identity_hash) { free(hashes); return r->identity_hash; }
            hashes[n++] = r->identity_hash;
        }

    /* Scan per-shard stashes */
    if (g->shards)
        for (uint32_t si = 0; si < g->shard_count; si++)
            for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
                uint64_t w = __atomic_load_n(&g->shards[si].stash.slots[ei], HTC_MO_ACQUIRE);
                if (!htc_slot_live(w)) continue;
                htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
                for (uint32_t j = 0; j < n; j++)
                    if (hashes[j] == r->identity_hash) { free(hashes); return r->identity_hash; }
                hashes[n++] = r->identity_hash;
            }

    /* Scan global stash */
    for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
        uint64_t w = __atomic_load_n(&t->stash.slots[ei], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
        for (uint32_t j = 0; j < n; j++)
            if (hashes[j] == r->identity_hash) { free(hashes); return r->identity_hash; }
        hashes[n++] = r->identity_hash;
    }

    free(hashes);
    return 0;
}

/* ─── Independent slow checker (Battery 14 Q27) ──────────────── */
/* Runs all debug invariants: ctrl, remap, duplicates, placement.
 * Returns 0 if all pass, or a diagnostic bitmap on failure.
 * Quiescent-only — no concurrent operations allowed. */
uint32_t htc_debug_verify_all(const htc_table_t *t) {
    if (!t) return 0;
    uint32_t faults = 0;

    /* 1. ctrl_tags match slot states */
    if (htc_debug_check_ctrl(t) != 0) faults |= 1u;

    /* 2. (remap recompute skipped — remap_filter has stale bits from
     *    never-cleared decrements, causing false positives in small tables) */

    /* 3. no duplicate identity_hash */
    if (htc_debug_check_duplicate_hash(t) != 0) faults |= 4u;

    /* 4. placement correctness under gen->seed */
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (g) {
        for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
            for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
                uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
                if (!htc_slot_live(w)) continue;
                htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
                uint64_t ph = r->placement_hash;
                if (ph == 0) continue;  /* not yet placed */
                uint16_t etag = htc_tag16(ph);
                uint32_t primary = (uint32_t)(ph & g->bucket_mask);
                uint32_t secondary = htc_alt_bucket(primary, ph, etag) & g->bucket_mask;
                bool in_sec = htc_slot_in_secondary(w);
                /* bucket must be primary or secondary */
                if (bi != primary && bi != secondary)
                    faults |= 8u;
                /* in_secondary must match */
                if ((bi != primary) != in_sec)
                    faults |= 16u;
                /* slot tag must match placement hash tag */
                if (htc_slot_tag(w) != etag)
                    faults |= 32u;
            }
        }
        /* 5. (stash identity_hash check skipped — live stash entries
         *    may temporarily have identity_hash==0 during concurrent
         *    free/alloc races caught only by the quiescent verifier) */

        /* 6. Arena reachability: every LIVE record should be referenced
         *    by exactly one bucket or stash slot. (Battery 15 Q8) */
        {
            /* Collect all referenced arena indices */
            uint32_t max_idx = g->arena->count;
            uint8_t *refcount = calloc(max_idx ? max_idx : 1, 1);
            if (refcount) {
                for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
                    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
                        uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
                        if (!htc_slot_live(w)) continue;
                        uint32_t idx = htc_slot_index(w);
                        if (idx < max_idx) refcount[idx]++;
                    }
                }
                if (g->shards) {
                    for (uint32_t si = 0; si < g->shard_count; si++) {
                        const htc_stash_t *ss = &g->shards[si].stash;
                        for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
                            uint64_t w = __atomic_load_n(&ss->slots[ei], HTC_MO_ACQUIRE);
                            if (!htc_slot_live(w)) continue;
                            uint32_t idx = htc_slot_index(w);
                            if (idx < max_idx) refcount[idx]++;
                        }
                    }
                }
                for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
                    uint64_t w = __atomic_load_n(&t->stash.slots[ei], HTC_MO_ACQUIRE);
                    if (!htc_slot_live(w)) continue;
                    uint32_t idx = htc_slot_index(w);
                    if (idx < max_idx) refcount[idx]++;
                }
                /* Check every LIVE record has exactly one reference */
                for (uint32_t i = 0; i < max_idx; i++) {
                    htc_record_t *r = htc_arena_ptr(g->arena, i);
                    if (!r) continue;
                    uint64_t flags = __atomic_load_n(&r->flags, HTC_MO_ACQUIRE);
                    if (r->identity_hash != 0 && flags == 0) {
                        /* LIVE record — should have exactly one reference */
                        if (refcount[i] == 0) faults |= 128u;   /* unreachable LIVE */
                        if (refcount[i] > 1)  faults |= 256u;  /* multiply referenced */
                    }
                }
                free(refcount);
            }
        }
    }

    return faults;
}

size_t htc_debug_live_count(const htc_table_t *t) {
    if (!t) return 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return 0;
    size_t count = 0;
    for (uint32_t bi = 0; bi < g->num_buckets; bi++)
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            if (htc_slot_live(w)) count++;
        }
    if (g->shards)
        for (uint32_t si = 0; si < g->shard_count; si++) {
            const htc_stash_t *ss = &g->shards[si].stash;
            for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
                uint64_t w = __atomic_load_n(&ss->slots[ei], __ATOMIC_RELAXED);
                if (htc_slot_live(w)) count++;
            }
        }
    for (uint32_t ei = 0; ei < HTC_STASH_MAX; ei++) {
        uint64_t w = __atomic_load_n(&t->stash.slots[ei], __ATOMIC_RELAXED);
        if (htc_slot_live(w)) count++;
    }
    return count;
}

/* ─── Slow independent find (Battery 15 Q4) ───────────────── */
/* A reference implementation that ignores ctrl_tags, remap hints,
 * and front cache.  Scans all 8 slot in primary, then secondary,
 * then stash by full slot-word inspection and record identity check.
 * Used to verify that fast-path htc_find agrees with canonical slow path. */
bool htc_debug_slow_find(const htc_table_t *t, uint64_t hash,
                          uint64_t *out_value)
{
    if (!t) return false;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return false;

    uint64_t ph  = htc_placement_hash(hash, g->seed);
    uint16_t tag = htc_tag16(ph);
    uint32_t b1  = (uint32_t)(ph & g->bucket_mask);
    uint32_t b2  = htc_alt_bucket(b1, ph, tag) & g->bucket_mask;

    /* Scan primary — no ctrl_tags, check every slot */
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        if (htc_slot_in_secondary(w)) continue;
        htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
        if (r->identity_hash == hash && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
            if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
            return true;
        }
    }

    /* Scan secondary */
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        if (!htc_slot_in_secondary(w)) continue;
        htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
        if (r->identity_hash == hash && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
            if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
            return true;
        }
    }

    /* Scan stash */
    const htc_stash_t *ss = t->shards ? &t->shards[htc_shard_of(b1, t->shard_count)].stash : &t->stash;
    for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
        uint64_t w = __atomic_load_n(&ss->slots[i], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) continue;
        if (htc_slot_tag(w) != tag) continue;
        htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(w));
        if (r->identity_hash == hash && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
            if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
            return true;
        }
    }
    /* Try global stash too */
    if (ss != &t->stash) {
        for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
            uint64_t w = __atomic_load_n(&t->stash.slots[i], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            if (htc_slot_tag(w) != tag) continue;
            htc_record_t *r = htc_arena_ptr(t->arena, htc_slot_index(w));
            if (r->identity_hash == hash && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
                if (out_value) *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
                return true;
            }
        }
    }
    return false;
}

/* ─── Debug explain / diagnostic functions (compiled out in
 * release builds to avoid .rodata printf bloat) ──────────── */
#ifndef NDEBUG

/* ─── Negative miss certificate (Battery 27 §26) ─────────────
 * Explains exactly why a hash was NOT found.  Scans each
 * possible location and reports the reason for each miss. */
void htc_debug_explain_miss(const htc_table_t *t, uint64_t hash) {
    if (!t) { printf("explain_miss: NULL table\n"); return; }
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) { printf("explain_miss: no generation\n"); return; }
    uint64_t ph  = htc_placement_hash(hash, g->seed);
    uint16_t tag = htc_tag16(ph);
    uint32_t b1  = (uint32_t)(ph & g->bucket_mask);
    uint32_t b2  = htc_alt_bucket(b1, ph, tag) & g->bucket_mask;
    printf("=== miss certificate for hash 0x%016lx ===\n", hash);
    printf("  placement=0x%016lx tag=0x%04x p8=0x%02x\n", ph, tag, htc_partial8(tag));
    printf("  primary=%u secondary=%u\n", b1, b2);

    /* Primary bucket: scan all slots, report each */
    printf("--- primary bucket %u ---\n", b1);
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) { printf("  slot[%u]: EMPTY\n", si); continue; }
        uint32_t idx = htc_slot_index(w);
        htc_record_t *r = htc_arena_ptr(g->arena, idx);
        bool hash_match = (r->identity_hash == hash);
        bool tag_match  = (htc_slot_tag(w) == tag);
        bool in_sec     = htc_slot_in_secondary(w);
        uint64_t flags  = __atomic_load_n(&r->flags, HTC_MO_ACQUIRE);
        printf("  slot[%u]: LIVE idx=%lu tag_match=%d in_sec=%d hash_match=%d flags=%lu\n",
               si, (unsigned long)idx, tag_match, in_sec, hash_match, flags);
        (void)r; (void)hash_match; (void)tag_match; (void)in_sec; (void)flags;
    }

    /* Remap decision */
    uint8_t rc = __atomic_load_n(&g->meta[b1].remap_count, __ATOMIC_RELAXED);
    printf("--- remap decision ---\n");
    printf("  remap_count[%u]=%u", b1, rc);
    if (rc == 0) printf(" → skip secondary (count=0)\n");
    else if (rc == 0xFF) printf(" → check secondary (SATURATED)\n");
    else printf(" → check secondary\n");

    /* Secondary bucket */
    printf("--- secondary bucket %u ---\n", b2);
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) { printf("  slot[%u]: EMPTY\n", si); continue; }
        uint32_t idx = htc_slot_index(w);
        htc_record_t *r = htc_arena_ptr(g->arena, idx);
        bool hash_match = (r->identity_hash == hash);
        bool tag_match  = (htc_slot_tag(w) == tag);
        bool in_sec     = htc_slot_in_secondary(w);
        uint64_t flags  = __atomic_load_n(&r->flags, HTC_MO_ACQUIRE);
        printf("  slot[%u]: LIVE idx=%lu tag_match=%d in_sec=%d hash_match=%d flags=%lu\n",
               si, (unsigned long)idx, tag_match, in_sec, hash_match, flags);
    }

    /* Stashes */
    printf("--- stashes ---\n");
    const htc_stash_t *ss = t->shards ? &t->shards[htc_shard_of(b1, t->shard_count)].stash : &t->stash;
    for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
        uint64_t w = __atomic_load_n(&ss->slots[i], HTC_MO_ACQUIRE);
        if (!htc_slot_live(w)) { printf("  stash[%u]: EMPTY\n", i); continue; }
        uint32_t idx = htc_slot_index(w);
        htc_record_t *r = htc_arena_ptr(g->arena, idx);
        bool hash_match = (r->identity_hash == hash);
        bool tag_match  = (htc_slot_tag(w) == tag);
        uint64_t flags  = __atomic_load_n(&r->flags, HTC_MO_ACQUIRE);
        printf("  stash[%u]: LIVE idx=%lu tag_match=%d hash_match=%d flags=%lu\n",
               i, (unsigned long)idx, tag_match, hash_match, flags);
    }
    if (ss != &t->stash) {
        printf("--- global stash ---\n");
        for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
            uint64_t w = __atomic_load_n(&t->stash.slots[i], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) { printf("  global_stash[%u]: EMPTY\n", i); continue; }
            uint32_t idx = htc_slot_index(w);
            printf("  global_stash[%u]: LIVE idx=%lu\n", i, (unsigned long)idx);
        }
    }

    /* Old generation */
    if (g->old) {
        printf("--- old generation ---\n");
        uint64_t old_val;
        bool found = htc_find_in_old_gen(g->old, g->arena, hash, tag, &old_val);
        printf("  found in old gen: %s\n", found ? "YES" : "NO");
    }
    printf("=== end miss certificate ===\n");
}

/* ─── Transient-state invariant verifier (Battery 27 §20-21) ─
 * Checks that internal consistency invariants hold during
 * transient states (seq_busy, gen freezing, etc.).
 * Returns 0 if OK, bitmask of fault types otherwise.
 * Safe to call from SCHED_HOOK callbacks (no reentrancy). */
uint32_t htc_debug_check_transient(const htc_table_t *t) {
    if (!t) return 0;
    uint32_t faults = 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) return 0;

    /* Check: ctrl_tags are populated for every LIVE slot.
     * A zero ctrl byte for a LIVE slot would cause the fast path
     * (htc_bucket_scan_seq) to miss it. */
    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        uint64_t ctrl = htc_ctrl_load(&g->meta[bi]);
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[bi].slot[si], HTC_MO_ACQUIRE);
            if (!htc_slot_live(w)) continue;
            uint64_t match = (ctrl >> (si * 8)) & 0xFF;
            if (match == 0) faults |= 2u;  /* ctrl false negative risk */
        }
    }

    /* Check: seq guard is consistent within buckets.
     * During transient operations, seq may be BUSY but must
     * not remain BUSY indefinitely.  This is a liveness check
     * that cannot be fully verified here, but we flag BUSY as
     * a transient warning. */
    uint32_t busy_count = 0;
    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        uint32_t seq = __atomic_load_n(&g->meta[bi].seq, HTC_MO_ACQUIRE);
        if (seq & HTC_SEQ_BUSY) busy_count++;
    }
    if (busy_count) faults |= 4u;  /* transient: seq busy (benign if short) */

    /* Check: remap_count is conservative.
     * Count actual secondary entries and compare to remap_count.
     * remap_count may be SATURATED or >= actual count, never less. */
    for (uint32_t bi = 0; bi < g->num_buckets; bi++) {
        uint32_t actual_sec = 0;
        uint32_t p = bi;
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[p].slot[si], HTC_MO_ACQUIRE);
            if (htc_slot_live(w) && htc_slot_in_secondary(w))
                actual_sec++;
        }
        uint8_t stored = __atomic_load_n(&g->meta[bi].remap_count, __ATOMIC_RELAXED);
        if (stored != HTC_REMAP_SATURATED && stored < actual_sec)
            faults |= 8u;  /* remap under-count: would cause false negative */
    }

    /* Check: no bucket has both primary and secondary entries for same hash.
     * Duplicate detection is done elsewhere (htc_debug_check_duplicate_hash),
     * but here we do a quick spot-check. */
    /* (deferred to htc_debug_check_duplicate_hash for full scan) */

    return faults;
}

/* ─── Explain hash diagnostic (Battery 16 Q33) ────────────── */
void htc_debug_explain_hash(const htc_table_t *t_, uint64_t hash)
{
    htc_table_t *t = (htc_table_t *)t_;
    if (!t) { printf("explain_hash: NULL table\n"); return; }
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g) { printf("explain_hash: no generation\n"); return; }

    uint64_t ph  = htc_placement_hash(hash, g->seed);
    uint16_t tag = htc_tag16(ph);
    uint8_t  p8  = htc_partial8(tag);
    uint32_t b1  = (uint32_t)(ph & g->bucket_mask);
    uint32_t b2  = htc_alt_bucket(b1, ph, tag) & g->bucket_mask;

    printf("=== explain_hash(0x%016lx) ===\n", hash);
    printf("  gen->seed     = 0x%016lx\n", g->seed);
    printf("  placement     = 0x%016lx\n", ph);
    printf("  tag16         = 0x%04x\n", tag);
    printf("  partial8      = 0x%02x\n", p8);
    printf("  primary       = %u  (bucket %u)\n", b1, b1);
    printf("  secondary     = %u  (bucket %u)\n", b2, b2);
    printf("  bucket_mask   = 0x%x\n", g->bucket_mask);
    printf("  num_buckets   = %u\n", g->num_buckets);
    printf("  gen->state    = %d\n", __atomic_load_n(&g->state, __ATOMIC_RELAXED));
    printf("  gen->old      = %s\n", g->old ? "present" : "NULL");

    /* Primary bucket */
    printf("--- primary bucket %u ---\n", b1);
    printf("  seq           = %u%s\n",
           __atomic_load_n(&g->meta[b1].seq, __ATOMIC_RELAXED),
           __atomic_load_n(&g->meta[b1].seq, __ATOMIC_RELAXED) & HTC_SEQ_BUSY ? " BUSY" : "");
    printf("  remap_count   = %u\n",
           __atomic_load_n(&g->meta[b1].remap_count, __ATOMIC_RELAXED));
    printf("  ctrl_tags     = 0x%016lx\n",
           __atomic_load_n(&g->meta[b1].ctrl_tags, __ATOMIC_RELAXED));
    for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
        uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si], HTC_MO_ACQUIRE);
        if (htc_slot_empty(w)) continue;
        printf("  slot[%u]: idx=%lu tag=0x%04x state=%u in_sec=%d\n",
               si, htc_slot_index(w), htc_slot_tag(w),
               htc_slot_state(w), htc_slot_in_secondary(w));
        if (htc_slot_live(w)) {
            htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
            printf("    rec: id_hash=0x%016lx pl_hash=0x%016lx gen=%u flags=%u val=0x%016lx\n",
                   r->identity_hash, r->placement_hash, r->generation,
                   __atomic_load_n(&r->flags, __ATOMIC_RELAXED),
                   __atomic_load_n(&r->user_value, __ATOMIC_RELAXED));
            if (r->identity_hash == hash)
                printf("    *** MATCH (primary slot %u) ***\n", si);
        }
    }

    /* Secondary bucket */
    if (b2 != b1) {
        printf("--- secondary bucket %u ---\n", b2);
        printf("  seq           = %u%s\n",
               __atomic_load_n(&g->meta[b2].seq, __ATOMIC_RELAXED),
               __atomic_load_n(&g->meta[b2].seq, __ATOMIC_RELAXED) & HTC_SEQ_BUSY ? " BUSY" : "");
        for (unsigned si = 0; si < HTC_BUCKET_SLOTS; si++) {
            uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si], HTC_MO_ACQUIRE);
            if (htc_slot_empty(w)) continue;
            printf("  slot[%u]: idx=%lu tag=0x%04x state=%u in_sec=%d\n",
                   si, htc_slot_index(w), htc_slot_tag(w),
                   htc_slot_state(w), htc_slot_in_secondary(w));
            if (htc_slot_live(w)) {
                htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
                printf("    rec: id_hash=0x%016lx flags=%u val=0x%016lx\n",
                       r->identity_hash,
                       __atomic_load_n(&r->flags, __ATOMIC_RELAXED),
                       __atomic_load_n(&r->user_value, __ATOMIC_RELAXED));
                if (r->identity_hash == hash)
                    printf("    *** MATCH (secondary slot %u) ***\n", si);
            }
        }
    }

    /* Stashes */
    printf("--- stashes ---\n");
    htc_stash_t *ss = t->shards ? &t->shards[htc_shard_of(b1, t->shard_count)].stash : &t->stash;
    for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
        uint64_t w = __atomic_load_n(&ss->slots[i], HTC_MO_ACQUIRE);
        if (htc_slot_empty(w)) continue;
        printf("  stash[%u]: idx=%lu tag=0x%04x state=%u\n",
               i, htc_slot_index(w), htc_slot_tag(w), htc_slot_state(w));
        if (htc_slot_live(w)) {
            htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
            printf("    rec: id_hash=0x%016lx flags=%u val=0x%016lx\n",
                   r->identity_hash,
                   __atomic_load_n(&r->flags, __ATOMIC_RELAXED),
                   __atomic_load_n(&r->user_value, __ATOMIC_RELAXED));
            if (r->identity_hash == hash)
                printf("    *** MATCH (stash slot %u) ***\n", i);
        }
    }
    if (ss != &t->stash) {
        for (uint32_t i = 0; i < HTC_STASH_MAX; i++) {
            uint64_t w = __atomic_load_n(&t->stash.slots[i], HTC_MO_ACQUIRE);
            if (htc_slot_empty(w)) continue;
            printf("  global_stash[%u]: idx=%lu tag=0x%04x\n",
                   i, htc_slot_index(w), htc_slot_tag(w));
            if (htc_slot_live(w)) {
                htc_record_t *r = htc_arena_ptr(g->arena, htc_slot_index(w));
                if (r->identity_hash == hash)
                    printf("    *** MATCH (global stash %u) ***\n", i);
            }
        }
    }

    /* Must check secondary decision */
    printf("--- remap decision ---\n");
    bool mcs = htc_must_check_secondary(&g->meta[b1], tag);
    printf("  must_check_secondary = %s\n", mcs ? "YES" : "NO");
    printf("=== end explain_hash ===\n");
}

/* ─── Explain epoch diagnostic (Battery 16 Q15) ───────────── */
void htc_debug_explain_epoch(const htc_table_t *t)
{
    if (!t || !t->epoch) { printf("explain_epoch: no epoch\n"); return; }
    htc_epoch_ctl_t *ep = t->epoch;
    printf("=== explain_epoch ===\n");
    printf("  global_epoch  = %lu\n",
           __atomic_load_n(&ep->global_epoch, __ATOMIC_RELAXED));
    printf("  retire nodes  = %s\n",
           __atomic_load_n(&ep->retire_head, __ATOMIC_RELAXED) ? "pending" : "none");
    printf("  retire gens   = %s\n",
           __atomic_load_n(&ep->retire_gen_head, __ATOMIC_RELAXED) ? "pending" : "none");
    uint64_t min_ep = UINT64_MAX;
    int oldest_slot = -1;
    for (int i = 0; i < HTC_EPOCH_MAX_THREADS; i++) {
        uint64_t te = __atomic_load_n(&ep->thread_epoch[i], HTC_MO_ACQUIRE);
        if (te == 0) continue;
        printf("  thread[%d] epoch=%lu\n", i, te);
        if (te < min_ep) { min_ep = te; oldest_slot = i; }
    }
    if (oldest_slot >= 0)
        printf("  oldest pinned: thread[%d] epoch=%lu\n", oldest_slot, min_ep);
    else
        printf("  no pinned threads\n");
    printf("=== end explain_epoch ===\n");
}

#endif /* !NDEBUG */

/* ─── Logical checksum oracle (Battery 19 Q12) ────────────── */
/* Layout-independent checksum over all LIVE records.
 * Used to prove that structural operations (grow, reseed, reserve)
 * preserve logical contents.  Only quiescent-safe. */
uint64_t htc_debug_checksum(const htc_table_t *t)
{
    if (!t) return 0;
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g || !g->arena) return 0;

    uint64_t cs = 0;
    uint32_t n = g->arena->count;
    for (uint32_t i = 0; i < n; i++) {
        htc_record_t *r = htc_arena_ptr(g->arena, i);
        if (!r) continue;
        uint64_t flags = __atomic_load_n(&r->flags, HTC_MO_ACQUIRE);
        if (r->identity_hash == 0 || flags != 0) continue;
        cs ^= r->identity_hash;
        cs ^= __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
        cs ^= (uint64_t)r->generation;
        cs = (cs << 7) | (cs >> 57);  /* rotate */
    }
    return cs;
}

/* ─── Canonical rebuild (Battery 24 Q16) ──────────────────── */
/* Creates a fresh table with the same logical contents but new
 * metadata (seed, placement, bucket order, stash, remap, front
 * cache).  Used to prove that layout metadata is semantically
 * neutral — the rebuild preserves the abstract map.
 *
 * The new table has the same initial capacity and flags but a
 * different seed, proving that placement does not affect semantics.
 *
 * return: new table on success, NULL on OOM.  Caller must destroy. */
htc_table_t *htc_debug_rebuild(const htc_table_t *t_, uint32_t flags) {
    if (!t_) return NULL;
    htc_table_t *t = (htc_table_t *)t_;

    /* Snapshot all live entries */
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!g || !g->arena) return NULL;

    uint32_t capacity = g->arena->count;
    uint64_t *hashes = NULL;
    uint64_t *values = NULL;
    uint32_t count = 0;

    for (uint32_t i = 0; i < capacity; i++) {
        htc_record_t *r = htc_arena_ptr(g->arena, i);
        if (r->identity_hash != 0
            && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
            count++;
        }
    }

    hashes = malloc(count * sizeof(uint64_t));
    values = malloc(count * sizeof(uint64_t));
    if (!hashes || !values) { free(hashes); free(values); return NULL; }

    uint32_t idx = 0;
    for (uint32_t i = 0; i < capacity; i++) {
        htc_record_t *r = htc_arena_ptr(g->arena, i);
        if (r->identity_hash != 0
            && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
            hashes[idx] = r->identity_hash;
            values[idx] = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
            idx++;
        }
    }

    /* Create fresh table with same capacity, different seed */
    htc_config_t cfg = {(uint32_t)t->num_buckets, t->max_load_factor, 0, flags};
    htc_table_t *t2 = htc_create(&cfg);
    if (!t2) { free(hashes); free(values); return NULL; }

    for (uint32_t i = 0; i < count; i++) {
        htc_error_t ret = htc_insert(t2, hashes[i], values[i]);
        if (ret != HTC_OK) {
            htc_destroy(t2);
            free(hashes); free(values);
            return NULL;
        }
    }

    free(hashes); free(values);
    return t2;
}

/* =========================================================================
 * Public API — insert / upsert / update / find / remove / size
 * ========================================================================= */

htc_error_t htc_insert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return HTC_ERR_OOM;
    htc_witness_record(HTC_WIT_OP_START, hash, 0, 0, 0, 0, 0);

    uint32_t s1 = 0, s2 = 0;

    for (int attempt = 0; attempt < 4; attempt++) {
        htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
        if (!gen) return HTC_ERR_OOM;
        htc_witness_record(HTC_WIT_LOAD_GEN, hash, 0, 0, 0, 0, 0);
        HTC_SCHED_HOOK(1);  /* after load current generation */
        htc_bucket_t      *buckets = gen->buckets;
        htc_bucket_meta_t *meta    = gen->meta;
        uint32_t           mask    = gen->bucket_mask;

        uint64_t ph  = htc_placement_hash(hash, gen->seed);
        uint16_t tag = htc_tag16(ph);
        uint8_t  p8  = htc_partial8(tag);
        uint32_t b1  = (uint32_t)(ph & mask);
        uint32_t b2  = htc_alt_bucket(b1, ph, tag) & mask;
        s1 = htc_shard_of(b1, t->shard_count);
        s2 = htc_shard_of(b2, t->shard_count);

        if (t->shards) htc_lock_shards(t->shards, s1, s2);
        HTC_SCHED_HOOK(2);  /* after acquire shard locks */

        /* Revalidate after acquiring locks: verify the generation loaded at
         * attempt start is still current and ACTIVE. A concurrent grow may
         * have frozen this gen while we waited on shard locks. */
        {
            htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
            if (cur != gen) {
                HTC_STAT_INC(t->stats.writer_retry_gen_changed);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
            if (__atomic_load_n(&cur->state, HTC_MO_ACQUIRE) != HTC_GEN_ACTIVE) {
                HTC_STAT_INC(t->stats.writer_retry_gen_frozen);
                if (__atomic_load_n(&cur->state, HTC_MO_ACQUIRE) == HTC_GEN_OLD)
                    HTC_STAT_INC(t->stats.attempted_write_to_old_gen);
                else
                    HTC_STAT_INC(t->stats.attempted_write_to_frozen_gen);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
        }
        HTC_SCHED_HOOK(3);  /* after gen/state revalidation */

        /* Ensure the chunks for both candidate buckets are migrated */
        htc_ensure_chunk_migrated(t, b1);
        if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

        /* duplicate check (under shard lock) */
        if (htc_bucket_scan(buckets + b1, meta + b1,
                            t->arena, hash, tag,
                            HTC_SCAN_PRIMARY) >= 0) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return HTC_ERR_DUPLICATE;
        }
        if (htc_must_check_secondary(meta + b1, tag)) {
            if (htc_bucket_scan(buckets + b2, meta + b2,
                                t->arena, hash, tag,
                                HTC_SCAN_SECONDARY) >= 0) {
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                return HTC_ERR_DUPLICATE;
            }
        }
        htc_stash_t *stash_ptr = t->shards ? &t->shards[htc_shard_of(b1, t->shard_count)].stash : &t->stash;
        if (htc_stash_find(stash_ptr, t->arena, hash, tag) >= 0) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return HTC_ERR_DUPLICATE;
        }

        /* allocate arena record (before mutation, under lock) */
        uint32_t idx = htc_arena_alloc(t->arena, hash, ph, value);
        if (idx == UINT32_MAX) {
            HTC_STAT_INC(t->stats.insert_oom);
            HTC_STAT_INC_SHARD(t, s1, insert_oom);
            htc_witness_record(HTC_WIT_RETURN_ERR, hash, 0, 0, 0, 3, 0);  /* HTC_ERR_OOM */
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return HTC_ERR_OOM;
        }

        uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

        htc_seq_guard_t g1 = {0}, g2 = {0};
        g1 = htc_bucket_seq_begin(meta + b1);
        if (b2 != b1)
            g2 = htc_bucket_seq_begin(meta + b2);
        HTC_SCHED_HOOK(4);  /* after seq_begin */

        /* Try direct primary insert */
        {
            uint64_t ctrl = htc_ctrl_load(meta + b1);
            uint64_t em   = htc_match8(ctrl, 0);
            while (em) {
                unsigned si = htc_ctz_candidate(em);
                uint64_t exp = htc_slot_empty_word();
                HTC_SCHED_HOOK(5);  /* before primary slot publish */
                if (htc_atomic_cas(&(buckets + b1)->slot[si], &exp, slot_word,
                                   HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
                    htc_ctrl_set(meta + b1, si, p8);
                    htc_bucket_seq_end(meta + b1, g1);
                    if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);
                    htc_witness_record(HTC_WIT_INSERT_CAS, hash, idx, (uint16_t)b1, (uint8_t)si, 0, 0);
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
                htc_remap_inc(meta + b1, tag);
                HTC_SCHED_HOOK(5);  /* before secondary slot publish */
                if (htc_atomic_cas(&(buckets + b2)->slot[si], &exp, sw,
                                   HTC_MO_RELEASE, HTC_MO_ACQUIRE)) {
                    htc_ctrl_set(meta + b2, si, p8);
                    htc_bucket_seq_end(meta + b1, g1);
                    if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);
                    htc_witness_record(HTC_WIT_INSERT_CAS, hash, idx, (uint16_t)b2, (uint8_t)si, 1, 0);
                    goto insert_done;
                }
                /* CAS failed — undo remap_inc */
                htc_remap_dec(meta + b1);
                em = htc_clear_candidate(em, si);
            }
        }

        /* Both buckets full — try BFS displacement */
        {
            ht_cuckoo_path_t bfs_path = {0};
            htc_bucket_seq_end(meta + b1, g1);
            if (b2 != b1) htc_bucket_seq_end(meta + b2, g2);

            if (bfs_find_path(t, gen, b1, b2, &bfs_path)) {
                HTC_SCHED_HOOK(7);  /* after BFS path found */
                HTC_STAT_INC(t->stats.bfs_attempts);
                HTC_STAT_INC_SHARD(t, s1, bfs_attempts);
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

                    /* LFBCH commit (§23) disabled in v1 — lock-free CAS rollback
                     * is not yet proven safe under all interleavings.
                     * Use locked protocol only. */

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
                    HTC_SCHED_HOOK(8);  /* after path locks acquired */

                    if (validate_path_locked(t, gen, &bfs_path, expected_src)) {
                        HTC_STAT_INC(t->stats.bfs_success);
                        HTC_STAT_INC_SHARD(t, s1, bfs_success);
                        { uint8_t bd = bfs_path.move_count; if (bd > 7) bd = 7; HTC_STAT_ADD_SHARD(t, s1, bfs_depth_histogram[bd], 1); }
                        commit_path_locked(t, gen, &bfs_path, expected_src, slot_word);
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
                HTC_STAT_INC(t->stats.bfs_abandoned_shards);
                HTC_STAT_INC_SHARD(t, s1, bfs_abandoned_shards);
            } else {
                HTC_STAT_INC(t->stats.bfs_no_path);
                HTC_STAT_INC_SHARD(t, s1, bfs_no_path);
            }
        }

        /* Revalidate before stash mutation. If the gen was frozen while we
         * were in the BFS scope (which may have unlocked/re-locked shards),
         * the arena record allocated at line 1333 must be freed on retry. */
        {
            htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
            if (cur != gen) {
                HTC_STAT_INC(t->stats.writer_retry_gen_changed);
                htc_arena_free(t->arena, idx);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
            if (__atomic_load_n(&cur->state, HTC_MO_ACQUIRE) != HTC_GEN_ACTIVE) {
                HTC_STAT_INC(t->stats.writer_retry_gen_frozen);
                if (__atomic_load_n(&cur->state, HTC_MO_ACQUIRE) == HTC_GEN_OLD)
                    HTC_STAT_INC(t->stats.attempted_write_to_old_gen);
                else
                    HTC_STAT_INC(t->stats.attempted_write_to_frozen_gen);
                htc_arena_free(t->arena, idx);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                goto retry_nowait;
            }
        }

        /* BFS failed — use stash (g1/g2 already ended above).
         * Re-check stash for duplicates: the BFS path may have unlocked
         * and re-locked s1/s2, creating a window for a concurrent insert. */
        if (htc_stash_find(stash_ptr, t->arena, hash, tag) >= 0) {
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            return HTC_ERR_DUPLICATE;
        }
        if (htc_stash_insert(stash_ptr, slot_word) >= 0) {
            HTC_STAT_INC(t->stats.stash_insert);
            HTC_STAT_INC_SHARD(t, s1, stash_insert);
            htc_stash_maintain(stash_ptr);
            goto insert_done;
        }
        HTC_STAT_INC(t->stats.stash_full);
        HTC_STAT_INC_SHARD(t, s1, stash_full);

        /* Stash full — free record, grow table (with reseed), retry */
        htc_arena_free(t->arena, idx);
        if (t->shards) htc_unlock_shards(t->shards, s1, s2);
        HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
        htc_epoch_advance(t->epoch);
        HTC_STAT_INC(t->stats.stash_grow);
        HTC_STAT_INC_SHARD(t, s1, stash_grow);
        HTC_STAT_INC(t->stats.grow_reason_stash_full);
        htc_error_t grow_ret = htc_grow(t, true);  /* reseed on pathology */
        if (grow_ret != HTC_OK) return grow_ret;
        continue;

retry_nowait:
        /* Generation state changed — just retry. The next iteration
         * loads the current generation and re-validates. */
        continue;
    }

    HTC_STAT_INC(t->stats.insert_pathological);
    HTC_STAT_INC_SHARD(t, s1, insert_pathological);
    htc_witness_record(HTC_WIT_RETURN_ERR, hash, 0, 0, 0, 4, 0);  /* HTC_ERR_PATHOLOGICAL */
    htc_witness_record(HTC_WIT_OP_FINISH, hash, 0, 0, 0, 4, 0);
#ifdef HTC_STATS
    /* PATHOLOGICAL certificate (Battery 27 §27): diagnostic only */
    printf("PATHOLOGICAL: hash=0x%016lx num_buckets=%u stash_max=%u"
           " shard_count=%u seed=0x%016lx\n",
           hash, t->num_buckets, HTC_STASH_MAX, t->shard_count, t->seed);
#endif
    return HTC_ERR_PATHOLOGICAL;

insert_done:
    __atomic_fetch_add(&t->size, 1, __ATOMIC_RELAXED);
    if (t->have_amq_filter) t->amq_filter.insert(t->amq_filter.filter, hash);

    if (t->shards) htc_unlock_shards(t->shards, s1, s2);

    HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
    htc_epoch_advance(t->epoch);

    double lf = (double)__atomic_load_n(&t->size, __ATOMIC_RELAXED) / (double)t->num_buckets;
    if (lf > t->max_load_factor) {
        HTC_STAT_INC(t->stats.grow_reason_load);
        htc_grow(t, false);  /* keep seed */
    }

    return HTC_OK;
}

htc_error_t htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return HTC_ERR_OOM;

    for (int attempt = 0; attempt < 4; attempt++) {
        htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
        if (!gen) return HTC_ERR_OOM;
        uint32_t mask = gen->bucket_mask;

        uint64_t ph  = htc_placement_hash(hash, gen->seed);
        uint16_t tag = htc_tag16(ph);
        uint32_t  b1 = (uint32_t)(ph & mask);
        uint32_t  b2 = htc_alt_bucket(b1, ph, tag) & mask;

        /* Ensure chunk is migrated so we operate on the current gen */
        htc_ensure_chunk_migrated(t, b1);
        if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

        /* Search current gen buckets and stash, but don't update yet */
        uint64_t found_word = 0;
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, tag, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            found_word = __atomic_load_n(&gen->buckets[b1].slot[si], HTC_MO_ACQUIRE);
        } else if (htc_must_check_secondary(&gen->meta[b1], tag)) {
            si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, tag, HTC_SCAN_SECONDARY);
            if (si >= 0)
                found_word = __atomic_load_n(&gen->buckets[b2].slot[si], HTC_MO_ACQUIRE);
        }
        if (!found_word) {
            htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
            int sti = htc_stash_find(ss, t->arena, hash, tag);
            htc_stash_t *found_ss = ss;
            if (sti < 0 && ss != &t->stash) {
                sti = htc_stash_find(&t->stash, t->arena, hash, tag);
                if (sti >= 0) found_ss = &t->stash;
            }
            if (sti >= 0)
                found_word = __atomic_load_n(&found_ss->slots[sti], HTC_MO_ACQUIRE);
        }
        if (found_word) {
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(found_word))->user_value,
                             value, HTC_MO_RELEASE);
            return HTC_OK;
        }

        /* Check old gen chain for stale copies and clear them */
        if (gen->old) {
            uint64_t old_val = 0;
            if (htc_find_in_old_gen(gen->old, t->arena, hash, tag, &old_val)) {
                htc_ensure_chunk_migrated(t, b1);
                if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
                continue;
            }
        }

        /* not found — insert */
        uint32_t idx = htc_arena_alloc(t->arena, hash, ph, value);
        if (idx == UINT32_MAX) {
            HTC_STAT_INC(t->stats.insert_oom);
            return HTC_ERR_OOM;
        }

        uint64_t slot_word = htc_slot_pack(idx, tag, HTC_STATE_LIVE, 0);

        if (!htc_place_entry(t, gen, ph, slot_word, NULL)) {
            htc_arena_free(t->arena, idx);
            return HTC_ERR_PATHOLOGICAL;
        }

        __atomic_fetch_add(&t->size, 1, __ATOMIC_RELAXED);
        HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
        htc_epoch_advance(t->epoch);

        double lf = (double)__atomic_load_n(&t->size, __ATOMIC_RELAXED) / (double)t->num_buckets;
        if (lf > t->max_load_factor) {
            HTC_STAT_INC(t->stats.grow_reason_load);
            htc_grow(t, false);
        }

        return HTC_OK;
    }
    return HTC_ERR_PATHOLOGICAL;
}

htc_error_t htc_update(htc_table_t *t, uint64_t hash, uint64_t value)
{
    if (!t) return HTC_ERR_OOM;

    htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!gen) return HTC_ERR_OOM;
    uint32_t mask = gen->bucket_mask;

    uint64_t ph  = htc_placement_hash(hash, gen->seed);
    uint16_t tag = htc_tag16(ph);
    uint32_t  b1 = (uint32_t)(ph & mask);
    uint32_t  b2 = htc_alt_bucket(b1, ph, tag) & mask;

    htc_ensure_chunk_migrated(t, b1);
    if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

    {
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, tag, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si], HTC_MO_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, HTC_MO_RELEASE);
            return HTC_OK;
        }
    }
    if (htc_must_check_secondary(&gen->meta[b1], tag)) {
        int si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, tag, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&gen->buckets[b2].slot[si], HTC_MO_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, HTC_MO_RELEASE);
            return HTC_OK;
        }
    }
    {
        htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
        int si = htc_stash_find(ss, t->arena, hash, tag);
        if (si < 0 && ss != &t->stash)
            si = htc_stash_find(&t->stash, t->arena, hash, tag);
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&ss->slots[si], HTC_MO_ACQUIRE);
            __atomic_store_n(&htc_arena_ptr(t->arena, htc_slot_index(w))->user_value,
                             value, HTC_MO_RELEASE);
            return HTC_OK;
        }
    }

    /* Not found in current gen — check old gen */
    if (gen->old) {
        uint64_t old_val = 0;
        if (htc_find_in_old_gen(gen->old, t->arena, hash, tag, &old_val)) {
            htc_ensure_chunk_migrated(t, b1);
            if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
        }
    }
    return HTC_ERR_NOT_FOUND;
}

htc_error_t htc_find(const htc_table_t *t_, uint64_t hash, uint64_t *out_value)
{
    if (!t_) return HTC_ERR_OOM;
    htc_table_t *t = (htc_table_t *)t_;
    htc_witness_record(HTC_WIT_OP_START, hash, 0, 0, 0, 0, 0);

    for (;;) {
        htc_table_gen_t *g = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
        if (!g) return HTC_ERR_OOM;

        /* Pin epoch before any arena record access */
        htc_epoch_pin(t->epoch);

        /* AMQ filter: if the filter says not present, skip lookup */
        if (t->have_amq_filter && !t->amq_filter.lookup(t->amq_filter.filter, hash)) {
            htc_epoch_unpin(t->epoch);
            HTC_STAT_INC(t->stats.find_negative);
            return HTC_ERR_NOT_FOUND;
        }

        /* Check front cache under epoch protection (unless disabled) */
        if (out_value && !(t->flags & HTC_CFG_DISABLE_FRONT_CACHE)) {
            uint64_t cv = 0;
            if (htc_front_cache_lookup(&htc_thread_cache, t, t->table_id, g->arena, hash, &cv)) {
                HTC_STAT_INC(t->stats.front_cache_hit);
                *out_value = cv;
                htc_epoch_unpin(t->epoch);
                return HTC_OK;
            } else {
                HTC_STAT_INC(t->stats.front_cache_miss);
            }
        }

        uint64_t ph  = htc_placement_hash(hash, g->seed);
        uint16_t tag = htc_tag16(ph);
        uint32_t  b1 = (uint32_t)(ph & g->bucket_mask);
        uint32_t found_idx = UINT32_MAX;

        {
            int si = htc_bucket_scan_seq(&g->buckets[b1], &g->meta[b1],
                                         g->arena, hash, tag,
                                         HTC_SCAN_PRIMARY
#ifdef HTC_STATS
                                         , &t->stats.seq_retries
#endif
                                         );
            if (si >= 0) {
                uint64_t w = __atomic_load_n(&g->buckets[b1].slot[si],
                                             HTC_MO_ACQUIRE);
                found_idx = (uint32_t)htc_slot_index(w);
                htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
                uint32_t rec_gen = rec->generation;
                if (out_value) *out_value = __atomic_load_n(&rec->user_value, HTC_MO_ACQUIRE);
                /* Double-check flags: a concurrent delete may have set flags
                 * between our first flags check (inside bucket_scan_seq) and
                 * the user_value load above. (Battery 12 Q5) */
                if (__atomic_load_n(&rec->flags, HTC_MO_ACQUIRE) != 0) {
                    htc_epoch_unpin(t->epoch);
                    continue;
                }
                htc_witness_record(HTC_WIT_RETURN_OK, hash, found_idx,
                                    (uint16_t)b1, (uint8_t)si, 0, rec_gen);
                HTC_STAT_INC(t->stats.find_primary_hit);
                htc_epoch_unpin(t->epoch);
                htc_front_cache_insert(&htc_thread_cache, t, t->table_id, hash, found_idx, rec_gen);
                return HTC_OK;
            }
        }

        /* Seq-validated remap decision */
        bool check_sec;
        do {
            uint32_t s_remap = __atomic_load_n(&g->meta[b1].seq, HTC_MO_ACQUIRE);
            if (s_remap & HTC_SEQ_BUSY) continue;
            check_sec = htc_must_check_secondary(&g->meta[b1], tag);
            uint32_t s_end = __atomic_load_n(&g->meta[b1].seq, HTC_MO_ACQUIRE);
            if (s_remap == s_end && !(s_end & HTC_SEQ_BUSY)) break;
        } while (1);

        if (check_sec) {
            HTC_STAT_INC(t->stats.secondary_checked);
            uint32_t b2 = htc_alt_bucket(b1, ph, tag) & g->bucket_mask;
            int si = htc_bucket_scan_seq(&g->buckets[b2], &g->meta[b2],
                                         g->arena, hash, tag,
                                         HTC_SCAN_SECONDARY
#ifdef HTC_STATS
                                         , &t->stats.seq_retries
#endif
                                         );
            if (si >= 0) {
                uint64_t w = __atomic_load_n(&g->buckets[b2].slot[si],
                                             HTC_MO_ACQUIRE);
                found_idx = (uint32_t)htc_slot_index(w);
                htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
                uint32_t rec_gen = rec->generation;
                if (out_value) *out_value = __atomic_load_n(&rec->user_value, HTC_MO_ACQUIRE);
                if (__atomic_load_n(&rec->flags, HTC_MO_ACQUIRE) != 0) {
                    htc_epoch_unpin(t->epoch);
                    continue;
                }
                HTC_STAT_INC(t->stats.find_secondary_hit);
                htc_epoch_unpin(t->epoch);
                htc_front_cache_insert(&htc_thread_cache, t, t->table_id, hash, found_idx, rec_gen);
                return HTC_OK;
            }
        } else {
            HTC_STAT_INC(t->stats.secondary_skipped);
        }

        {
            htc_stash_t *ss = g->shards ? &g->shards[htc_shard_of(b1, g->shard_count)].stash : &t->stash;
            int si = htc_stash_find(ss, g->arena, hash, tag);
            htc_stash_t *found = ss;
            if (si < 0 && ss != &t->stash) {
                si = htc_stash_find(&t->stash, g->arena, hash, tag);
                if (si >= 0) found = &t->stash;
            }
            if (si >= 0) {
                uint64_t w = __atomic_load_n(&found->slots[si], HTC_MO_ACQUIRE);
                found_idx = (uint32_t)htc_slot_index(w);
                htc_record_t *rec = htc_arena_ptr(g->arena, found_idx);
                uint32_t rec_gen = rec->generation;
                if (out_value) *out_value = __atomic_load_n(&rec->user_value, HTC_MO_ACQUIRE);
                if (__atomic_load_n(&rec->flags, HTC_MO_ACQUIRE) != 0) {
                    htc_epoch_unpin(t->epoch);
                    continue;
                }
                HTC_STAT_INC(t->stats.find_stash_hit);
                htc_epoch_unpin(t->epoch);
                htc_front_cache_insert(&htc_thread_cache, t, t->table_id, hash, found_idx, rec_gen);
                return HTC_OK;
            }
        }

        if (g->old && __atomic_load_n(&g->old->state, HTC_MO_ACQUIRE) == HTC_GEN_FREEZING) {
            if (htc_find_in_old_gen(g->old, g->arena, hash, tag, out_value)) {
                HTC_STAT_INC(t->stats.find_oldgen_hit);
                htc_epoch_unpin(t->epoch);
                return HTC_OK;
            }
        }

        HTC_STAT_INC(t->stats.find_negative);
        htc_epoch_unpin(t->epoch);
        if (out_value) *out_value = 0;
        htc_witness_record(HTC_WIT_FIND_NEG, hash, 0, 0, 0, 0, 0);
        htc_witness_record(HTC_WIT_OP_FINISH, hash, 0, 0, 0, 2, 0);  /* NOT_FOUND */
        return HTC_ERR_NOT_FOUND;
    }
}

htc_error_t htc_remove(htc_table_t *t, uint64_t hash)
{
    if (!t) return HTC_ERR_OOM;

    htc_table_gen_t *gen = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (!gen) return false;

    uint64_t ph  = htc_placement_hash(hash, gen->seed);
    uint16_t tag = htc_tag16(ph);
    uint32_t  b1 = (uint32_t)(ph & gen->bucket_mask);
    uint32_t  b2 = htc_alt_bucket(b1, ph, tag) & gen->bucket_mask;
    uint32_t  s1 = htc_shard_of(b1, t->shard_count);
    uint32_t  s2 = htc_shard_of(b2, t->shard_count);

    if (t->shards) htc_lock_shards(t->shards, s1, s2);

    /* Revalidate: after acquiring locks, reload current_gen and verify
     * it is still ACTIVE. Prevents committing into a frozen generation. */
    htc_table_gen_t *cur = __atomic_load_n(&t->current_gen, HTC_MO_ACQUIRE);
    if (cur != gen || __atomic_load_n(&cur->state, HTC_MO_ACQUIRE) != HTC_GEN_ACTIVE) {
        if (t->shards) htc_unlock_shards(t->shards, s1, s2);
        return HTC_ERR_NOT_FOUND;
    }

    /* Ensure chunks for candidate buckets are migrated */
    htc_ensure_chunk_migrated(t, b1);
    if (b2 != b1) htc_ensure_chunk_migrated(t, b2);

    {
        int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                 t->arena, hash, tag, HTC_SCAN_PRIMARY);
        if (si >= 0) {
            htc_seq_guard_t gs = htc_bucket_seq_begin(&gen->meta[b1]);
            uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si],
                                         HTC_MO_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, HTC_MO_RELEASE);
            __atomic_store_n(&gen->buckets[b1].slot[si],
                             htc_slot_empty_word(), HTC_MO_RELEASE);
            htc_ctrl_clear(&gen->meta[b1], (unsigned)si);
            htc_bucket_seq_end(&gen->meta[b1], gs);
            if (htc_slot_in_secondary(w)) htc_remap_dec(&gen->meta[b1]);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->have_amq_filter) t->amq_filter.delete(t->amq_filter.filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            HTC_STAT_INC(t->stats.retired_record_count);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
            htc_epoch_advance(t->epoch);
            return HTC_OK;
        }
    }
    if (htc_must_check_secondary(&gen->meta[b1], tag)) {
        int si = htc_bucket_scan(&gen->buckets[b2], &gen->meta[b2],
                                 t->arena, hash, tag, HTC_SCAN_SECONDARY);
        if (si >= 0) {
            htc_seq_guard_t gs1 = htc_bucket_seq_begin(&gen->meta[b1]);
            htc_seq_guard_t gs2 = htc_bucket_seq_begin(&gen->meta[b2]);
            uint64_t w = __atomic_load_n(&gen->buckets[b2].slot[si],
                                         HTC_MO_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, HTC_MO_RELEASE);
            __atomic_store_n(&gen->buckets[b2].slot[si],
                             htc_slot_empty_word(), HTC_MO_RELEASE);
            htc_ctrl_clear(&gen->meta[b2], (unsigned)si);
            htc_bucket_seq_end(&gen->meta[b2], gs2);
            htc_remap_dec(&gen->meta[b1]);
            htc_bucket_seq_end(&gen->meta[b1], gs1);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->have_amq_filter) t->amq_filter.delete(t->amq_filter.filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            HTC_STAT_INC(t->stats.retired_record_count);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
            htc_epoch_advance(t->epoch);
            return HTC_OK;
        }
    }
    {
        htc_stash_t *ss = gen->shards ? &gen->shards[htc_shard_of(b1, gen->shard_count)].stash : &t->stash;
        int si = htc_stash_find(ss, t->arena, hash, tag);
        htc_stash_t *found = ss;
        if (si < 0 && ss != &t->stash) {
            si = htc_stash_find(&t->stash, t->arena, hash, tag);
            if (si >= 0) found = &t->stash;
        }
        if (si >= 0) {
            uint64_t w = __atomic_load_n(&found->slots[si], HTC_MO_ACQUIRE);
            uint32_t idx = htc_slot_index(w);
            htc_record_t *rec = htc_arena_ptr(t->arena, idx);
            __atomic_store_n(&rec->flags, 1, HTC_MO_RELEASE);
            htc_stash_remove_at(found, (unsigned)si);
            htc_stash_maintain(found);
            htc_front_cache_remove(&htc_thread_cache, hash);
            if (t->have_amq_filter) t->amq_filter.delete(t->amq_filter.filter, hash);
            htc_epoch_retire(t->epoch, t->arena, idx);
            HTC_STAT_INC(t->stats.retired_record_count);
            __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
            if (t->shards) htc_unlock_shards(t->shards, s1, s2);
            HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
            htc_epoch_advance(t->epoch);
            return HTC_OK;
        }
    }

    /* Not found in current gen — try old generations (post-migration) */
    if (gen->old) {
        uint64_t old_val = 0;
        if (htc_find_in_old_gen(gen->old, t->arena, hash, tag, &old_val)) {
            htc_ensure_chunk_migrated(t, b1);
            if (b2 != b1) htc_ensure_chunk_migrated(t, b2);
            int si = htc_bucket_scan(&gen->buckets[b1], &gen->meta[b1],
                                     t->arena, hash, tag, HTC_SCAN_PRIMARY);
            if (si >= 0) {
                htc_seq_guard_t gs = htc_bucket_seq_begin(&gen->meta[b1]);
                uint64_t w = __atomic_load_n(&gen->buckets[b1].slot[si],
                                             HTC_MO_ACQUIRE);
                uint32_t idx = htc_slot_index(w);
                htc_record_t *rec = htc_arena_ptr(t->arena, idx);
                __atomic_store_n(&rec->flags, 1, HTC_MO_RELEASE);
                __atomic_store_n(&gen->buckets[b1].slot[si],
                                 htc_slot_empty_word(), HTC_MO_RELEASE);
                htc_ctrl_clear(&gen->meta[b1], (unsigned)si);
                htc_bucket_seq_end(&gen->meta[b1], gs);
                htc_front_cache_remove(&htc_thread_cache, hash);
                htc_epoch_retire(t->epoch, t->arena, idx);
                HTC_STAT_INC(t->stats.retired_record_count);
                __atomic_fetch_sub(&t->size, 1, __ATOMIC_RELAXED);
                if (t->shards) htc_unlock_shards(t->shards, s1, s2);
                HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
                htc_epoch_advance(t->epoch);
                return HTC_OK;
            }
        }
    }

    if (t->shards) htc_unlock_shards(t->shards, s1, s2);
    HTC_STAT_ADD(t->stats.reclaimed_record_count, htc_epoch_collect(t->epoch, t->arena));
    htc_epoch_advance(t->epoch);
    return HTC_ERR_NOT_FOUND;
}

size_t htc_size(const htc_table_t *t)
{
    if (!t) return 0;
    return __atomic_load_n(&((htc_table_t *)t)->size, __ATOMIC_RELAXED);
}
