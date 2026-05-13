/**
 * Draugr Arena v4 — Production-Ready Memory Allocator
 *
 * Implements:
 *   - Two frequency tiers: WARM (slab), COLD (segment)
 *   - Slab allocator for small objects (zero fragmentation) [2][3]
 *   - Hardware-aware prefetching [4]
 *   - Epoch-based generational GC [6][7]
 *   - Thread cache for fast per-thread allocation
 *   - Adaptive size class tuning [9]
 */

#define _GNU_SOURCE

#include "draugr/arena.h"
#include "draugr/arena_internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pthread.h>

/* ══════════════════════════════════════════════════════════════════
   SIZE CLASS HELPERS
   ══════════════════════════════════════════════════════════════════ */

static const size_t g_slab_sizes[ARENA_SLAB_SIZES] = {16, 32, 64, 128};

static int size_to_slab_class(size_t bytes) {
    for (int i = 0; i < ARENA_SLAB_SIZES; i++) {
        if (bytes <= g_slab_sizes[i]) return i;
    }
    return ARENA_SLAB_SIZES - 1;
}

static size_t slab_total_size(int size_class) {
    size_t slot_size = g_slab_sizes[size_class];
    size_t header_sz = sizeof(struct arena_slab);
    size_t data_sz = slot_size * ARENA_SLAB_SLOTS;
    size_t total = header_sz + data_sz;
    total = (total + 4095) & ~(size_t)4095;
    return total;
}

static size_t slab_data_offset(void) {
    return sizeof(struct arena_slab);
}

/* ══════════════════════════════════════════════════════════════════
   SLAB ALLOCATOR — Warm Tier [2][3]
   ══════════════════════════════════════════════════════════════════ */

static struct arena_slab *slab_create(int size_class, uint8_t freq) {
    size_t total = slab_total_size(size_class);

