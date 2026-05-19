#ifndef DRAUGR_HTC_INTERNAL_H
#define DRAUGR_HTC_INTERNAL_H

#include "draugr/htc.h"
#include "draugr/alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#if defined(__aarch64__) && !defined(__CBMC__)
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
    uint64_t identity_hash;
    uint64_t placement_hash;
    uint32_t generation;
    _Atomic uint32_t  flags;
    _Atomic uint64_t user_value;
} htc_record_t;

typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t slot[HTC_BUCKET_SLOTS];
} htc_bucket_t;

_Static_assert(sizeof(htc_bucket_t) == 64,      "htc_bucket_t must be 64 bytes");
_Static_assert(__alignof__(htc_bucket_t) == 64,  "htc_bucket_t must be 64B aligned");
_Static_assert(sizeof(htc_record_t) == 32,      "htc_record_t must be 32 bytes for cache density");

typedef struct __attribute__((aligned(16))) {
    _Atomic uint64_t ctrl_tags;
    _Atomic uint32_t seq;
    _Atomic uint16_t remap_filter;
    _Atomic uint8_t  remap_count;
    uint8_t           flags;
} htc_bucket_meta_t;

/* ─── State-machine diagrams (Battery 15 Q28) ──────────────────
 *
 * Record lifecycle:
 *   ALLOCATING (in htc_arena_alloc, identity_hash set, flags=0)
 *       │
 *       ▼ (slot CAS publishes — record becomes reachable)
 *   LIVE (flags=0, reachable from bucket/stash)
 *       │
 *       ▼ (htc_remove: flags=DELETED RELEASE)
 *   DELETED (logically deleted — all find paths reject)
 *       │
 *       ▼ (slot cleared, htc_epoch_retire called)
 *   RETIRED (on epoch retire list, not reachable from table)
 *       │
 *       ▼ (epoch_collect: retire_epoch < min_ep)
 *   FREE (htc_arena_free: identity_hash=0, generation++)
 *       │
 *       └──→ ALLOCATING (next htc_arena_alloc with generation++)
 *
 * Slot word lifecycle (bucket slot):
 *   EMPTY (state=0, ctrl_byte=0)
 *     │
 *     ▼ (CAS: EMPTY→LIVE with RELEASE)
 *   LIVE (state=3, tag+idx+in_sec, ctrl_byte=partial8(tag))
 *     │
 *     ▼ (RELEASE store: LIVE→EMPTY)
 *   EMPTY
 *
 * Slot word lifecycle (stash slot):
 *   EMPTY (state=0)
 *     │
 *     ▼ (CAS: EMPTY→LIVE with RELEASE)
 *   LIVE (state=3, tag+idx)
 *     │
 *     ▼ (RELEASE store: LIVE→EMPTY, no compaction)
 *   EMPTY
 *
 * Generation lifecycle:
 *   ACTIVE (writers may commit)
 *     │
 *     ▼ (htc_grow: CAS ACTIVE→FREEZING, lock all shards)
 *   FREEZING (no new writers, in-flight writers drain)
 *     │
 *     ▼ (htc_grow: rehash complete, mark OLD, publish new gen)
 *   OLD (not searched by new readers)
 *     │
 *     ▼ (htc_grow: htc_epoch_retire_gen)
 *   RETIRED (on epoch retire_gen list)
 *     │
 *     ▼ (epoch_collect: retire_epoch < min_ep)
 *   FREED (meta+buckets+migrated_bitmap freed, gen struct freed)
 *
 * Thread epoch lifecycle:
 *   UNREGISTERED (htc_epoch_tid == UINT_MAX)
 *     │
 *     ▼ (htc_epoch_pin: first call — CAS into thread_epoch[])
 *   REGISTERED (slot allocated, tid valid)
 *     │
 *     ├──→ PINNED (htc_epoch_pin: store global_epoch to slot)
 *     │        │
 *     │        ▼ (htc_epoch_unpin: store 0 to slot)
 *     │    UNPINNED (slot=0, thread known to be quiescent)
 *     │        │
 *     │        └──→ PINNED (next htc_epoch_pin)
 *     │
 *     ▼ (htc_thread_detach: memset cache, tid=UINT_MAX)
 *   UNREGISTERED
 *
 * Front-cache entry lifecycle:
 *   INVALID (valid=0)
 *     │
 *     ▼ (htc_front_cache_insert: set table, table_id, hash, gen, valid=1)
 *   VALID (may be returned by future htc_find)
 *     │
 *     ├──→ STALE (generation mismatch — htc_arena_free incremented it)
 *     ├──→ STALE (flags!=0 — record was deleted)
 *     ├──→ STALE (table/table_id mismatch — cross-table ABA protection)
 *     │         all STALE entries are skipped by lookup; never returned
 *     │
 *     ▼ (htc_front_cache_remove or next insert evicts slot)
 *   INVALID
 * ────────────────────────────────────────────────────────────────── */

