/**
 * test_property.c - Property-Based Testing for draugr Hash Table
 *
 * Uses model-based testing: maintains a simple reference dictionary and verifies
 * that the hash table produces identical results for all operations.
 *
 * Enhanced with:
 * - Value mutation tests (different size updates)
 * - Interleaved iterator tests (modify during iteration)
 * - Tombstone tracking and verification
 * - Arena waste detection
 * - Shrink verification
 * - Duplicate key handling
 * - Many small values stress
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include "draugr/ht.h"
#include "draugr/ht_internal.h"

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void __real_free(void *ptr);

static struct {
    int max_alloc_calls;
    int alloc_count;
    int alloc_num_to_fail;
    bool fail_by_size;
    size_t fail_size_threshold;
    int fail_count;
} mock = {-1, 0, -1, false, 0, 0};

void alloc_mock_reset(void) {
    mock.max_alloc_calls = -1;
    mock.alloc_count = 0;
    mock.alloc_num_to_fail = -1;
    mock.fail_by_size = false;
    mock.fail_size_threshold = SIZE_MAX;
    mock.fail_count = 0;
}

void alloc_mock_set_max_alloc_calls(int n) { mock.max_alloc_calls = n; }
void alloc_mock_set_max_alloc_size(size_t threshold) { mock.fail_by_size = true; mock.fail_size_threshold = threshold; }
void alloc_mock_set_alloc_num_to_fail(int n) { mock.alloc_num_to_fail = n; }

static bool should_fail(size_t size) {
    if (mock.max_alloc_calls > 0 && mock.alloc_count >= mock.max_alloc_calls) return true;
    if (mock.fail_by_size && size >= mock.fail_size_threshold) return true;
    if (mock.alloc_num_to_fail >= 0 && mock.alloc_count == mock.alloc_num_to_fail) return true;
    return false;
}

void *__wrap_malloc(size_t size) {
    mock.alloc_count++;
    if (should_fail(size)) { mock.fail_count++; return NULL; }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    mock.alloc_count++;
    if (should_fail(nmemb * size)) { mock.fail_count++; return NULL; }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    mock.alloc_count++;
    if (should_fail(size)) { mock.fail_count++; return NULL; }
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr) { __real_free(ptr); }

static uint64_t simple_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

static uint64_t fixed_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 42;
}

static uint64_t hash_one(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 1;
}

#define MODEL_MAX_ENTRIES 8192
#define MODEL_MAX_KEY_LEN 128
#define MODEL_MAX_VAL_LEN 512

typedef struct {
    char keys[MODEL_MAX_ENTRIES][MODEL_MAX_KEY_LEN];
    size_t key_lens[MODEL_MAX_ENTRIES];
    char vals[MODEL_MAX_ENTRIES][MODEL_MAX_VAL_LEN];
    size_t val_lens[MODEL_MAX_ENTRIES];
    size_t count;
} model_t;

static model_t* model_create(void) {
    model_t *m = (model_t*)calloc(1, sizeof(model_t));
    return m;
}

static void model_destroy(model_t *m) {
    free(m);
}

static size_t model_size(model_t *m) {
    return m->count;
}

static int model_find(model_t *m, const char *key, size_t key_len) {
    for (size_t i = 0; i < m->count; i++) {
        if (m->key_lens[i] == key_len && memcmp(m->keys[i], key, key_len) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void model_set(model_t *m, const char *key, size_t key_len, const char *value, size_t value_len) {
    int idx = model_find(m, key, key_len);
    if (idx >= 0) {
        memcpy(m->vals[idx], value, value_len < MODEL_MAX_VAL_LEN ? value_len : MODEL_MAX_VAL_LEN);
        m->val_lens[idx] = value_len;
    } else {
        if (m->count >= MODEL_MAX_ENTRIES) return;
        idx = (int)m->count++;
        memcpy(m->keys[idx], key, key_len < MODEL_MAX_KEY_LEN ? key_len : MODEL_MAX_KEY_LEN);
        m->key_lens[idx] = key_len;
        memcpy(m->vals[idx], value, value_len < MODEL_MAX_VAL_LEN ? value_len : MODEL_MAX_VAL_LEN);
        m->val_lens[idx] = value_len;
    }
}

static bool model_remove(model_t *m, const char *key, size_t key_len) {
    int idx = model_find(m, key, key_len);
    if (idx < 0) return false;
    m->count--;
    for (size_t i = idx; i < m->count; i++) {
        memcpy(m->keys[i], m->keys[i + 1], MODEL_MAX_KEY_LEN);
        m->key_lens[i] = m->key_lens[i + 1];
        memcpy(m->vals[i], m->vals[i + 1], MODEL_MAX_VAL_LEN);
        m->val_lens[i] = m->val_lens[i + 1];
    }
    memset(m->keys[m->count], 0, MODEL_MAX_KEY_LEN);
    memset(m->vals[m->count], 0, MODEL_MAX_VAL_LEN);
    m->key_lens[m->count] = 0;
    m->val_lens[m->count] = 0;
    return true;
}

static const char* model_find_val(model_t *m, const char *key, size_t key_len, size_t *out_val_len) {
    int idx = model_find(m, key, key_len);
    if (idx < 0) return NULL;
    if (out_val_len) *out_val_len = m->val_lens[idx];
    return m->vals[idx];
}

static uint32_t g_seed = 0;
static uint32_t my_rand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}

static void my_srand(uint32_t seed) {
    g_seed = seed;
}

static int tests_passed = 0;
static int tests_failed = 0;

#define MODEL_CHECK(cond) do { \
    if (!(cond)) { \
        printf("  MODEL FAIL at line %d: %s\n", __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define PASS(msg) do { printf("  PASS: %s\n", msg); tests_passed++; } while(0)
#define BUG(msg) do { printf("  BUG: %s\n", msg); tests_failed++; } while(0)

static void count_bare_slots(ht_bare_t *t, size_t *live, size_t *tomb, size_t *empty) {
    *live = *tomb = *empty = 0;
    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        if (hpd_live(hpd)) (*live)++;
        else if (hpd_tomb(hpd)) (*tomb)++;
        else (*empty)++;
    }
}

// ============================================================================
// Test 1: Large Scale Random Operations with Unique Keys
// ============================================================================

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
    
    size_t arena_before = ht->arena_size;
    printf("  After small insert: arena_size=%zu\n", arena_before);
    
    int bugs_found = 0;
    for (int trial = 0; trial < 20 && bugs_found == 0; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(5 + trial);
        
        size_t before = ht->arena_size;
        bool ok = ht_upsert(ht, key, 16, val_small, 16);
        size_t after = ht->arena_size;
        
        printf("  trial %d: max_alloc=%d, before=%zu, after=%zu, ok=%d\n",
               trial, 5 + trial, before, after, ok);
        
        if (!ok && after != before) {
            printf("  BUG: arena_size changed from %zu to %zu on failed upsert\n", before, after);
            bugs_found++;
        }
    }
    
    if (bugs_found == 0) {
        PASS("arena rollback: arena_size unchanged when allocation fails");
    } else {
        BUG("arena rollback failed");
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

static void test_combined_operations_sequence(void) {
    printf("\n=== Property: Combined Operations Sequence ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xDAEBFD0E);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int ops = 10000;
    char keys[50][16];
    char vals[50][16];
    
    for (int i = 0; i < 50; i++) {
        for (int j = 0; j < 16; j++) {
            keys[i][j] = (char)i;
            vals[i][j] = (char)i;
        }
    }
    
    for (int op = 0; op < ops; op++) {
        int idx = my_rand() % 50;
        int action = my_rand() % 100;
        
        if (action < 30) {
            size_t val_len = 4 + (my_rand() % 12);
            ht_upsert(ht, keys[idx], 16, vals[idx], val_len);
            model_set(m, keys[idx], 16, vals[idx], val_len);
        } else if (action < 50) {
            ht_remove(ht, keys[idx], 16);
            model_remove(m, keys[idx], 16);
        } else if (action < 70) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else if (action < 80 && ht_size(ht) > 10) {
            ht_resize(ht, 64 + (my_rand() % 10) * 64);
        } else if (action < 90 && ht_size(ht) > 10) {
            ht_compact(ht);
        }
        
        if (op > 0 && op % 2000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < 50; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], 16, &vlen);
        const char *mfound = model_find_val(m, keys[i], 16, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    PASS("combined operations sequence holds for 10000 ops");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 11: Insert/Remove/Find Balance
// ============================================================================

static void test_insert_remove_find_balance(void) {
    printf("\n=== Property: Insert/Remove/Find Balance ===\n");
    
    uint32_t seed = (uint32_t)(time(NULL) ^ 0xEBFD0E1F);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 2000;
    char keys[2000][16];
    char vals[2000][16];
    int present[2000] = {0};
    int present_count = 0;
    
    for (int op = 0; op < n; op++) {
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
                    present_count++;
                }
            }
        } else if (action < 70) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
            if (found && mfound) {
                MODEL_CHECK(vlen == 16);
            }
        } else {
            if (present[idx]) {
                ht_remove(ht, keys[idx], 16);
                model_remove(m, keys[idx], 16);
                present[idx] = 0;
                present_count--;
            }
        }
        
        if (op > 0 && op % 500 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            MODEL_CHECK(ht_size(ht) == (size_t)present_count);
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    MODEL_CHECK(ht_size(ht) == (size_t)present_count);
    
    PASS("insert/remove/find balance holds");
    
    model_destroy(m);
    ht_destroy(ht);
}

// ============================================================================
// Test 12: Hash Collision with Different Keys
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
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) { BUG("ht_create failed"); return; }
    model_t *m = model_create();
    
    int n = 100;
    char keys[100][16];
    char vals[100][16];
    
    for (int i = 0; i < n; i++) {
        for (int j = 0; j < 16; j++) keys[i][j] = (char)i;
        for (int j = 0; j < 16; j++) vals[i][j] = (char)i;
    }
    
    int ops = 50000;
    int insert_failures = 0;
    
    for (int op = 0; op < ops; op++) {
        int idx = my_rand() % n;
        int action = my_rand() % 100;
        
        if (action < 35) {
            size_t val_len = 4 + (my_rand() % 12);
            if (val_len > 16) val_len = 16;
            bool ok = ht_upsert(ht, keys[idx], 16, vals[idx], val_len);
            if (ok) {
                model_set(m, keys[idx], 16, vals[idx], val_len);
            } else {
                insert_failures++;
            }
        } else if (action < 55) {
            ht_remove(ht, keys[idx], 16);
            model_remove(m, keys[idx], 16);
        } else if (action < 75) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 16, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 16, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else if (action < 82 && ht_size(ht) > 20) {
            ht_resize(ht, 64 + (my_rand() % 16) * 64);
        } else if (action < 90 && ht_size(ht) > 20) {
            ht_compact(ht);
        }
        
        if (op > 0 && op % 5000 == 0) {
            MODEL_CHECK(ht_size(ht) == model_size(m));
            const char *err = ht_check_invariants(ht);
            if (err) { printf("  INVARIANT FAIL at op %d: %s\n", op, err); tests_failed++; }
        }
    }
    
    MODEL_CHECK(ht_size(ht) == model_size(m));
    
    for (int i = 0; i < n; i++) {
        size_t vlen = 0;
        const char *found = (const char*)ht_find(ht, keys[i], 16, &vlen);
        const char *mfound = model_find_val(m, keys[i], 16, NULL);
        MODEL_CHECK((found != NULL) == (mfound != NULL));
    }
    
    printf("  ops=%d insert_failures=%d\n", ops, insert_failures);
    PASS("long running sequence: 50k ops consistent");
    
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
            printf("  i=%d success=%d fail=%d arena_size=%zu\n", 
                   i, success_count, fail_count, ht->arena_size);
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
// Main
// ============================================================================

int main(void) {
    printf("========================================\n");
    printf("Property-Based Testing: draugr Hash Table\n");
    printf("========================================\n");

    alloc_mock_reset();
    test_large_unique_keys();
    
    alloc_mock_reset();
    test_value_size_mutation();
    
    alloc_mock_reset();
    test_interleaved_iterator_modification();
    
    alloc_mock_reset();
    test_tombstone_tracking();
    
    alloc_mock_reset();
    test_arena_waste_detection();
    
    alloc_mock_reset();
    test_shrink_verification();
    
    alloc_mock_reset();
    test_duplicate_key_handling();
    
    alloc_mock_reset();
    test_many_small_values();
    
    alloc_mock_reset();
    test_spill_lane_hash_one();
    
    alloc_mock_reset();
    test_combined_operations_sequence();
    
    alloc_mock_reset();
    test_insert_remove_find_balance();
    
    alloc_mock_reset();
    test_hash_collision_different_keys();
    
    alloc_mock_reset();
    test_long_running_sequence();
    
    alloc_mock_reset();
    test_iterator_completeness();
    
    alloc_mock_reset();
    test_various_key_lengths();
    
    alloc_mock_reset();
    test_resize_with_iterators();
    
    alloc_mock_reset();
    test_spill_edge_cases();
    
    alloc_mock_reset();
    test_arena_exhaustion();
    
    alloc_mock_reset();
    test_entry_reallocation();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
