#ifndef DRAUGR_HTC_INTERNAL_H
#define DRAUGR_HTC_INTERNAL_H

#include "draugr/htc.h"
#include "draugr/alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define HTC_BUCKET_SLOTS         8
#define HTC_TAG_ZERO_REPLACEMENT 1
#define HTC_STASH_GROW           4

/* BFS displacement parameters */
#define HTC_BFS_MAX_DEPTH       5
#define HTC_BFS_BUCKET_BUDGET  64
#define HTC_BFS_MAX_PATH        8
#define HTC_BFS_MAX_SHARDS     8   /* fall back to stash if path touches more */

#define HTC_STATE_EMPTY      0U
#define HTC_STATE_LIVE       1U
#define HTC_STATE_TENTATIVE  2U
#define HTC_STATE_MOVING     3U
#define HTC_STATE_DELETED    4U

#define HTC_SEQ_BUSY 1U
#define HTC_REMAP_SATURATED 0xFFU
#define HTC_DEFAULT_SHARD_COUNT 128
#define HTC_EPOCH_MAX_THREADS 64

#define HTC_SLOT_INDEX_MASK  0x000000FFFFFFFFFFULL
#define HTC_SLOT_INDEX_SHIFT 0
#define HTC_SLOT_TAG_SHIFT   40
#define HTC_SLOT_STATE_SHIFT 56
#define HTC_SLOT_SEC_MASK    0x0800000000000000ULL
#define HTC_SLOT_HOT_MASK    0x2000000000000000ULL

typedef struct {
    uint64_t full_hash;
    uint32_t generation;
    _Atomic uint32_t  flags;
    _Atomic uint64_t user_value;
} htc_record_t;

typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t slot[HTC_BUCKET_SLOTS];
} htc_bucket_t;

_Static_assert(sizeof(htc_bucket_t) == 64,      "htc_bucket_t must be 64 bytes");
_Static_assert(__alignof__(htc_bucket_t) == 64,  "htc_bucket_t must be 64B aligned");

typedef struct __attribute__((aligned(16))) {
    _Atomic uint64_t ctrl_tags;
    _Atomic uint32_t seq;
    _Atomic uint16_t remap_filter;
    _Atomic uint8_t  remap_count;
    uint8_t           flags;
} htc_bucket_meta_t;

_Static_assert(sizeof(htc_bucket_meta_t) == 16,      "htc_bucket_meta_t must be 16 bytes");
_Static_assert(__alignof__(htc_bucket_meta_t) == 16,  "htc_bucket_meta_t must be 16B aligned");

/* ─── Atomic abstraction layer (Phase 7) ──────────────────── */
/* On AArch64 with LSE, the compiler generates CASAL/LDADD etc.
   Without LSE it falls back to LL/SC loops via __atomic builtins.
   This layer provides consistent naming and a single point to
   add LSE hand-tuned paths later if needed. */

static inline uint64_t htc_atomic_load(const _Atomic uint64_t *p, int mo) {
    return __atomic_load_n(p, mo);
}
static inline void htc_atomic_store(_Atomic uint64_t *p, uint64_t v, int mo) {
    __atomic_store_n(p, v, mo);
}
static inline uint64_t htc_atomic_xchg(_Atomic uint64_t *p, uint64_t v, int mo) {
    return __atomic_exchange_n(p, v, mo);
}
static inline bool htc_atomic_cas(_Atomic uint64_t *p, uint64_t *exp, uint64_t des, int mo_s, int mo_f) {
    return __atomic_compare_exchange_n(p, exp, des, false, mo_s, mo_f);
}
static inline uint64_t htc_atomic_fetch_or(_Atomic uint64_t *p, uint64_t v, int mo) {
    return __atomic_fetch_or(p, v, mo);
}

/* ─── Spinlock for shard-level writer mutual exclusion ───── */
typedef struct {
    _Atomic uint8_t flag;
} htc_spinlock_t;

typedef struct {
    htc_record_t *records;
    uint32_t      count;
    uint32_t      capacity;
    uint32_t     *free_idx;
    uint32_t      free_count;
    uint32_t      free_cap;
    void         *allocator;
    htc_spinlock_t lock;
} htc_arena_t;

/* Stash: adaptive per-shard overflow. Grows 4→8→16→32 on pressure,
   shrinks when empty for many maintenance epochs. */
#define HTC_STASH_MIN      4
#define HTC_STASH_DEFAULT  8
#define HTC_STASH_MAX     32

typedef struct {
    _Atomic uint64_t *slots;
    uint32_t          capacity;
    uint32_t          size;
    void             *allocator;
    uint16_t          full_events;
    uint16_t          empty_epochs;
    htc_spinlock_t    lock;
} htc_stash_t;

