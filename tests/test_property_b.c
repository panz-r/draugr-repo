#include "property_utils.h"

static void test_multi_table_isolation(void) {
    printf("\n=== Property: Multi-Table Isolation ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEAD0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *t1 = ht_create(NULL, simple_hash, NULL, NULL);
    ht_table_t *t2 = ht_create(NULL, fixed_hash, NULL, NULL);
    ht_table_t *t3 = ht_create(NULL, hash_one, NULL, NULL);
    if (!t1 || !t2 || !t3) { BUG("ht_create failed"); return; }
    
    char key[16] = "shared_key";
    char val1[16] = "value_for_t1";
    char val2[16] = "value_for_t2";
    char val3[16] = "value_for_t3";
    
    ht_upsert(t1, key, 16, val1, 16);
    ht_upsert(t2, key, 16, val2, 16);
    ht_upsert(t3, key, 16, val3, 16);
    
    size_t vlen1 = 0, vlen2 = 0, vlen3 = 0;
    const char *f1 = ht_find(t1, key, 16, &vlen1);
    const char *f2 = ht_find(t2, key, 16, &vlen2);
    const char *f3 = ht_find(t3, key, 16, &vlen3);
    
    MODEL_CHECK(f1 != NULL && memcmp(f1, val1, 16) == 0);
    MODEL_CHECK(f2 != NULL && memcmp(f2, val2, 16) == 0);
    MODEL_CHECK(f3 != NULL && memcmp(f3, val3, 16) == 0);
    
    char new_val2[16] = "modified_t2";
    ht_upsert(t2, key, 16, new_val2, 16);
    
    f1 = ht_find(t1, key, 16, &vlen1);
    MODEL_CHECK(f1 != NULL && memcmp(f1, val1, 16) == 0);
    
    PASS("multi-table isolation: tables independent");
    
    ht_destroy(t1);
    ht_destroy(t2);
    ht_destroy(t3);
}

// ============================================================================
// Test 2: Long-Running Accumulation
// ============================================================================

static void test_long_running_accumulation(void) {
    printf("\n=== Property: Long-Running Accumulation ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEFULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    model_t *m = model_create();
    if (!ht || !m) { BUG("ht_create or model_create failed"); return; }
    
    char keys[100][64];
    char vals[100][64];
    
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 64; j++) {
            keys[i][j] = (char)(i ^ j);
            vals[i][j] = (char)(i * 7 ^ j);
        }
        ht_upsert(ht, keys[i], 64, vals[i], 64);
        model_set(m, keys[i], 64, vals[i], 64);
    }
    
    int op;
    for (op = 0; op < 1000000; op++) {
        unsigned action = (unsigned)(splitmix64(&sm_state) % 100);
        
        if (action < 40) {
            int idx = (int)(splitmix64(&sm_state) % 100);
            char newval[64];
            for (int j = 0; j < 64; j++) newval[j] = (char)(splitmix64(&sm_state) & 0xFF);
            bool ok = ht_upsert(ht, keys[idx], 64, newval, 64);
            if (ok) model_set(m, keys[idx], 64, newval, 64);
        } else if (action < 70) {
            int idx = (int)(splitmix64(&sm_state) % 100);
            size_t vlen = 0;
            const char *found = ht_find(ht, keys[idx], 64, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 64, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else if (action < 90) {
            int idx = (int)(splitmix64(&sm_state) % 100);
            ht_remove(ht, keys[idx], 64);
            model_remove(m, keys[idx], 64);
        } else {
            int idx = (int)(splitmix64(&sm_state) % 100);
            ht_upsert(ht, keys[idx], 64, vals[idx], 64);
            model_set(m, keys[idx], 64, vals[idx], 64);
        }
        
        if (op > 0 && op % 100000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); goto cleanup; }
            printf("  op=%d size=%zu verified\n", op, ht_size(ht));
        }
    }
    
    PASS("long running accumulation: 1M ops consistent");
    
cleanup:
    ht_destroy(ht);
    model_destroy(m);
}

// ============================================================================
// Test 3: Batch vs Interleaved Operations
// ============================================================================