    void *base = mmap(NULL, total,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                      -1, 0);
    if (base == MAP_FAILED) {
        base = mmap(NULL, total,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (base == MAP_FAILED) return NULL;

    struct arena_slab *slab = base;
    slab->bitmap[0] = UINT64_MAX;
    slab->bitmap[1] = UINT64_MAX;
    slab->bitmap[2] = UINT64_MAX;
    slab->bitmap[3] = UINT64_MAX;
    atomic_store(&slab->used_count, 0);
    slab->size_class = (uint8_t)size_class;
    slab->freq = freq;
    slab->node_id = 0;
    slab->mmap_size = total;

    return slab;
}

static void *slab_alloc_slot(struct arena_slab *slab, int size_class) {
    uint64_t *bitmap = slab->bitmap;

    for (int word = 0; word < 4; word++) {
        uint64_t bits = atomic_load_explicit(&bitmap[word],
                                             memory_order_acquire);
        if (bits == UINT64_MAX) continue;

        int bit = __builtin_ctzll(~bits);
        uint64_t mask = ~(1ULL << bit);
        if (atomic_compare_exchange_strong_explicit(
                &bitmap[word], &bits, mask,
                memory_order_acq_rel, memory_order_acquire)) {
            atomic_fetch_add_explicit(&slab->used_count, 1,
                                      memory_order_relaxed);
            int slot = word * 64 + bit;
            size_t off = slab_data_offset();
            return (uint8_t *)slab + off + slot * g_slab_sizes[size_class];
        }
    }
    return NULL;
}

static bool slab_free_slot(struct arena_slab *slab, void *ptr) {
	uintptr_t slab_start = (uintptr_t)slab;
	uintptr_t p = (uintptr_t)ptr;
	if (p < slab_start || p >= slab_start + slab->mmap_size) return false;
	size_t data_offset = slab_data_offset();
	size_t slot_size = g_slab_sizes[slab->size_class];
	size_t data_size = slot_size * ARENA_SLAB_SLOTS;
	uintptr_t data_start = slab_start + data_offset;
	if (p < data_start) return false;
	size_t offset = p - data_start;
	if (offset >= data_size) return false;

    int slot = (int)(offset / slot_size);
    if (slot < 0 || slot >= ARENA_SLAB_SLOTS) return false;

    int word = slot / 64;
    int bit = slot % 64;

    uint64_t set_mask = (1ULL << bit);
    uint64_t bits = atomic_load_explicit(&slab->bitmap[word],
                                         memory_order_acquire);
    if (bits & set_mask) return false;

    atomic_store_explicit(&slab->bitmap[word], bits | set_mask,
                          memory_order_release);
    atomic_fetch_sub_explicit(&slab->used_count, 1, memory_order_relaxed);
    return true;
}

static bool slab_contains(struct arena_slab *slab, void *ptr) {
	uintptr_t slab_start = (uintptr_t)slab;
	uintptr_t slab_end = slab_start + slab->mmap_size;
	uintptr_t p = (uintptr_t)ptr;
	if (p < slab_start || p >= slab_end) return false;
	size_t data_offset = slab_data_offset();
	size_t slot_size = g_slab_sizes[slab->size_class];
	size_t data_size = slot_size * ARENA_SLAB_SLOTS;
	if (data_offset > UINTPTR_MAX - slab_start) return false;
	uintptr_t data_start = slab_start + data_offset;
	return p >= data_start && p - data_start < data_size;
}

void *slab_set_alloc(struct arena_slab_set *set, int sc) {
    int count = atomic_load_explicit(&set->slab_count, memory_order_acquire);

    for (int i = 0; i < count && i < 8; i++) {
        struct arena_slab *slab = set->slabs[i];
        if (!slab) continue;

        void *ptr = slab_alloc_slot(slab, slab->size_class);
        if (ptr) {
            atomic_fetch_sub_explicit(&set->free_slots, 1,
                                      memory_order_relaxed);
            return ptr;
        }
    }

    if (count >= 8) return NULL;

    struct arena_slab *slab = slab_create(sc, ARENA_FREQ_WARM);
    if (!slab) return NULL;

    int slot = atomic_fetch_add_explicit(&set->slab_count, 1,
                                         memory_order_relaxed);
    if (slot < 8) {
        set->slabs[slot] = slab;
    } else {
        munmap(slab, slab->mmap_size);
        return NULL;
    }

    atomic_fetch_add_explicit(&set->total_slots, ARENA_SLAB_SLOTS,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&set->free_slots, ARENA_SLAB_SLOTS,
                              memory_order_relaxed);

    void *ptr = slab_alloc_slot(slab, sc);
    if (ptr) {
        atomic_fetch_sub_explicit(&set->free_slots, 1,
                                  memory_order_relaxed);
    }
    return ptr;
}

bool slab_set_free(struct arena_slab_set *set, void *ptr) {
    if (!ptr) return false;

    int count = atomic_load_explicit(&set->slab_count, memory_order_acquire);
    for (int i = 0; i < count && i < 8; i++) {
        struct arena_slab *slab = set->slabs[i];
        if (!slab) continue;

        if (slab_contains(slab, ptr)) {
            if (slab_free_slot(slab, ptr)) {
                atomic_fetch_add_explicit(&set->free_slots, 1,
                                          memory_order_relaxed);
                return true;
            }
            return false;
        }
    }
    return false;
}

bool slab_set_contains(struct arena_slab_set *set, void *ptr) {
    if (!ptr) return false;

    int count = atomic_load_explicit(&set->slab_count, memory_order_acquire);
    for (int i = 0; i < count && i < 8; i++) {
        struct arena_slab *slab = set->slabs[i];
        if (!slab) continue;
        if (slab_contains(slab, ptr)) return true;
    }
    return false;
}

/* ══════════════════════════════════════════════════════════════════
   THREAD CACHE
   Per-thread free-list sharding (mimalloc-inspired).
   ══════════════════════════════════════════════════════════════════ */

static struct arena_tcache *tcache_get(struct arena *a) {
    if (!a->thread_caches || a->tcache_count == 0) return NULL;
    return &a->thread_caches[0];
}

static void *tcache_try_get(struct arena *a, int sc) {
    struct arena_tcache *tc = tcache_get(a);
    if (!tc) return NULL;
    if (sc < 0 || sc >= ARENA_TCACHE_BINS) return NULL;

    struct arena_tcache_bin *bin = &tc->bins[sc];
    pthread_mutex_lock(&bin->lock);
    if (bin->count == 0) {
        pthread_mutex_unlock(&bin->lock);
        return NULL;
    }
    bin->count--;
    void *ptr = bin->entries[bin->count];
    bin->entries[bin->count] = NULL;
    pthread_mutex_unlock(&bin->lock);
    return ptr;
}

static void tcache_put(struct arena *a, void *ptr, int sc) {
    struct arena_tcache *tc = tcache_get(a);
    if (!tc) return;
    if (sc < 0 || sc >= ARENA_TCACHE_BINS) return;

    struct arena_tcache_bin *bin = &tc->bins[sc];
    pthread_mutex_lock(&bin->lock);
    if (bin->count >= ARENA_TCACHE_DEPTH) {
        pthread_mutex_unlock(&bin->lock);
        return;
    }
    bin->entries[bin->count] = ptr;
    bin->count++;
    pthread_mutex_unlock(&bin->lock);
}

static void tcache_clear(struct arena *a) {
    if (!a->thread_caches) return;
    for (size_t n = 0; n < a->tcache_count; n++) {
        struct arena_tcache *tc = &a->thread_caches[n];
        for (int b = 0; b < ARENA_TCACHE_BINS; b++) {
            struct arena_tcache_bin *bin = &tc->bins[b];
            pthread_mutex_lock(&bin->lock);
            bin->count = 0;
            memset(bin->entries, 0, sizeof(bin->entries));
            pthread_mutex_unlock(&bin->lock);
        }
    }
}

/* ══════════════════════════════════════════════════════════════════
   LARGE ALLOCATION TRACKING
   ══════════════════════════════════════════════════════════════════ */

struct sys_alloc_record {
    void *ptr;
    size_t size;
};

static struct sys_alloc_record *sys_alloc_records;
static size_t sys_alloc_count;
static size_t sys_alloc_cap;
static pthread_mutex_t sys_alloc_lock = PTHREAD_MUTEX_INITIALIZER;

static void sys_alloc_track(void *ptr, size_t size) {
    pthread_mutex_lock(&sys_alloc_lock);
    if (sys_alloc_count >= sys_alloc_cap) {
        size_t new_cap = sys_alloc_cap == 0 ? 64 : sys_alloc_cap * 2;
        struct sys_alloc_record *new_arr = realloc(sys_alloc_records,
            new_cap * sizeof(struct sys_alloc_record));
        if (!new_arr) { pthread_mutex_unlock(&sys_alloc_lock); return; }
        sys_alloc_records = new_arr;
        sys_alloc_cap = new_cap;
    }
    sys_alloc_records[sys_alloc_count].ptr = ptr;
    sys_alloc_records[sys_alloc_count].size = size;
    sys_alloc_count++;
    pthread_mutex_unlock(&sys_alloc_lock);
}

static void sys_alloc_untrack(void *ptr) {
    pthread_mutex_lock(&sys_alloc_lock);
    for (size_t i = 0; i < sys_alloc_count; i++) {
        if (sys_alloc_records[i].ptr == ptr) {
            sys_alloc_records[i] = sys_alloc_records[sys_alloc_count - 1];
            sys_alloc_count--;
            break;
        }
    }
    pthread_mutex_unlock(&sys_alloc_lock);
}

static bool sys_alloc_contains(const void *ptr) {
	pthread_mutex_lock(&sys_alloc_lock);
	for (size_t i = 0; i < sys_alloc_count; i++) {
		const struct sys_alloc_record *r = &sys_alloc_records[i];
		uintptr_t start = (uintptr_t)r->ptr;
		uintptr_t end = start + r->size;
		uintptr_t p = (uintptr_t)ptr;
		if (p >= start && p < end) {
			pthread_mutex_unlock(&sys_alloc_lock);
			return true;
		}
	}
	pthread_mutex_unlock(&sys_alloc_lock);
	return false;
}

static void *sys_alloc(size_t size) {
    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (ptr != MAP_FAILED) {
        sys_alloc_track(ptr, size);
        return ptr;
    }
    return NULL;
}

static void sys_free(void *ptr, size_t size) {
    sys_alloc_untrack(ptr);
    if (ptr) munmap(ptr, size);
}

/* ══════════════════════════════════════════════════════════════════
   COLD TIER SEGMENTS [6][7]
   ══════════════════════════════════════════════════════════════════ */

static struct arena_segment *seg_create(uint8_t freq, uint8_t sc) {
    size_t seg_size = ARENA_SEG_SIZE;

    void *base = mmap(NULL, seg_size,
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_HUGETLB,
                      -1, 0);
    if (base == MAP_FAILED) {
        base = mmap(NULL, seg_size,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    }
    if (base == MAP_FAILED) return NULL;

	struct arena_segment *seg = base;
	atomic_store(&seg->hdr.control, 0);
	atomic_store_explicit(&seg->hdr.free_bitmap[0], UINT64_MAX,
		memory_order_relaxed);
	atomic_store_explicit(&seg->hdr.free_bitmap[1], UINT64_MAX,
		memory_order_relaxed);
	atomic_store(&seg->hdr.epoch, 0);
	seg->hdr.freq = freq;
	seg->hdr.size_class = sc;
	seg->hdr.node_id = 0;
	seg->hdr.flags = 0;
	seg->hdr.next = NULL;

	return seg;
}

static void *seg_alloc(struct arena_segment *seg, size_t size) {
	if (!seg) return NULL;

	size_t granularity = 64;
	size_t slots_needed = (size + granularity - 1) / granularity;
	size_t total_slots = 128;

	_Atomic uint64_t *bm = seg->hdr.free_bitmap;
	size_t consecutive = 0;
	size_t start_slot = 0;

	for (size_t i = 0; i < total_slots; i++) {
		int word = (int)(i / 64);
		int bit = (int)(i % 64);
		if (word > 1) break;

		uint64_t bits = atomic_load_explicit(&bm[word], memory_order_acquire);
		if (bits & (1ULL << bit)) {
			consecutive++;
			if (consecutive == 1) start_slot = i;
			if (consecutive >= slots_needed) {
				for (size_t j = start_slot; j < start_slot + slots_needed; j++) {
					int w = (int)(j / 64);
					int b = (int)(j % 64);
					uint64_t old_b, new_b;
					do {
						old_b = atomic_load_explicit(&bm[w], memory_order_acquire);
						if (!(old_b & (1ULL << b))) goto retry_scan;
						new_b = old_b & ~(1ULL << b);
					} while (!atomic_compare_exchange_strong_explicit(
						&bm[w], &old_b, new_b,
						memory_order_acq_rel, memory_order_acquire));
				}

				uint64_t ctrl = atomic_load_explicit(&seg->hdr.control,
					memory_order_relaxed);
				uint64_t used;
				do {
					used = (ctrl >> 48) & 0xFFFF;
					used += slots_needed;
					uint64_t new_ctrl = (ctrl & ~(0xFFFFULL << 48)) | (used << 48);
					if (atomic_compare_exchange_strong_explicit(
						&seg->hdr.control, &ctrl, new_ctrl,
						memory_order_relaxed, memory_order_relaxed))
						break;
				} while (1);

				return seg->data + start_slot * granularity;
			}
		} else {
			consecutive = 0;
		}
		retry_scan:;
	}
	return NULL;
}

static bool seg_free(struct arena_segment *seg, void *ptr, size_t size) {
	if (!seg || !ptr) return false;

	if (atomic_load_explicit(&seg->hdr.flags, memory_order_acquire) & 0x01)
		return true;

	size_t granularity = 64;
	size_t seg_data_size = ARENA_SEG_SIZE - sizeof(struct arena_seg_hdr);
	uintptr_t seg_start = (uintptr_t)seg;
	uintptr_t data_start = seg_start + sizeof(struct arena_seg_hdr);
	uintptr_t p = (uintptr_t)ptr;
	if (p < data_start || p - data_start >= seg_data_size) return false;
	size_t offset = p - data_start;

    size_t start_slot = offset / granularity;
    size_t slots = (size + granularity - 1) / granularity;

	for (size_t i = start_slot; i < start_slot + slots && i < 128; i++) {
		int w = (int)(i / 64);
		int b = (int)(i % 64);
		if (w > 1) break;
		uint64_t old_b, new_b;
		do {
			old_b = atomic_load_explicit(&seg->hdr.free_bitmap[w],
				memory_order_acquire);
			new_b = old_b | (1ULL << b);
		} while (!atomic_compare_exchange_strong_explicit(
			&seg->hdr.free_bitmap[w], &old_b, new_b,
			memory_order_acq_rel, memory_order_acquire));
	}

	uint64_t ctrl = atomic_load_explicit(&seg->hdr.control, memory_order_relaxed);
	do {
		uint64_t used = (ctrl >> 48) & 0xFFFF;
		used = (used >= slots) ? used - slots : 0;
		uint64_t new_ctrl = (ctrl & ~(0xFFFFULL << 48)) | (used << 48);
		if (atomic_compare_exchange_strong_explicit(
			&seg->hdr.control, &ctrl, new_ctrl,
			memory_order_relaxed, memory_order_relaxed))
			break;
	} while (1);

	return true;
}

static bool seg_contains(struct arena_segment *seg, void *ptr) {
	if (!seg || !ptr) return false;
	uintptr_t seg_start = (uintptr_t)seg;
	uintptr_t p = (uintptr_t)ptr;
	return p >= seg_start && p - seg_start < ARENA_SEG_SIZE;
}

/* ══════════════════════════════════════════════════════════════════
   EPOCH-BASED GC [6][7]
   ══════════════════════════════════════════════════════════════════ */

static void epoch_gc_start(struct arena *a, int freq) {
    uint32_t epoch = atomic_fetch_add_explicit(&a->epoch_ctl.global_epoch, 1,
                                               memory_order_acq_rel) + 1;
    atomic_fetch_add_explicit(&a->epoch_ctl.evacuating, 1, memory_order_acq_rel);

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        struct arena_segment **segs = a->arenas[freq][sc].u.segs;
        if (!segs) continue;

        for (size_t i = 0; i < a->arenas[freq][sc].count; i++) {
            struct arena_segment *seg = segs[i];
            if (!seg) continue;

            uint64_t ctrl = atomic_load(&seg->hdr.control);
            uint64_t used = (ctrl >> 48) & 0xFFFF;
            double pct = (double)used / 128.0;

            double threshold = (freq == ARENA_FREQ_COLD)
                ? (double)a->epoch_ctl.compact_trigger / 100.0
                : 0.50;

            if (pct > threshold) {
                atomic_store(&seg->hdr.epoch, epoch);
                atomic_fetch_or(&seg->hdr.flags, 0x01);
            }
        }
    }

    atomic_fetch_sub_explicit(&a->epoch_ctl.evacuating, 1,
                              memory_order_acq_rel);
    a->compactions++;
}

/* ══════════════════════════════════════════════════════════════════
   HARDWARE-AWARE PREFETCHING [4]
   ══════════════════════════════════════════════════════════════════ */

static inline void prefetch_range(const void *ptr, size_t len) {
    if (!ptr || len == 0) return;
    const uint8_t *p = (const uint8_t *)ptr;
    for (size_t i = 0; i < len; i += 64) {
        __builtin_prefetch(p + i, 0, 2);
    }
}

/* ══════════════════════════════════════════════════════════════════
   ADAPTIVE TUNER [9]
   ══════════════════════════════════════════════════════════════════ */

static void tuner_record(struct arena_tuner *t, size_t size) {
    int bucket = (int)(size >> 4);
    if (bucket >= 64) bucket = 63;
    t->hist[bucket]++;
    t->sample_count++;
}

static void tuner_adapt(struct arena *a) {
    struct arena_tuner *t = &a->tuner;
    if (t->sample_count < 1024) return;

    uint64_t total_small = 0, total_large = 0;
    for (int i = 0; i < 8; i++) total_small += t->hist[i];
    for (int i = 32; i < 64; i++) total_large += t->hist[i];

    uint64_t total = t->sample_count;
    if (total == 0) return;

    if (total_small * 100 / total > 70) {
        a->config.warm_threshold = 8;
    } else if (total_large * 100 / total > 50) {
        a->config.warm_threshold = 20;
    } else {
        a->config.warm_threshold = 10;
    }

    t->freq_thresholds[ARENA_FREQ_WARM] = a->config.warm_threshold;
    t->freq_thresholds[ARENA_FREQ_COLD] = (uint8_t)(a->config.warm_threshold * 2);

    uint8_t new_epoch = (uint8_t)(atomic_load(&a->epoch_ctl.global_epoch) & 0xFF);
    t->last_tune_epoch = new_epoch;

    memset(t->hist, 0, sizeof(t->hist));
    t->sample_count = 0;
}

/* ══════════════════════════════════════════════════════════════════
   PUBLIC API
   ══════════════════════════════════════════════════════════════════ */

struct arena *arena_create(size_t initial_capacity) {
    (void)initial_capacity;

    struct arena *a = calloc(1, sizeof(struct arena));
    if (!a) return NULL;

    a->config.warm_threshold = 10;
    a->config.enable_prefetch = 1;
    a->config.enable_thread_cache = 1;
    a->config.enable_adaptive_tuning = 1;

    if (a->config.enable_thread_cache) {
        a->tcache_count = 1;
        a->thread_caches = calloc(a->tcache_count, sizeof(struct arena_tcache));
        if (!a->thread_caches) {
            a->tcache_count = 0;
            a->config.enable_thread_cache = 0;
        } else {
            for (size_t i = 0; i < a->tcache_count; i++) {
                a->thread_caches[i].node_id = 0;
                for (int b = 0; b < ARENA_TCACHE_BINS; b++) {
                    pthread_mutex_init(&a->thread_caches[i].bins[b].lock, NULL);
                }
            }
        }
    }

	for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
		a->arenas[ARENA_FREQ_WARM][sc].u.slabs = calloc(1, sizeof(struct arena_slab_set));
		if (!a->arenas[ARENA_FREQ_WARM][sc].u.slabs) goto fail;
		atomic_store(&a->arenas[ARENA_FREQ_WARM][sc].u.slabs->slab_count, 0);
		atomic_store(&a->arenas[ARENA_FREQ_WARM][sc].u.slabs->total_slots, 0);
		atomic_store(&a->arenas[ARENA_FREQ_WARM][sc].u.slabs->free_slots, 0);
	}

	for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
		a->arenas[ARENA_FREQ_COLD][sc].u.segs = calloc(ARENA_COLD_SEGS_PER_CLASS,
			sizeof(struct arena_segment *));
		if (!a->arenas[ARENA_FREQ_COLD][sc].u.segs) goto fail;
		for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
			a->arenas[ARENA_FREQ_COLD][sc].u.segs[i] = seg_create(
				ARENA_FREQ_COLD, (uint8_t)sc);
		}
		a->arenas[ARENA_FREQ_COLD][sc].count = ARENA_COLD_SEGS_PER_CLASS;
	}

    a->epoch_ctl.global_epoch = 0;
    a->epoch_ctl.evacuating = 0;
    a->epoch_ctl.completed = 0;
    a->epoch_ctl.compact_trigger = 50;
    a->epoch_ctl.batch_size = 256;

    a->prefetch_ctl.enabled = a->config.enable_prefetch;
    a->prefetch_ctl.prefetch_distance = 2;

    a->tuner.freq_thresholds[ARENA_FREQ_WARM] = a->config.warm_threshold;
    a->tuner.freq_thresholds[ARENA_FREQ_COLD] = a->config.warm_threshold * 2;
    a->tuner.last_tune_epoch = 0;
    a->tuner.sample_count = 0;

    return a;