/* ─── Shard: one per shard, covers a range of buckets ───── */
typedef struct __attribute__((aligned(64))) {
    htc_spinlock_t  lock;
    htc_stash_t     stash;
    htc_arena_t     arena;          /* per-shard record allocator */
    uint32_t        bucket_begin;
    uint32_t        bucket_count;
    _Atomic uint8_t migrated;
} htc_shard_t;

_Static_assert(sizeof(htc_shard_t) % 64 == 0, "htc_shard_t size must be a multiple of 64 bytes");

/* ─── Retire node for deferred epoch-based free ──────────── */
typedef struct htc_retire_node {
    uint32_t                    arena_idx;
    uint32_t                    shard_id;   /* which shard's arena owns this */
    uint64_t                    retire_epoch;
    struct htc_retire_node     *next;
} htc_retire_node_t;

/* ─── Epoch-based reclamation control ────────────────────── */
typedef struct {
    _Atomic uint64_t                  global_epoch;
    _Atomic uint64_t                  thread_epoch[HTC_EPOCH_MAX_THREADS];
    _Atomic(htc_retire_node_t *)      retire_head;
} htc_epoch_ctl_t;

/* ─── Seq guard for optimistic bucket reads ──────────────── */
typedef struct {
    uint32_t old_seq;
} htc_seq_guard_t;

/* ─── Thread-local front cache (Phase 8, optional) ───────── */
/* Non-authoritative per-thread cache for repeated lookups.
 * Thread-local storage avoids cache-line bouncing between CPUs. */
#define HTC_FRONT_CACHE_ENTRIES 128

typedef struct {
    uint64_t hash;
    uint32_t arena_idx;
    uint32_t generation;
    uint8_t  valid;
    uint8_t  _pad[3];
} htc_front_cache_entry_t;

typedef struct {
    htc_front_cache_entry_t entries[HTC_FRONT_CACHE_ENTRIES];
    uint32_t pos;
} htc_front_cache_t;

extern _Thread_local htc_front_cache_t htc_thread_cache;

/* ─── Table generation (for resize/migration) ────────────── */
#define HTC_CHUNK_SHIFT    6  /* 64 buckets per chunk */
#define HTC_CHUNK_SIZE     (1U << HTC_CHUNK_SHIFT)
#define HTC_CHUNK_MASK     (HTC_CHUNK_SIZE - 1)

typedef enum {
    HTC_GEN_ACTIVE   = 0,  /* writers may commit */
    HTC_GEN_FREEZING = 1,  /* resize in progress; new writers must retry */
    HTC_GEN_OLD      = 2,  /* read-only fallback */
} htc_gen_state_t;

typedef struct htc_table_gen {
    _Atomic uint32_t   state;
    htc_bucket_t      *buckets;
    htc_bucket_meta_t *meta;
    htc_arena_t       *arena;      /* per-shard arena access via htc_shard_arena() */
    htc_shard_t       *shards;
    uint64_t          *migrated_bitmap;
    uint32_t           bucket_mask;
    uint32_t           num_buckets;
    uint32_t           shard_count;
    uint32_t           chunk_count;
    struct htc_table_gen *old;
} htc_table_gen_t;

/* Returns the arena for a given bucket's shard */
static inline uint32_t htc_chunk_of(uint32_t bucket_id) {
    return bucket_id >> HTC_CHUNK_SHIFT;
}

static inline uint32_t htc_shard_of(uint32_t bucket_id, uint32_t shard_count) {
    return bucket_id % shard_count;
}

static inline htc_arena_t *htc_shard_arena(htc_shard_t *shards, uint32_t bucket, uint32_t sc) {
    return &shards[htc_shard_of(bucket, sc)].arena;
}

/* ─── Performance counters (optional, gated by HTC_STATS) ─── */
#ifdef HTC_STATS
typedef struct {
    _Atomic uint64_t find_primary_hit;
    _Atomic uint64_t find_secondary_hit;
    _Atomic uint64_t find_stash_hit;
    _Atomic uint64_t find_oldgen_hit;
    _Atomic uint64_t find_negative;
    _Atomic uint64_t secondary_skipped;
    _Atomic uint64_t secondary_checked;
    _Atomic uint64_t seq_retries;
    _Atomic uint64_t bfs_attempts;
    _Atomic uint64_t bfs_success;
    _Atomic uint64_t bfs_no_path;
    _Atomic uint64_t bfs_depth_histogram[8];
    _Atomic uint64_t stash_insert;
    _Atomic uint64_t stash_grow;
    _Atomic uint64_t stash_full;
    _Atomic uint64_t remap_saturations;
    _Atomic uint64_t front_cache_hit;
    _Atomic uint64_t front_cache_miss;
    _Atomic uint64_t grow_started;
    _Atomic uint64_t grow_copied_bucket_entries;
    _Atomic uint64_t grow_copied_stash_entries;
    _Atomic uint64_t writer_retry_gen_changed;
    _Atomic uint64_t writer_retry_gen_frozen;
    _Atomic uint64_t attempted_write_to_frozen_gen;
    _Atomic uint64_t attempted_write_to_old_gen;
} htc_stats_t;

