/**
 * test_migration.c - Test incremental resize with interleaved operations
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

    INV_CHECK(t, "test_migration_interleaved: final");

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

        INV_CHECK(t, "test_sequential_resizes: end of phase");

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

    INV_CHECK(t, "test_migration_spill: after resizes");

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

    INV_CHECK(t, "test_migration_collision: after all ops");

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

    INV_CHECK(t, "test_migration_zombie: after zombie inserts");

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

    INV_CHECK(t, "test_migration_auto_shrink: after delete");

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

    INV_CHECK(t, "test_migration_string_integrity: after deletes");

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

        INV_CHECK(t, "test_migration_compact_cycle: after compact");

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

// Test migration with heterogeneous value types and sizes
static int test_migration_heterogeneous_values(void) {
    printf("Test: migration with heterogeneous value types...\n");

    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Prepare diverse values */
    uint8_t one_byte = 0xAB;
    int64_t eight_byte = 0x123456789ABCDEF0LL;
    char big_buf[5][100];
    for (int i = 0; i < 5; i++) {
        memset(big_buf[i], 'A' + i, 99);
        big_buf[i][99] = '\0';
    }
    /* empty value: zero-length */
    uint8_t empty_val = 0;

    /* Insert diverse entries */
    ht_insert(t, "byte1", 5, &one_byte, 1);
    ht_insert(t, "byte2", 5, &one_byte, 1);
    ht_insert(t, "int64", 5, &eight_byte, sizeof(int64_t));
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "big%d", i);
        ht_insert(t, k, strlen(k), big_buf[i], 100);
    }
    ht_insert(t, "empty", 5, &empty_val, 0);

    /* Trigger growth by inserting many more entries */
    for (int i = 0; i < 80; i++) {
        char k[16]; snprintf(k, sizeof(k), "grow%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "test_migration_heterogeneous_values: after growth");

    /* Verify byte-exact after migration */
    /* 1-byte values */
    {
        size_t vl = 0;
        const uint8_t *v = ht_find(t, "byte1", 5, &vl);
        if (v == NULL || vl != 1 || *v != 0xAB) {
            printf("  FAIL: byte1 corrupted\n");
            ht_destroy(t); return 1;
        }
        v = ht_find(t, "byte2", 5, &vl);
        if (v == NULL || vl != 1 || *v != 0xAB) {
            printf("  FAIL: byte2 corrupted\n");
            ht_destroy(t); return 1;
        }
    }
    /* 8-byte value */
    {
        size_t vl = 0;
        const int64_t *v = ht_find(t, "int64", 5, &vl);
        if (v == NULL || vl != sizeof(int64_t) || *v != 0x123456789ABCDEF0LL) {
            printf("  FAIL: int64 corrupted\n");
            ht_destroy(t); return 1;
        }
    }
    /* 100-byte strings */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "big%d", i);
        size_t vl = 0;
        const char *v = ht_find(t, k, strlen(k), &vl);
        if (v == NULL || vl != 100 || memcmp(v, big_buf[i], 100) != 0) {
            printf("  FAIL: %s corrupted\n", k);
            ht_destroy(t); return 1;
        }
    }
    /* empty value */
    {
        size_t vl = 99;
        const void *v = ht_find(t, "empty", 5, &vl);
        if (v == NULL || vl != 0) {
            printf("  FAIL: empty value corrupted (vl=%zu)\n", vl);
            ht_destroy(t); return 1;
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration after many deletes — compact should preserve survivors
static int test_migration_many_deletes_before(void) {
    printf("Test: migration after many deletes before compact...\n");

    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 100 entries */
    for (int i = 0; i < 100; i++) {
        char k[16]; snprintf(k, sizeof(k), "del%d", i);
        int v = i * 13;
        ht_insert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Delete 80 of them */
    for (int i = 0; i < 80; i++) {
        char k[16]; snprintf(k, sizeof(k), "del%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Trigger compact */
    ht_compact(t);

    INV_CHECK(t, "test_migration_many_deletes_before: after compact");

    /* Verify remaining 20 entries (del80..del99) */
    for (int i = 80; i < 100; i++) {
        char k[16]; snprintf(k, sizeof(k), "del%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 13) {
            printf("  FAIL: del%d missing or wrong after compact (got %d)\n",
                   i, v ? *v : -1);
            ht_destroy(t); return 1;
        }
    }

    /* Verify deleted entries are gone */
    for (int i = 0; i < 80; i++) {
        char k[16]; snprintf(k, sizeof(k), "del%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v != NULL) {
            printf("  FAIL: deleted del%d still present\n", i);
            ht_destroy(t); return 1;
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test migration: spill entries survive growth, then more spill entries added
static int test_migration_spill_to_main(void) {
    printf("Test: migration spill-to-main with growth...\n");

    ht_config_t cfg = {
        .initial_capacity = 32,
        .max_load_factor = 0.5,
        .min_load_factor = 0.0,
        .zombie_window = 0
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 5 spill entries via ht_insert_with_hash(t, 0, ...) */
    int spill_vals[5];
    for (int i = 0; i < 5; i++) {
        spill_vals[i] = i * 100;
        ht_insert_with_hash(t, 0, &i, sizeof(int), &spill_vals[i], sizeof(int));
    }

    /* Verify spill entries are findable */
    for (int i = 0; i < 5; i++) {
        const int *v = ht_find_with_hash(t, 0, &i, sizeof(int), NULL);
        if (v == NULL || *v != spill_vals[i]) {
            printf("  FAIL: spill key %d not found before growth\n", i);
            ht_destroy(t); return 1;
        }
    }

    /* Insert enough normal entries to trigger growth */
    for (int i = 100; i < 300; i++) {
        int v = i * 7;
        ht_insert(t, &i, sizeof(int), &v, sizeof(int));
    }

    INV_CHECK(t, "test_migration_spill_to_main: after growth");

    /* Verify spill entries still findable after growth */
    for (int i = 0; i < 5; i++) {
        const int *v = ht_find_with_hash(t, 0, &i, sizeof(int), NULL);
        if (v == NULL || *v != spill_vals[i]) {
            printf("  FAIL: spill key %d lost after growth\n", i);
            ht_destroy(t); return 1;
        }
    }

    /* Insert 5 more spill entries */
    int spill_vals2[5];
    for (int i = 5; i < 10; i++) {
        spill_vals2[i - 5] = i * 200;
        ht_insert_with_hash(t, 0, &i, sizeof(int), &spill_vals2[i - 5], sizeof(int));
    }

    /* Verify all 10 spill entries */
    for (int i = 0; i < 10; i++) {
        const int *v = ht_find_with_hash(t, 0, &i, sizeof(int), NULL);
        int expected = (i < 5) ? spill_vals[i] : spill_vals2[i - 5];
        if (v == NULL || *v != expected) {
            printf("  FAIL: spill key %d wrong after second batch\n", i);
            ht_destroy(t); return 1;
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    printf("  size=%zu cap=%zu\n", st.size, st.capacity);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// Test prophylactic tombstones after compact with zombie_window
static int test_migration_prophylactic_tombstones(void) {
    printf("Test: migration prophylactic tombstones after compact...\n");

    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .zombie_window = 8
    };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert many entries */
    const int N = 200;
    for (int i = 0; i < N; i++) {
        int v = i * 3;
        ht_insert(t, &i, sizeof(int), &v, sizeof(int));
    }

    /* Delete many entries to create tombstones */
    for (int i = 0; i < N; i += 2) {
        ht_remove(t, &i, sizeof(int));
    }

    /* Check tombstone count before compact */
    ht_stats_t st_before;
    ht_stats(t, &st_before);
    size_t before_tombs = st_before.tombstone_cnt;

    /* Trigger a full compact */
    ht_compact(t);

    INV_CHECK(t, "test_migration_prophylactic_tombstones: after compact");

    /* After compact: verify remaining entries are correct */
    for (int i = 1; i < N; i += 2) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        if (v == NULL || *v != i * 3) {
            printf("  FAIL: key %d missing after compact\n", i);
            ht_destroy(t); return 1;
        }
    }
    /* Deleted entries should be gone */
    for (int i = 0; i < N; i += 2) {
        const int *v = ht_find(t, &i, sizeof(int), NULL);
        assert(v == NULL);
    }

    /* After compact: tombstone count should be lower (prophylactic tombstones
       are placed by graveyard hashing, but old debris should be cleaned) */
    ht_stats_t st_after;
    ht_stats(t, &st_after);
    printf("  before: size=%zu tombs=%zu cap=%zu\n",
           st_before.size, before_tombs, st_before.capacity);
    printf("  after:  size=%zu tombs=%zu cap=%zu\n",
           st_after.size, st_after.tombstone_cnt, st_after.capacity);

    /* Tombstones after compact should be <= before (old debris cleaned,
       only prophylactic tombstones remain) */
    assert(st_after.tombstone_cnt <= before_tombs);
    /* Size should be 100 (the odd-numbered survivors) */
    assert(st_after.size == 100);

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
    if (test_migration_heterogeneous_values()) return 1;
    if (test_migration_many_deletes_before()) return 1;
    if (test_migration_spill_to_main()) return 1;
    if (test_migration_prophylactic_tombstones()) return 1;

    printf("\nAll migration tests passed!\n");
    return 0;
}