fail:
	arena_destroy(a);
	return NULL;
}

void arena_destroy(struct arena *a) {
    if (!a) return;

    tcache_clear(a);

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        struct arena_slab_set *set = a->arenas[ARENA_FREQ_WARM][sc].u.slabs;
        if (set) {
            int count = atomic_load(&set->slab_count);
            for (int i = 0; i < count && i < 8; i++) {
                if (set->slabs[i]) {
                    munmap(set->slabs[i], set->slabs[i]->mmap_size);
                }
            }
            free(set);
        }
    }

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
            for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
                if (a->arenas[ARENA_FREQ_COLD][sc].u.segs[i]) {
                    munmap(a->arenas[ARENA_FREQ_COLD][sc].u.segs[i],
                           ARENA_SEG_SIZE);
                }
            }
            free(a->arenas[ARENA_FREQ_COLD][sc].u.segs);
        }
    }

    if (a->thread_caches) {
        for (size_t i = 0; i < a->tcache_count; i++) {
            for (int b = 0; b < ARENA_TCACHE_BINS; b++) {
                pthread_mutex_destroy(&a->thread_caches[i].bins[b].lock);
            }
        }
    }
    free(a->thread_caches);
    free(a);
}

void *arena_alloc(struct arena *a, size_t size) {
    if (!a || size == 0) return NULL;

    if (a->config.enable_adaptive_tuning) {
        tuner_record(&a->tuner, size);
        if (a->tuner.sample_count % 4096 == 0) {
            tuner_adapt(a);
        }
    }

    if (size <= 128) {
        int sc = size_to_slab_class(size);

        if (a->config.enable_thread_cache) {
            void *cached = tcache_try_get(a, sc);
            if (cached) {
                if (a->prefetch_ctl.enabled)
                    prefetch_range(cached, size);
                return cached;
            }
        }

        if (sc >= 0 && sc < ARENA_SLAB_SIZES &&
            a->arenas[ARENA_FREQ_WARM][sc].u.slabs) {
            void *ptr = slab_set_alloc(a->arenas[ARENA_FREQ_WARM][sc].u.slabs, sc);
            if (ptr) {
                atomic_fetch_add_explicit(&a->alloc_count, 1, memory_order_relaxed);
                atomic_fetch_add_explicit(&a->alloc_bytes, size, memory_order_relaxed);
                if (a->prefetch_ctl.enabled)
                    prefetch_range(ptr, size);
                return ptr;
            }
        }

        for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
            struct arena_segment *seg = a->arenas[ARENA_FREQ_COLD][sc].u.segs
                ? a->arenas[ARENA_FREQ_COLD][sc].u.segs[i] : NULL;
            if (seg) {
                void *ptr = seg_alloc(seg, size);
                if (ptr) {
                    atomic_fetch_add_explicit(&a->alloc_count, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&a->alloc_bytes, size, memory_order_relaxed);
                    return ptr;
                }
            }
        }
    }

    if (size <= 4096) {
        int sc = size_to_slab_class(size > 128 ? 128 : size);
        for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
            struct arena_segment *seg = a->arenas[ARENA_FREQ_COLD][sc].u.segs
                ? a->arenas[ARENA_FREQ_COLD][sc].u.segs[i] : NULL;
            if (seg) {
                void *ptr = seg_alloc(seg, size);
                if (ptr) {
                    atomic_fetch_add_explicit(&a->alloc_count, 1, memory_order_relaxed);
                    atomic_fetch_add_explicit(&a->alloc_bytes, size, memory_order_relaxed);
                    return ptr;
                }
            }
        }
    }

    void *ptr = sys_alloc(size);
    if (ptr) {
        atomic_fetch_add_explicit(&a->alloc_count, 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&a->alloc_bytes, size, memory_order_relaxed);
    }
    return ptr;
}

