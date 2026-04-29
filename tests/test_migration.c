/**
 * test_migration.c - Test incremental resize with interleaved operations
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

static uint64_t selective_zero_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    const char *k = (const char *)key;
    if (len > 0 && k[0] == 'z') return 0;
    return fnv1a_hash(key, len, ctx);
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

// Test migration with spill-lane entries surviving resize
static int test_migration_spill(void) {
    printf("Test: migration with spill-lane entries...\n");

    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);

    /* Insert spill entries (z-prefix) and normal entries */
    for (int i = 0; i < 15; i++) {
        char k[16]; snprintf(k, sizeof(k), "z%d", i);
        int v = i * 10;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "n%d", i);
        int v = i * 100;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

    /* Trigger multiple resizes */
    for (int i = 50; i < 200; i++) {
        char k[16]; snprintf(k, sizeof(k), "n%d", i);
        int v = i * 100;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

    /* Verify all spill entries survived */
    for (int i = 0; i < 15; i++) {
        char k[16]; snprintf(k, sizeof(k), "z%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 10) {
            printf("  FAIL: spill entry z%d missing or wrong\n", i);
            ht_destroy(t); return 1;
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu (15 spill + normal)\n", st.size, st.capacity);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration under extreme collision (all keys collide)
static int test_migration_collision(void) {
    printf("Test: migration under extreme collision (fixed_hash)...\n");

    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    int *present = calloc(200, sizeof(int));
    srand(9999);

    /* Insert 100, delete 30, insert 100 more — multiple resizes under collision */
    for (int i = 0; i < 100; i++) {
        char k[16]; snprintf(k, sizeof(k), "col%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
        present[i] = 1;
    }
    for (int i = 0; i < 100; i += 3) {
        char k[16]; snprintf(k, sizeof(k), "col%d", i);
        ht_remove(t, k, strlen(k));
        present[i] = 0;
    }
    for (int i = 100; i < 200; i++) {
        char k[16]; snprintf(k, sizeof(k), "col%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
        present[i] = 1;
    }

    /* Verify all present entries */
    for (int i = 0; i < 200; i++) {
        char k[16]; snprintf(k, sizeof(k), "col%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (present[i]) {
            if (v == NULL || *v != i) {
                printf("  FAIL: col%d missing\n", i);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            if (v != NULL) {
                printf("  FAIL: deleted col%d still present\n", i);
                free(present); ht_destroy(t); return 1;
            }
        }
    }

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration with zombie_window enabled
static int test_migration_zombie(void) {
    printf("Test: migration with zombie enabled...\n");

    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 4
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int *present = calloc(300, sizeof(int));
    srand(7777);

    /* Heavy insert/delete interleaving with zombie running */
    for (int i = 0; i < 200; i++) {
        char k[16]; snprintf(k, sizeof(k), "mz%d", i);
        int v = i * 5;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
        present[i] = 1;
    }
    /* Delete many to create tombstones */
    for (int i = 0; i < 200; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "mz%d", i);
        ht_remove(t, k, strlen(k));
        present[i] = 0;
    }
    /* Insert more — zombie steps fire on each insert */
    for (int i = 200; i < 300; i++) {
        char k[16]; snprintf(k, sizeof(k), "mz%d", i);
        int v = i * 5;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
        present[i] = 1;
    }

    /* Verify all */
    for (int i = 0; i < 300; i++) {
        char k[16]; snprintf(k, sizeof(k), "mz%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (present[i]) {
            if (v == NULL || *v != i * 5) {
                printf("  FAIL: mz%d missing\n", i);
                free(present); ht_destroy(t); return 1;
            }
        } else {
            assert(v == NULL);
        }
    }

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration with delete-triggered auto-shrink
static int test_migration_auto_shrink(void) {
    printf("Test: migration with auto-shrink...\n");

    ht_config_t cfg = {
        .initial_capacity = 128,
        .max_load_factor = 0.75,
        .min_load_factor = 0.25,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 100 — 100 > 128*0.75=96, triggers growth beyond 128 */
    int vals[100];
    for (int i = 0; i < 100; i++) {
        char k[16]; snprintf(k, sizeof(k), "as%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity > 128);

    /* Delete 80 — load drops, triggers auto-shrink */
    for (int i = 0; i < 80; i++) {
        char k[16]; snprintf(k, sizeof(k), "as%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats(t, &st);
    assert(st.size == 20);

    /* Verify survivors */
    for (int i = 80; i < 100; i++) {
        char k[16]; snprintf(k, sizeof(k), "as%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i) {
            printf("  FAIL: as%d missing after auto-shrink\n", i);
            ht_destroy(t); return 1;
        }
    }

    printf("  size=%zu cap=%zu (shrunk from grown size)\n", st.size, st.capacity);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration with string data — verify byte-exact integrity after resize

// Test migration with string data — verify byte-exact integrity after resize
static int test_migration_string_integrity(void) {
    printf("Test: migration string integrity after resize...\n");

    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const char *values[] = {
        "alpha", "bravo", "charlie", "delta", "echo",
        "foxtrot", "golf", "hotel", "india", "juliet",
        "kilo", "lima", "mike", "november", "oscar",
        "papa", "quebec", "romeo", "sierra", "tango"
    };

    /* Insert 20 string-valued entries */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "w%d", i);
        ht_insert(t, k, strlen(k), values[i], strlen(values[i]));
    }

    /* Insert 60 ints to trigger multiple resizes */
    for (int i = 0; i < 60; i++) {
        char k[8]; snprintf(k, sizeof(k), "n%d", i);
        int v = i * 7;
        ht_insert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Delete half the ints */
    for (int i = 0; i < 60; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "n%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Verify all 20 strings byte-exact */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "w%d", i);
        size_t vl = 0;
        const char *v = ht_find(t, k, strlen(k), &vl);
        if (v == NULL || vl != strlen(values[i]) || memcmp(v, values[i], vl) != 0) {
            printf("  FAIL: string w%d corrupted after migration\n", i);
            ht_destroy(t); return 1;
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration + compact cycle with invariant checks
static int test_migration_compact_cycle(void) {
    printf("Test: migration + compact cycle with invariants...\n");

    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int *present = calloc(200, sizeof(int));
    srand(8888);

    for (int cycle = 0; cycle < 5; cycle++) {
        /* Insert 30 */
        for (int i = 0; i < 30; i++) {
            int k = rand() % 200;
            char kb[16]; snprintf(kb, sizeof(kb), "mc%d", k);
            int v = k * 11 + cycle;
            ht_insert(t, kb, strlen(kb), &v, sizeof(int));
            present[k] = 1;
        }

        /* Delete 20 */
        for (int i = 0; i < 20; i++) {
            int k = rand() % 200;
            if (present[k]) {
                char kb[16]; snprintf(kb, sizeof(kb), "mc%d", k);
                ht_remove(t, kb, strlen(kb));
                present[k] = 0;
            }
        }

        /* Compact */
        ht_compact(t);

        /* Verify all present entries */
        for (int i = 0; i < 200; i++) {
            if (present[i]) {
                char kb[16]; snprintf(kb, sizeof(kb), "mc%d", i);
                const int *v = ht_find(t, kb, strlen(kb), NULL);
                if (v == NULL) {
                    printf("  FAIL: mc%d missing after cycle %d compact\n", i, cycle);
                    free(present); ht_destroy(t); return 1;
                }
            }
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  final: size=%zu cap=%zu\n", st.size, st.capacity);

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Wrapper main
int main(void) {
    printf("=== Migration Tests ===\n\n");

    srand(12345);

    if (test_migration_interleaved()) return 1;
    if (test_sequential_resizes()) return 1;
    if (test_migration_spill()) return 1;
    if (test_migration_collision()) return 1;
    if (test_migration_zombie()) return 1;
    if (test_migration_auto_shrink()) return 1;
    if (test_migration_string_integrity()) return 1;
    if (test_migration_compact_cycle()) return 1;

    printf("\nAll migration tests passed!\n");
    return 0;
}
