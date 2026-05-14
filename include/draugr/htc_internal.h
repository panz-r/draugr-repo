#ifndef DRAUGR_HTC_INTERNAL_H
#define DRAUGR_HTC_INTERNAL_H

#include "draugr/htc.h"
#include "draugr/alloc.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HTC_BUCKET_SLOTS         8
#define HTC_TAG_ZERO_REPLACEMENT 1
#define HTC_STASH_GROW           4

#define HTC_STATE_EMPTY      0U
#define HTC_STATE_LIVE       1U
#define HTC_STATE_TENTATIVE  2U
#define HTC_STATE_MOVING     3U
#define HTC_STATE_DELETED    4U

#define HTC_SEQ_BUSY 1U
#define HTC_REMAP_SATURATED 0xFFU

#define HTC_SLOT_INDEX_MASK  0x000000FFFFFFFFFFULL
#define HTC_SLOT_INDEX_SHIFT 0
#define HTC_SLOT_TAG_SHIFT   40
#define HTC_SLOT_STATE_SHIFT 56
#define HTC_SLOT_SEC_MASK    0x0800000000000000ULL

typedef struct {
    uint64_t full_hash;
    uint64_t user_value;
} htc_record_t;

typedef struct __attribute__((aligned(64))) {
    _Atomic uint64_t slot[HTC_BUCKET_SLOTS];
} htc_bucket_t;

typedef struct __attribute__((aligned(16))) {
    _Atomic uint64_t ctrl_tags;
    _Atomic uint32_t seq;
    _Atomic uint16_t remap_filter;
    _Atomic uint8_t  remap_count;
    uint8_t           flags;
} htc_bucket_meta_t;

typedef struct {
    htc_record_t *records;
    uint32_t      count;
    uint32_t      capacity;
    uint32_t     *free_idx;
    uint32_t      free_count;
    uint32_t      free_cap;
    void         *allocator;
} htc_arena_t;

typedef struct {
    _Atomic uint64_t *slots;
    uint32_t          capacity;
    uint32_t          size;
    void             *allocator;
} htc_stash_t;

struct htc_table {
    htc_bucket_t      *buckets;
    htc_bucket_meta_t *meta;
    htc_arena_t       *arena;
    htc_stash_t        stash;
    void    *allocator;
    uint32_t bucket_mask;
    uint32_t num_buckets;
    double   max_load_factor;
    size_t   size;
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

static inline uint32_t htc_shard_of(uint32_t bucket_id, uint32_t shard_count) {
    return bucket_id % shard_count;
}

static inline uint64_t htc_match8(uint64_t ctrl, uint8_t p) {
    uint64_t rep = 0x0101010101010101ULL * p;
    uint64_t x   = ctrl ^ rep;
    uint64_t z   = (x - 0x0101010101010101ULL) & ~x & 0x8080808080808080ULL;
    return z;
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
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    uint64_t clr = ~(0xFFULL << (i * 8));
    uint64_t set = ((uint64_t)p) << (i * 8);
    __atomic_store_n(&m->ctrl_tags, (old & clr) | set, __ATOMIC_RELAXED);
}

static inline void htc_ctrl_clear(htc_bucket_meta_t *m, unsigned i) {
    uint64_t old = __atomic_load_n(&m->ctrl_tags, __ATOMIC_RELAXED);
    __atomic_store_n(&m->ctrl_tags, old & ~(0xFFULL << (i * 8)), __ATOMIC_RELAXED);
}

static inline void htc_remap_inc(htc_bucket_meta_t *m, uint16_t tag) {
    __atomic_fetch_or(&m->remap_filter,
                      (uint16_t)(1U << (tag & 0xF)), __ATOMIC_RELAXED);
    uint8_t c = __atomic_load_n(&m->remap_count, __ATOMIC_RELAXED);
    if (c < HTC_REMAP_SATURATED - 1)
        __atomic_store_n(&m->remap_count, c + 1, __ATOMIC_RELAXED);
    else if (c < HTC_REMAP_SATURATED)
        __atomic_store_n(&m->remap_count, HTC_REMAP_SATURATED, __ATOMIC_RELAXED);
}

static inline void htc_remap_dec(htc_bucket_meta_t *m) {
    uint8_t c = __atomic_load_n(&m->remap_count, __ATOMIC_RELAXED);
    if (c == 0 || c == HTC_REMAP_SATURATED) return;
    __atomic_store_n(&m->remap_count, c - 1, __ATOMIC_RELAXED);
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

uint32_t      htc_arena_alloc(htc_arena_t *a, uint64_t hash, uint64_t value);
void          htc_arena_free(htc_arena_t *a, uint32_t idx);
htc_record_t *htc_arena_ptr(htc_arena_t *a, uint32_t idx);
bool htc_grow(htc_table_t *t);
int  htc_bucket_scan(htc_bucket_t *b, htc_bucket_meta_t *m,
                      htc_arena_t *a, uint64_t h, htc_scan_mode_t mode);
int  htc_stash_insert(htc_stash_t *s, uint64_t slot_word);
int  htc_stash_find(const htc_stash_t *s, htc_arena_t *a, uint64_t h);
void htc_stash_remove_at(htc_stash_t *s, unsigned idx);

#ifdef __cplusplus
}
#endif

#endif