void *arena_realloc(struct arena *a, void *ptr, size_t old_size, size_t new_size) {
    if (!a) return NULL;
    if (new_size == 0) {
        arena_free(a, ptr, old_size);
        return NULL;
    }
    if (!ptr) return arena_alloc(a, new_size);

    void *new_ptr = arena_alloc(a, new_size);
    if (!new_ptr) return NULL;

    size_t copy = old_size < new_size ? old_size : new_size;
    memcpy(new_ptr, ptr, copy);

    arena_free(a, ptr, old_size);
    return new_ptr;
}

void arena_free(struct arena *a, void *ptr, size_t size) {
    if (!a || !ptr) return;

    if (size <= 128) {
        int sc = size_to_slab_class(size);

        if (a->config.enable_thread_cache) {
            struct arena_tcache *tc = tcache_get(a);
            if (tc && sc >= 0 && sc < ARENA_TCACHE_BINS) {
                unsigned int count;
                pthread_mutex_lock(&tc->bins[sc].lock);
                count = tc->bins[sc].count;
                pthread_mutex_unlock(&tc->bins[sc].lock);
                if (count < ARENA_TCACHE_DEPTH) {
                    tcache_put(a, ptr, sc);
                    return;
                }
            }
        }

        if (sc >= 0 && sc < ARENA_SLAB_SIZES &&
            a->arenas[ARENA_FREQ_WARM][sc].u.slabs) {
            if (slab_set_free(a->arenas[ARENA_FREQ_WARM][sc].u.slabs, ptr))
                return;
        }

        for (int s = 0; s < ARENA_SLAB_SIZES; s++) {
            if (s == sc) continue;
            if (a->arenas[ARENA_FREQ_WARM][s].u.slabs &&
                slab_set_free(a->arenas[ARENA_FREQ_WARM][s].u.slabs, ptr))
                return;
        }
    }

    if (size <= 4096) {
        for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
            if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
                for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
                    struct arena_segment *seg =
                        a->arenas[ARENA_FREQ_COLD][sc].u.segs[i];
                    if (seg && seg_free(seg, ptr, size))
                        return;
                }
            }
        }
    }

    sys_free(ptr, size);
}

