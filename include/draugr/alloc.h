#ifndef DRAUGR_ALLOC_H
#define DRAUGR_ALLOC_H

#include <stdlib.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef DRAUGR_USE_MALLOC
struct arena { int stub; };
#endif

#ifdef DRAUGR_USE_MALLOC

#define DRAUGR_ALLOC(ctx, sz) malloc(sz)
#define DRAUGR_FREE(ctx, ptr, sz) free(ptr)
#define DRAUGR_REALLOC(ctx, ptr, os, ns) realloc(ptr, ns)
#define DRAUGR_CALLOC(ctx, n, sz) calloc(n, sz)

#else

#include "draugr/arena.h"

static inline void *DRAUGR_ALLOC(void *ctx, size_t size) {
    if (!ctx) return malloc(size);
    if (size == 0) size = 1;
    return arena_alloc(ctx, size);
}

static inline void DRAUGR_FREE(void *ctx, void *ptr, size_t size) {
    if (!ctx) { free(ptr); return; }
    if (size == 0) size = 1;
    arena_free(ctx, ptr, size);
}

static inline void *DRAUGR_REALLOC(void *ctx, void *ptr,
 size_t old_sz, size_t new_sz) {
 (void)old_sz;
    if (!ctx) return realloc(ptr, new_sz);
    if (new_sz == 0) new_sz = 1;
 return arena_realloc(ctx, ptr, old_sz, new_sz);
}

static inline void *DRAUGR_CALLOC(void *ctx, size_t n, size_t sz) {
    size_t total = n * sz;
    if (ctx && total == 0) total = 1;
    void *p = ctx ? arena_alloc(ctx, total) : calloc(1, total);
    if (p) memset(p, 0, total);
    return p;
}

#endif

#ifdef __cplusplus
}
#endif

#endif
