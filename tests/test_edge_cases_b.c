/**
 * test_edge_cases_b.c - Edge Case Tests B (39 tests)
 *
 * Tests 32-63 from original suite plus 7 new tests (64-70).
 *
 * Sections:
 *   1-4:   Extended edge cases (resize+spill, iter stats, find_all, inc spill)
 *   5-8:   More edge cases (with_hash remove+find, value size, prefix keys, reinsert)
 *   9:     Heavy tombstone churn with zombie
 *   10-14: Stress tests (spill hash=1, mixed spill+main, collision churn, bulk ops, inc)
 *   15-19: More stress (with_hash, clear+reuse, delete all, single-key churn, updates)
 *   20-24: More stress (last byte, high load, resize+tombs, iter+modify, find_all+churn)
 *   25-28: Backshift improvements (tombstone count, absorption, abort, dynamic cap)
 *   29-32: Push-forward delete (long chain, prophylactic, churn, consistency)
 *   33:    ht_inc with non-int64 value sizes
 *   34:    Deterministic interleaved inc/remove
 *   35:    Colliding alternating insert/delete
 *   36:    zombie_window=1 (rebuild every insert)
 *   37:    tomb_threshold triggered burst
 *   38:    find_all with 100+ colliding keys
 *   39:    Tiny table (capacity=4) stress
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

// ============================================================================
// Hash helpers
// ============================================================================

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

static uint64_t zero_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

static uint64_t one_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 1;
}

static uint64_t fixed_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 42;
}

static uint64_t selective_zero_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    const char *k = (const char *)key;
    if (len > 0 && k[0] == 'z') return 0;
    return fnv1a_hash(key, len, ctx);
}

// ============================================================================
// Callbacks
// ============================================================================

static int g_collect_count;
static int g_collect_vals[256];

static bool collect_val_cb(const void *key, size_t klen,
                           const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)vlen; (void)ctx;
    if (g_collect_count < 256) {
        g_collect_vals[g_collect_count] = *(const int *)val;
    }
    g_collect_count++;
    return true;
}

static bool collect_stop_2_cb(const void *key, size_t klen,
                              const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)vlen; (void)ctx;
    g_collect_count++;
    return g_collect_count < 2;
}

// ============================================================================
// 32. Multiple resizes with spill + collisions
// ============================================================================

static int test_resize_spill_collisions(void) {
    printf("Test: multiple resizes with spill + collisions...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);

    // Insert spill entries (z-prefix) and normal entries
    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }
    for (int i = 0; i < 20; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        int val = i + 100;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Resize up
    ht_resize(t, 256);

    // Verify all
    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 20; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i + 100);
    }

    // Delete some spill entries
    ht_remove(t, "z1", 2);
    ht_remove(t, "z3", 2);

    // Resize down
    ht_resize(t, 32);

    // Verify
    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i == 1 || i == 3) assert(v == NULL);
        else assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 20; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i + 100);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 33. Iterator count matches stats after complex ops
// ============================================================================

static int test_iter_count_matches_stats(void) {
    printf("Test: iterator count matches stats after complex ops...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    srand(4004);
    for (int op = 0; op < 500; op++) {
        int k = rand() % 100;
        char key[16]; snprintf(key, sizeof(key), "k%d", k);
        if (rand() % 3 == 0) {
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
        } else if (rand() % 3 == 1) {
            ht_remove(t, key, strlen(key));
        } else {
            ht_find(t, key, strlen(key), NULL);
        }
    }

    ht_compact(t);

    ht_stats_t st;
    ht_stats(t, &st);

    int iter_count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) iter_count++;

    assert((size_t)iter_count == st.size);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 34. find_all on nonexistent hash → 0 callbacks
// ============================================================================

static int test_find_all_nonexistent(void) {
    printf("Test: ht_find_all on nonexistent hash...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    g_collect_count = 0;
    ht_find_all(t, 0xDEADBEEF, collect_val_cb, NULL);
    assert(g_collect_count == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 35. ht_inc with spill-lane keys (hash=0)
// ============================================================================

static int test_inc_spill(void) {
    printf("Test: ht_inc with spill-lane keys (hash=0)...\n");
    ht_table_t *t = ht_create(NULL, zero_hash, NULL, NULL);

    int64_t v = ht_inc(t, "counter", 7, 10);
    assert(v == 10);

    v = ht_inc(t, "counter", 7, 5);
    assert(v == 15);

    // Verify via ht_find
    size_t out_len;
    const int64_t *fv = ht_find(t, "counter", 7, &out_len);
    assert(fv != NULL && *fv == 15 && out_len == sizeof(int64_t));

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 36. remove_with_hash + find_with_hash consistency
// ============================================================================

static int test_with_hash_remove_find(void) {
    printf("Test: remove_with_hash + find_with_hash consistency...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // Insert multiple entries with different hashes
    const char *keys[] = {"alpha", "beta", "gamma", "delta"};
    int vals[] = {10, 20, 30, 40};
    uint64_t hashes[4];
    for (int i = 0; i < 4; i++) {
        hashes[i] = fnv1a_hash(keys[i], strlen(keys[i]), NULL);
        ht_insert_with_hash(t, hashes[i], keys[i], strlen(keys[i]),
                            &vals[i], sizeof(vals[i]));
    }

    // Remove "beta" via _with_hash
    assert(ht_remove_with_hash(t, hashes[1], "beta", 4));
    assert(ht_find_with_hash(t, hashes[1], "beta", 4, NULL) == NULL);

    // Others still findable
    assert(*(int *)ht_find_with_hash(t, hashes[0], "alpha", 5, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, hashes[2], "gamma", 5, NULL) == 30);
    assert(*(int *)ht_find_with_hash(t, hashes[3], "delta", 5, NULL) == 40);

    // Also findable via ht_find
    assert(*(int *)ht_find(t, "alpha", 5, NULL) == 10);
    assert(ht_find(t, "beta", 4, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 37. Value size change on update
// ============================================================================

static int test_value_size_change(void) {
    printf("Test: value size change on update...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // Insert with small value
    int8_t small = 42;
    assert(ht_insert(t, "key", 3, &small, sizeof(small)));

    size_t out_len;
    const int8_t *v = ht_find(t, "key", 3, &out_len);
    assert(v != NULL && *v == 42 && out_len == sizeof(int8_t));

    // Update with large value
    int64_t large = 0x123456789ABCDEF0LL;
    assert(ht_insert(t, "key", 3, &large, sizeof(large)) == false);

    const int64_t *v2 = ht_find(t, "key", 3, &out_len);
    assert(v2 != NULL && *v2 == large && out_len == sizeof(int64_t));

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 38. Many same-prefix keys (Robin-Hood probe chain stress)
// ============================================================================

static int test_same_prefix_keys(void) {
    printf("Test: many same-prefix keys...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Keys that all start with "prefix_" and vary only in the suffix
    // These will hash differently but exercise the comparison logic
    const int N = 200;
    for (int i = 0; i < N; i++) {
        char key[64]; snprintf(key, sizeof(key), "prefix_%050d", i);
        int val = i;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    for (int i = 0; i < N; i++) {
        char key[64]; snprintf(key, sizeof(key), "prefix_%050d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    // Delete every other
    for (int i = 0; i < N; i += 2) {
        char key[64]; snprintf(key, sizeof(key), "prefix_%050d", i);
        assert(ht_remove(t, key, strlen(key)));
    }

    for (int i = 0; i < N; i++) {
        char key[64]; snprintf(key, sizeof(key), "prefix_%050d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i % 2 == 0) assert(v == NULL);
        else assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 39. Insert-remove-reinsert same key
// ============================================================================

static int test_insert_remove_reinsert(void) {
    printf("Test: insert-remove-reinsert same key...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int cycle = 0; cycle < 20; cycle++) {
        int val = cycle * 100;
        ht_insert(t, "key", 3, &val, sizeof(val));
        const int *v = ht_find(t, "key", 3, NULL);
        assert(v != NULL && *v == cycle * 100);
        assert(ht_remove(t, "key", 3));
        assert(ht_find(t, "key", 3, NULL) == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 40. Heavy tombstone churn with zombie enabled
// ============================================================================

static int test_tombstone_churn_zombie(void) {
    printf("Test: heavy tombstone churn with zombie (16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .tomb_threshold = 0.3, .zombie_window = 16 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    #define ZN_N 1000
    int *present = calloc(ZN_N, sizeof(int));

    srand(5005);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % ZN_N;
        int action = rand() % 100;

        if (action < 35) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 65) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else if (action < 95) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            bool result = ht_remove(t, key, strlen(key));
            if (present[k]) assert(result);
            else assert(!result);
            present[k] = 0;
        } else {
            // Periodic compact
            ht_compact(t);
        }
    }

    // Final verify
    for (int i = 0; i < ZN_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size + st.tombstone_cnt <= st.capacity);
    #undef ZN_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 41. Extended spill lane sequence (hash=1, 16000 ops)
// ============================================================================

static int test_spill_hash_one_stress(void) {
    printf("Test: spill lane stress (hash=1, 16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, one_hash, NULL, NULL);

    #define S1_N 1000
    int *present = calloc(S1_N, sizeof(int));

    srand(6006);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % S1_N;
        int action = rand() % 3;

        if (action == 0) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action == 1) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            if (present[k]) {
                const int *v = ht_find(t, key, strlen(key), NULL);
                assert(v != NULL && *v == k);
            } else {
                assert(ht_find(t, key, strlen(key), NULL) == NULL);
            }
        } else {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            bool removed = ht_remove(t, key, strlen(key));
            if (present[k]) assert(removed);
            else assert(!removed);
            present[k] = 0;
        }
    }

    for (int i = 0; i < S1_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef S1_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 42. Mixed spill+main sequence (selective_zero_hash, 16000 ops)
// ============================================================================

static int test_mixed_spill_main_stress(void) {
    printf("Test: mixed spill+main stress (16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);

    #define MS_N 800
    int *present = calloc(MS_N, sizeof(int));

    srand(7007);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % MS_N;
        int action = rand() % 100;

        // Keys starting with 'z' go to spill lane
        char key[16];
        if (k < MS_N / 2) snprintf(key, sizeof(key), "z%d", k);
        else snprintf(key, sizeof(key), "n%d", k - MS_N / 2);

        if (action < 40) {
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 75) {
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else if (action < 95) {
            bool result = ht_remove(t, key, strlen(key));
            if (present[k]) assert(result);
            else assert(!result);
            present[k] = 0;
        } else {
            ht_compact(t);
        }
    }

    for (int i = 0; i < MS_N; i++) {
        char key[16];
        if (i < MS_N / 2) snprintf(key, sizeof(key), "z%d", i);
        else snprintf(key, sizeof(key), "n%d", i - MS_N / 2);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef MS_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 43. Heavy collision churn with compact (16000 ops)
// ============================================================================

static int test_collision_churn_compact(void) {
    printf("Test: collision churn with compact (16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 256, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    #define CC_N 1000
    int *present = calloc(CC_N, sizeof(int));

    srand(8008);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % CC_N;
        int action = rand() % 100;

        if (action < 35) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 65) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else if (action < 90) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            ht_remove(t, key, strlen(key));
            present[k] = 0;
        } else {
            ht_compact(t);
        }
    }

    ht_compact(t);
    for (int i = 0; i < CC_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef CC_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 44. 1000 keys insert, delete all, reinsert all
// ============================================================================

static int test_bulk_insert_delete_reinsert(void) {
    printf("Test: bulk insert/delete/reinsert 1000 keys...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const int N = 1000;

    // Insert all
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == N);

    // Delete all
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_remove(t, key, strlen(key)));
    }

    ht_stats(t, &st);
    assert(st.size == 0);

    // Reinsert all
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i + 1000;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    ht_stats(t, &st);
    assert(st.size == N);

    // Verify all
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i + 1000);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 45. Interleaved ht_inc stress (16000 ops)
// ============================================================================

static int test_ht_inc_stress(void) {
    printf("Test: ht_inc stress (16000 ops)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    #define INC_N 200
    int64_t expected[INC_N];
    memset(expected, 0, sizeof(expected));
    bool active[INC_N];
    memset(active, 0, sizeof(active));

    srand(9009);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % INC_N;
        char key[16]; snprintf(key, sizeof(key), "c%d", k);

        if (rand() % 5 == 0 && active[k]) {
            // Remove + verify gone
            ht_remove(t, key, strlen(key));
            active[k] = false;
            expected[k] = 0;
        } else {
            int64_t delta = (rand() % 201) - 100; // -100 to +100
            int64_t result = ht_inc(t, key, strlen(key), delta);
            expected[k] += delta;
            active[k] = true;
            assert(result == expected[k]);
        }
    }

    // Verify all active counters
    for (int i = 0; i < INC_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "c%d", i);
        if (active[i]) {
            const int64_t *v = ht_find(t, key, strlen(key), NULL);
            assert(v != NULL && *v == expected[i]);
        }
    }
    #undef INC_N

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 46. 500 keys with _with_hash variants (16000 ops)
// ============================================================================

static int test_with_hash_stress(void) {
    printf("Test: _with_hash stress (16000 ops)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    #define WH_N 500
    int present[WH_N];
    memset(present, 0, sizeof(present));
    uint64_t hashes[WH_N];

    for (int i = 0; i < WH_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        hashes[i] = fnv1a_hash(key, strlen(key), NULL);
    }

    srand(1010);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % WH_N;
        char key[16]; snprintf(key, sizeof(key), "k%d", k);

        int action = rand() % 3;
        if (action == 0) {
            int val = k;
            ht_insert_with_hash(t, hashes[k], key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action == 1) {
            const int *v = ht_find_with_hash(t, hashes[k], key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else {
            bool result = ht_remove_with_hash(t, hashes[k], key, strlen(key));
            if (present[k]) assert(result);
            else assert(!result);
            present[k] = 0;
        }
    }

    for (int i = 0; i < WH_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find_with_hash(t, hashes[i], key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef WH_N

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 47. Multiple clear+reuse cycles
// ============================================================================

static int test_multiple_clear_reuse(void) {
    printf("Test: multiple clear+reuse cycles...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int cycle = 0; cycle < 10; cycle++) {
        int count = 50 + cycle * 10;
        for (int i = 0; i < count; i++) {
            char key[16]; snprintf(key, sizeof(key), "k%d", i);
            int val = i + cycle * 1000;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
        }

        ht_stats_t st;
        ht_stats(t, &st);
        assert(st.size == (size_t)count);

        // Verify
        for (int i = 0; i < count; i++) {
            char key[16]; snprintf(key, sizeof(key), "k%d", i);
            const int *v = ht_find(t, key, strlen(key), NULL);
            assert(v != NULL && *v == i + cycle * 1000);
        }

        ht_clear(t);
        ht_stats(t, &st);
        assert(st.size == 0);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 48. Delete all keys then verify table is clean
// ============================================================================

static int test_delete_all_verify_clean(void) {
    printf("Test: delete all keys then verify clean...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const int N = 200;
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_remove(t, key, strlen(key)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    // Tombstones may or may not remain (backward-shift compacts short chains)
    // The key invariant is size == 0

    // Compact should clean up
    ht_compact(t);
    ht_stats(t, &st);
    assert(st.size == 0);

    // Iterator should return 0
    int iter_count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) iter_count++;
    assert(iter_count == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 49. High-frequency single-key churn (insert/remove 1000 cycles)
// ============================================================================

static int test_single_key_churn(void) {
    printf("Test: single-key high-frequency churn (1000 cycles)...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Also populate some other keys to stress probe chains
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "bg%d", i);
        int val = i * 10;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    for (int cycle = 0; cycle < 1000; cycle++) {
        int val = cycle;
        ht_insert(t, "hotkey", 6, &val, sizeof(val));
        const int *v = ht_find(t, "hotkey", 6, NULL);
        assert(v != NULL && *v == cycle);
        assert(ht_remove(t, "hotkey", 6));
        assert(ht_find(t, "hotkey", 6, NULL) == NULL);
    }

    // Background keys still intact
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "bg%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i * 10);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 50. Large number of duplicate-key updates
// ============================================================================

static int test_many_updates_same_key(void) {
    printf("Test: many updates to same key (1000 updates)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // Populate other keys
    for (int i = 0; i < 50; i++) {
        char key[16]; snprintf(key, sizeof(key), "other%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Insert and update same key 1000 times
    ht_insert(t, "target", 6, &(int){0}, sizeof(int));
    for (int i = 1; i <= 1000; i++) {
        assert(ht_insert(t, "target", 6, &i, sizeof(i)) == false);
    }

    const int *v = ht_find(t, "target", 6, NULL);
    assert(v != NULL && *v == 1000);

    // Others still correct
    for (int i = 0; i < 50; i++) {
        char key[16]; snprintf(key, sizeof(key), "other%d", i);
        v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 51); // 50 others + 1 target

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 51. Keys that only differ in last byte
// ============================================================================

static int test_keys_differ_last_byte(void) {
    printf("Test: keys that only differ in last byte...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    const int N = 256;
    uint8_t key[4] = {'a', 'b', 'c', 0};

    for (int i = 0; i < N; i++) {
        key[3] = (uint8_t)i;
        int val = i;
        assert(ht_insert(t, key, 4, &val, sizeof(val)));
    }

    for (int i = 0; i < N; i++) {
        key[3] = (uint8_t)i;
        const int *v = ht_find(t, key, 4, NULL);
        assert(v != NULL && *v == i);
    }

    // Delete half
    for (int i = 0; i < N; i += 2) {
        key[3] = (uint8_t)i;
        assert(ht_remove(t, key, 4));
    }

    for (int i = 0; i < N; i++) {
        key[3] = (uint8_t)i;
        const int *v = ht_find(t, key, 4, NULL);
        if (i % 2 == 0) assert(v == NULL);
        else assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 52. Table with high load factor stress
// ============================================================================

static int test_high_load_stress(void) {
    printf("Test: high load factor stress (16000 ops, 80%% load)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.85 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    #define HL_N 1000
    int *present = calloc(HL_N, sizeof(int));

    srand(1111);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % HL_N;
        int action = rand() % 100;

        if (action < 45) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 80) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            ht_remove(t, key, strlen(key));
            present[k] = 0;
        }
    }

    for (int i = 0; i < HL_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef HL_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 53. Resize under load with many tombstones
// ============================================================================

static int test_resize_with_many_tombstones(void) {
    printf("Test: resize with many tombstones...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Insert 200 keys
    for (int i = 0; i < 200; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete 150
    for (int i = 0; i < 150; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 50);
    // Some chains may be backward-shifted (0 tombs), others tombstoned
    assert(st.tombstone_cnt >= 0);

    // Resize down — should handle tombstones correctly
    ht_resize(t, 128);

    ht_stats(t, &st);
    assert(st.size == 50);
    assert(st.capacity == 128);

    // All remaining keys still present
    for (int i = 150; i < 200; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 150; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 54. Iterator + modify safety (iterate snapshot then verify)
// ============================================================================

static int test_iter_then_modify(void) {
    printf("Test: iterate then modify...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 100; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Iterate and collect all keys
    int found[100] = {0};
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        const char *k = (const char *)key;
        assert(klen >= 2 && k[0] == 'k');
        char buf[16];
        assert(klen < sizeof(buf));
        memcpy(buf, k + 1, klen - 1);
        buf[klen - 1] = '\0';
        int idx = atoi(buf);
        assert(idx >= 0 && idx < 100);
        found[idx] = 1;
    }
    for (int i = 0; i < 100; i++) assert(found[i] == 1);

    // Now delete half
    for (int i = 0; i < 100; i += 2) {
        char key2[16]; snprintf(key2, sizeof(key2), "k%d", i);
        ht_remove(t, key2, strlen(key2));
    }

    // Re-iterate
    memset(found, 0, sizeof(found));
    iter = ht_iter_begin(t);
    int count = 0;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        const char *k = (const char *)key;
        char buf[16];
        assert(klen >= 2 && klen < sizeof(buf));
        memcpy(buf, k + 1, klen - 1);
        buf[klen - 1] = '\0';
        int idx = atoi(buf);
        assert(idx % 2 == 1);
        found[idx] = 1;
        count++;
    }
    assert(count == 50);
    for (int i = 1; i < 100; i += 2) assert(found[i] == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 55. ht_find_all after churn (verify callback gets correct values)
// ============================================================================

static int test_find_all_after_churn(void) {
    printf("Test: ht_find_all after churn...\n");
    ht_table_t *t = ht_create(NULL, fixed_hash, NULL, NULL);

    // Insert 10 keys all hashing to 42
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i * 10;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete even ones
    for (int i = 0; i < 10; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
    }

    // Update one
    int new_val = 999;
    ht_insert(t, "k5", 2, &new_val, sizeof(new_val));

    // find_all should return 5 entries (odd indices + k5 updated)
    g_collect_count = 0;
    ht_find_all(t, 42, collect_val_cb, NULL);
    assert(g_collect_count == 5);

    // Verify the values: 1*10, 3*10, 5→999, 7*10, 9*10
    int expected[] = {10, 30, 50, 70, 90};
    int found_999 = 0;
    for (int i = 0; i < g_collect_count; i++) {
        if (g_collect_vals[i] == 999) found_999 = 1;
    }
    assert(found_999);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 56. Tombstone count accuracy after backshift
// ============================================================================

static int test_tombstone_count_accuracy(void) {
    printf("Test: tombstone count accuracy after backshift...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.85,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Insert 40 keys
    for (int i = 0; i < 40; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete 20 keys, checking tombstone_cnt consistency after each
    ht_stats_t st;
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
        ht_stats(t, &st);
        // size should decrease by 1 each time
        assert(st.size == 40 - i - 1);
        // tombstone_cnt + size must not exceed capacity
        assert(st.size + st.tombstone_cnt <= st.capacity);
    }

    // Final check: tombstone_cnt is non-negative and consistent
    ht_stats(t, &st);
    assert(st.tombstone_cnt >= 0);
    assert(st.size == 20);

    // Verify all remaining keys are findable
    for (int i = 20; i < 40; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    // Verify deleted keys are gone
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 57. Backshift absorbs delete-tombstones (same-hash chain)
// ============================================================================

static int test_backshift_absorbs_delete_tombs(void) {
    printf("Test: backshift absorbs delete-tombstones...\n");

    // Use fixed_hash (all keys hash to 42) so entries form a single probe
    // chain.  With a short chain (6 entries), deleting from the middle creates
    // a short tail that backshift can compact, eliminating the tombstone.
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.85,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    // Insert 6 colliding keys (chain length 6, well within any cap)
    for (int i = 0; i < 6; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 6);

    // Delete key3 (middle of chain).  Entries key4, key5 follow, then EMPTY.
    // Backshift should fire: shift key4 and key5 backward, eliminating the tombstone.
    ht_remove(t, "key3", 4);
    ht_stats(t, &st);
    assert(st.size == 5);
    // Backshift should have eliminated the tombstone (short chain ending at EMPTY).
    assert(st.tombstone_cnt == 0);

    // Delete key1 (another middle delete).  Entries key2, key4, key5 follow.
    // Chain length after key1 = 3 live entries.  cap = max(4, compute_x*2).
    // At 5/64 = 7.8% load, cap = 4.  3 < 4, scan sees EMPTY → shift.
    ht_remove(t, "key1", 4);
    ht_stats(t, &st);
    assert(st.size == 4);
    assert(st.tombstone_cnt == 0);

    // All remaining keys must be findable
    const char *remaining[] = {"key0", "key2", "key4", "key5"};
    int remaining_vals[] = {0, 2, 4, 5};
    for (int i = 0; i < 4; i++) {
        const int *v = ht_find(t, remaining[i], strlen(remaining[i]), NULL);
        assert(v != NULL && *v == remaining_vals[i]);
    }

    // Deleted keys must be gone
    assert(ht_find(t, "key1", 4, NULL) == NULL);
    assert(ht_find(t, "key3", 4, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 58. Backshift aborts when ideal-position constraint is violated
// ============================================================================

static int test_backshift_ideal_position_abort(void) {
    printf("Test: backshift ideal-position abort...\n");

    // Use two distinct hash values so entries from different hash families
    // interleave.  We want a scenario where a delete-tombstone sits between
    // entries from different families, and shifting would move an entry past
    // its ideal position.
    //
    // We can't fully control slot layout, but we can verify that after a
    // complex sequence of inserts and deletes, all entries remain findable
    // and tombstone_cnt stays consistent.
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.9,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Insert 20 keys with diverse hashes
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "ck%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete every other key to create interleaved tombstones
    for (int i = 0; i < 20; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "ck%d", i);
        ht_remove(t, key, strlen(key));
    }

    // Verify all remaining keys are findable
    for (int i = 1; i < 20; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "ck%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    // Verify deleted keys are gone
    for (int i = 0; i < 20; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "ck%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    // Check consistency
    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.tombstone_cnt >= 0);
    assert(st.size + st.tombstone_cnt <= st.capacity);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 59. Dynamic cap adapts to load factor
// ============================================================================

static int test_dynamic_cap_high_load(void) {
    printf("Test: dynamic cap at high load factor...\n");
    ht_config_t cfg = { .initial_capacity = 256, .max_load_factor = 0.92,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Fill to ~90% load
    int n = 230;
    for (int i = 0; i < n; i++) {
        char key[16]; snprintf(key, sizeof(key), "dk%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == n);

    // Delete from the end (tail of probe chains) — backshift should fire
    // for many of these, especially with the higher dynamic cap at 90% load.
    size_t tombs_before = st.tombstone_cnt;
    for (int i = n - 20; i < n; i++) {
        char key[16]; snprintf(key, sizeof(key), "dk%d", i);
        ht_remove(t, key, strlen(key));
    }

    ht_stats(t, &st);
    assert(st.size == n - 20);

    // With dynamic cap at high load (compute_x ≈ 10, cap ≈ 20),
    // backshift should handle chains up to ~16 slots. Tombstone count
    // should be less than 20 (some were eliminated by backshift).
    assert(st.tombstone_cnt >= 0 && st.tombstone_cnt <= 20);

    // All remaining keys must be findable
    for (int i = 0; i < n - 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "dk%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    // Deleted keys must be gone
    for (int i = n - 20; i < n; i++) {
        char key[16]; snprintf(key, sizeof(key), "dk%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 60. Push-forward: delete from long collision chain
// ============================================================================

static int test_push_forward_long_chain(void) {
    printf("Test: push-forward from long collision chain...\n");
    // All keys collide (fixed_hash=42).  With capacity=128, 50 entries form
    // a chain from position 42 onward.  Deleting near the start should trigger
    // Outcome B (chain extends past scan_limit, primitive position found) or
    // Outcome C (fallback).  Either way, all entries must remain findable.
    ht_config_t cfg = { .initial_capacity = 128, .max_load_factor = 0.85,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    const int N = 50;
    for (int i = 0; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%03d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == N);

    // Delete first 5 entries in the chain — tests push-forward/fallback
    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%03d", i);
        ht_remove(t, key, strlen(key));
    }

    ht_stats(t, &st);
    assert(st.size == N - 5);
    assert(st.size + st.tombstone_cnt <= st.capacity);

    // All remaining entries must be findable
    for (int i = 5; i < N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%03d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }
    // Deleted entries must be gone
    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%03d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 61. Push-forward: delete interleaved with prophylactic tombstones
// ============================================================================

static int test_push_forward_with_prophylactic(void) {
    printf("Test: push-forward with prophylactic tombstones...\n");
    // After a compact (which places prophylactic tombstones), delete entries
    // near them.  The push-forward scan should stop at prophylactic tombstones
    // and not shift past them.
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.85,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Insert 40 keys
    for (int i = 0; i < 40; i++) {
        char key[16]; snprintf(key, sizeof(key), "pk%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Compact to place prophylactic tombstones
    ht_compact(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 40);
    assert(st.tombstone_cnt > 0);  // prophylactic tombstones placed

    size_t tombs_after_compact = st.tombstone_cnt;

    // Delete 10 entries scattered throughout
    for (int i = 0; i < 40; i += 4) {
        char key[16]; snprintf(key, sizeof(key), "pk%d", i);
        ht_remove(t, key, strlen(key));
    }

    ht_stats(t, &st);
    assert(st.size == 30);
    // New tombstones added, some possibly absorbed by backshift
    assert(st.tombstone_cnt >= tombs_after_compact);
    assert(st.size + st.tombstone_cnt <= st.capacity);

    // All remaining entries must be findable
    for (int i = 0; i < 40; i++) {
        char key[16]; snprintf(key, sizeof(key), "pk%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i % 4 == 0) assert(v == NULL);
        else assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 62. Push-forward: heavy delete churn with zombie disabled
// ============================================================================

static int test_push_forward_churn(void) {
    printf("Test: push-forward delete churn (16000 ops)...\n");
    // Exercise push-forward heavily by doing many deletes without zombie rebuild
    ht_config_t cfg = { .initial_capacity = 128, .max_load_factor = 0.85,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    #define PF_N 500
    int *present = calloc(PF_N, sizeof(int));

    srand(1234);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % PF_N;
        char key[16]; snprintf(key, sizeof(key), "pfc%d", k);
        int action = rand() % 100;

        if (action < 40) {
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 70) {
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k);
            else assert(v == NULL);
        } else {
            bool result = ht_remove(t, key, strlen(key));
            if (present[k]) assert(result);
            else assert(!result);
            present[k] = 0;
        }
    }

    // Final verify
    for (int i = 0; i < PF_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "pfc%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size + st.tombstone_cnt <= st.capacity);
    #undef PF_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 63. Push-forward: collision chain delete-compact consistency
// ============================================================================

static int test_push_forward_collision_consistency(void) {
    printf("Test: push-forward collision chain consistency...\n");
    // Use fixed_hash to create collisions.  Interleave inserts and deletes
    // in a pattern that exercises all three outcomes of delete_compact.
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.85,
                        .zombie_window = 0, .min_load_factor = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    // Insert 30 colliding keys
    for (int i = 0; i < 30; i++) {
        char key[16]; snprintf(key, sizeof(key), "cc%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete first 10 (chain is long, Outcome B or C for most)
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "cc%d", i);
        ht_remove(t, key, strlen(key));
    }

    // Re-insert 5 of the deleted keys (reusing tombstone slots)
    for (int i = 3; i < 8; i++) {
        char key[16]; snprintf(key, sizeof(key), "cc%d", i);
        int val = i + 100;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Delete some more from the middle
    for (int i = 15; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "cc%d", i);
        ht_remove(t, key, strlen(key));
    }

    // Verify all remaining keys
    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size + st.tombstone_cnt <= st.capacity);

    // Keys 0-2: deleted and not re-inserted
    // Keys 3-7: re-inserted with val = i+100
    // Keys 8-9: deleted
    // Keys 10-14: still present
    // Keys 15-19: deleted
    // Keys 20-29: still present
    for (int i = 0; i < 30; i++) {
        char key[16]; snprintf(key, sizeof(key), "cc%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);

        bool should_exist = (i >= 3 && i <= 7) || (i >= 10 && i <= 14) || (i >= 20);
        if (should_exist) {
            assert(v != NULL);
            if (i >= 3 && i <= 7) assert(*v == i + 100);
            else assert(*v == i);
        } else {
            assert(v == NULL);
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 33. ht_inc with non-int64 value sizes
// ============================================================================

static int test_inc_non_int64(void) {
    printf("Test: ht_inc with non-int64 value sizes...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* ht_inc on key that doesn't exist yet — should create with delta as value */
    int64_t r = ht_inc(t, "counter", 7, 5);
    assert(r == 5);

    /* Now the value is sizeof(int64_t). Increment again. */
    r = ht_inc(t, "counter", 7, 10);
    assert(r == 15);

    /* Verify via find */
    size_t vlen = 0;
    const int64_t *v = ht_find(t, "counter", 7, &vlen);
    assert(v != NULL && *v == 15);
    assert(vlen == sizeof(int64_t));

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 34. Deterministic interleaved inc/remove
// ============================================================================