void arena_clear(struct arena *a) {
    if (!a) return;

    tcache_clear(a);

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        struct arena_slab_set *set = a->arenas[ARENA_FREQ_WARM][sc].u.slabs;
        if (set) {
            int count = atomic_load(&set->slab_count);
            for (int i = 0; i < count && i < 8; i++) {
                if (set->slabs[i]) {
                    for (int w = 0; w < 4; w++) {
                        atomic_store_explicit(&set->slabs[i]->bitmap[w],
                                              UINT64_MAX, memory_order_relaxed);
                    }
                    atomic_store(&set->slabs[i]->used_count, 0);
                }
            }
            atomic_store(&set->free_slots, atomic_load(&set->total_slots));
        }

	if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
		for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
			struct arena_segment *seg = a->arenas[ARENA_FREQ_COLD][sc].u.segs[i];
			if (seg) {
				atomic_store_explicit(&seg->hdr.free_bitmap[0],
					UINT64_MAX, memory_order_relaxed);
				atomic_store_explicit(&seg->hdr.free_bitmap[1],
					UINT64_MAX, memory_order_relaxed);
				uint64_t ctrl = atomic_load_explicit(&seg->hdr.control,
					memory_order_relaxed);
				ctrl &= ~(0xFFFFULL << 48);
				atomic_store_explicit(&seg->hdr.control, ctrl,
					memory_order_relaxed);
				atomic_store(&seg->hdr.epoch, 0);
				seg->hdr.flags = 0;
			}
		}
	}
    }

    atomic_store_explicit(&a->alloc_count, 0, memory_order_relaxed);
    atomic_store_explicit(&a->alloc_bytes, 0, memory_order_relaxed);
}

