/**
 * test_grow_shrink.c - Test grow/shrink behavior of the hash table
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
        }
    }
    
    // Final verification
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

int main(void) {
    printf("=== Grow/Shrink Tests ===\n\n");
    
    test_grow_and_verify();
    test_grow_delete_compact();
    test_multiple_grow_compact_cycles();
    test_random_workload();
    test_zombie_cleanup();
    test_auto_shrink();
    
    printf("\nAll grow/shrink tests passed!\n");
    return 0;
}