/* ─── Transition centralization table (Battery 16 Q3) ─────────
 *
 * Every location transition should have one canonical implementation.
 *
 * D → P:  htc_place_entry (primary CAS path)
 * D → S:  htc_place_entry (secondary CAS path, includes remap_inc)
 * D → T:  htc_stash_insert (CAS into EMPTY stash slot)
 *
 * P → S:  htc_place_entry secondary path, or BFS commit via commit_path_locked
 * S → P:  BFS commit via commit_path_locked (validates/updates in_secondary)
 * P → T:  htc_insert stash-fallback after BFS failure
 * T → P:  htc_grow rehash (calls htc_rehash_place → htc_place_entry)
 * T → S:  htc_grow rehash (same reinsertion path)
 * S → T:  htc_insert stash-fallback (after BFS failure, entry was in S before)
 *
 * P → D:  htc_remove (primary bucket path)
 * S → D:  htc_remove (secondary bucket path, includes remap_dec)
 * T → D:  htc_stash_remove_at (RELEASE EMPTY store, no compaction)
 *
 * All bucket transitions go through seq_begin/seq_end.
 * All stash transitions are CAS-based (insert) or RELEASE-store (remove).
 * remap_inc/dec only called from bucket insert/remove/BFS paths.
 * ────────────────────────────────────────────────────────────────── */

/* ─── Memory-order table (Battery 22 Q1) ──────────────────────
 *
 * Every atomic field's allowed memory orders and justification.
 * RELAXED is sufficient where only atomicity (not ordering) is needed.
 *
 * slot word LOAD:
 *   ACQUIRE — pairs with RELEASE store to ensure record fields are
 *   visible after slot confirms LIVE.  On ARM, prevents reordering
 *   slot load before record field loads.  (If relaxed: reader could
 *   see LIVE slot but uninitialized record fields.)
 *
 * slot word STORE/CAS:
 *   RELEASE / ACQ_REL — pairs with reader ACQUIRE.  Ensures record
 *   initialization (identity_hash, placement_hash, flags=0, value)
 *   is visible to a reader that sees LIVE slot.
 *
 * record.flags LOAD:
 *   ACQUIRE — prevents flags load from reordering after value load
 *   or before slot validation.  Needed for flags-as-delete semantic.
 *
 * record.flags STORE (delete):
 *   RELEASE — ensures readers see flags=DELETED before slot clear.
 *   Paired with reader's ACQUIRE flags load.
 *
 * record.user_value LOAD:
 *   ACQUIRE — if value is a pointer, ensures pointed-to object is
 *   visible.  Paired with writer's RELEASE store.
 *
 * record.user_value STORE:
 *   RELEASE — ensures value writer's earlier stores are visible to
 *   reader who acquires this value.
 *
 * bucket.seq LOAD:
 *   ACQUIRE — provides acquire barrier for snapshot validity.
 *   Reader validates: s0 ACQUIRE, scan, s1 ACQUIRE; if s0==s1,
 *   writes between are invisible, confirming consistent snapshot.
 *
 * bucket.seq STORE (begin):
 *   RELEASE (old|BUSY) — paired with reader's ACQUIRE.  Reader
 *   sees BUSY and retries, ensuring no partial-write observation.
 *   ACQ_REL fence between BUSY store and slot writes.
 *
 * bucket.seq STORE (end):
 *   RELEASE (old+2) — pairs with reader's ACQUIRE.  Reader sees
 *   even stable seq, confirming writes between begin/end are visible.
 *
 * ctrl_tags LOAD:
 *   RELAXED — hint only; false positives are safe.
 *
 * ctrl_tags STORE:
 *   RELEASE — paired with reader's RELAXED load; RELEASE provides
 *   ordering so that slot writes are visible before ctrl update.
 *   (Reader uses RELAXED, so RELEASE is conservative.)
 *
 * remap_count / remap_filter:
 *   RELAXED — hints; stale positives are safe, stale negatives
 *   prevented by reading inside seq snapshot (which provides ACQUIRE).
 *
 * gen->state CAS (ACTIVE->FREEZING):
 *   ACQ_REL — ensures grow sees prior writer state and publishes
 *   freeze before continuing.
 *
 * gen->state STORE (OLD):
 *   RELEASE — ensures all reinserted entries are visible before
 *   readers see OLD and stop searching this gen.
 *
 * current_gen LOAD:
 *   ACQUIRE — ensures generation data is visible after loading gen.
 *
 * current_gen STORE:
 *   RELEASE — ensures gen is fully initialized before readers see it.
 *
 * epoch.global_epoch LOAD:
 *   ACQUIRE — prevents reader's epoch load from reordering before
 *   record access, ensuring epoch protects the right generation.
 *
 * epoch.global_epoch STORE:
 *   RELEASE — pairs with reader's ACQUIRE on epoch load.
 *
 * epoch.thread_epoch[] LOAD:
 *   ACQUIRE — ensures collector sees thread's most recent epoch,
 *   preventing premature reclamation.
 *
 * epoch.thread_epoch[] STORE (pin):
 *   RELEASE — paired with collector's ACQUIRE.  Ensures thread's
 *   epoch is visible to collector.
 *
 * epoch.thread_epoch[] STORE (unpin):
 *   RELEASE — ensures unpin (0) is visible; paired with collector's
 *   ACQUIRE that computes min_ep.
 *
 * shard lock:
 *   ACQUIRE on lock, RELEASE on unlock — standard mutex semantics.
 *
 * stash slot LOAD:
 *   ACQUIRE — pairs with RELEASE CAS or remove.  Ensures slot
 *   contents are visible before reader accesses record.
 *
 * stash slot CAS (insert):
 *   RELEASE on success — pairs with reader's ACQUIRE load.
 *
 * stash slot STORE (remove):
 *   RELEASE (EMPTY) — ensures record access completes before slot
 *   appears empty to reader; pairs with reader ACQUIRE.
 *
 * front cache entry fields:
 *   RELAXED — thread-local; no cross-thread ordering needed.
 *   table pointer and table_id checked before dereference.
 *
 * stats counters:
 *   RELAXED — diagnostic only; approximate under concurrency.
 * ────────────────────────────────────────────────────────────────── */