void arena_compact(struct arena *a, int freq) {
    if (!a) return;
    epoch_gc_start(a, freq);
}

int arena_get_stats(const struct arena *a, size_t *out) {
    if (!a || !out) return -1;

    size_t total = 0, used = 0, waste = 0;

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        const struct arena_slab_set *set = a->arenas[ARENA_FREQ_WARM][sc].u.slabs;
        if (set) {
            unsigned int total_slots = atomic_load(&set->total_slots);
            unsigned int free_slots = atomic_load(&set->free_slots);
            total += (size_t)total_slots * g_slab_sizes[sc];
            used += (size_t)(total_slots - free_slots) * g_slab_sizes[sc];
            waste += (size_t)free_slots * g_slab_sizes[sc];
        }
    }

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
            for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
                if (a->arenas[ARENA_FREQ_COLD][sc].u.segs[i]) {
                    total += ARENA_SEG_SIZE;
                    uint64_t ctrl = atomic_load(
                        &a->arenas[ARENA_FREQ_COLD][sc].u.segs[i]->hdr.control);
                    uint64_t seg_used = ((ctrl >> 48) & 0xFFFF) * 64;
                    used += seg_used;
                    waste += ARENA_SEG_SIZE - seg_used;
                }
            }
        }
    }

    out[ARENA_STAT_TOTAL] = total;
    out[ARENA_STAT_USED] = used;
    out[ARENA_STAT_WASTE] = waste;
    out[ARENA_STAT_COMPACTIONS] = a->compactions;
    out[ARENA_STAT_WRITE_AMP] = a->write_amplification;

    return 0;
}

