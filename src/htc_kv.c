/**
 * htc_kv.c — High-level concurrent key-value store backed by htc
 */

#include "draugr/htc_kv.h"
#include "draugr/htc_internal.h"
#include <string.h>

/* ─── Key-value entry stored in arena records ──────────────── */
typedef struct htc_kv_entry {
    void   *key;
    size_t  klen;
    void   *val;
    size_t  vlen;
    int     key_copy;   /* 1 if key was internally allocated */
    int     val_copy;   /* 1 if val was internally allocated */
    void   *allocator;  /* t->allocator, for deferred free */
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
    htc_kv_entry_t   *all_entries; /* intrusive list of live entries */
    htc_spinlock_t    list_lock;   /* protects all_entries mutations */
};

/* ─── Lifecycle ─────────────────────────────────────────────── */
htc_kv_t *htc_kv_create(const htc_config_t *cfg) {
    htc_kv_t *kv = (htc_kv_t *)DRAUGR_CALLOC(NULL, 1, sizeof(htc_kv_t));
    if (!kv) return NULL;
    kv->t = htc_create(cfg);
    if (!kv->t) { DRAUGR_FREE(NULL, kv, sizeof(htc_kv_t)); return NULL; }
    return kv;
}

void htc_kv_destroy(htc_kv_t *kv) {
    if (!kv) return;

    /* Drain pending epoch retire callbacks so removed entries are fully
     * freed (callback frees key/val + struct). After draining, only
     * never-removed live entries remain on the list. */
    htc_table_t *t = kv->t;
    void *alloc = t->allocator;
    (void)alloc;
    if (t->epoch && t->arena) {
        for (int i = 0; i < 8; i++) {
            htc_epoch_advance(t->epoch);
            htc_epoch_collect(t->epoch, t->arena);
        }
    }

    /* Walk remaining live entries — quiescent, no concurrent modifications.
     * Only entries that were never removed remain. */
    htc_kv_entry_t *e = kv->all_entries;
    while (e) {
        htc_kv_entry_t *next = e->next;
        if (e->key_copy) DRAUGR_FREE(alloc, e->key, e->klen ? e->klen : 1);
        if (e->val_copy) DRAUGR_FREE(alloc, e->val, e->vlen ? e->vlen : 1);
        DRAUGR_FREE(alloc, e, sizeof(htc_kv_entry_t));
        e = next;
    }

    htc_destroy(t);
    DRAUGR_FREE(NULL, kv, sizeof(htc_kv_t));
}

/* ─── List management ─────────────────────────────────────────── */

static void list_push(htc_kv_t *kv, htc_kv_entry_t *e) {
    htc_spin_lock(&kv->list_lock);
    e->next = kv->all_entries;
    kv->all_entries = e;
    htc_spin_unlock(&kv->list_lock);
}

static void list_remove(htc_kv_t *kv, htc_kv_entry_t *target) {
    htc_spin_lock(&kv->list_lock);
    htc_kv_entry_t **pp = &kv->all_entries;
    while (*pp) {
        if (*pp == target) {
            *pp = target->next;
            break;
        }
        pp = &(*pp)->next;
    }
    htc_spin_unlock(&kv->list_lock);
}

/* ─── Insert helper ─────────────────────────────────────────── */
static bool kv_insert(htc_kv_t *kv, htc_kv_entry_t *e, uint64_t hash) {
    uint64_t ptr = (uint64_t)(uintptr_t)e;
    if (htc_insert(kv->t, hash, ptr) != HTC_OK) return false;
    list_push(kv, e);
    return true;
}

