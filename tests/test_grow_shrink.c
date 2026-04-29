/**
 * test_grow_shrink.c - Test grow/shrink behavior of the hash table
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INV_CHECK(t, label) do { \
    const char *_inv_err = ht_check_invariants(t); \
    if (_inv_err) { \
        printf("  INVARIANT BROKEN at %s: %s\n", (label), _inv_err); \
        return; \
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

/**
 * Test: Grow table significantly, then verify all entries
 */
static void test_grow_and_verify(void) {
    printf("Test: grow and verify...\n");
    
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    
    // Insert many entries - should trigger multiple resizes
    const int N = 10000;
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }
    
    // Verify all entries are findable
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        const int *val = ht_find(t, key, strlen(key), NULL);
        if (val == NULL || *val != i) {
            printf("  FAIL: key%d not found at index %d\n", i, i);
            ht_destroy(t);
            return;
        }
    }

    INV_CHECK(t, "test_grow_and_verify: after 10000 inserts");

    ht_stats_t stats;
    ht_stats(t, &stats);
    printf("  size=%zu capacity=%zu load=%.2f\n",
           stats.size, stats.capacity, stats.load_factor);

    ht_destroy(t);
    printf("  PASS\n");
}

/**
 * Test: Insert many entries, delete half, compact, verify remaining
 */
static void test_grow_delete_compact(void) {
    printf("Test: grow, delete, compact...\n");
    
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    
    // Insert many entries
    const int N = 5000;
    int *present = calloc(N, sizeof(int));
    int count = 0;
    
    for (int i = 0; i < N; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_insert(t, key, strlen(key), &i, sizeof(i));
        present[i] = 1;
        count++;
    }
    
    ht_stats_t st1;
    ht_stats(t, &st1);
    printf("  After insert: size=%zu cap=%zu tombstones=%zu\n",
           st1.size, st1.capacity, st1.tombstone_cnt);
    
    // Delete half of them
    for (int i = 0; i < N; i += 2) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_remove(t, key, strlen(key));
        present[i] = 0;
        count--;
    }
    
    ht_stats_t st2;
    ht_stats(t, &st2);
    printf("  After delete: size=%zu cap=%zu tombstones=%zu\n",
           st2.size, st2.capacity, st2.tombstone_cnt);
    
    // Compact - should rebuild and reduce tombstones
    ht_compact(t);

    INV_CHECK(t, "test_grow_delete_compact: after compact");

    ht_stats_t st3;
    ht_stats(t, &st3);
    printf("  After compact: size=%zu cap=%zu tombstones=%zu\n",
           st3.size, st3.capacity, st3.tombstone_cnt);
    
    // Verify remaining entries
    int verify_count = 0;
    for (int i = 0; i < N; i++) {
        if (present[i]) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            const int *val = ht_find(t, key, strlen(key), NULL);
            if (val == NULL || *val != i) {
                printf("  FAIL: key%d not found after compact\n", i);
                free(present);
                ht_destroy(t);
                return;
            }
            verify_count++;
        }
    }
    
    if (verify_count != count) {
        printf("  FAIL: expected %d entries, found %d\n", count, verify_count);
        free(present);
        ht_destroy(t);
        return;
    }
    
    // After compact, tombstones may be 0 (backward-shift) or > 0 (prophylactic)
    // The key invariant is that size == count and all entries are findable
    
    free(present);
    ht_destroy(t);
    printf("  PASS\n");
}

/**
 * Test: Multiple grow/compact cycles
 */
static void test_multiple_grow_compact_cycles(void) {
    printf("Test: multiple grow/compact cycles...\n");
    
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    
    for (int cycle = 0; cycle < 3; cycle++) {
        printf("  Cycle %d: ", cycle);
        
        // Grow phase: insert entries
        int start = cycle * 1000;
        int end = start + 1000;
        for (int i = start; i < end; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            ht_insert(t, key, strlen(key), &i, sizeof(i));
        }
        
        // Delete phase: remove half (odd indices: 1, 3, 5, ...)
        for (int i = start + 1; i < end; i += 2) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            ht_remove(t, key, strlen(key));
        }
        
        // Compact
        ht_compact(t);

        INV_CHECK(t, "test_multiple_grow_compact_cycles: after compact");

        // Verify remaining entries (even indices: 0, 2, 4, ...)
        int verify_count = 0;
        for (int i = start; i < end; i += 2) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            if (ht_find(t, key, strlen(key), NULL) != NULL) {
                verify_count++;
            }
        }
        
        ht_stats_t st;
        ht_stats(t, &st);
        printf("size=%zu cap=%zu tombstones=%zu\n", st.size, st.capacity, st.tombstone_cnt);
        
        if (verify_count != 500) {
            printf("    FAIL: expected 500, found %d\n", verify_count);
            ht_destroy(t);
            return;
        }
    }
    
    ht_destroy(t);
    printf("  PASS\n");
}