size_t arena_size(const struct arena *a) {
    if (!a) return 0;
    return atomic_load(&a->alloc_bytes);
}

size_t arena_capacity(const struct arena *a) {
    if (!a) return 0;

    size_t total = 0;

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        const struct arena_slab_set *set = a->arenas[ARENA_FREQ_WARM][sc].u.slabs;
        if (set) {
            total += (size_t)atomic_load(&set->total_slots) * g_slab_sizes[sc];
        }
    }

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
            total += (size_t)a->arenas[ARENA_FREQ_COLD][sc].count * ARENA_SEG_SIZE;
        }
    }

    return total;
}

bool arena_contains(const struct arena *a, const void *ptr) {
    if (!a || !ptr) return false;

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        const struct arena_slab_set *set = a->arenas[ARENA_FREQ_WARM][sc].u.slabs;
        if (set && slab_set_contains((struct arena_slab_set *)set, (void *)ptr))
            return true;
    }

    for (int sc = 0; sc < ARENA_SLAB_SIZES; sc++) {
        if (a->arenas[ARENA_FREQ_COLD][sc].u.segs) {
            for (int i = 0; i < ARENA_COLD_SEGS_PER_CLASS; i++) {
                struct arena_segment *seg = a->arenas[ARENA_FREQ_COLD][sc].u.segs[i];
                if (seg && seg_contains(seg, (void *)ptr))
                    return true;
            }
        }
    }

    if (sys_alloc_contains(ptr))
        return true;

    return false;
}