_Static_assert(sizeof(htc_bucket_meta_t) == 16,      "htc_bucket_meta_t must be 16 bytes");
_Static_assert(__alignof__(htc_bucket_meta_t) == 16,  "htc_bucket_meta_t must be 16B aligned");

/* ─── Atomic ordering test mode (Battery 22 Q1) ────────────── */
/* Define HTC_TEST_RELAX_ATOMICS to weaken all ACQUIRE/RELEASE to
 * RELAXED for memory-order mutation testing.  If the test suite
 * fails, the weakened orders are proven necessary.  If it passes,
 * the orders may be stronger than needed.
 *
 * Usage: cmake -DCMAKE_C_FLAGS="-DHTC_TEST_RELAX_ATOMICS"
 *
 * When defined, all HTC_MO_ACQUIRE / HTC_MO_RELEASE / HTC_MO_ACQ_REL
 * expand to __ATOMIC_RELAXED, allowing the test suite to verify that
 * each non-relaxed order is actually required for correctness.
 *
 * As of v1, the following functions use HTC_MO macros directly:
 *   htc_place_entry, htc_stash_insert, htc_stash_remove_at,
 *   htc_remove, htc_find, htc_bucket_scan_seq, htc_bucket_scan,
 *   htc_stash_find, htc_find_in_old_gen, htc_front_cache_lookup,
 *   htc_bucket_seq_begin, htc_bucket_seq_end
 * Additional conversions are in progress. */
#ifdef HTC_TEST_RELAX_ATOMICS
#define HTC_MO_ACQUIRE  __ATOMIC_RELAXED
#define HTC_MO_RELEASE  __ATOMIC_RELAXED
#define HTC_MO_ACQ_REL  __ATOMIC_RELAXED
#define HTC_MO_SEQ_CST  __ATOMIC_RELAXED
#else
/* Default: HTC_MO_* expand to the standard __ATOMIC_* constants.
 * These must NOT be self-referencing (use the literal __ATOMIC_* values). */
