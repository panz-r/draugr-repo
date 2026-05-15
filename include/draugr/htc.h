#ifndef DRAUGR_HTC_H
#define DRAUGR_HTC_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct arena;

typedef struct htc_table htc_table_t;

typedef struct {
    uint32_t initial_buckets;
    double   max_load_factor;
    uint32_t shard_count;       /* default 128, 0 = use default */
    uint32_t flags;             /* bitfield: 1 = disable front cache */
} htc_config_t;

#define HTC_CFG_DISABLE_FRONT_CACHE 1u

htc_table_t *htc_create(const htc_config_t *cfg);
htc_table_t *htc_create_with_arena(const htc_config_t *cfg, struct arena *arena);
void         htc_destroy(htc_table_t *t);
void         htc_clear(htc_table_t *t);

bool htc_insert(htc_table_t *t, uint64_t hash, uint64_t value);
bool htc_upsert(htc_table_t *t, uint64_t hash, uint64_t value);
bool htc_update(htc_table_t *t, uint64_t hash, uint64_t value);

bool htc_find(const htc_table_t *t, uint64_t hash, uint64_t *out_value);

bool htc_remove(htc_table_t *t, uint64_t hash);

size_t htc_size(const htc_table_t *t);

#ifdef __cplusplus
}
#endif

#endif