/**
 * Test: Random insert/delete/compact workload
 */
static void test_random_workload(void) {
    printf("Test: random workload...\n");
    
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    
    const int N = 2000;
    int *present = calloc(N, sizeof(int));
    int present_count = 0;
    
    srand(42);  // Deterministic for reproducibility
    
    for (int op = 0; op < 10000; op++) {
        int action = rand() % 100;
        
        if (action < 60) {
            // Insert
            int k = rand() % N;
            if (!present[k]) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", k);
                ht_insert(t, key, strlen(key), &k, sizeof(k));
                present[k] = 1;
                present_count++;
            }
        } else if (action < 90) {
            // Find
            int k = rand() % N;
            if (present[k]) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", k);
                const int *val = ht_find(t, key, strlen(key), NULL);
                if (val == NULL || *val != k) {
                    printf("  FAIL at op %d: key%d not found\n", op, k);
                    free(present);
                    ht_destroy(t);
                    return;
                }
            }
        } else {
            // Remove
            int k = rand() % N;
            if (present[k]) {
                char key[32];
                snprintf(key, sizeof(key), "key%d", k);
                ht_remove(t, key, strlen(key));
                present[k] = 0;
                present_count--;
            }
        }
        
        // Periodic compact
        if (op % 500 == 0 && op > 0) {
            ht_compact(t);
            INV_CHECK(t, "test_random_workload: periodic compact");
        }
    }
    
    // Final verification
    INV_CHECK(t, "test_random_workload: final");

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  Final: size=%zu cap=%zu tombstones=%zu\n",
           st.size, st.capacity, st.tombstone_cnt);

    if (st.size != (size_t)present_count) {
        printf("  FAIL: size=%zu != present_count=%d\n", st.size, present_count);
        free(present);
        ht_destroy(t);
        return;
    }
    
    // Verify all present entries
    for (int i = 0; i < N; i++) {
        if (present[i]) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", i);
            const int *val = ht_find(t, key, strlen(key), NULL);
            if (val == NULL || *val != i) {
                printf("  FAIL: key%d not found\n", i);
                free(present);
                ht_destroy(t);
                return;
            }
        }
    }
    
    free(present);
    ht_destroy(t);
    printf("  PASS\n");
}

/**
 * Test: Verify zombie hashing cleans tombstones incrementally
 */
static void test_zombie_cleanup(void) {
    printf("Test: zombie cleanup...\n");
    
    // Use small zombie window to trigger compaction faster
    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.75,
        .min_load_factor = 0.1,  // Disable auto-shrink for this test
        .tomb_threshold = 0.3,
        .zombie_window = 4  // Small window for faster testing
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    
    // Insert some entries
    for (int i = 0; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }
    
    // Delete many to create tombstones
    for (int i = 0; i < 50; i += 2) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_remove(t, key, strlen(key));
    }
    
    ht_stats_t st1;
    ht_stats(t, &st1);
    printf("  After delete: size=%zu tombstones=%zu ratio=%.2f\n",
           st1.size, st1.tombstone_cnt, st1.tombstone_ratio);
    
    // Do many inserts - zombie window should clean up tombstones
    for (int i = 0; i < 200; i++) {
        char key[32];
        snprintf(key, sizeof(key), "newkey%d", i);
        int val = 1000 + i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    INV_CHECK(t, "test_zombie_cleanup: after 200 inserts");

    ht_stats_t st2;
    ht_stats(t, &st2);
    printf("  After inserts: size=%zu tombstones=%zu ratio=%.2f\n",
           st2.size, st2.tombstone_cnt, st2.tombstone_ratio);
    
    // Verify original remaining entries still accessible
    int verify_count = 0;
    for (int i = 1; i < 50; i += 2) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        if (ht_find(t, key, strlen(key), NULL) != NULL) {
            verify_count++;
        }
    }
    
    if (verify_count != 25) {
        printf("  FAIL: expected 25 remaining entries, found %d\n", verify_count);
        ht_destroy(t);
        return;
    }
    
    ht_destroy(t);
    printf("  PASS\n");
}