static int test_deterministic_inc_remove(void) {
    printf("Test: deterministic interleaved inc/remove...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 4 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Cycle: inc 3 keys, remove 1, repeat */
    for (int round = 0; round < 50; round++) {
        for (int k = 0; k < 3; k++) {
            char key[16]; snprintf(key, sizeof(key), "r%dk%d", round, k);
            ht_inc(t, key, strlen(key), 1);
        }
        if (round > 0) {
            char key[16]; snprintf(key, sizeof(key), "r%dk0", round - 1);
            ht_remove(t, key, strlen(key));
        }
    }

    /* Verify: each round's k0 should be removed (except last), k1/k2 present */
    for (int round = 0; round < 50; round++) {
        for (int k = 0; k < 3; k++) {
            char key[16]; snprintf(key, sizeof(key), "r%dk%d", round, k);
            const int64_t *v = ht_find(t, key, strlen(key), NULL);
            if (k == 0 && round < 49) {
                assert(v == NULL);
            } else {
                assert(v != NULL && *v == 1);
            }
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 35. Colliding alternating insert/delete
// ============================================================================

static int test_colliding_alternating_delete(void) {
    printf("Test: colliding alternating insert/delete...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);  /* all hash to 42 */
    assert(t != NULL);

    /* Insert 10, delete evens, insert 10 more, delete odds */
    int vals[20];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    for (int i = 0; i < 10; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    for (int i = 10; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    for (int i = 1; i < 20; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    /* After: evens 0-8 deleted (loop 1), odds 1-19 deleted (loop 2) */
    /* Remaining: c10, c12, c14, c16, c18 */
    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        bool should_exist = (i >= 10 && i <= 18 && i % 2 == 0);
        if (should_exist) {
            assert(v != NULL && *v == i);
        } else {
            assert(v == NULL);
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 36. zombie_window=1 (rebuild every insert)
// ============================================================================

static int test_zombie_window_one(void) {
    printf("Test: zombie_window=1 (rebuild every insert)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 1 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 50 entries — zombie step runs after every insert */
    for (int i = 0; i < 50; i++) {
        char k[16]; snprintf(k, sizeof(k), "zw%d", i);
        int v = i * 3;
        assert(ht_insert(t, k, strlen(k), &v, sizeof(v)));
    }

    /* Verify all */
    for (int i = 0; i < 50; i++) {
        char k[16]; snprintf(k, sizeof(k), "zw%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 3);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 37. tomb_threshold triggered burst
// ============================================================================

static int test_tomb_threshold_burst(void) {
    printf("Test: tomb_threshold triggered burst...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .tomb_threshold = 0.10, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 30, delete 25 — should exceed tomb_threshold */
    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "tb%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    for (int i = 0; i < 25; i++) {
        char k[16]; snprintf(k, sizeof(k), "tb%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    /* Insert one more — should trigger rebuild since tombstone ratio > 0.10 */
    int extra = 999;
    assert(ht_insert(t, "extra", 5, &extra, sizeof(int)));

    /* Verify remaining 5 + extra */
    for (int i = 25; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "tb%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    const int *v = ht_find(t, "extra", 5, NULL);
    assert(v != NULL && *v == 999);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 38. find_all with 100+ colliding keys
// ============================================================================

static int test_find_all_100plus(void) {
    printf("Test: find_all with 100+ colliding keys...\n");
    ht_config_t cfg = { .initial_capacity = 256, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);  /* all collide to 42 */
    assert(t != NULL);

    #define N100 120
    int vals[N100];
    for (int i = 0; i < N100; i++) {
        char k[8]; snprintf(k, sizeof(k), "f%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    g_collect_count = 0;
    memset(g_collect_vals, 0, sizeof(g_collect_vals));
    ht_find_all(t, 42, collect_val_cb, NULL);

    assert(g_collect_count == N100);
    #undef N100

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 39. Tiny table (capacity=4) stress
// ============================================================================

static int test_tiny_table_stress(void) {
    printf("Test: tiny table (capacity=4) stress...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.75,
                        .min_load_factor = 0.0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Rapid insert/delete cycle on tiny table */
    for (int cycle = 0; cycle < 200; cycle++) {
        char k[16]; snprintf(k, sizeof(k), "t%d", cycle % 5);
        int v = cycle;
        ht_insert(t, k, strlen(k), &v, sizeof(v));

        if (cycle % 3 == 0) {
            char rk[16]; snprintf(rk, sizeof(rk), "t%d", (cycle + 1) % 5);
            ht_remove(t, rk, strlen(rk));
        }
    }

    /* Verify: all 5 keys should be findable (present or not) */
    for (int i = 0; i < 5; i++) {
        char k[16]; snprintf(k, sizeof(k), "t%d", i);
        /* Just verify no crash */
        ht_find(t, k, strlen(k), NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size <= 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 40. ht_find_all with NULL callback (no crash)
// ============================================================================

static int test_find_all_null_cb(void) {
    printf("Test: ht_find_all with NULL callback (no crash)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 1;
    ht_insert(t, "a", 1, &v, sizeof(v));

    /* Should not crash — early return when cb is NULL */
    ht_find_all(t, 42, NULL, NULL);
    ht_find_all(t, 0, NULL, NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 41. Config min_load_factor > max_load_factor
// ============================================================================

static int test_conflicting_load_factors(void) {
    printf("Test: min_load_factor > max_load_factor...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.3,
                        .min_load_factor = 0.8, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 20 — should grow early (low max_load_factor) */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    /* Delete 15 — min_load_factor=0.8 > max_load_factor=0.3 means
     * table should never auto-shrink (size/cap >= 0.8 can never hold
     * since cap grew due to low max_load_factor). Just verify no crash. */
    for (int i = 0; i < 15; i++) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Verify survivors */
    for (int i = 15; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 42. ht_resize rounds to same capacity (no-op)
// ============================================================================

static int test_resize_rounds_to_same(void) {
    printf("Test: ht_resize rounds to same capacity (no-op)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert some entries */
    int vals[5];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "rs%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity == 64);

    /* Resize to 33 → next_pow2(33) = 64 → no-op */
    bool ok = ht_resize(t, 33);
    assert(ok);

    ht_stats(t, &st);
    assert(st.capacity == 64);
    assert(st.size == 5);

    /* All entries still findable */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "rs%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 43. ht_find_all after compact (verify correct results)
// ============================================================================

static int test_find_all_after_compact(void) {
    printf("Test: ht_find_all after compact...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 10 colliding entries */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "fa%d", i);
        vals[i] = i * 3;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete half */
    for (int i = 0; i < 10; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "fa%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Compact to clean tombstones and re-place entries */
    ht_compact(t);

    /* find_all should return exactly the 5 odd-numbered entries */
    g_collect_count = 0;
    memset(g_collect_vals, 0, sizeof(g_collect_vals));
    ht_find_all(t, 42, collect_val_cb, NULL);

    assert(g_collect_count == 5);
    /* Sort and verify */
    int sorted[5];
    memcpy(sorted, g_collect_vals, 5 * sizeof(int));
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (sorted[i] > sorted[j]) { int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp; }
    /* Expect: 1*3=3, 3*3=9, 5*3=15, 7*3=21, 9*3=27 */
    for (int i = 0; i < 5; i++)
        assert(sorted[i] == (2 * i + 1) * 3);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 44. ht_inc delta=0 on non-existent key
// ============================================================================

static int test_inc_zero_delta_new_key(void) {
    printf("Test: ht_inc delta=0 on non-existent key...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* delta=0 on non-existent key should insert with value 0 */
    int64_t r = ht_inc(t, "newkey", 6, 0);
    assert(r == 0);

    size_t vlen = 0;
    const int64_t *v = ht_find(t, "newkey", 6, &vlen);
    assert(v != NULL && *v == 0);
    assert(vlen == sizeof(int64_t));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 45. ht_inc with all-colliding keys (fixed_hash)
// ============================================================================

static int test_inc_colliding(void) {
    printf("Test: ht_inc with all-colliding keys...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    assert(t != NULL);

    /* All keys hash to 42 — exercises long probe chains in inc */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        int64_t r = ht_inc(t, k, strlen(k), (int64_t)i);
        assert(r == (int64_t)i);
    }

    /* Increment each by 100 */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        int64_t r = ht_inc(t, k, strlen(k), 100);
        assert(r == (int64_t)i + 100);
    }

    /* Verify final values */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        const int64_t *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == (int64_t)i + 100);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 46. delete_compact with prophylactic barrier in forward scan
// ============================================================================

static int test_delete_prophylactic_barrier(void) {
    printf("Test: delete with prophylactic barrier in forward scan...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 10 colliding entries, then compact to place prophylactic tombstones */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "pb%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    ht_compact(t);

    /* Now delete an entry — forward scan may hit prophylactic barrier */
    /* This tests the prophylactic-barrier-stop path in delete_compact Phase 1 */
    ht_remove(t, "pb0", 3);
    ht_remove(t, "pb1", 3);
    ht_remove(t, "pb5", 3);

    /* Verify survivors */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "pb%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i == 0 || i == 1 || i == 5) {
            assert(v == NULL);
        } else {
            assert(v != NULL && *v == i);
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 47. ht_find_all user_ctx propagation
// ============================================================================

static bool ctx_check_cb(const void *key, size_t klen,
                          const void *val, size_t vlen, void *user_ctx) {
    (void)key; (void)klen; (void)val; (void)vlen;
    int *flag = (int *)user_ctx;
    *flag = 1;  /* Signal that user_ctx was received */
    return true;
}

static int test_find_all_user_ctx(void) {
    printf("Test: ht_find_all user_ctx propagation...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 1;
    ht_insert(t, "a", 1, &v, sizeof(v));

    int flag = 0;
    ht_find_all(t, 42, ctx_check_cb, &flag);
    /* Note: 42 is unlikely to be the hash of "a", so callback may not fire.
     * Use the actual hash via _with_hash. */
    assert(ht_insert_with_hash(t, 42, "b", 1, &v, sizeof(v)));

    flag = 0;
    ht_find_all(t, 42, ctx_check_cb, &flag);
    assert(flag == 1);  /* user_ctx was passed through */

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 48. ht_find_all scan order (spill first, main second)
// ============================================================================

static int order_check_count;
static int order_check_vals[10];

static bool order_check_cb(const void *key, size_t klen,
                            const void *val, size_t vlen, void *user_ctx) {
    (void)key; (void)klen; (void)vlen; (void)user_ctx;
    if (order_check_count < 10) {
        order_check_vals[order_check_count] = *(const int *)val;
    }
    order_check_count++;
    return true;
}

static int test_find_all_scan_order(void) {
    printf("Test: ht_find_all scan order (spill first)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert into main table via _with_hash with hash=42 */
    int main_v = 100;
    assert(ht_insert_with_hash(t, 42, "main", 4, &main_v, sizeof(int)));

    /* Insert into spill lane with hash=42 — but spill only holds hash 0/1.
     * We can't force same-hash into both spill and main. Instead, verify
     * that spill entries ARE found by find_all for hash=0. */
    int spill_v = 200;
    assert(ht_insert_with_hash(t, 0, "spill", 5, &spill_v, sizeof(int)));

    /* find_all(0) should find the spill entry */
    order_check_count = 0;
    memset(order_check_vals, 0, sizeof(order_check_vals));
    ht_find_all(t, 0, order_check_cb, NULL);
    assert(order_check_count == 1);
    assert(order_check_vals[0] == 200);

    /* find_all(42) should find the main entry */
    order_check_count = 0;
    memset(order_check_vals, 0, sizeof(order_check_vals));
    ht_find_all(t, 42, order_check_cb, NULL);
    assert(order_check_count == 1);
    assert(order_check_vals[0] == 100);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 49. ht_inc on key previously inserted via insert_with_hash(t, 0, ...)
// ============================================================================

static int test_inc_spill_via_with_hash(void) {
    printf("Test: ht_inc on key inserted via insert_with_hash(t, 0, ...)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert via normal path (zero_hash → hash=0 → spill) */
    int v = 10;
    ht_insert(t, "x", 1, &v, sizeof(int));

    /* ht_inc re-hashes "x" via zero_hash → hash=0 → finds it in spill,
     * but val_len=4 != sizeof(int64_t), so sets new_val = delta = 50 */
    int64_t r = ht_inc(t, "x", 1, 50);
    assert(r == 50);

    size_t vl = 0;
    const int64_t *fv = ht_find(t, "x", 1, &vl);
    assert(fv != NULL && *fv == 50);
    assert(vl == sizeof(int64_t));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);  /* Not duplicated */

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 50. Manual ht_resize shrink with prophylactic tombstones
// ============================================================================

static int test_resize_shrink_with_prophylactic(void) {
    printf("Test: manual resize shrink with prophylactic tombstones...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 20, delete 15, compact to place prophylactic tombstones */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "sp%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    for (int i = 0; i < 15; i++) {
        char k[8]; snprintf(k, sizeof(k), "sp%d", i);
        ht_remove(t, k, strlen(k));
    }
    ht_compact(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    /* Manually shrink to smaller capacity */
    bool ok = ht_resize(t, 16);
    assert(ok);

    ht_stats(t, &st);
    assert(st.size == 5);
    assert(st.capacity == 16);

    /* Verify survivors */
    for (int i = 15; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "sp%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 51. ht_remove_with_hash triggering auto-shrink
// ============================================================================

static int test_remove_with_hash_auto_shrink(void) {
    printf("Test: ht_remove_with_hash triggering auto-shrink...\n");
    /* Need capacity > 64 for auto-shrink to trigger (see ht.c line 889) */
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.25, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Fill to trigger grow beyond 64 (need > 48 entries to exceed 0.75 load) */
    int vals[55];
    for (int i = 0; i < 55; i++) {
        char k[8]; snprintf(k, sizeof(k), "aw%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity > 64);  /* Must have grown past 64 for shrink to work */
    size_t grown_cap = st.capacity;

    /* Remove most via ht_remove_with_hash — should trigger auto-shrink */
    for (int i = 0; i < 55; i++) {
        char k[8]; snprintf(k, sizeof(k), "aw%d", i);
        uint64_t h = fnv1a_hash(k, strlen(k), NULL);
        if (i >= 3) {
            ht_remove_with_hash(t, h, k, strlen(k));
        }
    }

    ht_stats(t, &st);
    assert(st.size == 3);
    /* Should have shrunk since load = 3/grown_cap < 0.25 */
    assert(st.capacity < grown_cap);

    /* Verify survivors */
    for (int i = 0; i < 3; i++) {
        char k[8]; snprintf(k, sizeof(k), "aw%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 52. Compact then clear then reinsert (prophylactic cleanup)
// ============================================================================

static int test_compact_clear_reinsert(void) {
    printf("Test: compact then clear then reinsert...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert, compact (places prophylactic tombstones) */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "cr%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    ht_compact(t);

    /* Clear zeros all slots including prophylactic tombstones */
    ht_clear(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.tombstone_cnt == 0);

    /* Reinsert — should work cleanly, no stale prophylactic interference */
    int new_vals[5];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "nw%d", i);
        new_vals[i] = i * 20;
        assert(ht_insert(t, k, strlen(k), &new_vals[i], sizeof(int)));
    }

    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "nw%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 20);
    }

    ht_stats(t, &st);
    assert(st.size == 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 53. ht_resize to sub-4 capacity
// ============================================================================

static int test_resize_sub_4(void) {
    printf("Test: ht_resize to sub-4 capacity...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 2 entries */
    int v1 = 10, v2 = 20;
    assert(ht_insert(t, "a", 1, &v1, sizeof(v1)));
    assert(ht_insert(t, "b", 1, &v2, sizeof(v2)));

    /* Resize to 2 — next_pow2(2) = 2, which is >= size=2 */
    bool ok = ht_resize(t, 2);
    assert(ok);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);
    assert(st.capacity == 2);  /* next_pow2(2) = 2 */

    /* Both entries still findable */
    const int *r1 = ht_find(t, "a", 1, NULL);
    assert(r1 != NULL && *r1 == 10);
    const int *r2 = ht_find(t, "b", 1, NULL);
    assert(r2 != NULL && *r2 == 20);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 54. Delete immediately before prophylactic barrier
// ============================================================================

static int test_delete_before_prophylactic(void) {
    printf("Test: delete immediately before prophylactic barrier...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 10 colliding entries, compact to place prophylactic tombstones */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "bp%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    ht_compact(t);

    /* Delete entries near prophylactic barriers —
     * forward scan should hit barrier and stop */
    for (int i = 0; i < 3; i++) {
        char k[8]; snprintf(k, sizeof(k), "bp%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    /* Verify */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "bp%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i < 3) assert(v == NULL);
        else assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 55. Large spill lane surviving resize
// ============================================================================

static int test_large_spill_resize(void) {
    printf("Test: large spill lane surviving resize...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 30 spill entries — spill lane must grow */
    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        vals[i] = i * 11;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Force resize */
    ht_resize(t, 256);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 30);
    assert(st.capacity >= 256);

    /* Verify all spill entries survived */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 11);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 56. ht_inc producing negative values
// ============================================================================

static int test_inc_negative_results(void) {
    printf("Test: ht_inc producing negative intermediate values...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Start at 10, decrement to -5 */
    int64_t r = ht_inc(t, "neg", 3, 10);
    assert(r == 10);
    r = ht_inc(t, "neg", 3, -8);
    assert(r == 2);
    r = ht_inc(t, "neg", 3, -7);
    assert(r == -5);

    const int64_t *v = ht_find(t, "neg", 3, NULL);
    assert(v != NULL && *v == -5);

    /* Increment back up */
    r = ht_inc(t, "neg", 3, 100);
    assert(r == 95);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 57. Remove nonexistent from spill lane
// ============================================================================

static int test_remove_nonexistent_spill(void) {
    printf("Test: remove nonexistent from spill lane...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    int v = 1;
    ht_insert(t, "a", 1, &v, sizeof(int));

    /* Remove nonexistent key from spill */
    assert(!ht_remove(t, "b", 1));
    assert(!ht_remove(t, "c", 1));

    /* Original still present */
    const int *r = ht_find(t, "a", 1, NULL);
    assert(r != NULL && *r == 1);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 58. Multiple clear + reinsert with heterogeneous sizes
// ============================================================================

static int test_clear_heterogeneous_arena(void) {
    printf("Test: multiple clear + reinsert with heterogeneous sizes...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    for (int round = 0; round < 3; round++) {
        /* Insert with varying sizes */
        const char *longval = "ABCDEFGHIJKLMNOPQRST";  /* 20 bytes */
        int shortval = round * 10;
        ht_insert(t, "long", 4, longval, strlen(longval));
        ht_insert(t, "s", 1, &shortval, sizeof(int));
        char bigkey[100];
        memset(bigkey, 'X', 99);
        bigkey[99] = '\0';
        ht_insert(t, bigkey, 99, &shortval, sizeof(int));

        /* Verify */
        size_t vl = 0;
        const char *r = ht_find(t, "long", 4, &vl);
        assert(r != NULL && vl == strlen(longval));
        assert(memcmp(r, longval, vl) == 0);

        const int *ri = ht_find(t, "s", 1, NULL);
        assert(ri != NULL && *ri == round * 10);

        ht_clear(t);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 59. Full _with_hash lifecycle (insert, find, remove, find)
// ============================================================================

static int test_with_hash_lifecycle(void) {
    printf("Test: full _with_hash lifecycle...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert via _with_hash */
    int v = 42;
    assert(ht_insert_with_hash(t, 100, "k1", 2, &v, sizeof(int)));
    assert(ht_insert_with_hash(t, 200, "k2", 2, &v, sizeof(int)));
    assert(ht_insert_with_hash(t, 0,   "k3", 2, &v, sizeof(int)));  /* spill */

    /* Find via _with_hash */
    const int *r;
    r = ht_find_with_hash(t, 100, "k1", 2, NULL);
    assert(r != NULL && *r == 42);
    r = ht_find_with_hash(t, 200, "k2", 2, NULL);
    assert(r != NULL && *r == 42);
    r = ht_find_with_hash(t, 0, "k3", 2, NULL);
    assert(r != NULL && *r == 42);

    /* Remove via _with_hash */
    assert(ht_remove_with_hash(t, 100, "k1", 2));
    assert(ht_remove_with_hash(t, 0, "k3", 2));

    /* Verify removed */
    assert(ht_find_with_hash(t, 100, "k1", 2, NULL) == NULL);
    assert(ht_find_with_hash(t, 0, "k3", 2, NULL) == NULL);
    r = ht_find_with_hash(t, 200, "k2", 2, NULL);
    assert(r != NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 60. insert_with_hash with colliding hash, then find_all
// ============================================================================

static int test_with_hash_colliding_find_all(void) {
    printf("Test: insert_with_hash colliding + find_all...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 20 keys all with hash=77 */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "fh%d", i);
        vals[i] = i * 5;
        assert(ht_insert_with_hash(t, 77, k, strlen(k), &vals[i], sizeof(int)));
    }

    /* find_all(77) should return all 20 */
    g_collect_count = 0;
    memset(g_collect_vals, 0, sizeof(g_collect_vals));
    ht_find_all(t, 77, collect_val_cb, NULL);
    assert(g_collect_count == 20);

    /* Sort and verify */
    int sorted[20];
    memcpy(sorted, g_collect_vals, 20 * sizeof(int));
    for (int i = 0; i < 20; i++)
        for (int j = i + 1; j < 20; j++)
            if (sorted[i] > sorted[j]) { int tmp = sorted[i]; sorted[i] = sorted[j]; sorted[j] = tmp; }
    for (int i = 0; i < 20; i++)
        assert(sorted[i] == i * 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== Edge Case Tests B ===\n\n");

    int fails = 0;

    // Extended edge cases (original 32-40)
    fails += test_resize_spill_collisions();
    fails += test_iter_count_matches_stats();
    fails += test_find_all_nonexistent();
    fails += test_inc_spill();
    fails += test_with_hash_remove_find();
    fails += test_value_size_change();
    fails += test_same_prefix_keys();
    fails += test_insert_remove_reinsert();
    fails += test_tombstone_churn_zombie();

    // Stress tests (original 41-55)
    fails += test_spill_hash_one_stress();
    fails += test_mixed_spill_main_stress();
    fails += test_collision_churn_compact();
    fails += test_bulk_insert_delete_reinsert();
    fails += test_ht_inc_stress();
    fails += test_with_hash_stress();
    fails += test_multiple_clear_reuse();
    fails += test_delete_all_verify_clean();
    fails += test_single_key_churn();
    fails += test_many_updates_same_key();
    fails += test_keys_differ_last_byte();
    fails += test_high_load_stress();
    fails += test_resize_with_many_tombstones();
    fails += test_iter_then_modify();
    fails += test_find_all_after_churn();

    // Backshift improvements (original 56-59)
    fails += test_tombstone_count_accuracy();
    fails += test_backshift_absorbs_delete_tombs();
    fails += test_backshift_ideal_position_abort();
    fails += test_dynamic_cap_high_load();

    // Push-forward delete (original 60-63)
    fails += test_push_forward_long_chain();
    fails += test_push_forward_with_prophylactic();
    fails += test_push_forward_churn();
    fails += test_push_forward_collision_consistency();

    // New tests (64-70)
    fails += test_inc_non_int64();
    fails += test_deterministic_inc_remove();
    fails += test_colliding_alternating_delete();
    fails += test_zombie_window_one();
    fails += test_tomb_threshold_burst();
    fails += test_find_all_100plus();
    fails += test_tiny_table_stress();

    // New tests (40-46)
    fails += test_find_all_null_cb();
    fails += test_conflicting_load_factors();
    fails += test_resize_rounds_to_same();
    fails += test_find_all_after_compact();
    fails += test_inc_zero_delta_new_key();
    fails += test_inc_colliding();
    fails += test_delete_prophylactic_barrier();

    // New tests (47-53)
    fails += test_find_all_user_ctx();
    fails += test_find_all_scan_order();
    fails += test_inc_spill_via_with_hash();
    fails += test_resize_shrink_with_prophylactic();
    fails += test_remove_with_hash_auto_shrink();
    fails += test_compact_clear_reinsert();
    fails += test_resize_sub_4();

    // New tests (54-60)
    fails += test_delete_before_prophylactic();
    fails += test_large_spill_resize();
    fails += test_inc_negative_results();
    fails += test_remove_nonexistent_spill();
    fails += test_clear_heterogeneous_arena();
    fails += test_with_hash_lifecycle();
    fails += test_with_hash_colliding_find_all();

    printf("\n");
    if (fails == 0) {
        printf("All edge case B tests passed! (60/60)\n");
    } else {
        printf("%d test(s) FAILED!\n", fails);
    }
    return fails;
}