#define HTC_MO_ACQUIRE  __ATOMIC_ACQUIRE
#define HTC_MO_RELEASE  __ATOMIC_RELEASE
#define HTC_MO_ACQ_REL  __ATOMIC_ACQ_REL
#define HTC_MO_SEQ_CST  __ATOMIC_SEQ_CST
#endif

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

/* Arena record blocks — fixed-size pages so htc_arena_ptr is never
 * invalidated by concurrent arena growth (Battery 27 §15). */
#define HTC_ARENA_BLOCK_SHIFT  10
#define HTC_ARENA_BLOCK_SIZE   (1u << HTC_ARENA_BLOCK_SHIFT)
#define HTC_ARENA_BLOCK_MASK   (HTC_ARENA_BLOCK_SIZE - 1)

typedef struct htc_arena_block {
    struct htc_arena_block *next;
    htc_record_t            recs[HTC_ARENA_BLOCK_SIZE];
} htc_arena_block_t;
_Static_assert(sizeof(htc_arena_block_t) == 8 + HTC_ARENA_BLOCK_SIZE * sizeof(htc_record_t),
               "htc_arena_block_t layout changed");

typedef struct {
    _Atomic(htc_arena_block_t *) head;  /* first block in linked list */
    uint32_t                        count;
    uint32_t                       *free_idx;
    uint32_t                        free_count;
    uint32_t                        free_cap;
    void                           *allocator;
    htc_spinlock_t                  lock;
} htc_arena_t;
_Static_assert(sizeof(htc_arena_t) == 48, "htc_arena_t layout changed; review packing");

/* Stash: per-shard overflow, fixed at HTC_STASH_MAX entries.
 * Uses an embedded array so lock-free readers never see a dangling
 * pointer from concurrent reallocation. */
#define HTC_STASH_MIN      4
#define HTC_STASH_DEFAULT  8
#define HTC_STASH_MAX     32

typedef struct {
    _Atomic uint64_t slots[HTC_STASH_MAX];  /* 32 * 8 = 256 bytes */
    _Atomic uint32_t live_count;             /* 4 bytes: fast empty-stash skip in find */
    uint32_t          size;                  /* 4 bytes */
    void             *allocator;             /* 8 bytes */
    uint16_t          full_events;           /* 4 bytes (with empty_epochs) */
    uint16_t          empty_epochs;
} htc_stash_t;
_Static_assert(sizeof(htc_stash_t) == 280, "htc_stash_t layout changed; review packing");

/* ─── Per-shard counters MUST be defined before htc_shard_t ── */
#ifdef HTC_STATS
typedef struct {
    _Atomic uint64_t stash_insert;
    _Atomic uint64_t stash_full;
    _Atomic uint64_t stash_grow;
    _Atomic uint64_t bfs_attempts;
    _Atomic uint64_t bfs_success;
    _Atomic uint64_t bfs_no_path;
    _Atomic uint64_t bfs_abandoned_shards;
    _Atomic uint64_t bfs_depth_histogram[8];
    _Atomic uint64_t insert_oom;
    _Atomic uint64_t insert_pathological;
} htc_shard_stats_t;
#endif

/* ─── Shard: one per shard, covers a range of buckets ───── */
typedef struct __attribute__((aligned(64))) {
    htc_spinlock_t  lock;
    htc_stash_t     stash;
    htc_arena_t     arena;          /* per-shard record allocator */
    uint32_t        bucket_begin;
    uint32_t        bucket_count;
    _Atomic uint8_t migrated;
#ifdef HTC_STATS
    htc_shard_stats_t shard_stats;
#endif
} htc_shard_t;

_Static_assert(sizeof(htc_shard_t) % 64 == 0, "htc_shard_t size must be a multiple of 64 bytes");

/* ─── Retire node for deferred epoch-based free ──────────── */
typedef void (*htc_retire_fn)(void *ctx);

typedef struct htc_retire_node {
    uint32_t                    arena_idx;
    uint32_t                    shard_id;   /* which shard's arena owns this */
    uint64_t                    retire_epoch;
    htc_retire_fn               retire_cb;  /* NULL = arena_free, non-NULL = custom */
    void                       *retire_ctx; /* context for custom callback */
    struct htc_retire_node     *next;
} htc_retire_node_t;