#define HTC_STAT_INC(s) __atomic_fetch_add(&s, 1, __ATOMIC_RELAXED)

/** Print all stats counters to stdout (requires HTC_STATS). */
void htc_stats_print(const htc_table_t *t);
#else
typedef int htc_stats_t;
#define HTC_STAT_INC(s) ((void)0)
static inline void htc_stats_print(const htc_table_t *t) { (void)t; }
#endif

struct htc_table {
    /* Phase 1: core cuckoo data */
    htc_bucket_t      *buckets;
    htc_bucket_meta_t *meta;
    htc_arena_t       *arena;      /* global record arena */
    htc_stash_t        stash;
    void    *allocator;

    /* Phase 2: concurrency control */
    htc_shard_t       *shards;
    htc_epoch_ctl_t   *epoch;
    _Atomic(htc_table_gen_t *) current_gen;
    uint32_t           shard_count;
    uint32_t           flags;             /* from htc_config_t */

    /* Grow serialization */
    htc_spinlock_t     grow_lock;

    /* Optional AMQ filter for negative lookup acceleration (§25) */
    htc_amq_filter_t   amq_filter;
    int                have_amq_filter;

#ifdef HTC_STATS
    htc_stats_t        stats;
#endif

    _Atomic size_t     size;
    uint32_t bucket_mask;
    uint32_t num_buckets;
    double   max_load_factor;
};

static inline uint64_t htc_slot_pack(uint64_t idx, uint16_t tag,
                                      unsigned state, bool in_sec) {
    return (idx      & HTC_SLOT_INDEX_MASK)
         | ((uint64_t)(tag)   << HTC_SLOT_TAG_SHIFT)
         | ((uint64_t)(state) << HTC_SLOT_STATE_SHIFT)
         | (in_sec ? HTC_SLOT_SEC_MASK : 0);
}

static inline uint64_t htc_slot_index(uint64_t w) { return w & HTC_SLOT_INDEX_MASK; }
static inline uint16_t htc_slot_tag(uint64_t w) { return (uint16_t)((w >> HTC_SLOT_TAG_SHIFT) & 0xFFFF); }
static inline unsigned htc_slot_state(uint64_t w) { return (unsigned)((w >> HTC_SLOT_STATE_SHIFT) & 0x7); }
static inline bool htc_slot_in_secondary(uint64_t w) { return (w & HTC_SLOT_SEC_MASK) != 0; }
static inline bool htc_slot_live(uint64_t w) { return htc_slot_state(w) == HTC_STATE_LIVE; }
static inline bool htc_slot_empty(uint64_t w) { return htc_slot_state(w) == HTC_STATE_EMPTY; }
static inline uint64_t htc_slot_empty_word(void) { return htc_slot_pack(0, 0, HTC_STATE_EMPTY, 0); }

static inline uint16_t htc_tag16(uint64_t h) {
    uint16_t t = (uint16_t)((h >> 32) ^ (h >> 48));
    return t ? t : HTC_TAG_ZERO_REPLACEMENT;
}

static inline uint8_t htc_partial8(uint16_t tag) {
    uint8_t p = (uint8_t)(tag ^ (tag >> 8));
    return p ? p : HTC_TAG_ZERO_REPLACEMENT;
}

static inline uint32_t htc_mix32(uint32_t x) {
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = ((x >> 16) ^ x) * 0x45d9f3bU;
    x = (x >> 16) ^ x;
    return x;
}

static inline uint32_t htc_alt_bucket(uint32_t b1, uint64_t h, uint16_t tag) {
    return b1 ^ htc_mix32((uint32_t)(h >> 32) ^ ((uint32_t)tag << 16));
}

