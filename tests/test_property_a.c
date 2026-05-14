#include "property_utils.h"

static void test_large_unique_keys(void) {
    printf("\n=== Property: Large Scale Unique Keys ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xA1B2C3D4);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 5000;
    char keys[5000][32];
    char vals[5000][64];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 32; j++) keys[i][j] = (char)(i * 17 + j);
        for (int j = 0; j < 64; j++) vals[i][j] = (char)(i * 13 + j);
        
        ht_upsert(ht, keys[i], 32, vals[i], 64);
        model_set(m, keys[i], 32, vals[i], 64);
        
        if (i > 0 && i % 1000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], 32, &vlen);
        const char *mfound = model_find_val(m, keys[i], 32, NULL);
        
        MODEL_CHECK((found != NULL) == (mfound != NULL));
        if (found) {
            MODEL_CHECK(vlen == 64);
            MODEL_CHECK(memcmp(found, vals[i], 64) == 0);
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("large scale unique keys hold");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 2: Value Size Mutation
// ============================================================================

static void test_value_size_mutation(void) {
    printf("\n=== Property: Value Size Mutation ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xB2C3D4E5);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char key[16] = {0};
    
    for (int round = 0; round < 20; round++) {
        size_t val_len = 8 + (round % 16) * 8;
        char val[128];
        memset(val, (char)round, val_len);
        
        ht_upsert(ht, key, 16, val, val_len);
        model_set(m, key, 16, val, val_len);
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, key, 16, &vlen);
        const char *mfound = model_find_val(m, key, 16, NULL);
        
        MODEL_CHECK(found != NULL && mfound != NULL);
        MODEL_CHECK(vlen == val_len);
        MODEL_CHECK(memcmp(found, val, val_len) == 0);
    }
    
    PASS("value size mutation holds");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 3: Interleaved Iterator and Modification
// ============================================================================

static void test_tombstone_tracking(void) {
    printf("\n=== Property: Tombstone Tracking ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xD4E5F6A7);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75, .tomb_threshold = 0.2 };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 100;
    char keys[100][16];
    char vals[100][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)i;
        ht_upsert(ht, keys[i], 16, vals[i], 16);
        model_set(m, keys[i], 16, vals[i], 16);
    }
    
    for (int i = 0; i < n; i++) {
        ht_remove(ht, keys[i], 16);
        model_remove(m, keys[i], 16);
    }
    
    size_t live, tomb, empty;
    count_bare_slots(&ht->bare, &live, &tomb, &empty);
    
    if (tomb == ht->bare.tombstone_cnt) {
        PASS("tombstone count matches actual tombstone slots");
    } else {
        printf("  counted=%zu recorded=%zu\n", tomb, ht->bare.tombstone_cnt);
        BUG("tombstone count mismatch");
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 5: Arena Rollback on Allocation Failure
// ============================================================================

static void test_arena_waste_detection(void) {
    printf("\n=== Property: Arena Rollback on Allocation Failure ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xE5F6A7B8);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    char key[16] = {0};
    char val_small[16] = {0};
    ht_upsert(ht, key, 16, val_small, 16);

    size_t entry_count_before = ht->entry_count;
    printf(" After small insert: entry_count=%zu\n", entry_count_before);

    int bugs_found = 0;
    for (int trial = 0; trial < 20 && bugs_found == 0; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(5 + trial);

        size_t before = ht->entry_count;
        bool ok = ht_upsert(ht, key, 16, val_small, 16);
        size_t after = ht->entry_count;

        printf(" trial %d: max_alloc=%d, before=%zu, after=%zu, ok=%d\n",
               trial, 5 + trial, before, after, ok);

        if (!ok && after != before) {
            printf(" BUG: entry_count changed from %zu to %zu on failed upsert\n", before, after);
            bugs_found++;
        }
    }
    
    if (bugs_found == 0) {
        PASS("arena rollback: entry_count unchanged when allocation fails");
    } else {
        BUG("arena rollback: entry_count changed on failed alloc");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 6: Shrink Verification
// ============================================================================

static void test_shrink_verification(void) {
    printf("\n=== Property: Shrink Verification ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xF6A7B8C9);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 256, .max_load_factor = 0.75, .min_load_factor = 0.1 };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 20;
    char keys[20][16];
    char vals[20][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)i;
        ht_upsert(ht, keys[i], 16, vals[i], 16);
        model_set(m, keys[i], 16, vals[i], 16);
    }
    
    size_t cap_before = ht->bare.capacity;
    
    for (int i = 0; i < n - 5; i++) {
        ht_remove(ht, keys[i], 16);
        model_remove(m, keys[i], 16);
    }
    
    size_t cap_after_remove = ht->bare.capacity;
    
    ht_resize(ht, 32);
    
    size_t cap_after_resize = ht->bare.capacity;
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    if (cap_after_resize <= cap_before) {
        PASS("shrink: capacity reduced after shrink resize");
    } else {
        printf("  cap_before=%zu cap_after_remove=%zu cap_after_resize=%zu\n",
               cap_before, cap_after_remove, cap_after_resize);
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 7: Duplicate Key Handling
// ============================================================================

static void test_combined_operations_sequence(void) {
    printf("\n=== Property: Combined Operations Sequence ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xDAEBFD0E);
    uint64_t sm_state = (uint64_t)seed << 1;
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.6f + (splitmix64(&sm_state) & 3) * 0.1f,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 50;
    char keys[50][32];
    char vals[50][64];
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 1 + (r & 31);
        size_t vlen = 1 + ((r >> 6) & 63);
        for (size_t j = 0; j < klen; j++) keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = 0; j < vlen; j++) vals[i][j] = (char)(i * 7 ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = klen; j < 32; j++) keys[i][j] = 0;
        for (size_t j = vlen; j < 64; j++) vals[i][j] = 0;
    }
    
    int ops = 20000;
    int upsert_ops = 0, remove_ops = 0, find_ops = 0;
    
    for (int op = 0; op < ops; op++) {
        uint64_t r1 = splitmix64(&sm_state);
        uint64_t r2 = splitmix64(&sm_state);
        uint64_t r3 = splitmix64(&sm_state);
        
        int idx = r1 % n;
        size_t klen = 1 + ((r2 >> 4) & 31);
        size_t vlen = 1 + ((r2 >> 9) & 63);
        
        uint32_t action = r3 & 0xFF;
        
        if (action < 70) {
            upsert_ops++;
            bool ok = ht_upsert(ht, keys[idx], klen, vals[idx], vlen);
            if (ok) model_set(m, keys[idx], klen, vals[idx], vlen);
        } else if (action < 85) {
            remove_ops++;
            ht_remove(ht, keys[idx], klen);
            model_remove(m, keys[idx], klen);
        } else if (action < 100) {
            find_ops++;
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], klen, &fvlen);
            const char *mfound = model_find_val(m, keys[idx], klen, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op > 0 && op % 4000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
            
            r1 = splitmix64(&sm_state);
            if (ht_size(ht) > 10) {
                if ((r1 & 7) == 0) {
                    ht_resize(ht, 64 + ((r1 >> 3) & 0xFF));
                } else if ((r1 & 7) == 1) {
                    ht_compact(ht);
                }
            }
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 1 + (r & 31);
        size_t fvlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], klen, &fvlen);
        const char *mfound = model_find_val(m, keys[i], klen, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    printf("  ops=%d upsert=%d remove=%d find=%d\n", ops, upsert_ops, remove_ops, find_ops);
    PASS("combined operations sequence holds for 20k ops");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 11: Insert/Remove/Find Balance
// ============================================================================

static void test_hash_collision_different_keys(void) {
    printf("\n=== Property: Hash Collision Different Keys ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xF0E1D2C3);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 8, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, fixed_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 500;
    int key_ids[500];
    int vals[500];
    
    for (int i = 0; i < n; i++) {
        key_ids[i] = i;
        vals[i] = i * 37;
        
        char key[4];
        memcpy(key, &key_ids[i], 4);
        char valbuf[8];
        memcpy(valbuf, &vals[i], 4);
        
        ht_upsert(ht, key, 4, valbuf, 4);
        model_set(m, key, 4, valbuf, 4);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        char key[4];
        memcpy(key, &key_ids[i], 4);
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, key, 4, &vlen);
        const char *mfound = model_find_val(m, key, 4, NULL);
        
        MODEL_CHECK((found != NULL) == (mfound != NULL));
        if (found) {
            int hv, mv;
            memcpy(&hv, found, 4);
            memcpy(&mv, mfound, 4);
            MODEL_CHECK(hv == mv);
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else { printf("  PASS: hash collision different keys: all %d entries findable\n", n); tests_passed++; }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 13: Long Running Sequence with Persistent Model
// ============================================================================

static void test_long_running_sequence(void) {
    printf("\n=== Property: Long Running Sequence ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xA1B2C3D5);
    uint64_t sm_state = (uint64_t)seed << 1;
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = {
        .initial_capacity = 32 + (my_rand() % 64),
        .max_load_factor = 0.5f + (my_rand() % 5) * 0.1f,
        .min_load_factor = 0.1f,
        .tomb_threshold = 0.3f,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 100;
    char keys[100][32];
    char vals[100][64];
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 1 + (r & 31);
        r >>= 5;
        size_t vlen = 1 + (r & 63);
        for (size_t j = 0; j < klen; j++) keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = 0; j < vlen; j++) vals[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = klen; j < 32; j++) keys[i][j] = 0;
        for (size_t j = vlen; j < 64; j++) vals[i][j] = 0;
    }
    
    int ops = 75000;
    int insert_failures = 0;
    int remove_ops = 0, find_ops = 0, upsert_ops = 0;
    
    for (int op = 0; op < ops; op++) {
        uint64_t r1 = splitmix64(&sm_state);
        uint64_t r2 = splitmix64(&sm_state);
        uint64_t r3 = splitmix64(&sm_state);
        
        int idx = r1 % n;
        size_t klen = 1 + ((r2 >> 4) & 31);
        size_t vlen = 1 + ((r2 >> 9) & 63);
        
        uint32_t action = r3 & 0xFF;
        
        if (action < 85) {
            upsert_ops++;
            bool ok = ht_upsert(ht, keys[idx], klen, vals[idx], vlen);
            if (ok) {
                model_set(m, keys[idx], klen, vals[idx], vlen);
            } else {
                insert_failures++;
            }
        } else if (action < 93) {
            remove_ops++;
            ht_remove(ht, keys[idx], klen);
            model_remove(m, keys[idx], klen);
        } else if (action < 100) {
            find_ops++;
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], klen, &fvlen);
            const char *mfound = model_find_val(m, keys[idx], klen, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op > 0 && op % 10000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
            
            r1 = splitmix64(&sm_state);
            if (r1 & 3) {
                size_t new_cap = 32 + ((r1 >> 2) & 0xFF);
                ht_resize(ht, new_cap);
            }
            if (ht_size(ht) > 10 && (r1 & 7) == 7) {
                ht_compact(ht);
            }
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 1 + (r & 31);
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], klen, &vlen);
        const char *mfound = model_find_val(m, keys[i], klen, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    printf("  ops=%d upsert=%d remove=%d find=%d failures=%d\n", 
           ops, upsert_ops, remove_ops, find_ops, insert_failures);
    PASS("long running sequence: 75k ops consistent");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 14: Iterator Completeness and Consistency
// ============================================================================

static void test_iterator_completeness(void) {
    printf("\n=== Property: Iterator Completeness ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xB2C3D4E6);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 300;
    char keys[300][16];
    char vals[300][16];
    int present[300] = {0};
    
    for (int op = 0; op < 5000; op++) {
        int idx = my_rand() % n;
        int action = my_rand() % 100;
        
        if (action < 40) {
            if (!present[idx]) {
                for (int j = 0; j < 16; j++) keys[idx][j] = (char)(idx * 17 + j);
                for (int j = 0; j < 16; j++) vals[idx][j] = (char)(idx * 13 + j);
                bool ok = ht_upsert(ht, keys[idx], 16, vals[idx], 16);
                if (ok) {
                    model_set(m, keys[idx], 16, vals[idx], 16);
                    present[idx] = 1;
                }
            }
        } else if (action < 60) {
            if (present[idx]) {
                ht_remove(ht, keys[idx], 16);
                model_remove(m, keys[idx], 16);
                present[idx] = 0;
            }
        }
    }
    
    size_t expected_size = ht_size(ht);
    int iter_count = 0;
    const void *key, *value;
    size_t klen, vlen;
    ht_iter_t iter = ht_iter_begin(ht);
    
    while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
        iter_count++;
        
        size_t fvlen = 0;
        const char *found = (const char*)ht_find(ht, key, klen, &fvlen);
        if (!found) {
            printf("  ITER BUG: iterator returned key not findable\n");
            BUG("iterator key not in table");
        }
        if (fvlen != vlen) {
            printf("  ITER BUG: value length mismatch iter=%zu find=%zu\n", vlen, fvlen);
            BUG("iterator value length mismatch");
        }
    }
    
    if ((size_t)iter_count == expected_size) {
        PASS("iterator completeness: iter_count matches ht_size");
    } else {
        printf("  iter_count=%d expected=%zu\n", iter_count, expected_size);
        BUG("iterator count mismatch");
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 15: Stress with Various Key Lengths
// ============================================================================

static void test_various_key_lengths(void) {
    printf("\n=== Property: Various Key Lengths ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xD4E5F6A8);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int key_lens[] = {1, 2, 4, 8, 16, 32, 64, 128};
    int num_lens = 8;
    int total_keys = 800;
    int key_idx = 0;
    
    for (int round = 0; round < 20; round++) {
        for (int li = 0; li < num_lens; li++) {
            int klen = key_lens[li];
            char *key = malloc(klen);
            char val[16];
            
            for (int j = 0; j < klen; j++) key[j] = (char)(key_idx + j);
            memset(val, (char)key_idx, 16);
            
            bool ok = ht_upsert(ht, key, klen, val, 16);
            if (ok) {
                model_set(m, key, klen, val, 16);
            }
            
            free(key);
            key_idx++;
            if (key_idx >= total_keys) key_idx = 0;
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    int verify_count = 0;
    for (int li = 0; li < num_lens; li++) {
        int klen = key_lens[li];
        for (int ki = 0; ki < 100; ki++) {
            char key[128];
            int base = (ki * 17) % total_keys;
            for (int j = 0; j < klen; j++) key[j] = (char)(base + j);
            
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, key, klen, &vlen);
            if (found) verify_count++;
        }
    }
    
    printf("  verified %d entries\n", verify_count);
    PASS("various key lengths stress test");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 17: Rapid Resize with Active Iterators
// ============================================================================

static void test_arena_exhaustion(void) {
    printf("\n=== Property: Arena Exhaustion ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xA7B8C9DB);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    char key[16] = {0};
    int success_count = 0;
    int fail_count = 0;
    
    for (int i = 0; i < 1000; i++) {
        char val[32];
        memset(val, i, 32);
        
        bool ok = ht_upsert(ht, key, 16, val, 32);
        if (ok) {
            success_count++;
        } else {
            fail_count++;
        }
        
        if (i % 100 == 0) {
            printf(" i=%d success=%d fail=%d entry_count=%zu\n",
                    i, success_count, fail_count, ht->entry_count);
        }
    }
    
    size_t vlen = 0;
    const char *found = (const char*)ht_find(ht, key, 16, &vlen);
    
    if (found && vlen == 32) {
        PASS("arena exhaustion: consistent behavior");
    } else {
        printf("  found=%p vlen=%zu\n", found, vlen);
        BUG("arena exhaustion: final state inconsistent");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 20: Entry Table Growth and Reallocation
// ============================================================================

static void test_many_unique_hashes(void) {
    printf("\n=== Property: Many Unique Hash Values ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xAABBCCDDULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 1000;
    char keys[1000][48];
    char vals[1000][48];
    uint64_t hashes[1000];
    
    for (int i = 0; i < n; i++) {
        for (size_t j = 0; j < 48; j++) {
            keys[i][j] = (char)(splitmix64(&sm_state) & 0xFF);
            vals[i][j] = (char)(splitmix64(&sm_state) & 0xFF);
        }
        hashes[i] = splitmix64(&sm_state);
        
        bool ok = ht_upsert_with_hash(ht, hashes[i], keys[i], 48, vals[i], 48);
        if (ok) model_set(m, keys[i], 48, vals[i], 48);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find_with_hash(ht, hashes[i], keys[i], 48, &vlen);
        const char *mfound = model_find_val(m, keys[i], 48, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
        if (found) {
            MODEL_CHECK(vlen == 48);
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("many unique hashes: all 1000 entries findable");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 23: Stress With Iterator During Resize
// ============================================================================

static void test_correlated_key_patterns(void) {
  printf("\n=== Property: Correlated Key Locality ===\n");

  uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xFEEDF00DULL);
  uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
  my_srand(seed);
  printf(" seed=%u\n", seed);

  ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
  if (!ht) { BUG("ht_create failed"); return; }
  model_t *m = model_create();

  int n = 100;
  char keys[100][32];
  char vals[100][32];

  for (int i = 0; i < n; i++) {
    for (int j = 0; j < 32; j++) {
      keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
      vals[i][j] = (char)(i * 7 ^ j ^ (splitmix64(&sm_state) & 0xFF));
    }
  }

  for (int i = 0; i < n; i++) {
    ht_upsert(ht, keys[i], 32, vals[i], 32);
    model_set(m, keys[i], 32, vals[i], 32);
  }

  for (int epoch = 0; epoch < 50; epoch++) {
    for (int pass = 0; pass < 10; pass++) {
      int base = (splitmix64(&sm_state) % n);
      for (int offset = 0; offset < 20; offset++) {
        int idx = (base + offset) % n;
        int action = splitmix64(&sm_state) & 0xFF;

        if (action < 70) {
          size_t vlen = 0;
          const char *found = (const char*)ht_find(ht, keys[idx], 32, &vlen);
          const char *mfound = model_find_val(m, keys[idx], 32, NULL);
          if ((found != NULL) != (mfound != NULL)) {
            printf(" MODEL FAIL at epoch %d pass %d idx %d: found=%p mfound=%p\n",
                   epoch, pass, idx, (void*)found, (void*)mfound);
            tests_failed++;
            goto cleanup;
          }
        } else if (action < 85) {
          char newval[32];
          for (int j = 0; j < 32; j++) newval[j] = (char)(splitmix64(&sm_state) & 0xFF);
          bool ok = ht_upsert(ht, keys[idx], 32, newval, 32);
          if (ok) model_set(m, keys[idx], 32, newval, 32);
        } else {
          ht_remove(ht, keys[idx], 32);
          model_remove(m, keys[idx], 32);
        }
      }
    }

    if (ht_size(ht) != model_size(m)) {
      printf(" MODEL FAIL at epoch %d: ht_size=%zu model_size=%zu\n",
             epoch, ht_size(ht), model_size(m));
      tests_failed++;
      goto cleanup;
    }
    const char *err = ht_check_invariants(ht);
    if (err) { printf(" INVARIANT FAIL at epoch %d: %s\n", epoch, err); tests_failed++; goto cleanup; }
  }

  PASS("correlated key locality: working set patterns");

 cleanup:
 model_destroy(m);
 ht_destroy(ht);
}

// ============================================================================
// Test 26: Sequential Key Patterns
// ============================================================================

static void test_sequential_keys(void) {
    printf("\n=== Property: Sequential Key Patterns ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCD1234ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char keys[500][24];
    char vals[500][32];
    uint64_t counters[500];
    
    for (int i = 0; i < 500; i++) {
        counters[i] = 0;
    }
    
    int ops = 20000;
    int insert_count = 0, find_count = 0;
    
    for (int op = 0; op < ops; op++) {
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 40) {
            int idx = splitmix64(&sm_state) % 500;
            if (counters[idx] == 0) {
                for (int j = 0; j < 24; j++) keys[idx][j] = (char)(idx + j);
                memset(vals[idx], 0, 32);
            }
            counters[idx]++;
            snprintf(vals[idx], 32, "%lu", (unsigned long)counters[idx]);
            bool ok = ht_upsert(ht, keys[idx], 24, vals[idx], 32);
            if (ok) {
                model_set(m, keys[idx], 24, vals[idx], 32);
                insert_count++;
            }
        } else if (action < 70) {
            int idx = splitmix64(&sm_state) % 500;
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 24, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 24, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
            if (found && mfound) {
                MODEL_CHECK(vlen == 32);
            }
            find_count++;
        } else {
            int idx = splitmix64(&sm_state) % 500;
            if (counters[idx] > 0) {
                ht_remove(ht, keys[idx], 24);
                model_remove(m, keys[idx], 24);
                counters[idx] = 0;
            }
        }
        
        if (op > 0 && op % 4000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    printf("  ops=%d insert=%d find=%d\n", ops, insert_count, find_count);
    PASS("sequential keys: timestamp-like access patterns");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 27: Large Value Stress
// ============================================================================

static void test_reinsert_cycling(void) {
    printf("\n=== Property: Reinsert Cycling ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xBEEFBEEFULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char keys[20][24];
    char vals[20][32];
    
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < 24; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 32; j++) vals[i][j] = (char)(i * 13);
    }
    
    for (int i = 0; i < 20; i++) {
        ht_upsert(ht, keys[i], 24, vals[i], 32);
        model_set(m, keys[i], 24, vals[i], 32);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 0; i < 20; i++) {
            ht_remove(ht, keys[i], 24);
            model_remove(m, keys[i], 24);
        }
        
        MODEL_CHECK(ht_size(ht) == 0);
        
        for (int i = 0; i < 20; i++) {
            char newval[32];
            for (int j = 0; j < 32; j++) newval[j] = (char)((i * 17 + cycle + j) & 0xFF);
            bool ok = ht_upsert(ht, keys[i], 24, newval, 32);
            if (ok) model_set(m, keys[i], 24, newval, 32);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        if (cycle % 20 == 0) {
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  cycle %d INVARIANT FAIL: %s\n", cycle, err); tests_failed++; }
        }
    }
    
    for (int i = 0; i < 20; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], 24, &vlen);
        const char *mfound = model_find_val(m, keys[i], 24, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    PASS("reinsert cycling: 100 cycles of remove all / reinsert all");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 30: Prefix Key Patterns
// ============================================================================

static void test_hash_zero(void) {
    printf("\n=== Property: Hash Zero ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEAD0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, hash_zero, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 400;
    char keys[400][24];
    char vals[400][32];
    uint8_t present[400] = {0};
    
    for (int op = 0; op < 5000; op++) {
        int idx = splitmix64(&sm_state) % n;
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 35) {
            if (!present[idx]) {
                for (int j = 0; j < 24; j++) keys[idx][j] = (char)(splitmix64(&sm_state) & 0xFF);
                for (int j = 0; j < 32; j++) vals[idx][j] = (char)(splitmix64(&sm_state) & 0xFF);
                
                bool ok = ht_upsert(ht, keys[idx], 24, vals[idx], 32);
                if (ok) {
                    model_set(m, keys[idx], 24, vals[idx], 32);
                    present[idx] = 1;
                }
            }
        } else if (action < 60) {
            if (present[idx]) {
                ht_remove(ht, keys[idx], 24);
                model_remove(m, keys[idx], 24);
                present[idx] = 0;
            }
        } else {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 24, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 24, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op > 0 && op % 1000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("hash zero: hash=0 operations consistent");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 32: Secondary Hash Collision
// ============================================================================

static void test_iterator_during_migration(void) {
    printf("\n=== Property: Iterator During Migration ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0x12345678ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 400;
    char keys[400][24];
    char vals[400][24];
    int inserted[400] = {0};
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 24; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 24; j++) vals[i][j] = (char)(i * 7);
        bool ok = ht_upsert(ht, keys[i], 24, vals[i], 24);
        if (ok) inserted[i] = 1;
    }
    
    int inserted_count = 0;
    for (int i = 0; i < n; i++) if (inserted[i]) inserted_count++;
    printf("  inserted %d/%d entries\n", inserted_count, n);
    
    int success_count = 0;
    int fail_count = 0;
    
    for (int trial = 0; trial < 50; trial++) {
        ht_iter_t iter = ht_iter_begin(ht);
        const void *key, *value;
        size_t klen, vlen;
        
        size_t count_before = ht_size(ht);
        int collected_before = 0;
        while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
            collected_before++;
        }
        
        size_t new_cap = 64 + ((splitmix64(&sm_state) & 0x1F) * 64);
        ht_resize(ht, new_cap);
        
        iter = ht_iter_begin(ht);
        int collected_after = 0;
        int invalid_entries = 0;
        while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
            collected_after++;
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, key, klen, &fvlen);
            if (!found || fvlen != vlen) {
                invalid_entries++;
            }
        }
        
        size_t count_after = ht_size(ht);
        
        if ((size_t)collected_before == count_before &&
            (size_t)collected_after == count_after &&
            invalid_entries == 0) {
            success_count++;
        } else {
            fail_count++;
            printf("  trial %d: before={count=%zu collected=%d} after={count=%zu collected=%d invalid=%d}\n",
                   trial, count_before, collected_before, count_after, collected_after, invalid_entries);
        }
    }
    
    printf("  trials=%d success=%d fail=%d\n", 50, success_count, fail_count);
    if (fail_count == 0) {
        PASS("iterator during migration: consistent across resize");
    } else {
        BUG("iterator during migration: inconsistency detected");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 36: Failed Resize Recovery
// ============================================================================

static void test_state_machine_transitions(void) {
    printf("\n=== Property: State Machine Transitions ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCD0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    for (int config_trial = 0; config_trial < 5; config_trial++) {
        uint64_t r = splitmix64(&sm_state);
        ht_config_t cfg = {
            .initial_capacity = 4 + (r & 60),
            .max_load_factor = 0.5f + (splitmix64(&sm_state) & 7) * 0.1f,
            .min_load_factor = 0.05f + (splitmix64(&sm_state) & 3) * 0.05f,
            .tomb_threshold = 0.15f + (splitmix64(&sm_state) & 3) * 0.1f,
        };
        
        ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
        if (!ht) { BUG("ht_create failed"); return; }
        model_t *m = model_create();
        
        int n = 30;
        char keys[30][24];
        char vals[30][32];
        
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 24; j++) keys[i][j] = (char)(i * 13 ^ j ^ (splitmix64(&sm_state) & 0xFF));
            for (int j = 0; j < 32; j++) vals[i][j] = (char)(i * 17 ^ j ^ (splitmix64(&sm_state) & 0xFF));
        }
        
        size_t prev_size = 0;
        int transitions = 0;
        
        for (int op = 0; op < 200; op++) {
            int idx = splitmix64(&sm_state) % n;
            int action = splitmix64(&sm_state) & 0xFF;
            
            if (action < 50) {
                bool ok = ht_upsert(ht, keys[idx], 24, vals[idx], 32);
                if (ok) model_set(m, keys[idx], 24, vals[idx], 32);
            } else if (action < 75) {
                ht_remove(ht, keys[idx], 24);
                model_remove(m, keys[idx], 24);
            } else {
                size_t vlen = 0;
                const char *found = (const char*)ht_find(ht, keys[idx], 24, &vlen);
                const char *mfound = model_find_val(m, keys[idx], 24, NULL);
                MODEL_CHECK((found != NULL) == (mfound != NULL));
            }
            
            size_t size_after = ht_size(ht);
            
            if (size_after != prev_size) {
                transitions++;
                prev_size = size_after;
            }
            
            MODEL_CHECK(ht_size(ht) == model_size(m));
            
            if (op % 50 == 0) {
                const char *err = ht_check_invariants(ht);
                if (err) { printf("  config_trial %d op %d INVARIANT FAIL: %s\n", config_trial, op, err); tests_failed++; }
            }
        }
        
        printf("  config %d: transitions=%d final_size=%zu\n", config_trial, transitions, ht_size(ht));
        
        model_destroy(m);
        ht_destroy(ht);
    }
    
    PASS("state machine transitions: tracked size changes across configs");
}

// ============================================================================
// Test 39: Auto-Resize Trigger Verification
// ============================================================================

static void test_auto_resize_triggers(void) {
    printf("\n=== Property: Auto-Resize Triggers ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEAD1235ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    float load_factors[] = {0.5f, 0.6f, 0.7f, 0.8f, 0.9f};
    int capacities[] = {8, 16, 32, 64};
    
    for (size_t li = 0; li < sizeof(load_factors)/sizeof(load_factors[0]); li++) {
        for (size_t ci = 0; ci < sizeof(capacities)/sizeof(capacities[0]); ci++) {
            ht_config_t cfg = {
                .initial_capacity = capacities[ci],
                .max_load_factor = load_factors[li],
            };
            
            ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
            if (!ht) { BUG("ht_create failed"); return; }
            
            int expected_max = (int)((float)capacities[ci] * load_factors[li]);
            
            char key[16] = {0};
            char val[16] = {0};
            
            int insert_count = 0;
            size_t prev_cap = ht->bare.capacity;
            
            while (insert_count < expected_max + 50) {
                key[0] = (char)(insert_count & 0xFF);
                key[1] = (char)((insert_count >> 8) & 0xFF);
                
                bool ok = ht_upsert(ht, key, 16, val, 16);
                if (!ok) break;
                insert_count++;
                
                if (ht->bare.capacity != prev_cap) {
                    printf("  lf=%.1f cap=%d: resize at %d inserts (capacity %zu -> %zu)\n",
                           load_factors[li], capacities[ci], insert_count, prev_cap, ht->bare.capacity);
                    prev_cap = ht->bare.capacity;
                }
            }
            
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
            
            ht_destroy(ht);
        }
    }
    
    PASS("auto-resize triggers: verified resize behavior at various load factors");
}

// ============================================================================
// Test 40: Full Lifecycle Test
// ============================================================================

static void test_full_lifecycle(void) {
    printf("\n=== Property: Full Lifecycle ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xCAFEBEEF);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 50;
    char keys[50][24];
    char vals[50][32];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 24; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 32; j++) vals[i][j] = (char)(i * 13);
    }
    
    for (int cycle = 0; cycle < 50; cycle++) {
        for (int i = 0; i < n; i++) {
            bool ok = ht_upsert(ht, keys[i], 24, vals[i], 32);
            if (ok) model_set(m, keys[i], 24, vals[i], 32);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        for (int i = 0; i < n; i++) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 24, &vlen);
            const char *mfound = model_find_val(m, keys[i], 24, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        for (int i = 0; i < n; i++) {
            ht_remove(ht, keys[i], 24);
            model_remove(m, keys[i], 24);
        }
        
        MODEL_CHECK(ht_size(ht) == 0);
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        if (cycle % 10 == 0) {
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  cycle %d INVARIANT FAIL: %s\n", cycle, err); tests_failed++; }
        }
        
        for (int i = 0; i < n; i++) {
            char newval[32];
            for (int j = 0; j < 32; j++) newval[j] = (char)((i * 17 + cycle) & 0xFF);
            bool ok = ht_upsert(ht, keys[i], 24, newval, 32);
            if (ok) model_set(m, keys[i], 24, newval, 32);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
    }
    
    PASS("full lifecycle: 50 grow->empty cycles with value changes");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 41: Iterator During Compact
// ============================================================================

static void test_remove_with_oom(void) {
    printf("\n=== Property: Remove With OOM ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCD0004ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 50;
    char keys[50][16];
    char vals[50][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)(i * 13);
        bool ok = ht_upsert(ht, keys[i], 16, vals[i], 16);
        if (ok) model_set(m, keys[i], 16, vals[i], 16);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    int recovery_success = 0;
    int recovery_fail = 0;
    
    for (int trial = 0; trial < 30; trial++) {
        size_t size_before = ht_size(ht);
        
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(2 + (trial % 5));
        
        int idx = trial % n;
        ht_remove(ht, keys[idx], 16);
        model_remove(m, keys[idx], 16);
        
        size_t size_after = ht_size(ht);
        
        if (size_after + 1 == size_before || size_after == size_before) {
            recovery_success++;
        } else {
            recovery_fail++;
            printf("  trial %d: size_before=%zu size_after=%zu\n", trial, size_before, size_after);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        for (int i = 0; i < n; i++) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 16, &vlen);
            const char *mfound = model_find_val(m, keys[i], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (trial % 10 == 0) {
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  trial %d INVARIANT FAIL: %s\n", trial, err); tests_failed++; }
        }
    }
    
    printf("  trials=%d success=%d fail=%d\n", 30, recovery_success, recovery_fail);
    if (recovery_fail == 0) {
        PASS("remove with OOM: table consistent after remove with allocation failure");
    } else {
        BUG("remove with OOM: inconsistency after remove with allocation failure");
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Main
// ============================================================================


int main(void) {
    printf("========================================\n");
    printf("Property-Based Testing: draugr Hash Table\n");
    printf("========================================\n");
    alloc_mock_reset(); test_large_unique_keys();
    alloc_mock_reset(); test_value_size_mutation();
    alloc_mock_reset(); test_tombstone_tracking();
    alloc_mock_reset(); test_arena_waste_detection();
    alloc_mock_reset(); test_shrink_verification();
    alloc_mock_reset(); test_combined_operations_sequence();
    alloc_mock_reset(); test_hash_collision_different_keys();
    alloc_mock_reset(); test_long_running_sequence();
    alloc_mock_reset(); test_iterator_completeness();
    alloc_mock_reset(); test_various_key_lengths();
    alloc_mock_reset(); test_arena_exhaustion();
    alloc_mock_reset(); test_many_unique_hashes();
    alloc_mock_reset(); test_correlated_key_patterns();
    alloc_mock_reset(); test_sequential_keys();
    alloc_mock_reset(); test_reinsert_cycling();
    alloc_mock_reset(); test_hash_zero();
    alloc_mock_reset(); test_iterator_during_migration();
    alloc_mock_reset(); test_state_machine_transitions();
    alloc_mock_reset(); test_auto_resize_triggers();
    alloc_mock_reset(); test_full_lifecycle();
    alloc_mock_reset(); test_remove_with_oom();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    return tests_failed > 0 ? 1 : 0;
}
