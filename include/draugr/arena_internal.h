#ifndef DRAUGR_ARENA_INTERNAL_H
#define DRAUGR_ARENA_INTERNAL_H

#include "draugr/arena.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

void  *slab_set_alloc(struct arena_slab_set *set, int sc);
bool   slab_set_free(struct arena_slab_set *set, void *ptr);
bool   slab_set_contains(struct arena_slab_set *set, void *ptr);

#ifdef __cplusplus
}
#endif

#endif
