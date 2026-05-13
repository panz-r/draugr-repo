#ifndef DRAUGR_ARENA_H
#define DRAUGR_ARENA_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Frequency tiers ──────────────────────────────────────────── */
enum arena_freq {
    ARENA_FREQ_HOT  = 0,
    ARENA_FREQ_WARM = 1,
    ARENA_FREQ_COLD = 2,
};

#define ARENA_FREQ_COUNT 3

/* ─── Adaptive size classes (tuned at runtime) [9] ─────────────── */
#define ARENA_MIN_CLASSES  8
#define ARENA_MAX_CLASSES  24

/* ─── NUMA configuration ──────────────────────────────────────── */
#define ARENA_MAX_NUMA_NODES 16

/* ─── Write buffer for hot tier [1] ───────────────────────────── */
#define ARENA_WBUF_SLOTS       64
#define ARENA_WBUF_FLUSH_THRESH 48

struct arena_wbuf_slot {
    uint32_t key_len;
    uint32_t val_len;
    uint8_t key[64];
    uint8_t val[256];
};

struct arena_write_buffer {
    struct arena_wbuf_slot slots[ARENA_WBUF_SLOTS];
    _Atomic unsigned int count;
    _Atomic unsigned int epoch;
    uint8_t freq;
    uint8_t size_class;
    bool sealed;
};

/* ─── Slab allocator for warm tier [2][3] ─────────────────────── */
#define ARENA_SLAB_SIZES  4
#define ARENA_SLAB_SLOTS  256
#define ARENA_SLAB_ORDER  12

struct arena_slab {
    uint64_t bitmap[4];
    _Atomic unsigned int used_count;
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

/* ─── NUMA-aware thread cache [8] ─────────────────────────────── */
#define ARENA_TCACHE_BINS    4
#define ARENA_TCACHE_DEPTH   32

struct arena_tcache_bin {
	void *entries[ARENA_TCACHE_DEPTH];
	_Atomic unsigned int count;
	pthread_mutex_t lock;
};

struct arena_tcache {
    struct arena_tcache_bin bins[ARENA_TCACHE_BINS];
    uint8_t node_id;
    uint8_t padding[7];
};

/* ─── Segment for cold tier ────────────────────────────────────── */
#define ARENA_SEG_SHIFT 16
#define ARENA_SEG_SIZE  (1U << ARENA_SEG_SHIFT)
#define ARENA_COLD_SEGS_PER_CLASS 4

struct arena_seg_hdr {
	_Atomic uint64_t control;
	_Atomic uint64_t free_bitmap[2];
	_Atomic unsigned int epoch;
	uint8_t freq;
	uint8_t size_class;
	uint8_t node_id;
	uint8_t flags;
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
    uint8_t numa_aware;
};

/* ─── Adaptive tuner [9] ───────────────────────────────────────── */
struct arena_tuner {
    uint32_t hist[64];
    uint32_t sample_count;
    uint8_t freq_thresholds[3];
    uint8_t last_tune_epoch;
};

/* ─── Ring entry header for hot tier ──────────────────────────── */
struct arena_ring_entry {
    uint32_t total_len;
    uint16_t key_len;
    uint16_t val_len;
};

/* ─── Main arena structure ───────────────────────────────────── */
struct arena {
    struct {
        union {
            struct {
                struct arena_write_buffer *wbuf;
                uint8_t *ring_base;
                size_t ring_cap;
                _Atomic size_t ring_pos;
            } hot;
            struct arena_slab_set *slabs;
            struct arena_segment **segs;
        } u;
        size_t count;
        size_t total_bytes;
        size_t used_bytes;
        size_t waste_bytes;
    } arenas[ARENA_FREQ_COUNT][ARENA_MAX_CLASSES];

    struct arena_tcache *thread_caches;
    size_t tcache_count;

    struct arena_epoch_ctl epoch_ctl;
    struct arena_prefetch_ctl prefetch_ctl;
    struct arena_tuner tuner;

    struct {
        uint8_t hot_threshold;
        uint8_t warm_threshold;
        uint8_t enable_numa;
        uint8_t enable_prefetch;
        uint8_t enable_thread_cache;
        uint8_t enable_adaptive_tuning;
        uint32_t numa_node_count;
    } config;

    size_t total_memory;
    size_t write_amplification;
    size_t compactions;
    size_t wbuf_flushes;

    _Atomic size_t alloc_count;
    _Atomic size_t alloc_bytes;

    int numa_node;
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
    ARENA_STAT_WBUF_FLUSHES = 4,
    ARENA_STAT_WRITE_AMP    = 5,
    ARENA_STAT_COUNT        = 6,
};

#ifdef __cplusplus
}
#endif

#endif
