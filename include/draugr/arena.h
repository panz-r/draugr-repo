#ifndef DRAUGR_ARENA_H
#define DRAUGR_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Branch prediction hints ──────────────────────────────────── */
#if defined(__GNUC__) || defined(__clang__)
#define ARENA_LIKELY(x)   __builtin_expect(!!(x), 1)
#define ARENA_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define ARENA_LIKELY(x)   (x)
#define ARENA_UNLIKELY(x) (x)
#endif

/* ─── Cache line size (build-time platform check) ─────────────── */
#if defined(__x86_64__) || defined(_M_X64)
#define ARENA_CACHE_LINE 64
#elif defined(__aarch64__) || defined(_M_ARM64)
#define ARENA_CACHE_LINE 64
#else
#define ARENA_CACHE_LINE 64
#endif

/* ─── Frequency tiers ──────────────────────────────────────────── */
enum arena_freq {
    ARENA_FREQ_WARM = 0,
    ARENA_FREQ_COLD = 1,
};

#define ARENA_FREQ_COUNT 2

/* ─── Slab allocator — warm tier [2][3] ───────────────────────── */
#define ARENA_SLAB_SIZES  8
#define ARENA_SLAB_SLOTS  256

struct arena_slab {
    uint64_t bitmap[4];
    _Atomic unsigned int used_count;
    _Atomic uint32_t free_top;
    uint32_t free_stack[ARENA_SLAB_SLOTS];
    uint8_t size_class;
    uint8_t freq;
    uint16_t node_id;
    size_t mmap_size;
    uint8_t data[];
};

struct arena_slab_set {
    struct arena_slab *slabs[8];
    _Atomic int slab_count;
    _Atomic unsigned int total_slots;
    _Atomic unsigned int free_slots;
};

/* ─── Thread cache ────────────────────────────────────────────── */
#define ARENA_TCACHE_BINS    ARENA_SLAB_SIZES
#define ARENA_TCACHE_DEPTH   32

struct arena_tcache_bin {
	void *entries[ARENA_TCACHE_DEPTH];
	unsigned int count;
	pthread_mutex_t lock;
} __attribute__((aligned(ARENA_CACHE_LINE)));

struct arena_tcache {
    struct arena_tcache_bin bins[ARENA_TCACHE_BINS];
};

/* ─── Segment — cold tier ─────────────────────────────────────── */
#define ARENA_SEG_SHIFT 16
#define ARENA_SEG_SIZE  (1U << ARENA_SEG_SHIFT)
#define ARENA_COLD_SEGS_PER_CLASS 4

struct arena_seg_hdr {
	_Atomic uint64_t control;      /* bits 0-47: reserved, bits 48-63: used slot count */
	_Atomic uint64_t free_bitmap[2];
	_Atomic unsigned int epoch;
	uint8_t freq;
	uint8_t size_class;
	uint8_t node_id;
	_Atomic uint8_t flags;         /* bit 0: evacuating */
	struct arena_seg_hdr *next;
};

struct arena_segment {
    struct arena_seg_hdr hdr;
    uint8_t data[];
};

/* ─── Epoch-based GC control [6][7] ───────────────────────────── */
struct arena_epoch_ctl {
    _Atomic unsigned int global_epoch;
    _Atomic unsigned int evacuating;
    _Atomic unsigned int completed;
    uint32_t compact_trigger;
    size_t batch_size;
};

/* ─── Hardware prefetch control [4] ────────────────────────────── */
struct arena_prefetch_ctl {
    bool enabled;
    uint8_t prefetch_distance;
};

/* ─── Adaptive tuner [9] ───────────────────────────────────────── */
struct arena_tuner {
    uint32_t hist[64];
    uint32_t sample_count;
    uint8_t freq_thresholds[2];
    uint8_t last_tune_epoch;
};

/* ─── Deferred free list (mimalloc-style batched free) ────────── */
struct arena_deferred_entry {
    void *ptr;
    size_t size;
};

#define ARENA_DEFERRED_BATCH 64

struct arena_deferred {
    struct arena_deferred_entry entries[ARENA_DEFERRED_BATCH];
    unsigned int count;
};

/* ─── Main arena structure ───────────────────────────────────── */
struct arena {
    struct {
        union {
            struct arena_slab_set *slabs;
            struct arena_segment **segs;
        } u;
        size_t count;
        size_t total_bytes;
        size_t used_bytes;
        size_t waste_bytes;
    } arenas[ARENA_FREQ_COUNT][ARENA_SLAB_SIZES];

    struct arena_tcache *thread_caches;
    size_t tcache_count;

    struct arena_deferred *deferred;

    struct arena_epoch_ctl epoch_ctl;
    struct arena_prefetch_ctl prefetch_ctl;
    struct arena_tuner tuner;

    struct {
        uint8_t warm_threshold;
        uint8_t enable_prefetch;
        uint8_t enable_thread_cache;
        uint8_t enable_adaptive_tuning;
    } config;

    size_t total_memory;
    size_t write_amplification;
    size_t compactions;

    _Atomic size_t alloc_count;
    _Atomic size_t alloc_bytes;
};

/* ─── Public API ──────────────────────────────────────────────── */

/*
 * Thread safety: arena_create, arena_alloc, arena_free, and arena_destroy
 * are individually safe for concurrent access. arena_clear and arena_compact
 * require external synchronization — no concurrent alloc/free may be in
 * progress when these are called.
 */
struct arena *arena_create(size_t initial_capacity);
void arena_destroy(struct arena *a);

void *arena_alloc(struct arena *a, size_t size);
void *arena_realloc(struct arena *a, void *ptr, size_t old_size, size_t new_size);
void arena_free(struct arena *a, void *ptr, size_t size);

void arena_free_deferred(struct arena *a, void *ptr, size_t size);
void arena_flush_deferred(struct arena *a);

void arena_clear(struct arena *a);
void arena_compact(struct arena *a, int freq);

int arena_get_stats(const struct arena *a, size_t *out);
size_t arena_size(const struct arena *a);
size_t arena_capacity(const struct arena *a);
bool arena_contains(const struct arena *a, const void *ptr);

/* ─── Stats helpers ───────────────────────────────────────────── */
enum {
    ARENA_STAT_TOTAL        = 0,
    ARENA_STAT_USED         = 1,
    ARENA_STAT_WASTE        = 2,
    ARENA_STAT_COMPACTIONS  = 3,
    ARENA_STAT_WRITE_AMP    = 4,
    ARENA_STAT_COUNT        = 5,
};

#ifdef __cplusplus
}
#endif

#endif
