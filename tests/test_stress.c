/**
 * test_stress.c - Heavy stress tests for the hash table
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INV_CHECK(t, op) do { \
    const char *_inv_err = ht_check_invariants(t); \
    if (_inv_err) { \
        printf("  INVARIANT BROKEN at op %d: %s\n", (op), _inv_err); \
        return 1; \
    } \
} while (0)

static uint64_t fnv1a_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static uint64_t fixed_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 42;
}

static uint64_t zero_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

/* Original: 100k random ops with binary keys */
static int stress_random(void) {
    printf("Stress: 100k random ops with binary keys...\n");
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
                printf("  FAIL op %d: insert key %d\n", i, k);
                return 1;
            }
        } else if (op < 85) {
            ht_find(t, keys[k].data, keys[k].len, NULL);
        } else {
            ht_remove(t, keys[k].data, keys[k].len);
        }

        if (i % 5000 == 4999) INV_CHECK(t, i);
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    printf("  size=%zu cap=%zu tombs=%zu load=%.2f\n",
           stats.size, stats.capacity, stats.tombstone_cnt, stats.load_factor);

    for (int i = 0; i < N; i++) free(keys[i].data);
    free(keys);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Stress with all-colliding keys (fixed_hash) — exercises long probe chains */
static int stress_collision(void) {
    printf("Stress: 100k ops with all-colliding keys (fixed_hash)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    const int N = 2000;
    int *present = calloc(N, sizeof(int));
    int present_count = 0;
    int vals[2000];
    srand(2001);

    for (int op = 0; op < 100000; op++) {
        int k = rand() % N;
        int action = rand() % 100;
        if (action < 50) {
            vals[k] = k + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            if (!present[k]) { present[k] = 1; present_count++; }
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (v == NULL || *v != vals[k]) {
                printf("  FAIL op %d: insert/find key %d\n", op, k);
                free(present); ht_destroy(t); return 1;
            }
        } else if (action < 80) {
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k] && (v == NULL || *v != vals[k])) {
                printf("  FAIL op %d: find present key %d\n", op, k);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
                present_count--;
            }
        }

        if (op % 5000 == 4999) INV_CHECK(t, op);
    }

    /* Verify all present entries */
    int verify = 0;
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            if (v == NULL || *v != vals[i]) {
                printf("  FAIL: key %d missing or wrong value\n", i);
                free(present); ht_destroy(t); return 1;
            }
            verify++;
        } else {
            assert(v == NULL);
        }
    }
    assert(verify == present_count);

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu (all collide to hash=42)\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Stress with all-spill-lane entries (zero_hash) */
static int stress_spill(void) {
    printf("Stress: 50k ops with all-spill-lane keys (zero_hash)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    const int N = 500;
    int *present = calloc(N, sizeof(int));
    int vals[500];
    srand(3003);

    for (int op = 0; op < 50000; op++) {
        int k = rand() % N;
        int action = rand() % 100;
        if (action < 50) {
            vals[k] = k * 7 + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            present[k] = 1;
        } else if (action < 80) {
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k]) {
                if (v == NULL || *v != vals[k]) {
                    printf("  FAIL op %d: find key %d\n", op, k);
                    free(present); ht_destroy(t); return 1;
                }
            }
        } else {
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
            }
        }
    }

    /* Verify */
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            assert(v != NULL && *v == vals[i]);
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu (all in spill lane)\n", st.size);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Stress with ht_inc operations under heavy load */
static int stress_inc(void) {
    printf("Stress: 80k ops with ht_inc under heavy load...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    const int N = 1000;
    int64_t *expected = calloc(N, sizeof(int64_t));
    int *present = calloc(N, sizeof(int));
    srand(4004);

    for (int op = 0; op < 80000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 40) {
            /* ht_inc — creates or increments */
            int64_t delta = (rand() % 201) - 100;  /* -100 to +100 */
            int64_t result = ht_inc(t, &k, sizeof(int), delta);
            if (!present[k]) {
                expected[k] = delta;
                present[k] = 1;
            } else {
                expected[k] += delta;
            }
            if (result != expected[k]) {
                printf("  FAIL op %d: ht_inc key %d expected %ld got %ld\n",
                       op, k, (long)expected[k], (long)result);
                free(expected); free(present); ht_destroy(t); return 1;
            }
        } else if (action < 75) {
            /* find */
            const int64_t *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k]) {
                if (v == NULL || *v != expected[k]) {
                    printf("  FAIL op %d: find key %d\n", op, k);
                    free(expected); free(present); ht_destroy(t); return 1;
                }
            }
        } else {
            /* remove */
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
                expected[k] = 0;
            }
        }

        if (op % 5000 == 4999) INV_CHECK(t, op);
    }

    /* Verify all present */
    for (int i = 0; i < N; i++) {
        const int64_t *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            if (v == NULL || *v != expected[i]) {
                printf("  FAIL: key %d expected %ld\n", i, (long)expected[i]);
                free(expected); free(present); ht_destroy(t); return 1;
            }
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    free(expected);
    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Stress with continuous stats verification */
static int stress_stats_invariants(void) {
    printf("Stress: 50k ops with stats verification...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.15, .zombie_window = 8 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    const int N = 2000;
    int *present = calloc(N, sizeof(int));
    int present_count = 0;
    int vals[2000];
    srand(5555);

    for (int op = 0; op < 50000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 45) {
            vals[k] = k + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            if (!present[k]) { present[k] = 1; present_count++; }
        } else if (action < 80) {
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k] && (v == NULL || *v != vals[k])) {
                printf("  FAIL op %d: find key %d\n", op, k);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
                present_count--;
            }
        }

        /* Every 5000 ops: verify stats consistency */
        if (op % 5000 == 4999) {
            INV_CHECK(t, op);
            ht_stats_t st;
            ht_stats(t, &st);
            if (st.size != (size_t)present_count) {
                printf("  FAIL op %d: stats.size=%zu != tracked=%d\n",
                       op, st.size, present_count);
                free(present); ht_destroy(t); return 1;
            }
            double expected_lf = (double)st.size / (double)st.capacity;
            if (st.load_factor < expected_lf - 0.01 || st.load_factor > expected_lf + 0.01) {
                printf("  FAIL op %d: load_factor mismatch\n", op);
                free(present); ht_destroy(t); return 1;
            }
        }
    }

    /* Final: verify all present entries */
    int verify = 0;
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            if (v == NULL || *v != vals[i]) {
                printf("  FAIL: key %d wrong\n", i);
                free(present); ht_destroy(t); return 1;
            }
            verify++;
        } else {
            assert(v == NULL);
        }
    }
    assert(verify == present_count);

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu tombs=%zu\n", st.size, st.capacity, st.tombstone_cnt);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Stress the delete_compact paths with heavy churn targeting tombstone accuracy */
static int stress_delete_compact(void) {
    printf("Stress: 30k delete-heavy ops targeting delete_compact...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 4 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    const int N = 200;
    int *present = calloc(N, sizeof(int));
    int vals[200];
    srand(6666);

    for (int op = 0; op < 30000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 40) {
            vals[k] = op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            present[k] = 1;
        } else if (action < 60) {
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k] && (v == NULL || *v != vals[k])) {
                printf("  FAIL op %d: find %d\n", op, k);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            /* Heavy delete emphasis */
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
            }
        }

        /* Compact every 5000 ops */
        if (op % 5000 == 4999) {
            INV_CHECK(t, op);
            ht_compact(t);
        }
    }

    /* Verify */
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            if (v == NULL || *v != vals[i]) {
                printf("  FAIL: key %d missing after stress\n", i);
                free(present); ht_destroy(t); return 1;
            }
        }
    }

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Heavy churn — 50k ops, 70% delete rate, small table, all collide */
static int stress_churn(void) {
    printf("Stress: 50k ops with 70%% delete rate on small table (fixed_hash)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    const int N = 500;
    int *present = calloc(N, sizeof(int));
    int present_count = 0;
    int vals[500];
    srand(1111);

    for (int op = 0; op < 50000; op++) {
        int k = rand() % N;
        int action = rand() % 100;
        if (action < 30) {
            /* insert */
            vals[k] = k + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            if (!present[k]) { present[k] = 1; present_count++; }
        } else {
            /* delete (70% rate) */
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
                present_count--;
            }
        }

        /* Every 5000 ops: verify stats.size matches tracked count */
        if (op % 5000 == 4999) {
            INV_CHECK(t, op);
            ht_stats_t st;
            ht_stats(t, &st);
            if (st.size != (size_t)present_count) {
                printf("  FAIL op %d: stats.size=%zu != tracked=%d\n",
                       op, st.size, present_count);
                free(present); ht_destroy(t); return 1;
            }
            /* Verify all present entries are findable */
            for (int i = 0; i < N; i++) {
                const int *v = ht_find(t, &i, sizeof(int), NULL);
                if (present[i]) {
                    if (v == NULL || *v != vals[i]) {
                        printf("  FAIL op %d: present key %d not findable\n", op, i);
                        free(present); ht_destroy(t); return 1;
                    }
                }
            }
        }
    }

    /* Final verify all present entries */
    int verify = 0;
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            assert(v != NULL && *v == vals[i]);
            verify++;
        } else {
            assert(v == NULL);
        }
    }
    assert(verify == present_count);

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu present_count=%d\n", st.size, st.capacity, present_count);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* 20k ops targeting the spill lane exclusively (zero_hash) */
static int stress_spill_heavy(void) {
    printf("Stress: 20k ops targeting spill lane exclusively (zero_hash)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    const int N = 500;
    int *present = calloc(N, sizeof(int));
    int vals[500];
    srand(2222);

    for (int op = 0; op < 20000; op++) {
        int k = rand() % N;
        int action = rand() % 100;
        if (action < 50) {
            /* 50% insert */
            vals[k] = k * 3 + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            present[k] = 1;
        } else if (action < 70) {
            /* 20% find */
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k]) {
                if (v == NULL || *v != vals[k]) {
                    printf("  FAIL op %d: find key %d\n", op, k);
                    free(present); ht_destroy(t); return 1;
                }
            }
        } else {
            /* 30% remove */
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
            }
        }
    }

    /* Verify at end */
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            assert(v != NULL && *v == vals[i]);
        } else {
            assert(v == NULL);
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu (spill lane only)\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* 30k ops mixing ht_insert/ht_insert_with_hash/find/remove variants */
static int stress_mixed_api(void) {
    printf("Stress: 30k ops mixing API variants (_with_hash)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    const int N = 500;
    int *present = calloc(N, sizeof(int));
    int vals[500];
    int use_spill[500]; /* track which keys are forced to spill */
    memset(use_spill, 0, sizeof(use_spill));
    srand(3333);

    for (int op = 0; op < 30000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 35) {
            /* Insert — half via normal API, half forced to spill via hash=0 */
            vals[k] = k + op;
            if (rand() % 2 == 0) {
                ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
                use_spill[k] = 0;
            } else {
                ht_insert_with_hash(t, 0, &k, sizeof(int), &vals[k], sizeof(int));
                use_spill[k] = 1;
            }
            present[k] = 1;
        } else if (action < 60) {
            /* Find — use matching API variant */
            const int *v;
            if (use_spill[k]) {
                v = ht_find_with_hash(t, 0, &k, sizeof(int), NULL);
            } else {
                v = ht_find(t, &k, sizeof(int), NULL);
            }
            if (present[k]) {
                if (v == NULL || *v != vals[k]) {
                    printf("  FAIL op %d: find key %d (spill=%d)\n", op, k, use_spill[k]);
                    free(present); ht_destroy(t); return 1;
                }
            }
        } else if (action < 80) {
            /* Find via normal API — only non-spill entries reachable this way */
            if (present[k] && !use_spill[k]) {
                const int *v = ht_find(t, &k, sizeof(int), NULL);
                if (v == NULL || *v != vals[k]) {
                    printf("  FAIL op %d: normal find key %d\n", op, k);
                    free(present); ht_destroy(t); return 1;
                }
            }
        } else {
            /* Remove — use matching API variant */
            if (present[k]) {
                if (use_spill[k]) {
                    ht_remove_with_hash(t, 0, &k, sizeof(int));
                } else {
                    ht_remove(t, &k, sizeof(int));
                }
                present[k] = 0;
            }
        }
    }

    /* Verify all present entries at end */
    for (int i = 0; i < N; i++) {
        const int *v;
        if (use_spill[i]) {
            v = ht_find_with_hash(t, 0, &i, sizeof(int), NULL);
        } else {
            v = ht_find(t, &i, sizeof(int), NULL);
        }
        if (present[i]) {
            if (v == NULL || *v != vals[i]) {
                printf("  FAIL: key %d missing at end (spill=%d)\n", i, use_spill[i]);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            assert(v == NULL);
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* 40k ops with 60% updates (reinsert same key), track latest value */
static int stress_update_heavy(void) {
    printf("Stress: 40k ops with 60%% updates (reinsert same key)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    const int N = 500;
    int *present = calloc(N, sizeof(int));
    int vals[500];
    srand(4444);

    /* Pre-populate all keys */
    for (int i = 0; i < N; i++) {
        vals[i] = i;
        ht_insert(t, &i, sizeof(int), &vals[i], sizeof(int));
        present[i] = 1;
    }

    for (int op = 0; op < 40000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 60) {
            /* Update: reinsert same key with new value */
            vals[k] = k + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            present[k] = 1;
        } else if (action < 80) {
            /* Find */
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k]) {
                if (v == NULL || *v != vals[k]) {
                    printf("  FAIL op %d: find key %d expected %d got %d\n",
                           op, k, vals[k], v ? *v : -1);
                    free(present); ht_destroy(t); return 1;
                }
            }
        } else {
            /* Delete */
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
            }
        }

        if (op % 5000 == 4999) INV_CHECK(t, op);
    }

    /* Verify all present entries have latest value */
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            if (v == NULL || *v != vals[i]) {
                printf("  FAIL: key %d expected %d got %d at end\n",
                       i, vals[i], v ? *v : -1);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            assert(v == NULL);
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* 20k ops with ht_compact every 1000 ops */
static int stress_compact_between_ops(void) {
    printf("Stress: 20k ops with ht_compact every 1000 ops...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    const int N = 300;
    int *present = calloc(N, sizeof(int));
    int vals[300];
    srand(5556);

    for (int op = 0; op < 20000; op++) {
        int k = rand() % N;
        int action = rand() % 100;

        if (action < 45) {
            vals[k] = k + op;
            ht_insert(t, &k, sizeof(int), &vals[k], sizeof(int));
            present[k] = 1;
        } else if (action < 80) {
            const int *v = ht_find(t, &k, sizeof(int), NULL);
            if (present[k] && (v == NULL || *v != vals[k])) {
                printf("  FAIL op %d: find key %d\n", op, k);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            if (present[k]) {
                ht_remove(t, &k, sizeof(int));
                present[k] = 0;
            }
        }

        /* Compact every 1000 ops, then verify data integrity */
        if (op % 1000 == 999) {
            INV_CHECK(t, op);
            ht_compact(t);
            for (int i = 0; i < N; i++) {
                const int *v = ht_find(t, &i, sizeof(int), NULL);
                if (present[i]) {
                    if (v == NULL || *v != vals[i]) {
                        printf("  FAIL after compact at op %d: key %d wrong\n", op, i);
                        free(present); ht_destroy(t); return 1;
                    }
                } else {
                    assert(v == NULL);
                }
            }
        }
    }

    /* Final verify */
    for (int i = 0; i < N; i++) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (present[i]) {
            assert(v != NULL && *v == vals[i]);
        } else {
            assert(v == NULL);
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

/* Wrapper main that calls all stress tests */
int main(void) {
    printf("=== Stress Tests ===\n\n");

    if (stress_random()) return 1;
    if (stress_collision()) return 1;
    if (stress_spill()) return 1;
    if (stress_inc()) return 1;
    if (stress_stats_invariants()) return 1;
    if (stress_delete_compact()) return 1;
    if (stress_churn()) return 1;
    if (stress_spill_heavy()) return 1;
    if (stress_mixed_api()) return 1;
    if (stress_update_heavy()) return 1;
    if (stress_compact_between_ops()) return 1;

    printf("\nAll stress tests passed!\n");
    return 0;
}
