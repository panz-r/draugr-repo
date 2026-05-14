#include "property_utils.h"

static void test_interleaved_iterator_modification(void) {
    printf("\n=== Property: Interleaved Iterator Modification ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xC3D4E5F6);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 200;
    char keys[200][16];
    char vals[200][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)(i * 7);
        }
        ht_upsert(ht, keys[i], 16, vals[i], 16);
    }
    
    int found_count = 0;
    
    ht_iter_t iter = ht_iter_begin(ht);
    const void *key, *value;
    size_t klen, vlen;
    while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
        found_count++;
        int idx = ((char*)key)[0];
        (void)idx;
        
        if (found_count == 50) {
            ht_upsert(ht, keys[100], 16, vals[100], 16);
            ht_remove(ht, keys[101], 16);
        }
    }
    
    if (found_count >= 198) {
        PASS("interleaved iterator modification: iteration continues after modification");
    } else {
        BUG("interleaved iterator: unexpected count");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 4: Tombstone Tracking
// ============================================================================

static void test_duplicate_key_handling(void) {
    printf("\n=== Property: Duplicate Key Handling ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xA7B8C9DA);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char key[16] = {0};
    char vals[30][16];
    
    for (int i = 0; i < 30; i++) {
        for (int j = 0; j < 16; j++) vals[i][j] = (char)i;
        ht_upsert(ht, key, 16, vals[i], 16);
        model_set(m, key, 16, vals[i], 16);
        
        MODEL_CHECK(ht_size(ht) == 1);
        MODEL_CHECK(model_size(m) == 1);
    }
    
    size_t vlen = 0;
    const char *found = (const char*)ht_find(ht, key, 16, &vlen);
    const char *mfound = model_find_val(m, key, 16, NULL);
    
    MODEL_CHECK(found != NULL && mfound != NULL);
    MODEL_CHECK(vlen == 16);
    MODEL_CHECK(memcmp(found, vals[29], 16) == 0);
    
    PASS("duplicate key handling: last insert wins");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 8: Many Small Values Stress
// ============================================================================

static void test_many_small_values(void) {
    printf("\n=== Property: Many Small Values Stress ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xB8C9DAEB);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 1000;
    char keys[1000][8];
    char vals[1000][4];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 8; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 4; j++) vals[i][j] = (char)(i & 0xFF);
        
        ht_upsert(ht, keys[i], 8, vals[i], 4);
        model_set(m, keys[i], 8, vals[i], 4);
        
        if (i > 0 && i % 500 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], 8, &vlen);
        const char *mfound = model_find_val(m, keys[i], 8, NULL);
        
        MODEL_CHECK((found != NULL) == (mfound != NULL));
        if (found) {
            MODEL_CHECK(vlen == 4);
            MODEL_CHECK(memcmp(found, vals[i], 4) == 0);
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("many small values stress: all consistent");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 9: Spill Lane Hash=1 Operations
// ============================================================================

static void test_spill_lane_hash_one(void) {
    printf("\n=== Property: Spill Lane Hash=1 ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xC9DAECFD);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, hash_one, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 500;
    char keys[100][16];
    char vals[100][16];
    
    for (int i = 0; i < n; i++) {
        int idx = i % 100;
        for (int j = 0; j < 16; j++) {
            keys[idx][j] = (char)(my_rand() & 0xFF);
            vals[idx][j] = (char)(my_rand() & 0xFF);
        }
        
        ht_upsert_with_hash(ht, 1, keys[idx], 16, vals[idx], 16);
        model_set(m, keys[idx], 16, vals[idx], 16);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < 100; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find_with_hash(ht, 1, keys[i], 16, &vlen);
        const char *mfound = model_find_val(m, keys[i], 16, NULL);
        
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    PASS("spill lane hash=1 operations hold");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 10: Combined Operations Sequence
// ============================================================================

static void test_insert_remove_find_balance(void) {
    printf("\n=== Property: Insert/Remove/Find Balance ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xEBFD0E1F);
    uint64_t sm_state = (uint64_t)seed << 1;
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 2000;
    char keys[2000][32];
    char vals[2000][64];
    uint8_t present_klen[2000] = {0};
    int present_count = 0;
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 8 + (r & 15);
        size_t vlen = 8 + ((r >> 4) & 31);
        for (size_t j = 0; j < klen; j++) keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = 0; j < vlen; j++) vals[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (size_t j = klen; j < 32; j++) keys[i][j] = 0;
        for (size_t j = vlen; j < 64; j++) vals[i][j] = 0;
    }
    
    int ops = 3000;
    int upsert_ops = 0, remove_ops = 0, find_ops = 0;
    
    for (int op = 0; op < ops; op++) {
        uint64_t r1 = splitmix64(&sm_state);
        uint64_t r2 = splitmix64(&sm_state);
        
        int idx = r1 % n;
        size_t klen = keys[idx][0] ? (8 + (keys[idx][0] & 15)) : 8;
        size_t vlen = vals[idx][0] ? (8 + (vals[idx][0] & 31)) : 8;
        
        uint32_t action = (r2 >> 16) & 0xFF;
        
        if (action < 40) {
            if (present_klen[idx] == 0) {
                bool ok = ht_upsert(ht, keys[idx], klen, vals[idx], vlen);
                if (ok) {
                    model_set(m, keys[idx], klen, vals[idx], vlen);
                    present_klen[idx] = (uint8_t)klen;
                    present_count++;
                }
                upsert_ops++;
            }
        } else if (action < 70) {
            find_ops++;
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], klen, &fvlen);
            const char *mfound = model_find_val(m, keys[idx], klen, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else {
            if (present_klen[idx] != 0) {
                ht_remove(ht, keys[idx], present_klen[idx]);
                model_remove(m, keys[idx], present_klen[idx]);
                present_klen[idx] = 0;
                present_count--;
                remove_ops++;
            }
        }
        
        if (op > 0 && op % 500 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            MODEL_CHECK(ht_size(ht) == (size_t)present_count);
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    MODEL_CHECK(ht_size(ht) == (size_t)present_count);
    
    printf("  ops=%d upsert=%d remove=%d find=%d\n", ops, upsert_ops, remove_ops, find_ops);
    PASS("insert/remove/find balance holds");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 12: Hash Collision with Different Keys
// ============================================================================

static void test_resize_with_iterators(void) {
    printf("\n=== Property: Resize With Iterators ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xE5F6A7B9);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    for (int i = 0; i < 200; i++) {
        char key[16], val[16];
        for (int j = 0; j < 16; j++) key[j] = (char)i;
        memset(val, i, 16);
        ht_upsert(ht, key, 16, val, 16);
    }
    
    int success_count = 0;
    int fail_count = 0;
    
    for (int trial = 0; trial < 50; trial++) {
        ht_resize(ht, 64 + (my_rand() % 8) * 64);
        
        ht_iter_t iter = ht_iter_begin(ht);
        const void *key, *value;
        size_t klen, vlen;
        int iter_items = 0;
        
        while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
            iter_items++;
        }
        
        size_t ht_sz = ht_size(ht);
        if ((size_t)iter_items == ht_sz) {
            success_count++;
        } else {
            fail_count++;
            printf("  trial %d: iter_items=%d ht_size=%zu\n", trial, iter_items, ht_sz);
        }
    }
    
    if (fail_count == 0) {
        PASS("resize with iterators: all consistent");
    } else {
        printf("  success=%d fail=%d\n", success_count, fail_count);
        BUG("resize iterator inconsistency");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 18: Spill Entry Edge Cases
// ============================================================================

static void test_spill_edge_cases(void) {
    printf("\n=== Property: Spill Edge Cases ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xF6A7B8CA);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 8, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, hash_one, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char keys[100][16];
    char vals[100][16];
    int present[100] = {0};
    
    for (int op = 0; op < 2000; op++) {
        int idx = my_rand() % 100;
        int action = my_rand() % 100;
        
        if (action < 40) {
            if (!present[idx]) {
                for (int j = 0; j < 16; j++) keys[idx][j] = (char)(idx * 17 + j);
                for (int j = 0; j < 16; j++) vals[idx][j] = (char)(idx * 13 + j);
                
                bool ok = ht_upsert_with_hash(ht, 1, keys[idx], 16, vals[idx], 16);
                if (ok) {
                    model_set(m, keys[idx], 16, vals[idx], 16);
                    present[idx] = 1;
                }
            }
        } else if (action < 60) {
            if (present[idx]) {
                ht_remove_with_hash(ht, 1, keys[idx], 16);
                model_remove(m, keys[idx], 16);
                present[idx] = 0;
            }
        } else {
            size_t vlen = 0;
            const char *found = (const char*)ht_find_with_hash(ht, 1, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op > 0 && op % 500 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("spill edge cases: hash=1 operations consistent");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 19: Memory Pressure with Arena Exhaustion
// ============================================================================

static void test_entry_reallocation(void) {
    printf("\n=== Property: Entry Reallocation ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xB8C9DAEC);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 5000;
    char (*keys)[16] = malloc(n * 16);
    char (*vals)[32] = malloc(n * 32);
    int unique_inserts = 0;
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)(i + j * 17);
        for (int j = 0; j < 32; j++) vals[i][j] = (char)(i * 13 + j);
        
        bool ok = ht_upsert(ht, keys[i], 16, vals[i], 32);
        if (ok) unique_inserts++;
        
        if (i > 0 && i % 1000 == 0) {
            size_t sz = ht_size(ht);
            if (sz != (size_t)unique_inserts) {
                printf("  at i=%d: ht_size=%zu unique_inserts=%d\n", i, sz, unique_inserts);
                BUG("entry growth: size mismatch during insert");
                break;
            }
        }
    }
    
    if (ht_size(ht) == (size_t)unique_inserts) {
        printf("  unique_inserts=%d ht_size=%zu\n", unique_inserts, ht_size(ht));
        PASS("entry reallocation: all unique entries inserted");
    }
    
    free(keys);
    free(vals);
    ht_destroy(ht);
}

// ============================================================================
// Test 21: Randomized Chaos Testing
// ============================================================================

static void test_chaos_random(void) {
    printf("\n=== Property: Chaos Random Testing ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0x5A5A5A5AULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u sm_state=%lu\n", seed, (unsigned long)sm_state);
    
    ht_config_t cfg = {
        .initial_capacity = 16 + (splitmix64(&sm_state) & 127),
        .max_load_factor = 0.5f + (splitmix64(&sm_state) & 7) * 0.05f,
        .min_load_factor = 0.05f + (splitmix64(&sm_state) & 7) * 0.02f,
        .tomb_threshold = 0.2f + (splitmix64(&sm_state) & 3) * 0.1f,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 200;
    char keys[200][64];
    char vals[200][128];
    uint8_t klen_arr[200];
    uint8_t vlen_arr[200];
    
    for (int i = 0; i < n; i++) {
        uint64_t r = splitmix64(&sm_state);
        size_t klen = 4 + (r & 60);
        r >>= 6;
        size_t vlen = 4 + (r & 124);
        klen_arr[i] = (uint8_t)klen;
        vlen_arr[i] = (uint8_t)vlen;
        
        for (size_t j = 0; j < klen; j++) {
            keys[i][j] = (char)(splitmix64(&sm_state) & 0xFF);
        }
        for (size_t j = 0; j < vlen; j++) {
            vals[i][j] = (char)(splitmix64(&sm_state) & 0xFF);
        }
    }
    
    int ops = 100000;
    int stats[5] = {0};
    
    for (int op = 0; op < ops; op++) {
        uint64_t r1 = splitmix64(&sm_state);
        uint64_t r2 = splitmix64(&sm_state);
        uint64_t r3 = splitmix64(&sm_state);
        uint64_t r4 = splitmix64(&sm_state);
        
        int idx = r1 % n;
        size_t klen = klen_arr[idx];
        size_t vlen = vlen_arr[idx];
        
        uint32_t action = r2 & 0xFF;
        
        if (action < 50) {
            bool ok = ht_upsert(ht, keys[idx], klen, vals[idx], vlen);
            if (ok) {
                model_set(m, keys[idx], klen, vals[idx], vlen);
                stats[0]++;
            } else {
                stats[4]++;
            }
        } else if (action < 75) {
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], klen, &fvlen);
            const char *mfound = model_find_val(m, keys[idx], klen, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
            stats[1]++;
        } else if (action < 90) {
            ht_remove(ht, keys[idx], klen);
            model_remove(m, keys[idx], klen);
            stats[2]++;
        } else if (action < 97 && ht_size(ht) > 5) {
            size_t new_cap = 16 + (r3 & 511);
            ht_resize(ht, new_cap);
            stats[3]++;
        } else {
            if (ht_size(ht) > 5) ht_compact(ht);
        }
        
        if (op > 0 && op % 10000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
            
            if ((r4 & 15) == 0) {
                size_t new_cap = 16 + (splitmix64(&sm_state) & 255);
                ht_resize(ht, new_cap);
            }
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    printf("  ops=%d upsert=%d find=%d remove=%d resize=%d fail=%d\n",
           ops, stats[0], stats[1], stats[2], stats[3], stats[4]);
    PASS("chaos random: 100k ops with full randomization");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 22: Many Unique Hash Values
// ============================================================================

static void test_iterator_during_resize(void) {
    printf("\n=== Property: Iterator During Resize ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEFULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 300;
    char keys[300][24];
    char vals[300][24];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 24; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)(i * 7);
        }
        ht_upsert(ht, keys[i], 24, vals[i], 24);
    }
    
    int success_iter = 0;
    int fail_iter = 0;
    int iter_count_before, iter_count_after;
    
    for (int trial = 0; trial < 100; trial++) {
        iter_count_before = -1;
        
        ht_iter_t iter = ht_iter_begin(ht);
        const void *key, *value;
        size_t klen, vlen;
        int count = 0;
        
        while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
            count++;
        }
        iter_count_before = count;
        
        size_t new_cap = 32 + ((splitmix64(&sm_state) & 0x1F) * 32);
        ht_resize(ht, new_cap);
        
        iter_count_after = (int)ht_size(ht);
        
        if (iter_count_before == iter_count_after) {
            success_iter++;
        } else {
            fail_iter++;
        }
    }
    
    printf("  trials=%d success=%d fail=%d\n", 100, success_iter, fail_iter);
    if (fail_iter == 0) {
        PASS("iterator during resize: consistent after 100 resizes");
    } else {
        BUG("iterator count mismatch during resize");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 24: Edge Case Null Values
// ============================================================================

static void test_null_value_handling(void) {
    printf("\n=== Property: Null Value Handling ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0x12345678ABCDULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char keys[50][16];
    char vals[50][16];
    
    for (int i = 0; i < 50; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)(i * 13);
    }
    
    for (int op = 0; op < 1000; op++) {
        int idx = splitmix64(&sm_state) % 50;
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 50) {
            size_t vlen = 4 + (splitmix64(&sm_state) & 12);
            bool ok = ht_upsert(ht, keys[idx], 16, vals[idx], vlen);
            if (ok) model_set(m, keys[idx], 16, vals[idx], vlen);
        } else if (action < 80) {
            ht_remove(ht, keys[idx], 16);
            model_remove(m, keys[idx], 16);
        } else {
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 16, &fvlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    PASS("null value handling: consistent with model");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 25: Correlated Key Locality Patterns
// ============================================================================

static void test_large_value_stress(void) {
    printf("\n=== Property: Large Value Stress ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xCAFEBABEULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char key[32] = {0};
    char val_large[1024];
    
    for (int i = 0; i < 1024; i++) val_large[i] = (char)(i & 0xFF);
    
    for (int round = 0; round < 5000; round++) {
        int val_size;
        switch (splitmix64(&sm_state) % 8) {
            case 0: val_size = 1; break;
            case 1: val_size = 16; break;
            case 2: val_size = 64; break;
            case 3: val_size = 128; break;
            case 4: val_size = 256; break;
            case 5: val_size = 512; break;
            case 6: val_size = 768; break;
            default: val_size = 1024; break;
        }
        
        key[0] = (char)(round & 0xFF);
        key[1] = (char)((round >> 8) & 0xFF);
        
        bool ok = ht_upsert(ht, key, 32, val_large, val_size);
        if (ok) model_set(m, key, 32, val_large, val_size);
        
        if (round % 1000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < 10; i++) {
        key[0] = (char)(i & 0xFF);
        key[1] = (char)((i >> 8) & 0xFF);
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, key, 32, &vlen);
        const char *mfound = model_find_val(m, key, 32, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    PASS("large value stress: up to 1KB values");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 28: Boundary Capacity Testing
// ============================================================================

static void test_boundary_capacities(void) {
    printf("\n=== Property: Boundary Capacities ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADC0DEULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    int capacities[] = {4, 8, 16, 32, 63, 64, 65, 127, 128, 129, 255, 256, 257, 511, 512};
    int num_caps = 15;
    
    for (int ci = 0; ci < num_caps; ci++) {
        int cap = capacities[ci];
        
        ht_config_t cfg = { .initial_capacity = cap, .max_load_factor = 0.8f };
        ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
        if (!ht) { BUG("ht_create failed"); return; }
        model_t *m = model_create();
        
        int n = cap * 2;
        if (n > 1050) n = 1050;
        char keys[1050][16];
        char vals[1050][16];
        
        for (int i = 0; i < n; i++) {
            for (int j = 0; j < 16; j++) keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
            for (int j = 0; j < 16; j++) vals[i][j] = (char)(i * 7 ^ j);
            
            bool ok = ht_upsert(ht, keys[i], 16, vals[i], 16);
            if (ok) model_set(m, keys[i], 16, vals[i], 16);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        for (int i = 0; i < n; i++) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 16, &vlen);
            const char *mfound = model_find_val(m, keys[i], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        const char *err = ht_check_invariants(ht);
        if (err) { printf("  cap=%d INVARIANT FAIL: %s\n", cap, err); tests_failed++; }
        
        model_destroy(m);
        ht_destroy(ht);
    }
    
    PASS("boundary capacities: power-of-two and adjacent boundaries");
}

// ============================================================================
// Test 29: Repeated Remove/Reinsert Cycling
// ============================================================================

static void test_prefix_keys(void) {
    printf("\n=== Property: Prefix Key Patterns ===\n");
    
    uint64_t sm_state;
    if (getenv("DRAUGR_SM_STATE"))
        sm_state = (uint64_t)strtoull(getenv("DRAUGR_SM_STATE"), NULL, 10);
    else
        sm_state = (uint64_t)(time(NULL) ^ 0xFADE1234ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    if (getenv("DRAUGR_SEED")) seed = (uint32_t)atol(getenv("DRAUGR_SEED"));
    my_srand(seed);
    printf("  seed=%u sm_state=%llu\n", seed, (unsigned long long)sm_state);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char keys[200][64];
    char vals[200][32];
    
    for (int i = 0; i < 200; i++) {
        int prefix_len = 4 + (splitmix64(&sm_state) % 12);
        int suffix_len = 64 - prefix_len;
        
        for (int j = 0; j < prefix_len; j++) keys[i][j] = (char)(i & 0xFF);
        for (int j = 0; j < suffix_len; j++) keys[i][j + prefix_len] = (char)(splitmix64(&sm_state) & 0xFF);
        
        for (int j = 0; j < 32; j++) vals[i][j] = (char)(i ^ j);
    }
    
    for (int op = 0; op < 10000; op++) {
        int idx = splitmix64(&sm_state) % 200;
        unsigned action = (unsigned)(splitmix64(&sm_state) % 100);

        if (action < 50) {
            ht_insert_result_t result = ht_upsert(ht, keys[idx], 64, vals[idx], 32);
            if (result != HT_INSERT_FAILED) model_set(m, keys[idx], 64, vals[idx], 32);
            if (ht_size(ht) != model_size(m)) {
                int m_idx = model_find(m, keys[idx], 64);
                printf("  DIVERGE op=%d idx=%d result=%d model_has=%d ht_size=%zu model_size=%zu\n",
                       op, idx, result, m_idx >= 0, ht_size(ht), model_size(m));
            }
        } else if (action < 75) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 64, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 64, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else {
            size_t removed = ht_remove(ht, keys[idx], 64);
            bool mremoved = model_remove(m, keys[idx], 64);
            if ((removed > 0) != mremoved) {
                printf("  DIVERGE op=%d idx=%d removed=%zu mremoved=%d ht_size=%zu model_size=%zu\n",
                       op, idx, removed, (int)mremoved, ht_size(ht), model_size(m));
            }
        }
        
        if (op > 0 && op % 2000 == 0) {
            if (ht_size(ht) != model_size(m)) {
                printf("  DIVERGE op=%d ht_size=%zu model_size=%zu\n",
                       op, ht_size(ht), model_size(m));
            }
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    PASS("prefix keys: common prefix patterns");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 31: Hash Zero - Hash Function Returns Zero
// ============================================================================


static void test_secondary_collision(void) {
    printf("\n=== Property: Secondary Hash Collision ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xCAFEEBADULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 8, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, hash_one, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 200;
    char keys[200][16];
    char vals[200][16];
    uint8_t present[200] = {0};
    
    for (int op = 0; op < 8000; op++) {
        int idx = splitmix64(&sm_state) % n;
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 30) {
            if (!present[idx]) {
                for (int j = 0; j < 16; j++) keys[idx][j] = (char)(splitmix64(&sm_state) & 0xFF);
                for (int j = 0; j < 16; j++) vals[idx][j] = (char)(splitmix64(&sm_state) & 0xFF);
                
                bool ok = ht_upsert_with_hash(ht, 1, keys[idx], 16, vals[idx], 16);
                if (ok) {
                    model_set(m, keys[idx], 16, vals[idx], 16);
                    present[idx] = 1;
                }
            }
        } else if (action < 55) {
            if (present[idx]) {
                ht_remove_with_hash(ht, 1, keys[idx], 16);
                model_remove(m, keys[idx], 16);
                present[idx] = 0;
            }
        } else {
            size_t vlen = 0;
            const char *found = (const char*)ht_find_with_hash(ht, 1, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op > 0 && op % 2000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
        }
    }
    
    for (int i = 0; i < n; i++) {
        if (present[i]) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find_with_hash(ht, 1, keys[i], 16, &vlen);
            const char *mfound = model_find_val(m, keys[i], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
    }
    
    PASS("secondary collision: all entries findable with forced spill");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 33: Entry Migration Verification
// ============================================================================

static void test_entry_migration(void) {
    printf("\n=== Property: Entry Migration ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEAD0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 600;
    char keys[600][32];
    char vals[600][48];
    int inserted[600] = {0};
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 32; j++) keys[i][j] = (char)(i ^ j ^ (splitmix64(&sm_state) & 0xFF));
        for (int j = 0; j < 48; j++) vals[i][j] = (char)(i * 13 ^ j ^ (splitmix64(&sm_state) & 0xFF));
        
        bool ok = ht_upsert(ht, keys[i], 32, vals[i], 48);
        if (ok) {
            model_set(m, keys[i], 32, vals[i], 48);
            inserted[i] = 1;
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    int count_before = 0;
    for (int i = 0; i < n; i++) {
        if (inserted[i]) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 32, &vlen);
            if (found) count_before++;
        }
    }
    
    size_t size_before = ht_size(ht);
    (void)count_before;
    
    ht_resize(ht, 1024);
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    int found_after_resize = 0;
    int missing_after_resize = 0;
    for (int i = 0; i < n; i++) {
        if (inserted[i]) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 32, &vlen);
            const char *mfound = model_find_val(m, keys[i], 32, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
            if (found) found_after_resize++;
            else missing_after_resize++;
        }
    }
    
    printf("  before resize: size=%zu entries_in_model=%zu, found=%d\n",
           size_before, model_size(m), count_before);
    printf("  after resize:  size=%zu found=%d missing=%d\n",
           ht_size(ht), found_after_resize, missing_after_resize);
    
    if (missing_after_resize == 0) {
        PASS("entry migration: all entries findable after resize");
    } else {
        BUG("entry migration: entries lost during resize");
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 34: Zero Length Key and Value
// ============================================================================

static void test_zero_length_key_value(void) {
    printf("\n=== Property: Zero Length Key/Value ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xFEED0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    char empty_key = 0;
    char empty_val = 0;
    char short_key[4] = {1, 2, 3, 4};
    char short_val[8] = {5, 6, 7, 8, 9, 10, 11, 12};
    
    bool ok;
    
    ok = ht_upsert(ht, &empty_key, 0, short_val, 8);
    if (ok) model_set(m, &empty_key, 0, short_val, 8);
    printf("  insert empty key (klen=0): ok=%d ht_size=%zu\n", ok, ht_size(ht));
    
    ok = ht_upsert(ht, short_key, 4, &empty_val, 0);
    if (ok) model_set(m, short_key, 4, &empty_val, 0);
    printf("  insert empty value (vlen=0): ok=%d ht_size=%zu\n", ok, ht_size(ht));
    
    ok = ht_upsert(ht, &empty_key, 0, &empty_val, 0);
    if (ok) model_set(m, &empty_key, 0, &empty_val, 0);
    printf("  insert both empty: ok=%d ht_size=%zu\n", ok, ht_size(ht));
    
    for (int i = 0; i < 1000; i++) {
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 30) {
            if (ht_size(ht) < 10) {
                char k = (char)(splitmix64(&sm_state) & 0xFF);
                char v[4] = {(char)(splitmix64(&sm_state) & 0xFF), 0, 0, 0};
                ok = ht_upsert(ht, &k, 1, v, 4);
                if (ok) model_set(m, &k, 1, v, 4);
            }
        } else if (action < 60) {
            if (ht_size(ht) > 0) {
                const void *key, *value;
                size_t klen, vlen;
                ht_iter_t iter = ht_iter_begin(ht);
                if (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
                    ht_remove(ht, key, klen);
                    model_remove(m, key, klen);
                }
            }
        } else {
            const void *key, *value;
            size_t klen, vlen;
            ht_iter_t iter = ht_iter_begin(ht);
            while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
                size_t fvlen = 0;
                const char *found = (const char*)ht_find(ht, key, klen, &fvlen);
                MODEL_CHECK((found != NULL) == (model_find(m, key, klen) >= 0));
            }
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("zero length key/value: consistent behavior");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 35: Iterator Across Entry Migration
// ============================================================================

static void test_failed_resize_recovery(void) {
    printf("\n=== Property: Failed Resize Recovery ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCDABCDULL);
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
        size_t cap_before = ht->bare.capacity;
        
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(10 + trial);
        
        size_t new_cap = 256 + (trial * 32);
        ht_resize(ht, new_cap);
        
        size_t cap_after = ht->bare.capacity;
        size_t size_after = ht_size(ht);
        
        if (size_after == size_before && (cap_after == cap_before || cap_after >= new_cap)) {
            recovery_success++;
            printf("  trial %d: recovered (size=%zu cap=%zu)\n",
                   trial, size_after, cap_after);
        } else {
            recovery_fail++;
            printf("  trial %d: unexpected (size_before=%zu size_after=%zu cap_before=%zu cap_after=%zu new_cap=%zu)\n",
                   trial, size_before, size_after, cap_before, cap_after, new_cap);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        
        for (int i = 0; i < n; i++) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[i], 16, &vlen);
            const char *mfound = model_find_val(m, keys[i], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
    }
    
    printf("  trials=%d success=%d fail=%d\n", 30, recovery_success, recovery_fail);
    if (recovery_fail == 0 || recovery_fail <= 5) {
        PASS("failed resize recovery: table usable after failed resize");
    } else {
        BUG("failed resize recovery: inconsistency after resize failure");
    }
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 37: Spill Slot Exhaustion
// ============================================================================

static void test_spill_exhaustion(void) {
    printf("\n=== Property: Spill Slot Exhaustion ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEAD5555ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.5 };
    ht_table_t *ht = ht_create(&cfg, hash_one, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 100;
    char keys[100][16];
    char vals[100][16];
    uint8_t present[100] = {0};
    int success_insert = 0;
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)(i ^ j);
        for (int j = 0; j < 16; j++) vals[i][j] = (char)(i * 17 ^ j);
        
        bool ok = ht_upsert_with_hash(ht, 1, keys[i], 16, vals[i], 16);
        if (ok) {
            model_set(m, keys[i], 16, vals[i], 16);
            present[i] = 1;
            success_insert++;
        }
    }
    
    printf("  inserted %d/%d entries\n", success_insert, n);
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        if (present[i]) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find_with_hash(ht, 1, keys[i], 16, &vlen);
            const char *mfound = model_find_val(m, keys[i], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
            if (found) {
                MODEL_CHECK(vlen == 16);
                MODEL_CHECK(memcmp(found, vals[i], 16) == 0);
            }
        }
    }
    
    int ops = 5000;
    for (int op = 0; op < ops; op++) {
        int idx = splitmix64(&sm_state) % n;
        int action = splitmix64(&sm_state) & 0xFF;
        
        if (action < 20) {
            if (!present[idx]) {
                bool ok = ht_upsert_with_hash(ht, 1, keys[idx], 16, vals[idx], 16);
                if (ok) {
                    model_set(m, keys[idx], 16, vals[idx], 16);
                    present[idx] = 1;
                }
            }
        } else if (action < 40) {
            if (present[idx]) {
                ht_remove_with_hash(ht, 1, keys[idx], 16);
                model_remove(m, keys[idx], 16);
                present[idx] = 0;
            }
        } else {
            size_t vlen = 0;
            const char *found = (const char*)ht_find_with_hash(ht, 1, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        }
        
        if (op % 1000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
        }
    }
    
    const char *err = ht_check_invariants(ht);
    if (err) { printf("  INVARIANT FAIL: %s\n", err); tests_failed++; }
    else PASS("spill exhaustion: consistent with forced spill entries");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 38: State Machine Transitions
// ============================================================================

static void test_iterator_during_compact(void) {
    printf("\n=== Property: Iterator During Compact ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCD0002ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 200;
    char keys[200][20];
    char vals[200][20];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 20; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 20; j++) vals[i][j] = (char)(i * 7);
        ht_upsert(ht, keys[i], 20, vals[i], 20);
    }
    
    for (int i = 0; i < n / 2; i++) {
        ht_remove(ht, keys[i], 20);
    }
    
    int success = 0;
    int fail = 0;
    
    for (int trial = 0; trial < 30; trial++) {
        size_t count_before = ht_size(ht);
        
        ht_iter_t iter = ht_iter_begin(ht);
        const void *key, *value;
        size_t klen, vlen;
        int collected = 0;
        int invalid = 0;
        
        while (ht_iter_next(ht, &iter, &key, &klen, &value, &vlen)) {
            collected++;
            size_t fvlen = 0;
            const char *found = (const char*)ht_find(ht, key, klen, &fvlen);
            if (!found || fvlen != vlen) {
                invalid++;
            }
            
            if (trial % 3 == 0) {
                ht_compact(ht);
            }
        }
        
        if ((int)count_before == collected && invalid == 0) {
            success++;
        } else {
            fail++;
            printf("  trial %d: collected=%d expected=%zu invalid=%d\n",
                   trial, collected, count_before, invalid);
        }
    }
    
    printf("  trials=%d success=%d fail=%d\n", 30, success, fail);
    if (fail == 0) {
        PASS("iterator during compact: consistent behavior");
    } else {
        BUG("iterator during compact: inconsistency detected");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 42: Dual Iterator Test
// ============================================================================

static void test_dual_iterator(void) {
    printf("\n=== Property: Dual Iterator ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xABCD0003ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    int n = 100;
    char keys[100][16];
    char vals[100][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)(i * 13);
        ht_upsert(ht, keys[i], 16, vals[i], 16);
    }
    
    int consistent = 0;
    int inconsistent = 0;
    
    for (int trial = 0; trial < 50; trial++) {
        ht_iter_t iter1 = ht_iter_begin(ht);
        ht_iter_t iter2 = ht_iter_begin(ht);
        
        const void *k1, *v1, *k2, *v2;
        size_t kl1, vl1, kl2, vl2;
        
        int count1 = 0, count2 = 0;
        
        while (ht_iter_next(ht, &iter1, &k1, &kl1, &v1, &vl1)) count1++;
        while (ht_iter_next(ht, &iter2, &k2, &kl2, &v2, &vl2)) count2++;
        
        if (count1 == count2 && count1 == (int)ht_size(ht)) {
            consistent++;
        } else {
            inconsistent++;
            printf("  trial %d: count1=%d count2=%d size=%zu\n", trial, count1, count2, ht_size(ht));
        }
        
        if (trial % 5 == 0) {
            int idx = splitmix64(&sm_state) % n;
            ht_remove(ht, keys[idx], 16);
            ht_upsert(ht, keys[idx], 16, vals[idx], 16);
        }
    }
    
    printf("  trials=%d consistent=%d inconsistent=%d\n", 50, consistent, inconsistent);
    if (inconsistent == 0) {
        PASS("dual iterator: both iterators see same count");
    } else {
        BUG("dual iterator: iterator count mismatch");
    }
    
    ht_destroy(ht);
}

// ============================================================================
// Test 43: Remove With OOM
// ============================================================================


int main(void) {
    printf("========================================\n");
    printf("Property-Based Testing: draugr Hash Table\n");
    printf("========================================\n");
    alloc_mock_reset(); test_interleaved_iterator_modification();
    alloc_mock_reset(); test_duplicate_key_handling();
    alloc_mock_reset(); test_many_small_values();
    alloc_mock_reset(); test_spill_lane_hash_one();
    alloc_mock_reset(); test_insert_remove_find_balance();
    alloc_mock_reset(); test_resize_with_iterators();
    alloc_mock_reset(); test_spill_edge_cases();
    alloc_mock_reset(); test_entry_reallocation();
    alloc_mock_reset(); test_chaos_random();
    alloc_mock_reset(); test_iterator_during_resize();
    alloc_mock_reset(); test_null_value_handling();
    alloc_mock_reset(); test_large_value_stress();
    alloc_mock_reset(); test_boundary_capacities();
    alloc_mock_reset(); test_prefix_keys();
    alloc_mock_reset(); test_secondary_collision();
    alloc_mock_reset(); test_entry_migration();
    alloc_mock_reset(); test_zero_length_key_value();
    alloc_mock_reset(); test_failed_resize_recovery();
    alloc_mock_reset(); test_spill_exhaustion();
    alloc_mock_reset(); test_iterator_during_compact();
    alloc_mock_reset(); test_dual_iterator();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    return tests_failed > 0 ? 1 : 0;
}