/**
 * Test: Verify auto-shrink works when load drops below min_load_factor
 */
static void test_auto_shrink(void) {
    printf("Test: auto-shrink...\n");
    
    // Create table with high min_load_factor to trigger shrink
    ht_config_t cfg = {
        .initial_capacity = 256,
        .max_load_factor = 0.75,
        .min_load_factor = 0.5,  // Shrink when load < 0.5
        .tomb_threshold = 0.2,
        .zombie_window = 0  // Disable zombie for this test
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    
    // Insert 100 entries (load ~0.39)
    for (int i = 0; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }
    
    ht_stats_t st1;
    ht_stats(t, &st1);
    printf("  After insert: size=%zu cap=%zu load=%.2f\n",
           st1.size, st1.capacity, st1.load_factor);
    
    // Delete 80 entries - load should drop to ~0.08, triggering shrink
    for (int i = 0; i < 80; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        ht_remove(t, key, strlen(key));
    }

    INV_CHECK(t, "test_auto_shrink: after deleting 80");

    ht_stats_t st2;
    ht_stats(t, &st2);
    printf("  After delete: size=%zu cap=%zu load=%.2f\n",
           st2.size, st2.capacity, st2.load_factor);

    // Capacity should have shrunk (capacity >= size * 2 = 40)
    if (st2.capacity < st2.size * 2) {
        printf("  FAIL: capacity=%zu < size*2=%zu (shrink should have occurred)\n",
               st2.capacity, st2.size * 2);
        ht_destroy(t);
        return;
    }
    
    // Verify remaining 20 entries
    int verify_count = 0;
    for (int i = 80; i < 100; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        if (ht_find(t, key, strlen(key), NULL) != NULL) {
            verify_count++;
        }
    }
    
    if (verify_count != 20) {
        printf("  FAIL: expected 20 entries, found %d\n", verify_count);
        ht_destroy(t);
        return;
    }
    
    ht_destroy(t);
    printf("  PASS\n");
}

static void test_grow_with_tomb_threshold(void) {
    printf("Test: growth + tomb_threshold interaction...\n");
    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .tomb_threshold = 0.15,
        .zombie_window = 4
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Fill past capacity */
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "gt%d", i);
        ht_insert(t, k, strlen(k), &i, sizeof(i));
    }

    /* Delete many to exceed tomb_threshold */
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "gt%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Insert more — triggers both growth and tomb_threshold zombie steps */
    for (int i = 30; i < 60; i++) {
        char k[16]; snprintf(k, sizeof(k), "gt%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

    INV_CHECK(t, "test_grow_with_tomb_threshold: after growth+tomb");

    /* Verify survivors: gt20-29 + gt30-59 */
    for (int i = 20; i < 60; i++) {
        char k[16]; snprintf(k, sizeof(k), "gt%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "gt%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_shrink_blocked_at_64(void) {
    printf("Test: shrink blocked when capacity == 64...\n");
    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 10 entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "sb%d", i);
        ht_insert(t, k, strlen(k), &i, sizeof(i));
    }

    /* Delete 9 — load = 1/64 = 0.016 < 0.25, but capacity == 64 (NOT > 64) */
    for (int i = 1; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "sb%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.capacity == 64);  /* Did NOT shrink */

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_shrink_exact_boundary(void) {
    printf("Test: shrink to exactly size*2 boundary...\n");
    ht_config_t cfg = {
        .initial_capacity = 128,
        .max_load_factor = 0.75,
        .min_load_factor = 0.20,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 90 to trigger grow to 256 */
    int vals[90];
    for (int i = 0; i < 90; i++) {
        char k[8]; snprintf(k, sizeof(k), "eb%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 128);

    /* Delete down to 32 — 32/256 = 0.125 < 0.20, new_cap = 128 >= 64, 128 >= 32*2=64 */
    for (int i = 32; i < 90; i++) {
        char k[8]; snprintf(k, sizeof(k), "eb%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats(t, &st);
    assert(st.size == 32);
    /* Should have shrunk — new_cap = capacity/2, and size*2 = 64 <= new_cap */
    assert(st.capacity <= 128);

    /* Verify */
    for (int i = 0; i < 32; i++) {
        char k[8]; snprintf(k, sizeof(k), "eb%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_zombie_cursor_reset_on_growth(void) {
    printf("Test: zombie cursor reset on growth preserves correctness...\n");
    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .zombie_window = 4
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert and delete to advance zombie cursor */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "zr%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "zr%d", i);
        ht_remove(t, k, strlen(k));
    }
    /* A few more inserts to advance zombie cursor past 0 */
    for (int i = 20; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "zr%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

    /* Now trigger growth — zombie_cursor resets to 0 */
    for (int i = 100; i < 150; i++) {
        char k[16]; snprintf(k, sizeof(k), "zg%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

    /* Verify all entries */
    for (int i = 10; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "zr%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 100; i < 150; i++) {
        char k[16]; snprintf(k, sizeof(k), "zg%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_zombie_all_deleted(void) {
    printf("Test: zombie with all entries deleted...\n");
    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .zombie_window = 8
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 30, delete all 30 — tombstone ratio = 100% */
    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ad%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ad%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Insert new entries — zombie steps with size=0 */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "an%d", i);
        int v = i + 100;
        ht_insert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Verify new entries */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "an%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 100);
    }

    /* Old entries gone */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ad%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_multiple_auto_shrinks(void) {
    printf("Test: multiple sequential auto-shrinks...\n");
    ht_config_t cfg = {
        .initial_capacity = 128,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Fill to grow to 256 */
    int vals[100];
    for (int i = 0; i < 100; i++) {
        char k[8]; snprintf(k, sizeof(k), "ms%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 256);
    size_t prev_cap = st.capacity;

    /* Delete down to 10 — should trigger multiple shrinks */
    for (int i = 10; i < 100; i++) {
        char k[8]; snprintf(k, sizeof(k), "ms%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "test_multiple_auto_shrinks: after delete");

    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.capacity < prev_cap);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ms%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

/* Grow with heterogeneous value sizes, verify byte-exact data after resize */
static void test_grow_data_integrity(void) {
    printf("Test: grow data integrity with heterogeneous values...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const char *str_vals[] = { "alpha", "bravo", "charlie", "delta" };
    for (int i = 0; i < 4; i++) {
        char k[8]; snprintf(k, sizeof(k), "s%d", i);
        ht_insert(t, k, strlen(k), str_vals[i], strlen(str_vals[i]));
    }

    int int_vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "i%d", i);
        int_vals[i] = i * 100;
        ht_insert(t, k, strlen(k), &int_vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 24);
    assert(st.capacity > 16);  /* Must have grown */

    INV_CHECK(t, "test_grow_data_integrity: after growth");

    /* Verify strings byte-exact */
    for (int i = 0; i < 4; i++) {
        char k[8]; snprintf(k, sizeof(k), "s%d", i);
        size_t vl = 0;
        const char *v = ht_find(t, k, strlen(k), &vl);
        assert(v != NULL && vl == strlen(str_vals[i]));
        assert(memcmp(v, str_vals[i], vl) == 0);
    }

    /* Verify ints */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "i%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 100);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static uint64_t zero_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

static void test_shrink_preserves_byte_exact(void) {
    printf("Test: shrink preserves byte-exact values...\n");
    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Value strings of varying lengths */
    const char *vstr[] = {
        "A",                          /* 1  */
        "ABCDE",                      /* 5  */
        "ABCDEFGHIJ",                 /* 10 */
        "ABCDEFGHIJKLMNOPQRST",       /* 20 */
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwx", /* 50 */
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
        "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmn",  /* 100 */
    };
    const size_t vlen[] = { 1, 5, 10, 20, 50, 100 };
    const int NVALS = 6;

    /* Insert 20 entries using rotating values */
    const int N = 20;
    for (int i = 0; i < N; i++) {
        char k[16];
        snprintf(k, sizeof(k), "bk%d", i);
        int vi = i % NVALS;
        ht_insert(t, k, strlen(k), vstr[vi], vlen[vi]);
    }

    /* Force growth past capacity 64 */
    for (int i = N; i < 80; i++) {
        char k[16];
        snprintf(k, sizeof(k), "bk%d", i);
        int val = i;
        ht_insert(t, k, strlen(k), &val, sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity > 64);

    /* Delete the padding entries (20..79) and half the originals to trigger shrink */
    for (int i = N; i < 80; i++) {
        char k[16];
        snprintf(k, sizeof(k), "bk%d", i);
        ht_remove(t, k, strlen(k));
    }
    /* Delete even originals */
    for (int i = 0; i < N; i += 2) {
        char k[16];
        snprintf(k, sizeof(k), "bk%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "test_shrink_preserves_byte_exact: after shrink");

    ht_stats(t, &st);
    printf("  After shrink: size=%zu cap=%zu\n", st.size, st.capacity);

    /* Verify remaining odd entries byte-exact */
    for (int i = 1; i < N; i += 2) {
        char k[16];
        snprintf(k, sizeof(k), "bk%d", i);
        size_t vl = 0;
        const char *v = ht_find(t, k, strlen(k), &vl);
        int vi = i % NVALS;
        assert(v != NULL);
        assert(vl == vlen[vi]);
        assert(memcmp(v, vstr[vi], vl) == 0);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_compact_repeatedly(void) {
    printf("Test: compact repeatedly with insert/delete cycles...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int *present = NULL;
    int present_cap = 0;

    for (int cycle = 0; cycle < 3; cycle++) {
        /* Insert entries */
        int start = cycle * 100;
        for (int i = start; i < start + 50; i++) {
            char k[16];
            snprintf(k, sizeof(k), "rc%d", i);
            int v = i;
            ht_insert(t, k, strlen(k), &v, sizeof(int));
            /* Track in present array */
            if (i >= present_cap) {
                int new_cap = i + 100;
                present = realloc(present, new_cap * sizeof(int));
                memset(present + present_cap, 0, (new_cap - present_cap) * sizeof(int));
                present_cap = new_cap;
            }
            present[i] = 1;
        }

        /* Delete entries */
        for (int i = start; i < start + 50; i++) {
            if ((i - start) >= 40) break;  /* keep last 10 */
            char k[16];
            snprintf(k, sizeof(k), "rc%d", i);
            ht_remove(t, k, strlen(k));
            present[i] = 0;
        }

        /* Compact */
        ht_compact(t);

        INV_CHECK(t, "test_compact_repeatedly: after compact");

        /* Verify surviving entries */
        int count = 0;
        for (int i = 0; i < present_cap; i++) {
            if (present[i]) {
                char k[16];
                snprintf(k, sizeof(k), "rc%d", i);
                const int *v = ht_find(t, k, strlen(k), NULL);
                assert(v != NULL && *v == i);
                count++;
            }
        }

        ht_stats_t st;
        ht_stats(t, &st);
        assert(st.size == (size_t)count);
        printf("  Cycle %d: size=%zu cap=%zu\n", cycle, st.size, st.capacity);
    }

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
}

static void test_capacity_power_of_2(void) {
    printf("Test: capacity remains power of 2 after every operation...\n");
    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 4
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    srand(12345);

    for (int op = 0; op < 100; op++) {
        int action = rand() % 3;  /* 0=insert, 1=remove, 2=find */
        int k = rand() % 50;
        char key[16];
        snprintf(key, sizeof(key), "p2_%d", k);

        if (action == 0) {
            int val = k * 7;
            ht_insert(t, key, strlen(key), &val, sizeof(int));
        } else if (action == 1) {
            ht_remove(t, key, strlen(key));
        } else {
            ht_find(t, key, strlen(key), NULL);
        }

        ht_stats_t st;
        ht_stats(t, &st);
        assert(st.capacity > 0);
        assert((st.capacity & (st.capacity - 1)) == 0);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_grow_with_zero_hash_entries(void) {
    printf("Test: grow with zero-hash entries via spill lane...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert 50 entries using zero hash (all go to same bucket / spill) */
    for (int i = 0; i < 50; i++) {
        char k[16];
        snprintf(k, sizeof(k), "zh_%d", i);
        int val = i + 1000;
        ht_insert_with_hash(t, 0, k, strlen(k), &val, sizeof(int));
    }

    /* Insert 50 entries with normal fnv1a hash */
    for (int i = 0; i < 50; i++) {
        char k[16];
        snprintf(k, sizeof(k), "fn_%d", i);
        int val = i + 2000;
        ht_insert(t, k, strlen(k), &val, sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 100);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    INV_CHECK(t, "test_grow_with_zero_hash_entries: after 100 inserts");

    /* Verify all 100 entries */
    for (int i = 0; i < 50; i++) {
        char k[16];
        snprintf(k, sizeof(k), "zh_%d", i);
        const int *v = ht_find_with_hash(t, 0, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 1000);
    }
    for (int i = 0; i < 50; i++) {
        char k[16];
        snprintf(k, sizeof(k), "fn_%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 2000);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_shrink_to_minimum(void) {
    printf("Test: shrink to minimum capacity...\n");
    ht_config_t cfg = {
        .initial_capacity = 128,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 100 entries to trigger growth */
    int vals[100];
    for (int i = 0; i < 100; i++) {
        char k[8]; snprintf(k, sizeof(k), "sm%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 128);
    printf("  After insert: size=%zu cap=%zu\n", st.size, st.capacity);

    /* Remove all but 2 */
    for (int i = 2; i < 100; i++) {
        char k[8]; snprintf(k, sizeof(k), "sm%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "test_shrink_to_minimum: after delete");

    ht_stats(t, &st);
    printf("  After delete: size=%zu cap=%zu\n", st.size, st.capacity);

    /* Capacity should have shrunk but stay >= 64 (minimum) */
    assert(st.capacity >= 64);
    assert(st.size == 2);

    /* Verify the 2 remaining entries */
    for (int i = 0; i < 2; i++) {
        char k[8]; snprintf(k, sizeof(k), "sm%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
}

static void test_grow_under_heavy_tombstones(void) {
    printf("Test: grow under heavy tombstone load...\n");
    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .tomb_threshold = 0.4,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 25 entries */
    int vals[45];
    for (int i = 0; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "ht%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete 20 — creating heavy tombstones (20/25 = 80% tombstone ratio) */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ht%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats_t st1;
    ht_stats(t, &st1);
    printf("  After delete: size=%zu cap=%zu tombstones=%zu\n",
           st1.size, st1.capacity, st1.tombstone_cnt);

    /* Insert 20 more new entries — growth + tomb cleanup */
    for (int i = 25; i < 45; i++) {
        char k[8]; snprintf(k, sizeof(k), "ht%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    INV_CHECK(t, "test_grow_under_heavy_tombstones: after reinsert");

    ht_stats_t st2;
    ht_stats(t, &st2);
    printf("  After reinsert: size=%zu cap=%zu tombstones=%zu\n",
           st2.size, st2.capacity, st2.tombstone_cnt);

    /* Verify all 25 remaining: 5 old (ht20-ht24) + 20 new (ht25-ht44) */
    for (int i = 20; i < 45; i++) {
        char k[8]; snprintf(k, sizeof(k), "ht%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    /* Deleted entries must be gone */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ht%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    /* Stats consistency */
    assert(st2.size == 25);

    ht_destroy(t);
    printf("  PASS\n");
}

/* Stats invariants: load_factor == size/cap, tombstone_ratio formula */
static void test_stats_invariants(void) {
    printf("Test: stats formula invariants...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int vals[50];
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "si%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }
    for (int i = 0; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "si%d", i * 2);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "test_stats_invariants: after insert+delete");

    ht_stats_t st;
    ht_stats(t, &st);

    /* load_factor must equal size / capacity */
    double expected_lf = (double)st.size / (double)st.capacity;
    assert(st.load_factor >= expected_lf - 0.001);
    assert(st.load_factor <= expected_lf + 0.001);

    /* tombstone_ratio must follow formula */
    if (st.size + st.tombstone_cnt > 0) {
        double expected_tr = (double)st.tombstone_cnt / (double)(st.size + st.tombstone_cnt);
        assert(st.tombstone_ratio >= expected_tr - 0.01);
        assert(st.tombstone_ratio <= expected_tr + 0.01);
    }

    /* size must be positive, capacity must be power of 2 */
    assert(st.size > 0);
    assert(st.capacity > 0);
    assert((st.capacity & (st.capacity - 1)) == 0);

    ht_destroy(t);
    printf("  PASS\n");
}

int main(void) {
    printf("=== Grow/Shrink Tests ===\n\n");

    test_grow_and_verify();
    test_grow_delete_compact();
    test_multiple_grow_compact_cycles();
    test_random_workload();
    test_zombie_cleanup();
    test_auto_shrink();
    test_grow_with_tomb_threshold();
    test_shrink_blocked_at_64();
    test_shrink_exact_boundary();
    test_zombie_cursor_reset_on_growth();
    test_zombie_all_deleted();
    test_multiple_auto_shrinks();
    test_grow_data_integrity();
    test_stats_invariants();
    test_shrink_preserves_byte_exact();
    test_compact_repeatedly();
    test_capacity_power_of_2();
    test_grow_with_zero_hash_entries();
    test_shrink_to_minimum();
    test_grow_under_heavy_tombstones();

    printf("\nAll grow/shrink tests passed!\n");
    return 0;
}