htc_kv_entry_t *kv_entry_alloc(void *alloc, const void *key, size_t klen,
                                const void *val, size_t vlen, int copy_keys)
{
    htc_kv_entry_t *e = (htc_kv_entry_t *)DRAUGR_CALLOC(alloc, 1, sizeof(htc_kv_entry_t));
    if (!e) return NULL;
    e->allocator = alloc;

    if (copy_keys) {
        e->key = DRAUGR_ALLOC(alloc, klen ? klen : 1);
        if (!e->key) { DRAUGR_FREE(alloc, e, sizeof(htc_kv_entry_t)); return NULL; }
        memcpy(e->key, key, klen);
        e->key_copy = 1;
    } else {
        e->key = (void *)key;
    }
    e->klen = klen;

    if (copy_keys) {
        e->val = DRAUGR_ALLOC(alloc, vlen ? vlen : 1);
        if (!e->val) {
            if (e->key_copy) DRAUGR_FREE(alloc, e->key, e->klen ? e->klen : 1);
            DRAUGR_FREE(alloc, e, sizeof(htc_kv_entry_t));
            return NULL;
        }
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
    htc_kv_entry_t *e = kv_entry_alloc(kv->t->allocator, key, klen, val, vlen, 0);
    if (!e) return false;
    if (!kv_insert(kv, e, hash)) {
        DRAUGR_FREE(kv->t->allocator, e, sizeof(htc_kv_entry_t));
        return false;
    }
    return true;
}

bool htc_kv_insert_copy(htc_kv_t *kv, const void *key, size_t klen,
                        const void *val, size_t vlen)
{
    if (!kv || !key) return false;
    uint64_t hash = kv_hash(key, klen);
    htc_kv_entry_t *e = kv_entry_alloc(kv->t->allocator, key, klen, val, vlen, 1);
    if (!e) return false;
    if (!kv_insert(kv, e, hash)) {
        void *alloc = kv->t->allocator;
        (void)alloc;
        if (e->key_copy) DRAUGR_FREE(alloc, e->key, e->klen ? e->klen : 1);
        if (e->val_copy) DRAUGR_FREE(alloc, e->val, e->vlen ? e->vlen : 1);
        DRAUGR_FREE(alloc, e, sizeof(htc_kv_entry_t));
        return false;
    }
    return true;
}

/* Deferred free callback for epoch-based entry reclamation.
 * Entry has already been removed from the all_entries list,
 * so we can free key/val copies and the struct itself. */
static void kv_entry_retire_cb(void *ctx) {
    htc_kv_entry_t *e = (htc_kv_entry_t *)ctx;
    void *alloc = e->allocator;
    (void)alloc;
    if (e->key_copy) DRAUGR_FREE(alloc, e->key, e->klen ? e->klen : 1);
    if (e->val_copy) DRAUGR_FREE(alloc, e->val, e->vlen ? e->vlen : 1);
    DRAUGR_FREE(alloc, e, sizeof(htc_kv_entry_t));
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

    /* Scoped find: epoch stays pinned through key comparison, remove,
     * and entry reclamation — prevents the entry from being freed by
     * concurrent epoch_collect between any of these steps. */
    uint64_t ptr = 0;
    htc_error_t ret = htc_find_scoped(kv->t, hash, &ptr);
    if (ret != HTC_OK) return false;  /* epoch already unpinned */

    htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
    bool match = keys_equal(key, klen, e->key, e->klen);

    if (!match) {
        htc_epoch_unpin(kv->t->epoch);
        return false;
    }

    /* Remove from hash table while epoch is still pinned. */
    htc_error_t r = htc_remove(kv->t, hash);
    if (r != HTC_OK) {
        htc_epoch_unpin(kv->t->epoch);
        return false;
    }

    /* Remove from tracking list and defer free via epoch reclamation.
     * List removal is synchronous (under spinlock); struct free is
     * deferred until all threads have passed the removal epoch.
     * Epoch remains pinned through retire_custom so that the OOM
     * synchronous-fallback path (which frees e immediately) cannot
     * race with a concurrent epoch_collect on another thread. */
    list_remove(kv, e);
    htc_epoch_retire_custom(kv->t->epoch, kv_entry_retire_cb, e);
    htc_epoch_unpin(kv->t->epoch);
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
        if (__atomic_load_n(&r->flags, __ATOMIC_ACQUIRE) != 0) continue;  /* removed */
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

    /* Pin epoch while scanning arena records so that entries freed by
     * concurrent removes (and retired via epoch reclamation) are not
     * reclaimed mid-scan.  Without pinning, DRAUGR_USE_MALLOC mode can
     * free the htc_kv_entry_t struct, making e->klen/e->vlen a UAF. */
    htc_epoch_pin(kv->t->epoch);

    htc_table_gen_t *g = __atomic_load_n(&kv->t->current_gen, __ATOMIC_ACQUIRE);
    if (g && g->arena) {
        /* Count blocks */
        htc_arena_block_t *b = __atomic_load_n(&g->arena->head, __ATOMIC_RELAXED);
        while (b) { total += sizeof(htc_arena_block_t); b = b->next; }
        total += (size_t)g->arena->free_cap * sizeof(uint32_t);
        for (uint32_t i = 0; i < g->arena->count; i++) {
            htc_record_t *r = htc_arena_ptr(g->arena, i);
            if (!r) continue;
            if (__atomic_load_n(&r->flags, __ATOMIC_ACQUIRE) != 0) continue;
            uint64_t ptr = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
            if (ptr) {
                htc_kv_entry_t *e = (htc_kv_entry_t *)(uintptr_t)ptr;
                total += sizeof(htc_kv_entry_t) + e->klen + e->vlen;
            }
        }
    }

    htc_epoch_unpin(kv->t->epoch);
    return total;
}
