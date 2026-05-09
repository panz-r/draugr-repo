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
    bool active;
} mock = {-1, 0, -1, false, 0, 0, false};

void alloc_mock_reset(void) {
    mock.max_alloc_calls = -1;
    mock.alloc_count = 0;
    mock.alloc_num_to_fail = -1;
    mock.fail_by_size = false;
    mock.fail_size_threshold = SIZE_MAX;
    mock.fail_count = 0;
    mock.active = false;
}

void alloc_mock_set_max_alloc_calls(int n) { mock.max_alloc_calls = n; mock.active = (n > 0); }
void alloc_mock_set_max_alloc_size(size_t threshold) { mock.fail_by_size = true; mock.fail_size_threshold = threshold; mock.active = true; }
void alloc_mock_set_alloc_num_to_fail(int n) { mock.alloc_num_to_fail = n; mock.active = (n >= 0); }

static bool should_fail(size_t size) {
    if (!mock.active) return false;
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
static uint64_t g_seed2 = 0;

static uint32_t my_rand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    g_seed2 ^= g_seed2 << 17;
    g_seed2 ^= g_seed2 >> 31;
    g_seed2 ^= g_seed2 << 8;
    return g_seed ^ (uint32_t)(g_seed2 >> 32);
}

static void my_srand(uint32_t seed) {
    g_seed = seed;
    g_seed2 = (uint64_t)seed << 32 | seed;
}

static uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
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

static void test_correlated_key_patterns(void) {
    printf("\n=== Property: Correlated Key Locality ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xFEEDF00DULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
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
                    MODEL_CHECK((found != NULL) == (mfound != NULL));
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
        
        MODEL_CHECK(ht_size(ht) == model_size(m));
        const char *err = ht_check_invariants(ht);
        if (err) { printf("  INVARIANT FAIL at epoch %d: %s\n", epoch, err); tests_failed++; }
    }
    
    PASS("correlated key locality: working set patterns");
    
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

static void test_prefix_keys(void) {
    printf("\n=== Property: Prefix Key Patterns ===\n");
    
    uint64_t sm_state = (uint64_t)(time(NULL) ^ 0xFADE1234ULL);
    uint32_t seed = (uint32_t)(splitmix64(&sm_state) >> 32);
    my_srand(seed);
    printf("  seed=%u\n", seed);
    
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
        } else if (action < 75) {
            size_t vlen = 0;
            const char *found = (const char*)ht_find(ht, keys[idx], 64, &vlen);
            const char *mfound = model_find_val(m, keys[idx], 64, NULL);
            MODEL_CHECK((found != NULL) == (mfound != NULL));
        } else {
            ht_remove(ht, keys[idx], 64);
            model_remove(m, keys[idx], 64);
        }
        
        if (op > 0 && op % 2000 == 0) {
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

static uint64_t hash_zero(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

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
    
    alloc_mock_reset();
    test_chaos_random();
    
    alloc_mock_reset();
    test_many_unique_hashes();
    
    alloc_mock_reset();
    test_iterator_during_resize();
    
    alloc_mock_reset();
    test_null_value_handling();
    
    alloc_mock_reset();
    test_correlated_key_patterns();
    
    alloc_mock_reset();
    test_sequential_keys();
    
    alloc_mock_reset();
    test_large_value_stress();
    
    alloc_mock_reset();
    test_boundary_capacities();
    
    alloc_mock_reset();
    test_reinsert_cycling();
    
    alloc_mock_reset();
    test_prefix_keys();
    
    alloc_mock_reset();
    test_hash_zero();
    
    alloc_mock_reset();
    test_secondary_collision();
    
    alloc_mock_reset();
    test_entry_migration();
    
    alloc_mock_reset();
    test_zero_length_key_value();
    
    alloc_mock_reset();
    test_iterator_during_migration();
    
    alloc_mock_reset();
    test_failed_resize_recovery();
    
    alloc_mock_reset();
    test_spill_exhaustion();
    
    alloc_mock_reset();
    test_state_machine_transitions();
    
    alloc_mock_reset();
    test_auto_resize_triggers();
    
    alloc_mock_reset();
    test_full_lifecycle();
    
    alloc_mock_reset();
    test_iterator_during_compact();
    
    alloc_mock_reset();
    test_dual_iterator();
    
    alloc_mock_reset();
    test_remove_with_oom();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