/* ─── Epoch-based reclamation control ────────────────────── */
typedef struct htc_retire_gen {
    struct htc_table_gen *gen;
    uint64_t              retire_epoch;
    struct htc_retire_gen *next;
} htc_retire_gen_t;

typedef struct {
    _Atomic uint64_t                  global_epoch;       /* offset 0, hot: every pin/advance */
    uint8_t                           _pad_ep[56];        /* pad to 64 to isolate from thread_epoch */
    _Atomic uint64_t                  thread_epoch[HTC_EPOCH_MAX_THREADS];  /* offset 64, per-thread writes */
    _Atomic(htc_retire_node_t *)      retire_head;
    _Atomic(htc_retire_gen_t *)       retire_gen_head;
} htc_epoch_ctl_t;
_Static_assert(offsetof(htc_epoch_ctl_t, thread_epoch) == 64, "thread_epoch must start on its own cache line");

/* ─── Seq guard for optimistic bucket reads ──────────────── */
typedef struct {
    uint32_t old_seq;
} htc_seq_guard_t;

/* ─── Thread-local front cache (Phase 8, optional) ───────── */
/* Non-authoritative per-thread cache for repeated lookups.
 * Thread-local storage avoids cache-line bouncing between CPUs.
 * Each entry is tagged with the table pointer to prevent cross-table
 * reuse after a table is destroyed and a new one is created. */
#define HTC_FRONT_CACHE_ENTRIES 128

typedef struct {
    const void *table;      /* table instance pointer */
    uint64_t    table_id;   /* monotonically increasing incarnation — prevents address-reuse false hits */
    uint64_t    hash;
    uint32_t    arena_idx;
    uint32_t    generation;
    uint8_t     valid;
    uint8_t     _pad[3];
} htc_front_cache_entry_t;
_Static_assert(sizeof(htc_front_cache_entry_t) == 40, "front cache entry struct layout changed; review packing");

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
    uint64_t           seed;       /* placement seed for this generation */
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

/* ─── Debug invariants (debug builds only) ────────────────── */
#ifndef NDEBUG
uint32_t htc_debug_check_ctrl(const htc_table_t *t);
uint32_t htc_debug_recompute_remap(const htc_table_t *t);
uint64_t htc_debug_check_duplicate_hash(const htc_table_t *t);
size_t   htc_debug_live_count(const htc_table_t *t);
uint32_t htc_debug_verify_all(const htc_table_t *t);  /* composite: ctrl+remap+dup+placement */
bool     htc_debug_slow_find(const htc_table_t *t, uint64_t hash,
                              uint64_t *out_value);    /* independent of ctrl/remap/cache hints */
void     htc_debug_explain_hash(const htc_table_t *t, uint64_t hash);
void     htc_debug_explain_epoch(const htc_table_t *t);
void     htc_debug_explain_miss(const htc_table_t *t, uint64_t hash);
uint32_t htc_debug_check_transient(const htc_table_t *t);
uint64_t htc_debug_checksum(const htc_table_t *t);
htc_table_t *htc_debug_rebuild(const htc_table_t *t, uint32_t flags);
#endif

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
    _Atomic uint64_t old_gen_count;          /* generations in chain */
    _Atomic uint64_t old_gen_buckets_bytes;
    _Atomic uint64_t bfs_abandoned_shards;
    _Atomic uint64_t retired_record_count;
    _Atomic uint64_t reclaimed_record_count;
    _Atomic uint64_t insert_oom;              /* allocation failure during insert */
    _Atomic uint64_t insert_pathological;     /* max retries exceeded, adversarial */
    _Atomic uint64_t grow_reason_load;        /* grow triggered by load factor */
    _Atomic uint64_t grow_reason_stash_full;  /* grow triggered by stash overflow */
    _Atomic uint64_t grow_reseed_count;       /* number of times reseed was performed */
} htc_stats_t;

#define HTC_STAT_INC(s)   __atomic_fetch_add(&s, 1, __ATOMIC_RELAXED)
#define HTC_STAT_ADD(s,v) __atomic_fetch_add(&s, v, __ATOMIC_RELAXED)
#define HTC_STAT_INC_SHARD(t, sid, fld) \
    __atomic_fetch_add(&(t)->shards[sid].shard_stats.fld, 1, __ATOMIC_RELAXED)
#define HTC_STAT_ADD_SHARD(t, sid, fld, v) \
    __atomic_fetch_add(&(t)->shards[sid].shard_stats.fld, v, __ATOMIC_RELAXED)

