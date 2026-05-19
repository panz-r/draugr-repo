/**
 * htc_kv.c — High-level concurrent key-value store backed by htc
 */

#include "draugr/htc_kv.h"
#include "draugr/htc_internal.h"
#include <stdlib.h>
#include <string.h>

/* ─── Key-value entry stored in arena records ──────────────── */
typedef struct htc_kv_entry {
    void   *key;
    size_t  klen;
    void   *val;
    size_t  vlen;
    int     key_copy;   /* 1 if key was internally allocated */
    int     val_copy;   /* 1 if val was internally allocated */
    int     freed;      /* 1 if deferred-free callback already ran */
    struct htc_kv_entry *next; /* intrusive list for lifecycle tracking */
} htc_kv_entry_t;

/* ─── Hash function (FNV-1a for arbitrary byte strings) ────── */
static uint64_t kv_hash(const void *key, size_t klen) {
    const unsigned char *p = (const unsigned char *)key;
    uint64_t h = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < klen; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

/* ─── Key equality (full comparison, not just hash) ────────── */
static bool keys_equal(const void *a, size_t alen, const void *b, size_t blen) {
    return alen == blen && memcmp(a, b, alen) == 0;
}

/* ─── htc_kv structure ─────────────────────────────────────── */
struct htc_kv {
    htc_table_t      *t;
    htc_kv_entry_t   *all_entries; /* intrusive list of every allocated entry */
};

/* ─── Lifecycle ─────────────────────────────────────────────── */
htc_kv_t *htc_kv_create(const htc_config_t *cfg) {
    htc_kv_t *kv = (htc_kv_t *)calloc(1, sizeof(htc_kv_t));
    if (!kv) return NULL;
    kv->t = htc_create(cfg);
    if (!kv->t) { free(kv); return NULL; }
    return kv;
}

void htc_kv_destroy(htc_kv_t *kv) {
    if (!kv) return;

    /* Drain pending epoch retire callbacks so all freed flags are set
     * before we walk the list.  After draining, every removed entry
     * has freed==1 and its key/val already freed by the callback. */
    htc_table_t *t = kv->t;
    if (t->epoch && t->arena) {
        for (int i = 0; i < 8; i++) {
            htc_epoch_advance(t->epoch);
            htc_epoch_collect(t->epoch, t->arena);
        }
    }

    /* Walk the intrusive list — quiescent, no concurrent modifications.
     * freed==1: key/val freed by callback, just free the struct.
     * freed==0: live entry, free key/val then the struct. */
    htc_kv_entry_t *e = kv->all_entries;
    while (e) {
        htc_kv_entry_t *next = e->next;
        if (!e->freed) {
            if (e->key_copy) free(e->key);
            if (e->val_copy) free(e->val);
        }
        free(e);
        e = next;
    }

    htc_destroy(t);
    free(kv);
}

/* ─── Insert helper ─────────────────────────────────────────── */
static bool kv_insert(htc_kv_t *kv, htc_kv_entry_t *e, uint64_t hash) {
    uint64_t ptr = (uint64_t)(uintptr_t)e;
    if (htc_insert(kv->t, hash, ptr) != HTC_OK) return false;
    /* Push onto tracking list (lock-free stack via CAS) */
    htc_kv_entry_t *old;
    do {
        old = kv->all_entries;
        e->next = old;
    } while (!__atomic_compare_exchange_n(&kv->all_entries, &old, e,
                                          false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
    return true;
}

htc_kv_entry_t *kv_entry_alloc(const void *key, size_t klen,
                                const void *val, size_t vlen, int copy_keys)
{
    htc_kv_entry_t *e = (htc_kv_entry_t *)calloc(1, sizeof(htc_kv_entry_t));
    if (!e) return NULL;

    if (copy_keys) {
        e->key = malloc(klen ? klen : 1);
        if (!e->key) { free(e); return NULL; }
        memcpy(e->key, key, klen);
        e->key_copy = 1;
    } else {
        e->key = (void *)key;
    }
    e->klen = klen;

    if (copy_keys) {
        e->val = malloc(vlen ? vlen : 1);
        if (!e->val) { if (e->key_copy) free(e->key); free(e); return NULL; }
        memcpy(e->val, val, vlen);
        e->val_copy = 1;
    } else {
        e->val = (void *)val;
    }
    e->vlen = vlen;

    return e;
}

bool htc_kv_insert(htc_kv_t *kv, const void *key, size_t klen,
                   const void *val, size_t vlen)
{
    if (!kv || !key) return false;
    uint64_t hash = kv_hash(key, klen);
    htc_kv_entry_t *e = kv_entry_alloc(key, klen, val, vlen, 0);
    if (!e) return false;
    if (!kv_insert(kv, e, hash)) {
        free(e);
        return false;
    }
    return true;
}

bool htc_kv_insert_copy(htc_kv_t *kv, const void *key, size_t klen,
                        const void *val, size_t vlen)
{
    if (!kv || !key) return false;
    uint64_t hash = kv_hash(key, klen);
    htc_kv_entry_t *e = kv_entry_alloc(key, klen, val, vlen, 1);
    if (!e) return false;
    if (!kv_insert(kv, e, hash)) {
        if (e->key_copy) free(e->key);
        if (e->val_copy) free(e->val);
        free(e);
        return false;
    }
    return true;
}

/* Deferred free callback for epoch-based entry reclamation.
 * Frees key/val copies and marks the entry freed. The struct
 * stays on the all_entries list for htc_kv_destroy to free. */
static void kv_entry_retire_cb(void *ctx) {
    htc_kv_entry_t *e = (htc_kv_entry_t *)ctx;
    if (e->key_copy) free(e->key);
    if (e->val_copy) free(e->val);
    e->key = NULL;
    e->val = NULL;
    e->freed = 1;
}

/* ─── Find ──────────────────────────────────────────────────── */
bool htc_kv_find(htc_kv_t *kv, const void *key, size_t klen,
                 void *val, size_t *vlen)
{
    if (!kv || !key) return false;
    uint64_t hash = kv_hash(key, klen);

    /* Scoped find: epoch stays pinned through key comparison */
    uint64_t ptr = 0;
    htc_error_t ret = htc_find_scoped(kv->t, hash, &ptr);
    if (ret != HTC_OK) return false;  /* epoch already unpinned */

    htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
    bool found = keys_equal(key, klen, e->key, e->klen);

    if (found) {
        if (val && vlen) {
            size_t copy_len = e->vlen < *vlen ? e->vlen : *vlen;
            memcpy(val, e->val, copy_len);
            *vlen = e->vlen;
        } else if (vlen) {
            *vlen = e->vlen;
        }
    }
    htc_epoch_unpin(kv->t->epoch);
    return found;
}

/* ─── Remove ────────────────────────────────────────────────── */
bool htc_kv_remove(htc_kv_t *kv, const void *key, size_t klen) {
    if (!kv || !key) return false;
    uint64_t hash = kv_hash(key, klen);

    /* Scoped find: epoch stays pinned through key comparison */
    uint64_t ptr = 0;
    htc_error_t ret = htc_find_scoped(kv->t, hash, &ptr);
    if (ret != HTC_OK) return false;  /* epoch already unpinned */

    htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
    bool match = keys_equal(key, klen, e->key, e->klen);
    htc_epoch_unpin(kv->t->epoch);
    /* After unpin, e may no longer be safe to dereference */

    if (!match) return false;

    htc_error_t r = htc_remove(kv->t, hash);
    if (r != HTC_OK) return false;

    /* Defer free of entry via epoch reclamation */
    htc_epoch_retire_custom(kv->t->epoch, kv_entry_retire_cb, e);
    return true;
}

void htc_kv_set_filter(htc_kv_t *kv, const htc_amq_filter_t *amq) {
    if (kv) htc_set_filter(kv->t, amq);
}

/* ─── Iterator ──────────────────────────────────────────────── */
void htc_kv_iter_init(htc_kv_iter_t *it, const htc_kv_t *kv) {
    it->kv = kv;
    it->idx = 0;
    it->key = NULL;
    it->klen = 0;
    it->val = NULL;
    it->vlen = 0;
    if (kv) {
        htc_table_gen_t *g = __atomic_load_n(&kv->t->current_gen, __ATOMIC_ACQUIRE);
        it->count = g && g->arena ? g->arena->count : 0;
    } else {
        it->count = 0;
    }
}

bool htc_kv_iter_next(htc_kv_iter_t *it) {
    if (!it || !it->kv) return false;
    htc_table_gen_t *g = __atomic_load_n(&it->kv->t->current_gen, __ATOMIC_ACQUIRE);
    if (!g || !g->arena) return false;

    while (it->idx < it->count) {
        uint32_t i = it->idx++;
        htc_record_t *r = htc_arena_ptr(g->arena, i);
        if (!r || r->identity_hash == 0) continue;  /* freed/deleted slot */
        uint64_t ptr = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
        if (!ptr) continue;

        htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
        it->key = e->key;
        it->klen = e->klen;
        it->val = e->val;
        it->vlen = e->vlen;
        return true;
    }
    return false;
}

/* ─── Stats ─────────────────────────────────────────────────── */
size_t htc_kv_count(const htc_kv_t *kv) {
    return kv ? htc_size(kv->t) : 0;
}

size_t htc_kv_memory_bytes(const htc_kv_t *kv) {
    if (!kv) return 0;
    size_t total = sizeof(htc_kv_t);
    htc_table_gen_t *g = __atomic_load_n(&kv->t->current_gen, __ATOMIC_ACQUIRE);
    if (g && g->arena) {
        /* Count blocks */
        htc_arena_block_t *b = __atomic_load_n(&g->arena->head, __ATOMIC_RELAXED);
        while (b) { total += sizeof(htc_arena_block_t); b = b->next; }
        total += (size_t)g->arena->free_cap * sizeof(uint32_t);
        for (uint32_t i = 0; i < g->arena->count; i++) {
            htc_record_t *r = htc_arena_ptr(g->arena, i);
            if (!r) continue;
            uint64_t ptr = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
            if (ptr) {
                htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
                total += sizeof(htc_kv_entry_t) + e->klen + e->vlen;
            }
        }
    }
    return total;
}
