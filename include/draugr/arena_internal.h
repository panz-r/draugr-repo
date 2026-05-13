#ifndef DRAUGR_ARENA_INTERNAL_H
#define DRAUGR_ARENA_INTERNAL_H

#include "draugr/arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct arena_write_buffer *wbuf_create(int freq, int sc);
int    wbuf_add(struct arena_write_buffer *wbuf,
                const void *key, size_t klen,
                const void *val, size_t vlen);
void   wbuf_flush(struct arena *a, int freq, int sc);

void  *slab_set_alloc(struct arena_slab_set *set, int node, int sc);
bool   slab_set_free(struct arena_slab_set *set, void *ptr);
bool   slab_set_contains(struct arena_slab_set *set, void *ptr);

#ifdef __cplusplus
}
#endif

#endif