/** Print all stats counters to stdout (requires HTC_STATS). */
void htc_stats_print(const htc_table_t *t);
void htc_stats_reset(htc_table_t *t);
#else
typedef int htc_stats_t;
typedef int htc_shard_stats_t;
#define HTC_STAT_INC(s)       ((void)0)
#define HTC_STAT_ADD(s,v)     ((void)(v))
#define HTC_STAT_INC_SHARD(t, sid, fld) ((void)0)
#define HTC_STAT_ADD_SHARD(t, sid, fld, v) ((void)(v))
static inline void htc_stats_print(const htc_table_t *t) { (void)t; }
static inline void htc_stats_reset(htc_table_t *t) { (void)t; }
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

    /* Current placement seed for this table instance.
     * Updated on pathology-triggered grow. */
    uint64_t           seed;

    /* Monotonically increasing incarnation ID — prevents front-cache
     * false hits when a freed table's address is reused by malloc. */
    uint64_t           table_id;

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

static inline uint64_t htc_mix64(uint64_t x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static inline uint64_t htc_placement_hash(uint64_t id_hash, uint64_t seed) {
    return htc_mix64(id_hash ^ seed);
}

static inline uint32_t htc_alt_bucket(uint32_t b1, uint64_t h, uint16_t tag) {
    return b1 ^ htc_mix32((uint32_t)(h >> 32) ^ ((uint32_t)tag << 16));
}

static inline uint64_t htc_match8(uint64_t ctrl, uint8_t p) {
#if defined(__aarch64__) && !defined(__CBMC__)
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

static inline void htc_cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __asm__ volatile("pause");
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

static inline void htc_ctrl_set(htc_bucket_meta_t *m, unsigned i, uint8_t p) {
    uint64_t set = ((uint64_t)p) << (i * 8);
    uint64_t clr = ~(0xFFULL << (i * 8));
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    uint64_t desired = (old & clr) | set;
    while (!__atomic_compare_exchange_n(&m->ctrl_tags, &old, desired,
                                        false, HTC_MO_RELEASE, __ATOMIC_RELAXED)) {
        desired = (old & clr) | set;
        htc_cpu_relax();
    }
}

static inline void htc_ctrl_clear(htc_bucket_meta_t *m, unsigned i) {
    uint64_t clr = ~(0xFFULL << (i * 8));
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    uint64_t desired = old & clr;
    while (!__atomic_compare_exchange_n(&m->ctrl_tags, &old, desired,
                                        false, HTC_MO_RELEASE, __ATOMIC_RELAXED)) {
        desired = old & clr;
        htc_cpu_relax();
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
    while (__atomic_exchange_n(&lk->flag, 1, HTC_MO_ACQUIRE))
        htc_cpu_relax();
}

static inline void htc_spin_unlock(htc_spinlock_t *lk) {
    __atomic_store_n(&lk->flag, 0, HTC_MO_RELEASE);
}

/* ============================================================================
 * Bucket sequence counter helpers (Phase 2)
 * Writer marks bucket BUSY before mutation, readers validate via seq.
 * ============================================================================ */

static inline htc_seq_guard_t htc_bucket_seq_begin(htc_bucket_meta_t *m) {
    uint32_t old = __atomic_load_n(&m->seq, __ATOMIC_RELAXED);
    while (old & HTC_SEQ_BUSY) {
        htc_cpu_relax();
        old = __atomic_load_n(&m->seq, HTC_MO_ACQUIRE);
    }
    __atomic_store_n(&m->seq, old | HTC_SEQ_BUSY, HTC_MO_RELEASE);
    __atomic_thread_fence(HTC_MO_ACQ_REL);
    return (htc_seq_guard_t){ .old_seq = old };
}

static inline void htc_bucket_seq_end(htc_bucket_meta_t *m, htc_seq_guard_t g) {
    __atomic_thread_fence(HTC_MO_RELEASE);
    uint32_t new_seq = (g.old_seq + 2u) & ~HTC_SEQ_BUSY;
#ifdef HTC_TEST_SMALL_SEQ_BITS
    new_seq &= (1u << HTC_TEST_SMALL_SEQ_BITS) - 1;
#endif
    __atomic_store_n(&m->seq, new_seq, HTC_MO_RELEASE);
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
void    htc_epoch_retire_custom(htc_epoch_ctl_t *ep, htc_retire_fn cb, void *ctx);

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

uint32_t      htc_arena_alloc(htc_arena_t *a, uint64_t identity_hash,
                               uint64_t placement_hash, uint64_t value);
void          htc_arena_free(htc_arena_t *a, uint32_t idx);
htc_record_t *htc_arena_ptr(htc_arena_t *a, uint32_t idx);

/* ─── Front cache inline operations (Phase 8) ───────────── */
static inline bool htc_front_cache_lookup(const htc_front_cache_t *c,
                                           const void *table,
                                           uint64_t table_id,
                                           htc_arena_t *a, uint64_t hash,
                                           uint64_t *out_value) {
    for (uint32_t i = 0; i < HTC_FRONT_CACHE_ENTRIES; i++) {
        if (c->entries[i].valid
            && c->entries[i].table == table
            && c->entries[i].table_id == table_id
            && c->entries[i].hash == hash) {
            if (c->entries[i].arena_idx >= a->count) continue;
            htc_record_t *r = htc_arena_ptr(a, c->entries[i].arena_idx);
            if (r->generation == c->entries[i].generation
                && r->identity_hash == hash
                && __atomic_load_n(&r->flags, HTC_MO_ACQUIRE) == 0) {
                *out_value = __atomic_load_n(&r->user_value, HTC_MO_ACQUIRE);
                /* Double-check flags after value load: a concurrent delete
                 * may have set flags between the first flags check and the
                 * value load. (Battery 12 Q5) */
                if (__atomic_load_n(&r->flags, HTC_MO_ACQUIRE) != 0)
                    continue;
                return true;
            }
        }
    }
    return false;
}
static inline void htc_front_cache_insert(htc_front_cache_t *c,
                                           const void *table,
                                           uint64_t table_id,
                                           uint64_t hash, uint32_t arena_idx,
                                           uint32_t generation) {
    uint32_t p = c->pos++ % HTC_FRONT_CACHE_ENTRIES;
    c->entries[p].table     = table;
    c->entries[p].table_id  = table_id;
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

/* ─── Deterministic scheduler hooks for adversarial testing ── */
/* Empty by default; define HTC_SCHED_HOOK(id) externally to
 * intercept execution at known race-prone points and control
 * thread interleaving in tests.  No-ops in release builds
 * (no fences, no branches).  Hook callbacks must not reenter
 * the table — non-reentrant.  (Battery 8 Q11, Battery 13 Q28)
 *
 * Hook-to-bug mapping:
 *   1  after load current gen          lost_insert_during_grow
 *   2  after acquire shard locks       general writer ordering
 *   3  after gen/state revalidation    gen-frozen writer recovery
 *   4  after seq_begin                 bucket visibility timing
 *   5  before slot publish             secondary insert remap race
 *   7  after BFS path found            BFS commit vs concurrent grow
 *   8  after path locks acquired       path commit serialization
 *   9  after grow freezes old          freeze vs concurrent insert
 *  10  after grow locks all shards     grow vs concurrent writers
 *  12  before publish new gen          lost_insert_during_grow
 *  13  after publish new gen           old reader vs new gen
 *  14  before epoch retire free        retire vs concurrent reader
 *  15  before freed retired generation gen-retire vs pinned reader
 */
#ifndef HTC_SCHED_HOOK
#define HTC_SCHED_HOOK(id) ((void)(id))
#endif

/* ─── Linearization witness logging (Battery 12 Q2) ──────── */
/* Optional per-thread ring buffer that records operation linearization
 * events for debug replay.  Define HTC_WITNESS and HTC_WITNESS_ENTRIES. */

#ifdef HTC_WITNESS
#define HTC_WITNESS_ENTRIES 4096

typedef enum {
    HTC_WIT_LOAD_GEN      = 1,
    HTC_WIT_LOCK_SHARD    = 2,
    HTC_WIT_GEN_VALID     = 3,
    HTC_WIT_SEQ_BEGIN     = 4,
    HTC_WIT_SLOT_CAS      = 5,
    HTC_WIT_SLOT_WRITE    = 6,
    HTC_WIT_REMAP_INC     = 7,
    HTC_WIT_REMAP_DEC     = 8,
    HTC_WIT_BFS_FOUND     = 9,
    HTC_WIT_PATH_LOCK     = 10,
    HTC_WIT_GROW_FREEZE   = 11,
    HTC_WIT_GROW_LOCK_ALL = 12,
    HTC_WIT_GROW_PUBLISH  = 13,
    HTC_WIT_EPOCH_RETIRE  = 14,
    HTC_WIT_GEN_FREE      = 15,
    HTC_WIT_RETURN_OK     = 64,
    HTC_WIT_RETURN_ERR    = 65,
    HTC_WIT_INSERT_CAS    = 66,
    HTC_WIT_REMOVE_FLAGS  = 67,
    HTC_WIT_FIND_HIT      = 68,
    HTC_WIT_FIND_NEG      = 69,
    HTC_WIT_OP_START      = 70,
    HTC_WIT_OP_FINISH     = 71,
} htc_witness_event_t;

typedef struct {
    uint64_t   clock;
    uint64_t   hash;
    uint32_t   arena_idx;
    uint16_t   bucket;
    uint8_t    slot;
    uint8_t    event;
    uint8_t    result;
    uint16_t   gen_id;
    uint16_t   _pad;
} htc_witness_t;

typedef struct {
    htc_witness_t entries[HTC_WITNESS_ENTRIES];
    uint32_t      head;
    uint32_t      count;
    uint32_t      clock;
} htc_witness_log_t;

extern _Thread_local htc_witness_log_t htc_witness_log;

static inline void htc_witness_record(uint8_t event, uint64_t hash,
                                       uint32_t arena_idx, uint16_t bucket,
                                       uint8_t slot, uint8_t result,
                                       uint16_t gen_id) {
    htc_witness_log_t *log = &htc_witness_log;
    uint32_t idx = (log->head + log->count) % HTC_WITNESS_ENTRIES;
    if (log->count < HTC_WITNESS_ENTRIES) log->count++;
    else log->head = (log->head + 1) % HTC_WITNESS_ENTRIES;
    log->entries[idx].clock     = log->clock++;
    log->entries[idx].hash      = hash;
    log->entries[idx].arena_idx = arena_idx;
    log->entries[idx].bucket    = bucket;
    log->entries[idx].slot      = slot;
    log->entries[idx].event     = event;
    log->entries[idx].result    = result;
    log->entries[idx].gen_id    = gen_id;
}
#else
#define htc_witness_record(e, h, a, b, s, r, g) ((void)0)
#endif

htc_error_t htc_grow(htc_table_t *t, bool reseed);

/* Scoped find: identical to htc_find but leaves epoch pinned on HTC_OK
 * return.  Caller must call htc_epoch_unpin(t->epoch) when done with the
 * returned value.  On non-OK returns, the epoch is already unpinned. */
htc_error_t htc_find_scoped(const htc_table_t *t, uint64_t hash, uint64_t *out_value);

int  htc_bucket_scan(htc_bucket_t *b, htc_bucket_meta_t *m,
                      htc_arena_t *a, uint64_t identity_hash,
                      uint16_t tag, htc_scan_mode_t mode);
int  htc_stash_insert(htc_stash_t *s, uint64_t slot_word);
int  htc_stash_find(const htc_stash_t *s, htc_arena_t *a,
                     uint64_t identity_hash, uint16_t tag);
void htc_stash_remove_at(htc_stash_t *s, unsigned idx);
void htc_stash_maintain(htc_stash_t *s);

/* Epoch operations (Phase 2) */
void htc_epoch_retire(htc_epoch_ctl_t *ep, htc_arena_t *a, uint32_t arena_idx);
bool htc_epoch_retire_gen(htc_epoch_ctl_t *ep, struct htc_table_gen *gen);
uint64_t htc_epoch_collect(htc_epoch_ctl_t *ep, htc_arena_t *a);
void htc_epoch_advance(htc_epoch_ctl_t *ep);

/* Thread detach: release epoch slot and clear front cache. (Battery 8 Q3) */
void htc_thread_detach(void);

/* Migration (Phase 6) */
bool htc_resize_start(htc_table_t *t, uint32_t new_num_buckets);
void htc_migrate_chunk(htc_table_t *t, uint32_t chunk_id);
void htc_ensure_chunk_migrated(htc_table_t *t, uint32_t bucket_id);
void htc_resize_finish(htc_table_t *t);
bool htc_find_in_old_gen(const htc_table_gen_t *g, htc_arena_t *arena,
                          uint64_t identity_hash, uint16_t tag,
                          uint64_t *out_value);

#ifdef __cplusplus
}
#endif

#endif