static void test_batch_vs_interleaved(void) {
    printf("\n=== Property: Batch vs Interleaved Operations ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0001ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht_batch = ht_create(NULL, simple_hash, NULL, NULL);
    ht_table_t *ht_inter = ht_create(NULL, simple_hash, NULL, NULL);
    model_t *m_batch = model_create();
    model_t *m_inter = model_create();
    if (!ht_batch || !ht_inter || !m_batch || !m_inter) { BUG("create failed"); goto cleanup; }
    
    char keys[1000][32];
    char vals[1000][32];
    
    for (int i = 0; i < 1000; i++) {
        for (int j = 0; j < 32; j++) {
            keys[i][j] = (char)(i ^ j);
            vals[i][j] = (char)(i * 3 ^ j);
        }
    }
    
    for (int i = 0; i < 1000; i++) {
        ht_upsert(ht_batch, keys[i], 32, vals[i], 32);
        model_set(m_batch, keys[i], 32, vals[i], 32);
    }
    
    for (int i = 0; i < 400; i++) {
        ht_upsert(ht_inter, keys[i], 32, vals[i], 32);
        model_set(m_inter, keys[i], 32, vals[i], 32);
    }
    for (int i = 0; i < 400; i++) {
        size_t vlen = 0;
        ht_find(ht_inter, keys[i], 32, &vlen);
        model_find_val(m_inter, keys[i], 32, NULL);
    }
    for (int i = 400; i < 700; i++) {
        ht_upsert(ht_inter, keys[i], 32, vals[i], 32);
        model_set(m_inter, keys[i], 32, vals[i], 32);
    }
    for (int i = 700; i < 1000; i++) {
        ht_upsert(ht_inter, keys[i], 32, vals[i], 32);
        model_set(m_inter, keys[i], 32, vals[i], 32);
    }
    
    MODEL_CHECK(ht_size(ht_batch) == ht_size(ht_inter));
    MODEL_CHECK(ht_size(ht_batch) == model_size(m_batch));
    MODEL_CHECK(ht_size(ht_inter) == model_size(m_inter));
    
    for (int i = 0; i < 1000; i++) {
        size_t vlen1 = 0, vlen2 = 0;
        const char *f1 = ht_find(ht_batch, keys[i], 32, &vlen1);
        const char *f2 = ht_find(ht_inter, keys[i], 32, &vlen2);
        MODEL_CHECK((f1 != NULL) == (f2 != NULL));
        if (f1 && f2) {
            MODEL_CHECK(vlen1 == vlen2);
            MODEL_CHECK(memcmp(f1, f2, vlen1) == 0);
        }
    }
    
    PASS("batch vs interleaved: same state achieved");
    
cleanup:
    if (ht_batch) ht_destroy(ht_batch);
    if (ht_inter) ht_destroy(ht_inter);
    if (m_batch) model_destroy(m_batch);
    if (m_inter) model_destroy(m_inter);
}

// ============================================================================
// Test 4: Collision Stress
// ============================================================================

static void test_collision_stress(void) {
    printf("\n=== Property: Collision Stress ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xCAFEBABULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, zero_hash, NULL, NULL);
    model_t *m = model_create();
    if (!ht || !m) { BUG("create failed"); goto cleanup; }
    
    char keys[500][32];
    char vals[500][32];
    
    for (int i = 0; i < 500; i++) {
        for (int j = 0; j < 32; j++) {
            keys[i][j] = (char)(i ^ j);
            vals[i][j] = (char)(i * 7 ^ j);
        }
        ht_upsert(ht, keys[i], 32, vals[i], 32);
        model_set(m, keys[i], 32, vals[i], 32);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int op = 0; op < 20000; op++) {
        int idx = (int)(splitmix64(&sm_state) % 500);
        unsigned action = (unsigned)(splitmix64(&sm_state) % 100);
        
        if (action < 50) {
            size_t vlen = 0;
            const char *found = ht_find(ht, keys[idx], 32, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 32, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else if (action < 80) {
            char newval[32];
            for (int j = 0; j < 32; j++) newval[j] = (char)(splitmix64(&sm_state) & 0xFF);
            bool ok = ht_upsert(ht, keys[idx], 32, newval, 32);
            if (ok) model_set(m, keys[idx], 32, newval, 32);
        } else {
            ht_remove(ht, keys[idx], 32);
            model_remove(m, keys[idx], 32);
        }
    }
    
    for (int i = 0; i < 500; i++) {
        size_t vlen = 0;
        const char *found = ht_find(ht, keys[i], 32, &vlen);
        const char *mfound = model_find_val(m, keys[i], 32, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    PASS("collision stress: 20k ops with hash=0");
    
cleanup:
    if (ht) ht_destroy(ht);
    if (m) model_destroy(m);
}

// ============================================================================
// Test 5: Delete-Reinsert Cycling
// ============================================================================

static void test_delete_reinsert_cycling(void) {
    printf("\n=== Property: Delete-Reinsert Cycling ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0002ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    model_t *m = model_create();
    if (!ht || !m) { BUG("create failed"); goto cleanup; }
    
    char keys[20][32];
    char vals[20][32];
    
    for (int i = 0; i < 20; i++) {
        for (int j = 0; j < 32; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)(i * 7);
        }
        ht_upsert(ht, keys[i], 32, vals[i], 32);
        model_set(m, keys[i], 32, vals[i], 32);
    }
    
    for (int cycle = 0; cycle < 100; cycle++) {
        for (int i = 0; i < 20; i++) {
            ht_remove(ht, keys[i], 32);
            model_remove(m, keys[i], 32);
        }
        MODEL_CHECK(ht_size(ht) == 0);
        
        for (int i = 0; i < 20; i++) {
            ht_upsert(ht, keys[i], 32, vals[i], 32);
            model_set(m, keys[i], 32, vals[i], 32);
        }
        MODEL_CHECK(ht_size(ht) == 20);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    PASS("delete reinsert cycling: 100 cycles pass");
    
cleanup:
    if (ht) ht_destroy(ht);
    if (m) model_destroy(m);
}

// ============================================================================
// Test 6: Progressive Load Factor Testing
// ============================================================================

static void test_progressive_load_factor(void) {
    printf("\n=== Property: Progressive Load Factor ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0003ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_config_t configs[] = {
        {.initial_capacity = 16, .max_load_factor = 0.5},
        {.initial_capacity = 16, .max_load_factor = 0.7},
        {.initial_capacity = 16, .max_load_factor = 0.9},
        {.initial_capacity = 16, .max_load_factor = 0.99},
    };
    int num_configs = 4;
    
    for (int cfg = 0; cfg < num_configs; cfg++) {
        ht_table_t *ht = ht_create(&configs[cfg], simple_hash, NULL, NULL);
        model_t *m = model_create();
        if (!ht || !m) { BUG("create failed"); continue; }
        
        char keys[200][32];
        char vals[200][32];
        
        for (int i = 0; i < 200; i++) {
            for (int j = 0; j < 32; j++) {
                keys[i][j] = (char)(i ^ j);
                vals[i][j] = (char)(i * 3 ^ j);
            }
        }
        
        for (int i = 0; i < 200; i++) {
            ht_upsert(ht, keys[i], 32, vals[i], 32);
            model_set(m, keys[i], 32, vals[i], 32);
        }
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        MODEL_CHECK(ht_size(ht) == 200);
        
        ht_destroy(ht);
        model_destroy(m);
    }
    
    PASS("progressive load factor: different LF settings consistent");
}

// ============================================================================
// Test 7: Memory Pattern Verification
// ============================================================================

static void test_memory_pattern_verification(void) {
    printf("\n=== Property: Memory Pattern Verification ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0004ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    char keys[100][64];
    char vals[100][128];
    
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 64; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 128; j++) vals[i][j] = (char)(i ^ j);
        ht_upsert(ht, keys[i], 64, vals[i], 128);
    }
    
    for (int i = 0; i < 100; i++) {
        size_t vlen = 0;
        const char *found = ht_find(ht, keys[i], 64, &vlen);
        if (!found) { printf("  MISSING key %d\n", i); goto cleanup; }
        if (vlen != 128) { printf("  WRONG vlen for key %d: %zu\n", i, vlen); goto cleanup; }
        if (memcmp(found, vals[i], 128) != 0) { printf("  CORRUPT value for key %d\n", i); goto cleanup; }
    }
    
    for (int i = 0; i < 100; i++) {
        char small_val[8] = {(char)i, (char)(i+1), 0};
        ht_upsert(ht, keys[i], 64, small_val, 8);
    }
    
    for (int i = 0; i < 100; i++) {
        size_t vlen = 0;
        const char *found = ht_find(ht, keys[i], 64, &vlen);
        if (!found) { printf("  MISSING after shrink key %d\n", i); goto cleanup; }
        if (vlen != 8) { printf("  WRONG vlen after shrink key %d: %zu\n", i, vlen); goto cleanup; }
    }
    
    PASS("memory pattern: alignment and fragmentation verified");
    
cleanup:
    ht_destroy(ht);
}

// ============================================================================
// Test 8: Iterator Stability During Modification
// ============================================================================

static void test_iterator_during_modification_stress(void) {
    printf("\n=== Property: Iterator Modification Stress ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0005ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    
    char keys[50][32];
    char vals[50][32];
    
    for (int i = 0; i < 50; i++) {
        for (int j = 0; j < 32; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)(i * 7);
        }
        ht_upsert(ht, keys[i], 32, vals[i], 32);
    }
    
    for (int iter = 0; iter < 1000; iter++) {
        ht_iter_t it = ht_iter_begin(ht);
        const void *key, *value;
        size_t klen, vlen;
        int count = 0;
        
        while (ht_iter_next(ht, &it, &key, &klen, &value, &vlen)) {
            count++;
            ht_upsert(ht, key, klen, vals[iter % 50], 32);
        }
        
        for (int i = 0; i < 50; i += 3) {
            ht_remove(ht, keys[i], 32);
        }
        
        for (int i = 0; i < 50; i += 3) {
            ht_upsert(ht, keys[i], 32, vals[i], 32);
        }
        
        if (iter % 100 == 0) {
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at iter %d: %s\n", iter, err); goto cleanup; }
        }
    }
    
    PASS("iterator during modification stress: 1000 iterations pass");
    
cleanup:
    ht_destroy(ht);
}

// ============================================================================
// Test 9: Table Copy Semantics
// ============================================================================

static void test_table_copy_semantics(void) {
    printf("\n=== Property: Table Copy Semantics ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0006ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *src = ht_create(NULL, simple_hash, NULL, NULL);
    if (!src) { BUG("ht_create failed"); return; }
    
    char keys[50][32];
    char vals[50][32];
    
    for (int i = 0; i < 50; i++) {
        for (int j = 0; j < 32; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)(i * 3);
        }
        ht_upsert(src, keys[i], 32, vals[i], 32);
    }
    
    MODEL_CHECK(ht_size(src) == 50);
    
    ht_table_t *dst = ht_create(NULL, simple_hash, NULL, NULL);
    if (!dst) { BUG("ht_create dst failed"); goto cleanup; }
    
    for (int i = 0; i < 50; i++) {
        size_t vlen = 0;
        const char *found = ht_find(src, keys[i], 32, &vlen);
        if (found) {
            ht_upsert(dst, keys[i], 32, found, vlen);
        }
    }
    
    MODEL_CHECK(ht_size(dst) == ht_size(src));
    
    for (int i = 0; i < 50; i++) {
        size_t vlen1 = 0, vlen2 = 0;
        const char *f1 = ht_find(src, keys[i], 32, &vlen1);
        const char *f2 = ht_find(dst, keys[i], 32, &vlen2);
        MODEL_CHECK((f1 != NULL) == (f2 != NULL));
        MODEL_CHECK(vlen1 == vlen2);
    }
    
    PASS("table copy semantics: merge produces equivalent state");
    
cleanup:
    ht_destroy(src);
    ht_destroy(dst);
}

// ============================================================================
// Test 10: Edge Value Combinations
// ============================================================================

static void test_edge_value_combinations(void) {
    printf("\n=== Property: Edge Value Combinations ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xFEEDFEEDULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    model_t *m = model_create();
    if (!ht || !m) { BUG("create failed"); goto cleanup; }
    
    char empty_key[1] = {0};
    char empty_val[1] = {0};
    char large_key[256];
    char large_val[256];
    
    memset(large_key, 0xAB, 256);
    memset(large_val, 0xCD, 256);
    
    ht_upsert(ht, empty_key, 0, empty_val, 0);
    model_set(m, empty_key, 0, empty_val, 0);
    
    ht_upsert(ht, large_key, 256, large_val, 256);
    model_set(m, large_key, 256, large_val, 256);
    
    if (ht_size(ht) != 2 || model_size(m) != 2) {
        printf("  FAIL at init: ht_size=%zu model_size=%zu\n", ht_size(ht), model_size(m));
        tests_failed++; goto cleanup;
    }
    
    for (int op = 0; op < 10000; op++) {
        int choice = (int)(splitmix64(&sm_state) % 4);
        if (choice == 0) {
            size_t vlen = 0;
            const char *f = ht_find(ht, empty_key, 0, &vlen);
            const char *mf = model_find_val(m, empty_key, 0, NULL);
            if ((f != NULL) != (mf != NULL)) { tests_failed++; goto cleanup; }
        } else if (choice == 1) {
            size_t vlen = 0;
            const char *f = ht_find(ht, large_key, 256, &vlen);
            const char *mf = model_find_val(m, large_key, 256, NULL);
            if ((f != NULL) != (mf != NULL)) { tests_failed++; goto cleanup; }
        } else if (choice == 2) {
            memset(large_val, (char)(op & 0xFF), 256);
            if (ht_upsert(ht, large_key, 256, large_val, 256))
                model_set(m, large_key, 256, large_val, 256);
        } else {
            empty_val[0] = (char)(op & 0xFF);
            if (ht_upsert(ht, empty_key, 0, empty_val, 1))
                model_set(m, empty_key, 0, empty_val, 1);
        }
    }
    
    PASS("edge value combinations: null/empty/max-size pass");
    
cleanup:
    if (ht) ht_destroy(ht);
    if (m) model_destroy(m);
}

// ============================================================================
// Test 11: Tombstone Accumulation and Cleanup
// ============================================================================

static void test_tombstone_accumulation_cleanup(void) {
    printf("\n=== Property: Tombstone Accumulation ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xDEADBEEF0007ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    model_t *m = model_create();
    if (!ht || !m) { BUG("create failed"); goto cleanup; }
    
    char keys[100][32];
    char vals[100][32];
    
    for (int i = 0; i < 100; i++) {
        for (int j = 0; j < 32; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 32; j++) vals[i][j] = (char)(i * 7);
    }
    
    for (int i = 0; i < 80; i++) {
        ht_upsert(ht, keys[i], 32, vals[i], 32);
        model_set(m, keys[i], 32, vals[i], 32);
    }
    
    for (int i = 0; i < 80; i += 80/60) {
        ht_remove(ht, keys[i], 32);
        model_remove(m, keys[i], 32);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 80; i < 100; i++) {
        ht_upsert(ht, keys[i], 32, vals[i], 32);
        model_set(m, keys[i], 32, vals[i], 32);
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    PASS("tombstone accumulation cleanup: stats consistent");
    
cleanup:
    if (ht) ht_destroy(ht);
    if (m) model_destroy(m);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    alloc_mock_reset();
    
    printf("========================================\n");
    printf("Property-Based Testing B: Extended Tests\n");
    printf("========================================\n");
    
    test_multi_table_isolation();
    test_long_running_accumulation();
    test_batch_vs_interleaved();
    test_collision_stress();
    test_delete_reinsert_cycling();
    test_progressive_load_factor();
    test_memory_pattern_verification();
    test_iterator_during_modification_stress();
    test_table_copy_semantics();
    test_edge_value_combinations();
    test_tombstone_accumulation_cleanup();
    
    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");
    
    return tests_failed > 0 ? 1 : 0;
}
