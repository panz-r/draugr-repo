/**
 * test_migration.c - Test incremental resize with interleaved operations
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

// Test incremental resize with interleaved inserts, deletes, and lookups
static int test_migration_interleaved(void) {
    printf("Test: migration with interleaved ops...\n");
    
    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.5,  // Trigger resize at 50% load
        .min_load_factor = 0.0,  // Disable auto-shrink
        .tomb_threshold = 0.5,
        .zombie_window = 0,      // Disable zombie
        .zombie_window = 0       // Disable zombie
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    
    const int MAX_KEYS = 500;
    int *present = calloc(MAX_KEYS, sizeof(int));
    int present_count = 0;
    
    // Insert initial batch
    for (int i = 0; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        int val = i * 100;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
        present[i] = 1;
        present_count++;
        
        // After EVERY insert: 10 lookups of known keys + 10 lookups of unknown keys
        for (int j = 0; j < 10; j++) {
            int k = rand() % present_count;  // Known key
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "key%d", k);
            const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
            if (!v || *v != k * 100) {
                printf("  FAIL: known lookup key%d failed after insert %d\n", k, i);
                free(present);
                ht_destroy(t);
                return 1;
            }
        }
        for (int j = 0; j < 10; j++) {
            int k = MAX_KEYS + rand() % 1000;  // Unknown key
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "key%d", k);
            const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
            if (v != NULL) {
                printf("  FAIL: unknown lookup key%d found=%d after insert %d\n", k, v ? *v : -1, i);
                free(present);
                ht_destroy(t);
                return 1;
            }
        }
    }
    
    // Insert more to trigger resize, with lookups after EVERY insert
    for (int i = 50; i < MAX_KEYS; i++) {
        char key[32];
        snprintf(key, sizeof(key), "key%d", i);
        int val = i * 100;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
        present[i] = 1;
        present_count++;
        
        // After EVERY insert: lookups
        for (int j = 0; j < 10; j++) {
            // Find a key that is actually present
            int tries = 0;
            int k;
            do {
                k = rand() % MAX_KEYS;
                tries++;
            } while (!present[k] && tries < 20);
            
            if (present[k]) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "key%d", k);
                const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
                if (!v || *v != k * 100) {
                    printf("  FAIL: known lookup key%d failed after insert %d\n", k, i);
                    free(present);
                    ht_destroy(t);
                    return 1;
                }
            }
        }
        for (int j = 0; j < 10; j++) {
            int k = MAX_KEYS + rand() % 1000;  // Unknown key
            char kbuf[32];
            snprintf(kbuf, sizeof(kbuf), "key%d", k);
            const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
            if (v != NULL) {
                printf("  FAIL: unknown lookup key%d found after insert %d\n", k, i);
                free(present);
                ht_destroy(t);
                return 1;
            }
        }
        
        // Every 20 inserts: do some deletes with interleaved lookups
        if (i % 20 == 0) {
            for (int d = 0; d < 10; d++) {
                int k = rand() % i;
                if (present[k]) {
                    char kbuf[32];
                    snprintf(kbuf, sizeof(kbuf), "key%d", k);
                    ht_remove(t, kbuf, strlen(kbuf));
                    present[k] = 0;
                    present_count--;
                    
                    // After EVERY delete: lookups
                    for (int j = 0; j < 5; j++) {
                        int k2 = rand() % i;  // May or may not be present
                        char kbuf2[32];
                        snprintf(kbuf2, sizeof(kbuf2), "key%d", k2);
                        const int *v = ht_find(t, kbuf2, strlen(kbuf2), NULL);
                        if (present[k2]) {
                            if (!v || *v != k2 * 100) {
                                printf("  FAIL: lookup key%d failed after delete\n", k2);
                                free(present);
                                ht_destroy(t);
                                return 1;
                            }
                        } else {
                            if (v != NULL) {
                                printf("  FAIL: deleted key%d still found\n", k2);
                                free(present);
                                ht_destroy(t);
                                return 1;
                            }
                        }
                    }
                }
            }
        }
    }
    
    printf("  Ops: insert %d, delete ~100, lookups ~%d\n", MAX_KEYS, MAX_KEYS * 20);
    
    // Final verification
    ht_stats_t st;
    ht_stats(t, &st);
    printf("  Final: size=%zu cap=%zu\n", st.size, st.capacity);
    
    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test multiple sequential resizes with interleaved lookups
static int test_sequential_resizes(void) {
    printf("Test: sequential resizes with interleaved ops...\n");
    
    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    
    int *present = calloc(200, sizeof(int));
    int next_key = 0;
    
    // Multiple phases with resizes
    for (int phase = 0; phase < 4; phase++) {
        // Insert 40 entries with lookups after each
        for (int i = 0; i < 40; i++) {
            char key[32];
            snprintf(key, sizeof(key), "key%d", next_key);
            int val = next_key * 100;  // Simple: value = key * 100
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[next_key] = 1;
            next_key++;
            
            // After EVERY insert: verify some random present keys and some absent keys
            for (int j = 0; j < 5; j++) {
                int k = rand() % next_key;
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "key%d", k);
                const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
                int expected = k * 100;
                if (present[k]) {
                    if (!v || *v != expected) {
                        printf("  FAIL: lookup key%d failed after insert phase %d i=%d (found %d)\n", 
                               k, phase, i, v ? *v : -1);
                        free(present);
                        ht_destroy(t);
                        return 1;
                    }
                } else {
                    if (v != NULL) {
                        printf("  FAIL: unknown key%d found after insert phase %d\n", k, phase);
                        free(present);
                        ht_destroy(t);
                        return 1;
                    }
                }
            }
        }
        
        // Do 20 random deletes with lookups after each
        for (int d = 0; d < 20; d++) {
            int k = rand() % next_key;
            if (present[k]) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "key%d", k);
                ht_remove(t, kbuf, strlen(kbuf));
                present[k] = 0;
                
                // After EVERY delete: lookups
                for (int j = 0; j < 5; j++) {
                    int k2 = rand() % next_key;
                    char kbuf2[32];
                    snprintf(kbuf2, sizeof(kbuf2), "key%d", k2);
                    const int *v = ht_find(t, kbuf2, strlen(kbuf2), NULL);
                    if (present[k2]) {
                        int expected = k2 * 100;
                        if (!v || *v != expected) {
                            printf("  FAIL: deleted-but-should-be-present key%d not found (found %d)\n", 
                                   k2, v ? *v : -1);
                            free(present);
                            ht_destroy(t);
                            return 1;
                        }
                    }
                }
            }
        }
        
        // Verify all present keys
        for (int k = 0; k < next_key; k++) {
            if (present[k]) {
                char kbuf[32];
                snprintf(kbuf, sizeof(kbuf), "key%d", k);
                const int *v = ht_find(t, kbuf, strlen(kbuf), NULL);
                int expected = k * 100;
                if (!v || *v != expected) {
                    printf("  FAIL: key%d missing at end of phase %d (found %d)\n", k, phase, v ? *v : -1);
                    free(present);
                    ht_destroy(t);
                    return 1;
                }
            }
        }
        
        ht_stats_t st;
        ht_stats(t, &st);
        printf("  Phase %d: size=%zu cap=%zu\n", phase, st.size, st.capacity);
    }
    
    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

int main(void) {
    printf("=== Migration Tests ===\n\n");
    
    srand(12345);
    
    if (test_migration_interleaved()) return 1;
    if (test_sequential_resizes()) return 1;
    
    printf("\nAll migration tests passed!\n");
    return 0;
}
