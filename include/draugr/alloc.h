#ifndef DRAUGR_ALLOC_H
#define DRAUGR_ALLOC_H

/**
 * Draugr Allocator Abstraction Layer
 *
 * Provides zero-overhead allocation that compiles to:
 * - Direct malloc/calloc/realloc/free in DRAUGR_USE_MALLOC mode (arena excluded)
 * - Arena-backed allocation with optional external arena in full builds
 *
 * Key design:
 * - t->allocator != NULL -> use arena_alloc(t->allocator, size)
 * - t->allocator == NULL -> use malloc (individual kv-pair allocation)
 * - In DRAUGR_USE_MALLOC: arena_alloc() is compile-time unreachable (always NULL ctx)
 *
 * Zero overhead in DRAUGR_USE_MALLOC mode:
 * - No function pointer indirection
 * - No extra branches (single predictable cmp + jmp in full builds)
 * - arena_alloc() calls compile to nothing when ctx is provably NULL
 *
 * Usage:
 * void *p = DRAUGR_ALLOC(t->allocator, 1024);
 * DRAUGR_FREE(t->allocator, ptr, size);
 * void *r = DRAUGR_REALLOC(t->allocator, ptr, old_sz, new_sz);
 * void *c = DRAUGR_CALLOC(1, sizeof(struct foo));
 */

#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/* --- Stub arena for DRAUGR_USE_MALLOC builds --- */
#ifdef DRAUGR_USE_MALLOC
struct arena { int stub; };
#endif

/* --- DRAUGR_USE_MALLOC mode: direct stdlib, no arena dependency --- */
#ifdef DRAUGR_USE_MALLOC

#define DRAUGR_ALLOC(ctx, sz) malloc(sz)
#define DRAUGR_FREE(ctx, ptr, sz) free(ptr)
#define DRAUGR_REALLOC(ctx, ptr, os, ns) realloc(ptr, ns)
#define DRAUGR_CALLOC(ctx, n, sz) calloc(n, sz)

/* --- Full arena mode --- */
#else

#include "draugr/arena.h"

static inline void *DRAUGR_ALLOC(void *ctx, size_t size) {
 return arena_alloc(ctx, size);
}

static inline void DRAUGR_FREE(void *ctx, void *ptr, size_t size) {
 arena_free(ctx, ptr, size);
}

static inline void *DRAUGR_REALLOC(void *ctx, void *ptr,
 size_t old_sz, size_t new_sz) {
 (void)old_sz;
 return arena_realloc(ctx, ptr, old_sz, new_sz);
}

#define DRAUGR_CALLOC(ctx, n, sz) calloc(n, sz)

#endif /* DRAUGR_USE_MALLOC */

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_ALLOC_H */
