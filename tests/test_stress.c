#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint64_t fnv1a_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

int main() {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    const int N = 5000;
    typedef struct {
        uint8_t *data;
        size_t len;
        int val;
    } key_info_t;
    key_info_t *keys = calloc(N, sizeof(key_info_t));
    srand(1337);
    for (int i = 0; i < N; i++) {
        keys[i].len = (rand() % 32) + 4;
        keys[i].data = malloc(keys[i].len);
        for (size_t j = 0; j < keys[i].len; j++) keys[i].data[j] = rand() % 256;
        keys[i].val = i;
    }

    for (int i = 0; i < 100000; i++) {
        int k = rand() % N;
        int op = rand() % 100;
        if (op < 45) {
            ht_insert(t, keys[k].data, keys[k].len, &keys[k].val, sizeof(int));
            const int *v = ht_find(t, keys[k].data, keys[k].len, NULL);
            if (v == NULL || *v != keys[k].val) {
                printf("FAIL op %d: insert key %d\n", i, k);
                return 1;
            }
        } else if (op < 85) {
            ht_find(t, keys[k].data, keys[k].len, NULL);
        } else {
            ht_remove(t, keys[k].data, keys[k].len);
        }
    }
    printf("All 100k ops verified!\n");

    ht_stats_t stats;
    ht_stats(t, &stats);
    printf("Final stats: size=%zu capacity=%zu tombstones=%zu load=%.2f\n",
           stats.size, stats.capacity, stats.tombstone_cnt, stats.load_factor);

    for (int i = 0; i < N; i++) free(keys[i].data);
    free(keys);
    ht_destroy(t);
    return 0;
}