static inline uint64_t htc_match8(uint64_t ctrl, uint8_t p) {
#if defined(__aarch64__)
    /* NEON: vceq returns 0xFF per matching byte. Mask to 0x80 to match
     * the SWAR convention used by htc_ctz_candidate / htc_clear_candidate. */
    uint8x8_t vctrl = vcreate_u8(ctrl);
    uint8x8_t vpat  = vdup_n_u8(p);
    uint8x8_t veq   = vceq_u8(vctrl, vpat);
    return vget_lane_u64(vreinterpret_u64_u8(veq), 0) & 0x8080808080808080ULL;
#else
    /* SWAR fallback for non-ARM targets */
    uint64_t rep = 0x0101010101010101ULL * p;
    uint64_t x   = ctrl ^ rep;
    uint64_t z   = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    return z;
#endif
}

static inline unsigned htc_ctz_candidate(uint64_t mask) {
    return (unsigned)(__builtin_ctzll(mask) >> 3);
}

static inline uint64_t htc_clear_candidate(uint64_t mask, unsigned i) {
    return mask & ~(0x80ULL << (i * 8));
}

static inline uint64_t htc_ctrl_load(const htc_bucket_meta_t *m) {
    return __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
}

static inline void htc_ctrl_set(htc_bucket_meta_t *m, unsigned i, uint8_t p) {
    uint64_t set = ((uint64_t)p) << (i * 8);
    uint64_t clr = ~(0xFFULL << (i * 8));
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    uint64_t desired = (old & clr) | set;
    while (!__atomic_compare_exchange_n(&m->ctrl_tags, &old, desired,
                                        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        desired = (old & clr) | set;
    }
}

static inline void htc_ctrl_clear(htc_bucket_meta_t *m, unsigned i) {
    uint64_t clr = ~(0xFFULL << (i * 8));
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    uint64_t desired = old & clr;
    while (!__atomic_compare_exchange_n(&m->ctrl_tags, &old, desired,
                                        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
        desired = old & clr;
    }
}

static inline void htc_remap_inc(htc_bucket_meta_t *m, uint16_t tag) {
    __atomic_fetch_or(&m->remap_filter,
                      (uint16_t)(1U << (tag & 0xF)), __ATOMIC_RELAXED);
    uint8_t c = __atomic_fetch_add(&m->remap_count, 1, __ATOMIC_RELAXED);
    if (c >= HTC_REMAP_SATURATED - 1)
        __atomic_store_n(&m->remap_count, HTC_REMAP_SATURATED, __ATOMIC_RELAXED);
}

static inline void htc_remap_dec(htc_bucket_meta_t *m) {
    uint8_t c = __atomic_load_n(&m->remap_count, __ATOMIC_RELAXED);
    if (c == 0 || c == HTC_REMAP_SATURATED) return;
    __atomic_fetch_sub(&m->remap_count, 1, __ATOMIC_RELAXED);
}

static inline bool htc_must_check_secondary(const htc_bucket_meta_t *m,
                                             uint16_t tag) {
    uint8_t c = __atomic_load_n(&m->remap_count, __ATOMIC_RELAXED);
    if (c == 0) return false;
    if (c == HTC_REMAP_SATURATED) return true;
    uint16_t f = __atomic_load_n(&m->remap_filter, __ATOMIC_RELAXED);
    return (f & (1U << (tag & 0xF))) != 0;
}

typedef enum {
    HTC_SCAN_PRIMARY   = 0,
    HTC_SCAN_SECONDARY = 1,
} htc_scan_mode_t;

/* ============================================================================
 * Spinlock helpers (Phase 2)
 * ============================================================================ */

static inline void htc_spin_lock(htc_spinlock_t *lk) {
    while (__atomic_exchange_n(&lk->flag, 1, __ATOMIC_ACQUIRE))
        ;
}

static inline void htc_spin_unlock(htc_spinlock_t *lk) {
    __atomic_store_n(&lk->flag, 0, __ATOMIC_RELEASE);
}

/* ============================================================================
 * Bucket sequence counter helpers (Phase 2)
 * Writer marks bucket BUSY before mutation, readers validate via seq.
 * ============================================================================ */

static inline htc_seq_guard_t htc_bucket_seq_begin(htc_bucket_meta_t *m) {
    uint32_t old = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
    while (old & HTC_SEQ_BUSY)
        old = __atomic_load_n(&m->seq, __ATOMIC_ACQUIRE);
    __atomic_store_n(&m->seq, old | HTC_SEQ_BUSY, __ATOMIC_RELEASE);
    __atomic_thread_fence(__ATOMIC_ACQ_REL);
    return (htc_seq_guard_t){ .old_seq = old };
}

static inline void htc_bucket_seq_end(htc_bucket_meta_t *m, htc_seq_guard_t g) {
    __atomic_thread_fence(__ATOMIC_RELEASE);
    __atomic_store_n(&m->seq, (g.old_seq + 2u) & ~HTC_SEQ_BUSY, __ATOMIC_RELEASE);
}

/* ============================================================================
 * Epoch helpers (Phase 2)
 * Readers pin epoch before accessing arena records;
 * writers retire records after clearing slots.
 * Thread-local TID auto-registers on first call; crashes/forgetful threads
 * block GC until their slot is manually cleared.
 * ============================================================================ */

uint64_t htc_epoch_pin(htc_epoch_ctl_t *ep);
void    htc_epoch_unpin(htc_epoch_ctl_t *ep);

/* ============================================================================
 * Shard locking helpers (Phase 2)
 * Always acquire shard locks in ascending shard ID.
 * ============================================================================ */

static inline void htc_lock_shards(htc_shard_t *shards,
                                    uint32_t a, uint32_t b) {
    if (a == b) { htc_spin_lock(&shards[a].lock); return; }
    if (a < b)  { htc_spin_lock(&shards[a].lock); htc_spin_lock(&shards[b].lock); }
    else        { htc_spin_lock(&shards[b].lock); htc_spin_lock(&shards[a].lock); }
}

static inline void htc_unlock_shards(htc_shard_t *shards,
                                      uint32_t a, uint32_t b) {
    if (a == b) { htc_spin_unlock(&shards[a].lock); return; }
    htc_spin_unlock(&shards[a].lock);
    htc_spin_unlock(&shards[b].lock);
}

uint32_t      htc_arena_alloc(htc_arena_t *a, uint64_t hash, uint64_t value);
void          htc_arena_free(htc_arena_t *a, uint32_t idx);
htc_record_t *htc_arena_ptr(htc_arena_t *a, uint32_t idx);

/* ─── Front cache inline operations (Phase 8) ───────────── */
static inline bool htc_front_cache_lookup(const htc_front_cache_t *c,
                                           htc_arena_t *a, uint64_t hash,
                                           uint64_t *out_value) {
    for (uint32_t i = 0; i < HTC_FRONT_CACHE_ENTRIES; i++) {
        if (c->entries[i].valid && c->entries[i].hash == hash) {
            if (c->entries[i].arena_idx >= a->count) continue;
            htc_record_t *r = htc_arena_ptr(a, c->entries[i].arena_idx);
            if (r->generation == c->entries[i].generation && r->full_hash == hash
                && __atomic_load_n(&r->flags, __ATOMIC_RELAXED) == 0) {
                *out_value = __atomic_load_n(&r->user_value, __ATOMIC_RELAXED);
                return true;
            }
        }
    }
    return false;
}
static inline void htc_front_cache_insert(htc_front_cache_t *c,
                                           uint64_t hash, uint32_t arena_idx,
                                           uint32_t generation) {
    uint32_t p = c->pos++ % HTC_FRONT_CACHE_ENTRIES;
    c->entries[p].valid     = 1;
    c->entries[p].hash      = hash;
    c->entries[p].arena_idx = arena_idx;
    c->entries[p].generation = generation;
}
static inline void htc_front_cache_remove(htc_front_cache_t *c, uint64_t hash) {
    for (uint32_t i = 0; i < HTC_FRONT_CACHE_ENTRIES; i++)
        if (c->entries[i].valid && c->entries[i].hash == hash)
            c->entries[i].valid = 0;
}

bool htc_grow(htc_table_t *t);
int  htc_bucket_scan(htc_bucket_t *b, htc_bucket_meta_t *m,
                      htc_arena_t *a, uint64_t h, htc_scan_mode_t mode);
int  htc_stash_insert(htc_stash_t *s, uint64_t slot_word);
int  htc_stash_find(const htc_stash_t *s, htc_arena_t *a, uint64_t h);
void htc_stash_remove_at(htc_stash_t *s, unsigned idx);
void htc_stash_maintain(htc_stash_t *s);

/* Epoch operations (Phase 2) */
void htc_epoch_retire(htc_epoch_ctl_t *ep, htc_arena_t *a, uint32_t arena_idx);
void htc_epoch_collect(htc_epoch_ctl_t *ep, htc_arena_t *a);
void htc_epoch_advance(htc_epoch_ctl_t *ep);

/* Migration (Phase 6) */
bool htc_resize_start(htc_table_t *t, uint32_t new_num_buckets);
void htc_migrate_chunk(htc_table_t *t, uint32_t chunk_id);
void htc_ensure_chunk_migrated(htc_table_t *t, uint32_t bucket_id);
void htc_resize_finish(htc_table_t *t);
bool htc_find_in_old_gen(const htc_table_gen_t *g, htc_arena_t *arena,
                          uint64_t hash, uint64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif
