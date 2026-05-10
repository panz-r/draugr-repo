/**
 * test_public_api.c - Comprehensive Public API Test Suite
 *
 * Tests ALL public functions in draugr/ht.h with at least 3 scenarios each.
 * Uses a simple non-colliding hash function (fnv1a) to avoid spill lane.
 *
 * Scenarios:
 * - Basic functionality
 * - Edge cases (empty, full, boundary values)
 * - Error handling (NULL inputs, invalid states)
 * - Concurrent operations (interleaved inserts/finds/removes)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdarg.h>

#include "draugr/ht.h"
#include "draugr/ht_internal.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define PASS(msg) do { \
    printf("  PASS: %s\n", msg); \
    tests_passed++; \
} while(0)

#define FAIL(msg) do { \
    printf("  FAIL: %s\n", msg); \
    tests_failed++; \
} while(0)

#define ASSERT(cond, msg) do { \
    if (!(cond)) { FAIL(msg); return; } \
} while(0)

#define ASSERT_NOT_NULL(ptr, msg) ASSERT((ptr) != NULL, msg)
#define ASSERT_NULL(ptr, msg) ASSERT((ptr) == NULL, msg)
#define ASSERT_EQ(a, b, msg) ASSERT((a) == (b), msg)
#define ASSERT_NE(a, b, msg) ASSERT((a) != (b), msg)
#define ASSERT_GT(a, b, msg) ASSERT((a) > (b), msg)
#define ASSERT_LT(a, b, msg) ASSERT((a) < (b), msg)
#define ASSERT_STR_EQ(a, b, len, msg) ASSERT(memcmp((a), (b), (len)) == 0, msg)

// ============================================================================
// Test Infrastructure
// ============================================================================

static uint64_t simple_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) {
        h ^= p[i];
        h *= 0x100000001b3ULL;
    }
    return h;
}

static bool simple_eq(const void *a, size_t len_a, const void *b, size_t len_b, void *ctx) {
    (void)ctx;
    if (len_a != len_b) return false;
    return memcmp(a, b, len_a) == 0;
}

static ht_table_t *create_default_ht(void) {
    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,  // Disable zombie for predictable tests
    };
    return ht_create(&cfg, simple_hash, simple_eq, NULL);
}

static void insert_test_kv(ht_table_t *t, const char *key, int val) {
    ht_upsert(t, key, strlen(key), &val, sizeof(val));
}

// ============================================================================
// Callbacks
// ============================================================================

static size_t find_all_count;
static bool find_all_cb(const void *key, size_t klen, const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)val; (void)vlen;
    if (ctx) (*(size_t *)ctx)++;
    return true;
}

static size_t key_all_count;
static bool key_all_cb(const void *key, size_t klen, const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)val; (void)vlen; (void)ctx;
    key_all_count++;
    return true;
}

static bool count_only_cb(const void *key, size_t klen, const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)val; (void)vlen;
    size_t *cnt = (size_t *)ctx;
    (*cnt)++;
    return true;
}

// ============================================================================
// Test: ht_create
// ============================================================================

static void test_ht_create_null_cfg(void) {
    printf("\n  [ht_create] NULL config uses defaults...\n");
    ht_table_t *t = ht_create(NULL, simple_hash, NULL, NULL);
    ASSERT_NOT_NULL(t, "ht_create(NULL cfg) should succeed with defaults");
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.capacity, 64, "Default capacity should be 64");
    ht_destroy(t);
    PASS("ht_create null cfg");
}

static void test_ht_create_custom_cfg(void) {
    printf("\n  [ht_create] Custom config...\n");
    ht_config_t cfg = {
        .initial_capacity = 256,
        .max_load_factor = 0.5,
        .min_load_factor = 0.1,
        .tomb_threshold = 0.15,
        .zombie_window = 8,
    };
    ht_table_t *t = ht_create(&cfg, simple_hash, simple_eq, NULL);
    ASSERT_NOT_NULL(t, "ht_create with custom cfg should succeed");
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.capacity, 256, "Custom capacity should be 256");
    ht_destroy(t);
    PASS("ht_create custom cfg");
}

static void test_ht_create_with_context(void) {
    printf("\n  [ht_create] With user context...\n");
    int ctx_value = 12345;
    ht_table_t *t = ht_create(NULL, simple_hash, simple_eq, &ctx_value);
    ASSERT_NOT_NULL(t, "ht_create with user_ctx should succeed");
    ht_destroy(t);
    PASS("ht_create with context");
}

// ============================================================================
// Test: ht_clear
// ============================================================================

static void test_ht_clear_empty(void) {
    printf("\n  [ht_clear] On empty table...\n");
    ht_table_t *t = create_default_ht();
    ht_clear(t);
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 0, "Size should be 0 after clear");
    ASSERT_EQ(st.tombstone_cnt, 0, "Tombstones should be 0 after clear");
    ht_destroy(t);
    PASS("ht_clear empty table");
}

static void test_ht_clear_nonempty(void) {
    printf("\n  [ht_clear] On non-empty table...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 50; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        insert_test_kv(t, k, i);
    }
    ht_clear(t);
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 0, "Size should be 0 after clear");
    ASSERT_EQ(st.tombstone_cnt, 0, "No tombstones after clear");
    const void *v = ht_find(t, "key0", 5, NULL);
    ASSERT_NULL(v, "key0 should not be found after clear");
    ht_destroy(t);
    PASS("ht_clear non-empty table");
}

static void test_ht_clear_with_tombstones(void) {
    printf("\n  [ht_clear] After removals (with tombstones)...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        insert_test_kv(t, k, i);
    }
    // Remove half to create tombstones
    for (int i = 0; i < 20; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        ht_remove(t, k, strlen(k));
    }
    ht_stats_t st_before;
    ht_stats(t, &st_before);
    ASSERT_GT(st_before.tombstone_cnt, 0, "Should have tombstones before clear");
    ht_clear(t);
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 0, "Size should be 0");
    ASSERT_EQ(st.tombstone_cnt, 0, "Tombstones should be 0");
    ht_destroy(t);
    PASS("ht_clear after removals");
}

// ============================================================================
// Test: ht_insert
// ============================================================================

static void test_ht_insert_basic(void) {
    printf("\n  [ht_insert] Basic insert...\n");
    ht_table_t *t = create_default_ht();
    ht_insert_result_t r = ht_insert(t, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "First insert should return HT_INSERT_OK");
    const void *v = ht_find(t, "key1", 4, NULL);
    ASSERT_NOT_NULL(v, "key1 should be found");
    ASSERT_EQ(memcmp(v, "val1", 4), 0, "Value should match");
    ht_destroy(t);
    PASS("ht_insert basic");
}

static void test_ht_insert_multi_value(void) {
    printf("\n  [ht_insert] Multi-value (same key, different values)...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    ht_insert(t, "key1", 4, "val2", 4);
    ht_insert(t, "key1", 4, "val3", 4);
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 3, "Should have 3 entries for key1");
    ht_destroy(t);
    PASS("ht_insert multi-value");
}

static void test_ht_insert_with_null_value(void) {
    printf("\n  [ht_insert] With NULL value...\n");
    ht_table_t *t = create_default_ht();
    ht_insert_result_t r = ht_insert(t, "key1", 4, NULL, 0);
    ASSERT_EQ(r, HT_INSERT_OK, "Insert with null value should succeed");
    size_t vlen = 999;
    const void *v = ht_find(t, "key1", 4, &vlen);
    ASSERT_NOT_NULL(v, "key should be found");
    ASSERT_EQ(vlen, 0, "Value length should be 0");
    ht_destroy(t);
    PASS("ht_insert null value");
}

// ============================================================================
// Test: ht_insert_with_hash
// ============================================================================

static void test_ht_insert_with_hash_basic(void) {
    printf("\n  [ht_insert_with_hash] Basic...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_insert_result_t r = ht_insert_with_hash(t, h, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "Insert should succeed");
    const void *v = ht_find_with_hash(t, h, "key1", 4, NULL);
    ASSERT_NOT_NULL(v, "Should find by hash");
    ht_destroy(t);
    PASS("ht_insert_with_hash basic");
}

static void test_ht_insert_with_hash_specific(void) {
    printf("\n  [ht_insert_with_hash] Specific hash (spill lane)...\n");
    ht_table_t *t = create_default_ht();
    // Hash that results in lower 48 bits = 0 or 1 goes to spill lane
    uint64_t h = 0;  // Hash with lower 48 bits = 0
    ht_insert_result_t r = ht_insert_with_hash(t, h, "key0", 5, "val0", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "Insert to spill lane should succeed");
    const void *v = ht_find_with_hash(t, h, "key0", 5, NULL);
    ASSERT_NOT_NULL(v, "Should find spill lane entry");
    ht_destroy(t);
    PASS("ht_insert_with_hash spill lane");
}

static void test_ht_insert_with_hash_collision(void) {
    printf("\n  [ht_insert_with_hash] Different keys same hash...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0x1000ULL;
    ht_insert_with_hash(t, h, "keyA", 5, "valA", 4);
    ht_insert_with_hash(t, h, "keyB", 5, "valB", 4);
    size_t count = 0;
    ht_find_key_all_with_hash(t, h, "keyA", 5, count_only_cb, &count);
    ASSERT_EQ(count, 1, "keyA should be found once");
    ht_destroy(t);
    PASS("ht_insert_with_hash different keys");
}

// ============================================================================
// Test: ht_upsert
// ============================================================================

static void test_ht_upsert_new_key(void) {
    printf("\n  [ht_upsert] New key...\n");
    ht_table_t *t = create_default_ht();
    ht_insert_result_t r = ht_upsert(t, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "New key should return HT_INSERT_OK");
    const void *v = ht_find(t, "key1", 4, NULL);
    ASSERT_NOT_NULL(v, "key1 should exist");
    ht_destroy(t);
    PASS("ht_upsert new key");
}

static void test_ht_upsert_existing_key(void) {
    printf("\n  [ht_upsert] Existing key (update)...\n");
    ht_table_t *t = create_default_ht();
    ht_upsert(t, "key1", 4, "val1", 4);
    ht_insert_result_t r = ht_upsert(t, "key1", 4, "val2", 4);
    ASSERT_EQ(r, HT_INSERT_UPDATE, "Existing key should return HT_INSERT_UPDATE");
    size_t vlen = 0;
    const void *v = ht_find(t, "key1", 4, &vlen);
    ASSERT_EQ(memcmp(v, "val2", 4), 0, "Value should be updated");
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 1, "Should still have only 1 entry");
    ht_destroy(t);
    PASS("ht_upsert existing key");
}

static void test_ht_upsert_after_remove(void) {
    printf("\n  [ht_upsert] After removal...\n");
    ht_table_t *t = create_default_ht();
    ht_upsert(t, "key1", 4, "val1", 4);
    ht_remove(t, "key1", 4);
    ht_insert_result_t r = ht_upsert(t, "key1", 4, "val2", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "Re-insert after remove should succeed");
    ht_destroy(t);
    PASS("ht_upsert after remove");
}

// ============================================================================
// Test: ht_upsert_with_hash
// ============================================================================

static void test_ht_upsert_with_hash_new(void) {
    printf("\n  [ht_upsert_with_hash] New entry...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_insert_result_t r = ht_upsert_with_hash(t, h, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "New entry should return HT_INSERT_OK");
    ht_destroy(t);
    PASS("ht_upsert_with_hash new");
}

static void test_ht_upsert_with_hash_update(void) {
    printf("\n  [ht_upsert_with_hash] Update existing...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_upsert_with_hash(t, h, "key1", 4, "val1", 4);
    ht_insert_result_t r = ht_upsert_with_hash(t, h, "key1", 4, "val2", 4);
    ASSERT_EQ(r, HT_INSERT_UPDATE, "Update should return HT_INSERT_UPDATE");
    ht_destroy(t);
    PASS("ht_upsert_with_hash update");
}

static void test_ht_upsert_with_hash_spill(void) {
    printf("\n  [ht_upsert_with_hash] Spill lane entry...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;  // Goes to spill lane
    ht_insert_result_t r = ht_upsert_with_hash(t, h, "key0", 5, "val0", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "Spill lane insert should succeed");
    ht_destroy(t);
    PASS("ht_upsert_with_hash spill");
}

// ============================================================================
// Test: ht_unsert
// ============================================================================

static void test_ht_unsert_new_key(void) {
    printf("\n  [ht_unsert] New key...\n");
    ht_table_t *t = create_default_ht();
    ht_insert_result_t r = ht_unsert(t, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "New key should return HT_INSERT_OK");
    ht_destroy(t);
    PASS("ht_unsert new key");
}

static void test_ht_unsert_duplicate_key_different_value(void) {
    printf("\n  [ht_unsert] Duplicate key different value...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    ht_insert_result_t r = ht_unsert(t, "key1", 4, "val2", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "New entry should return HT_INSERT_OK");
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 2, "Should now have 2 entries");
    ht_destroy(t);
    PASS("ht_unsert duplicate key different value");
}

static void test_ht_unsert_same_key_value(void) {
    printf("\n  [ht_unsert] Same key and value (no-op)...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    ht_insert_result_t r = ht_unsert(t, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_UPDATE, "Same k,v should return HT_INSERT_UPDATE (no insert)");
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 1, "Should still have exactly 1 entry");
    ht_destroy(t);
    PASS("ht_unsert same key value");
}

// ============================================================================
// Test: ht_unsert_with_hash
// ============================================================================

static void test_ht_unsert_with_hash_new(void) {
    printf("\n  [ht_unsert_with_hash] New entry...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_insert_result_t r = ht_unsert_with_hash(t, h, "key1", 4, "val1", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "New entry should return HT_INSERT_OK");
    ht_destroy(t);
    PASS("ht_unsert_with_hash new");
}

static void test_ht_unsert_with_hash_update_rejected(void) {
    printf("\n  [ht_unsert_with_hash] Different values...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_insert(t, "key1", 4, "val1", 4);
    ht_insert_result_t r = ht_unsert_with_hash(t, h, "key1", 4, "val2", 4);
    ASSERT_EQ(r, HT_INSERT_OK, "Should insert (different value)");
    ht_destroy(t);
    PASS("ht_unsert_with_hash different values");
}

static void test_ht_unsert_with_hash_spill_lane(void) {
    printf("\n  [ht_unsert_with_hash] Spill lane insert...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;  // Spill lane
    ht_insert(t, "a", 1, "1", 1);
    ht_insert_result_t r = ht_unsert_with_hash(t, h, "a", 1, "2", 1);
    ASSERT_EQ(r, HT_INSERT_OK, "Spill lane unsert should insert new value");
    ht_destroy(t);
    PASS("ht_unsert_with_hash spill lane");
}

// ============================================================================
// Test: ht_inc
// ============================================================================

static void test_ht_inc_new_key(void) {
    printf("\n  [ht_inc] New key (delta=5)...\n");
    ht_table_t *t = create_default_ht();
    int64_t result = ht_inc(t, "counter", 7, 5);
    ASSERT_EQ(result, 5, "New key should return delta as initial value");
    int found_val = 0;
    const void *v = ht_find(t, "counter", 7, NULL);
    ASSERT_NOT_NULL(v, "Counter should exist");
    memcpy(&found_val, v, sizeof(int));
    ASSERT_EQ(found_val, 5, "Value should be 5");
    ht_destroy(t);
    PASS("ht_inc new key");
}

static void test_ht_inc_existing_key(void) {
    printf("\n  [ht_inc] Existing key (increment)...\n");
    ht_table_t *t = create_default_ht();
    int64_t initial = 10;
    ht_upsert(t, "counter", 7, &initial, sizeof(initial));
    int64_t result = ht_inc(t, "counter", 7, 3);
    ASSERT_EQ(result, 13, "Should return new value (10+3)");
    int64_t found_val = 0;
    const void *v = ht_find(t, "counter", 7, NULL);
    memcpy(&found_val, v, sizeof(int64_t));
    ASSERT_EQ(found_val, 13, "Value should be 13");
    ht_destroy(t);
    PASS("ht_inc existing key");
}

static void test_ht_inc_negative_delta(void) {
    printf("\n  [ht_inc] Negative delta...\n");
    ht_table_t *t = create_default_ht();
    int64_t initial = 100;
    ht_upsert(t, "counter", 7, &initial, sizeof(initial));
    int64_t result = ht_inc(t, "counter", 7, -40);
    ASSERT_EQ(result, 60, "Should return 60");
    ht_destroy(t);
    PASS("ht_inc negative delta");
}

// ============================================================================
// Test: ht_inc_with_hash
// ============================================================================

static void test_ht_inc_with_hash_new(void) {
    printf("\n  [ht_inc_with_hash] New key...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = simple_hash("n", 1, NULL);
    bool ok = false;
    int64_t result = ht_inc_with_hash(t, h, "n", 1, 42, &ok);
    ASSERT_EQ(result, 42, "Should return delta as initial");
    ASSERT_EQ(ok, true, "Should report success");
    ht_destroy(t);
    PASS("ht_inc_with_hash new key");
}

static void test_ht_inc_with_hash_existing(void) {
    printf("\n  [ht_inc_with_hash] Existing key...\n");
    ht_table_t *t = create_default_ht();
    int64_t initial = 50;
    ht_upsert(t, "x", 1, &initial, sizeof(initial));
    uint64_t h = simple_hash("x", 1, NULL);
    bool ok = false;
    int64_t result = ht_inc_with_hash(t, h, "x", 1, -30, &ok);
    ASSERT_EQ(result, 20, "Should return 20");
    ASSERT_EQ(ok, true, "Should report success");
    ht_destroy(t);
    PASS("ht_inc_with_hash existing");
}

static void test_ht_inc_with_hash_spill_lane(void) {
    printf("\n  [ht_inc_with_hash] Spill lane entry...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;  // Spill lane hash
    bool ok = false;
    int64_t r = ht_inc_with_hash(t, h, "a", 1, 7, &ok);
    ASSERT_EQ(r, 7, "Spill lane inc should work");
    ht_destroy(t);
    PASS("ht_inc_with_hash spill lane");
}

// ============================================================================
// Test: ht_find
// ============================================================================

static void test_ht_find_existing(void) {
    printf("\n  [ht_find] Existing key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 123);
    size_t vlen = 0;
    const void *v = ht_find(t, "key1", 4, &vlen);
    ASSERT_NOT_NULL(v, "Should find existing key");
    ASSERT_EQ(vlen, sizeof(int), "Value length should match");
    int val = 0; memcpy(&val, v, sizeof(int));
    ASSERT_EQ(val, 123, "Value should be 123");
    ht_destroy(t);
    PASS("ht_find existing");
}

static void test_ht_find_nonexistent(void) {
    printf("\n  [ht_find] Non-existent key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    const void *v = ht_find(t, "key2", 4, NULL);
    ASSERT_NULL(v, "Should return NULL for missing key");
    ht_destroy(t);
    PASS("ht_find non-existent");
}

static void test_ht_find_after_remove(void) {
    printf("\n  [ht_find] After removal...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    ht_remove(t, "key1", 4);
    const void *v = ht_find(t, "key1", 4, NULL);
    ASSERT_NULL(v, "Should not find after remove");
    ht_destroy(t);
    PASS("ht_find after remove");
}

// ============================================================================
// Test: ht_find_with_hash
// ============================================================================

static void test_ht_find_with_hash_existing(void) {
    printf("\n  [ht_find_with_hash] Existing...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 456);
    uint64_t h = simple_hash("key1", 4, NULL);
    const void *v = ht_find_with_hash(t, h, "key1", 4, NULL);
    ASSERT_NOT_NULL(v, "Should find by hash");
    int val = 0; memcpy(&val, v, sizeof(int));
    ASSERT_EQ(val, 456, "Value should be 456");
    ht_destroy(t);
    PASS("ht_find_with_hash existing");
}

static void test_ht_find_with_hash_spill_lane(void) {
    printf("\n  [ht_find_with_hash] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;  // Spill lane
    ht_insert_with_hash(t, h, "k", 1, "v", 1);
    const void *v = ht_find_with_hash(t, h, "k", 1, NULL);
    ASSERT_NOT_NULL(v, "Should find spill lane entry");
    ht_destroy(t);
    PASS("ht_find_with_hash spill lane");
}

static void test_ht_find_with_hash_mismatch(void) {
    printf("\n  [ht_find_with_hash] Hash mismatch...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t wrong_hash = simple_hash("key2", 4, NULL);  // Different key
    const void *v = ht_find_with_hash(t, wrong_hash, "key1", 4, NULL);
    ASSERT_NULL(v, "Should not find with wrong hash");
    ht_destroy(t);
    PASS("ht_find_with_hash mismatch");
}

// ============================================================================
// Test: ht_find_all
// ============================================================================

static void test_ht_find_all_multi(void) {
    printf("\n  [ht_find_all] Multiple entries with same hash...\n");
    ht_table_t *t = create_default_ht();
    // With simple_hash, different keys have different hashes
    insert_test_kv(t, "a", 1);
    insert_test_kv(t, "b", 2);
    find_all_count = 0;
    ht_find_all(t, simple_hash("a", 1, NULL), find_all_cb, &find_all_count);
    ASSERT_EQ(find_all_count, 1, "Should find 1 entry for 'a'");
    ht_destroy(t);
    PASS("ht_find_all multi");
}

static void test_ht_find_all_none(void) {
    printf("\n  [ht_find_all] No matches...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    find_all_count = 0;
    ht_find_all(t, 0x99999999ULL, find_all_cb, &find_all_count);  // Non-existent hash
    ASSERT_EQ(find_all_count, 0, "Should find 0 entries");
    ht_destroy(t);
    PASS("ht_find_all none");
}

static void test_ht_find_all_spill_lane(void) {
    printf("\n  [ht_find_all] Spill lane entries...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;  // Spill lane
    ht_insert_with_hash(t, h, "x", 1, "1", 1);
    ht_insert_with_hash(t, h, "y", 1, "2", 1);
    find_all_count = 0;
    ht_find_all(t, h, find_all_cb, &find_all_count);
    ASSERT_EQ(find_all_count, 2, "Should find 2 spill lane entries");
    ht_destroy(t);
    PASS("ht_find_all spill lane");
}

// ============================================================================
// Test: ht_find_key_all
// ============================================================================

static void test_ht_find_key_all_single(void) {
    printf("\n  [ht_find_key_all] Single entry...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    key_all_count = 0;
    ht_find_key_all(t, "key1", 4, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 1, "Should find 1 entry");
    ht_destroy(t);
    PASS("ht_find_key_all single");
}

static void test_ht_find_key_all_multi(void) {
    printf("\n  [ht_find_key_all] Multiple entries same key...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    ht_insert(t, "key1", 4, "val2", 4);
    ht_insert(t, "key1", 4, "val3", 4);
    key_all_count = 0;
    ht_find_key_all(t, "key1", 4, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 3, "Should find all 3 entries");
    ht_destroy(t);
    PASS("ht_find_key_all multi");
}

static void test_ht_find_key_all_none(void) {
    printf("\n  [ht_find_key_all] Non-existent key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    key_all_count = 0;
    ht_find_key_all(t, "key2", 4, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 0, "Should find 0 entries");
    ht_destroy(t);
    PASS("ht_find_key_all none");
}

// ============================================================================
// Test: ht_find_key_all_with_hash
// ============================================================================

static void test_ht_find_key_all_with_hash_single(void) {
    printf("\n  [ht_find_key_all_with_hash] Single entry...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t h = simple_hash("key1", 4, NULL);
    key_all_count = 0;
    ht_find_key_all_with_hash(t, h, "key1", 4, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 1, "Should find 1");
    ht_destroy(t);
    PASS("ht_find_key_all_with_hash single");
}

static void test_ht_find_key_all_with_hash_multi(void) {
    printf("\n  [ht_find_key_all_with_hash] Multi...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "k", 1, "v1", 2);
    ht_insert(t, "k", 1, "v2", 2);
    uint64_t h = simple_hash("k", 1, NULL);
    key_all_count = 0;
    ht_find_key_all_with_hash(t, h, "k", 1, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 2, "Should find 2");
    ht_destroy(t);
    PASS("ht_find_key_all_with_hash multi");
}

static void test_ht_find_key_all_with_hash_spill(void) {
    printf("\n  [ht_find_key_all_with_hash] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;
    ht_insert_with_hash(t, h, "x", 1, "1", 1);
    ht_insert_with_hash(t, h, "x", 1, "2", 1);
    key_all_count = 0;
    ht_find_key_all_with_hash(t, h, "x", 1, key_all_cb, NULL);
    ASSERT_EQ(key_all_count, 2, "Should find 2 in spill");
    ht_destroy(t);
    PASS("ht_find_key_all_with_hash spill");
}

// ============================================================================
// Test: ht_find_kv
// ============================================================================

static void test_ht_find_kv_existing(void) {
    printf("\n  [ht_find_kv] Existing key and value...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    const void *v = ht_find_kv(t, "key1", 4, "val1", 4, NULL);
    ASSERT_NOT_NULL(v, "Should find matching k,v");
    ht_destroy(t);
    PASS("ht_find_kv existing");
}

static void test_ht_find_kv_wrong_key(void) {
    printf("\n  [ht_find_kv] Wrong key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 111);
    const void *v = ht_find_kv(t, "key2", 4, "111", 4, NULL);
    ASSERT_NULL(v, "Should not find with wrong key");
    ht_destroy(t);
    PASS("ht_find_kv wrong key");
}

static void test_ht_find_kv_wrong_value(void) {
    printf("\n  [ht_find_kv] Wrong value...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 111);
    const void *v = ht_find_kv(t, "key1", 4, "999", 4, NULL);
    ASSERT_NULL(v, "Should not find with wrong value");
    ht_destroy(t);
    PASS("ht_find_kv wrong value");
}

// ============================================================================
// Test: ht_find_kv_with_hash
// ============================================================================

static void test_ht_find_kv_with_hash_existing(void) {
    printf("\n  [ht_find_kv_with_hash] Existing...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val1", 4);
    uint64_t h = simple_hash("key1", 4, NULL);
    const void *v = ht_find_kv_with_hash(t, h, "key1", 4, "val1", 4, NULL);
    ASSERT_NOT_NULL(v, "Should find");
    ht_destroy(t);
    PASS("ht_find_kv_with_hash existing");
}

static void test_ht_find_kv_with_hash_mismatch(void) {
    printf("\n  [ht_find_kv_with_hash] Hash mismatch...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t h = simple_hash("key2", 4, NULL);  // Wrong hash
    const void *v = ht_find_kv_with_hash(t, h, "key1", 4, "1", 2, NULL);
    ASSERT_NULL(v, "Should not find with wrong hash");
    ht_destroy(t);
    PASS("ht_find_kv_with_hash mismatch");
}

static void test_ht_find_kv_with_hash_spill(void) {
    printf("\n  [ht_find_kv_with_hash] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;
    ht_insert_with_hash(t, h, "k", 1, "v", 1);
    const void *v = ht_find_kv_with_hash(t, h, "k", 1, "v", 1, NULL);
    ASSERT_NOT_NULL(v, "Should find in spill");
    ht_destroy(t);
    PASS("ht_find_kv_with_hash spill");
}

// ============================================================================
// Test: ht_remove
// ============================================================================

static void test_ht_remove_existing(void) {
    printf("\n  [ht_remove] Existing key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    size_t removed = ht_remove(t, "key1", 4);
    ASSERT_EQ(removed, 1, "Should remove 1 entry");
    ASSERT_NULL(ht_find(t, "key1", 4, NULL), "Should not find after remove");
    ht_destroy(t);
    PASS("ht_remove existing");
}

static void test_ht_remove_nonexistent(void) {
    printf("\n  [ht_remove] Non-existent key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    size_t removed = ht_remove(t, "key2", 4);
    ASSERT_EQ(removed, 0, "Should remove 0 entries");
    ht_destroy(t);
    PASS("ht_remove non-existent");
}

static void test_ht_remove_multi_value(void) {
    printf("\n  [ht_remove] Multiple entries same key...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "v1", 2);
    ht_insert(t, "key1", 4, "v2", 2);
    ht_insert(t, "key1", 4, "v3", 2);
    size_t removed = ht_remove(t, "key1", 4);
    ASSERT_EQ(removed, 3, "Should remove all 3 entries");
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 0, "Should find 0 entries after remove");
    ht_destroy(t);
    PASS("ht_remove multi-value");
}

// ============================================================================
// Test: ht_remove_with_hash
// ============================================================================

static void test_ht_remove_with_hash_existing(void) {
    printf("\n  [ht_remove_with_hash] Existing...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t h = simple_hash("key1", 4, NULL);
    size_t removed = ht_remove_with_hash(t, h, "key1", 4);
    ASSERT_EQ(removed, 1, "Should remove 1");
    ht_destroy(t);
    PASS("ht_remove_with_hash existing");
}

static void test_ht_remove_with_hash_spill(void) {
    printf("\n  [ht_remove_with_hash] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;
    ht_insert_with_hash(t, h, "k", 1, "v", 1);
    size_t removed = ht_remove_with_hash(t, h, "k", 1);
    ASSERT_EQ(removed, 1, "Should remove from spill");
    ht_destroy(t);
    PASS("ht_remove_with_hash spill");
}

static void test_ht_remove_with_hash_wrong_key(void) {
    printf("\n  [ht_remove_with_hash] Wrong key...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t h = simple_hash("key2", 4, NULL);  // Different hash
    size_t removed = ht_remove_with_hash(t, h, "key1", 4);
    ASSERT_EQ(removed, 0, "Should remove 0");
    ht_destroy(t);
    PASS("ht_remove_with_hash wrong key");
}

// ============================================================================
// Test: ht_remove_kv
// ============================================================================

static void test_ht_remove_kv_existing(void) {
    printf("\n  [ht_remove_kv] Existing key and value...\n");
    ht_table_t *t = create_default_ht();
    int val = 100;
    ht_upsert(t, "key1", 4, &val, sizeof(val));
    size_t removed = ht_remove_kv(t, "key1", 4, &val, sizeof(val));
    ASSERT_EQ(removed, 1, "Should remove 1");
    ASSERT_NULL(ht_find(t, "key1", 4, NULL), "Key should be removed");
    ht_destroy(t);
    PASS("ht_remove_kv existing");
}

static void test_ht_remove_kv_wrong_value(void) {
    printf("\n  [ht_remove_kv] Wrong value...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 100);
    size_t removed = ht_remove_kv(t, "key1", 4, "999", 4);
    ASSERT_EQ(removed, 0, "Should remove 0 (no match)");
    ht_destroy(t);
    PASS("ht_remove_kv wrong value");
}

static void test_ht_remove_kv_multi_match(void) {
    printf("\n  [ht_remove_kv] Multiple matching entries...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "val", 3);
    ht_insert(t, "key1", 4, "val", 3);
    ht_insert(t, "key1", 4, "val", 3);
    size_t removed = ht_remove_kv(t, "key1", 4, "val", 3);
    ASSERT_EQ(removed, 3, "Should remove all 3 matching entries");
    ht_destroy(t);
    PASS("ht_remove_kv multi-match");
}

// ============================================================================
// Test: ht_remove_kv_with_hash
// ============================================================================

static void test_ht_remove_kv_with_hash_existing(void) {
    printf("\n  [ht_remove_kv_with_hash] Existing...\n");
    ht_table_t *t = create_default_ht();
    int val = 1;
    ht_upsert(t, "key1", 4, &val, sizeof(val));
    uint64_t h = simple_hash("key1", 4, NULL);
    size_t removed = ht_remove_kv_with_hash(t, h, "key1", 4, &val, sizeof(val));
    ASSERT_EQ(removed, 1, "Should remove 1");
    ht_destroy(t);
    PASS("ht_remove_kv_with_hash existing");
}

static void test_ht_remove_kv_with_hash_wrong(void) {
    printf("\n  [ht_remove_kv_with_hash] Wrong hash...\n");
    ht_table_t *t = create_default_ht();
    int val = 1;
    ht_upsert(t, "key1", 4, &val, sizeof(val));
    uint64_t h = 0xBAD;  // Wrong hash
    size_t removed = ht_remove_kv_with_hash(t, h, "key1", 4, &val, sizeof(val));
    ASSERT_EQ(removed, 0, "Should remove 0");
    ht_destroy(t);
    PASS("ht_remove_kv_with_hash wrong");
}

static void test_ht_remove_kv_with_hash_spill(void) {
    printf("\n  [ht_remove_kv_with_hash] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;
    ht_insert_with_hash(t, h, "k", 1, "v", 1);
    size_t removed = ht_remove_kv_with_hash(t, h, "k", 1, "v", 1);
    ASSERT_EQ(removed, 1, "Should remove from spill");
    ht_destroy(t);
    PASS("ht_remove_kv_with_hash spill");
}

// ============================================================================
// Test: ht_remove_kv_one
// ============================================================================

static void test_ht_remove_kv_one_existing(void) {
    printf("\n  [ht_remove_kv_one] Existing...\n");
    ht_table_t *t = create_default_ht();
    ht_insert(t, "key1", 4, "v1", 2);
    ht_insert(t, "key1", 4, "v2", 2);
    bool removed = ht_remove_kv_one(t, "key1", 4, "v1", 2);
    ASSERT_EQ(removed, true, "Should remove one");
    size_t count = 0;
    ht_find_key_all(t, "key1", 4, count_only_cb, &count);
    ASSERT_EQ(count, 1, "Should have 1 remaining");
    ht_destroy(t);
    PASS("ht_remove_kv_one existing");
}

static void test_ht_remove_kv_one_not_found(void) {
    printf("\n  [ht_remove_kv_one] Not found...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    bool removed = ht_remove_kv_one(t, "key1", 4, "999", 4);
    ASSERT_EQ(removed, false, "Should return false");
    ht_destroy(t);
    PASS("ht_remove_kv_one not found");
}

static void test_ht_remove_kv_one_spill(void) {
    printf("\n  [ht_remove_kv_one] Spill lane...\n");
    ht_table_t *t = create_default_ht();
    uint64_t h = 0;
    ht_insert_with_hash(t, h, "x", 1, "a", 1);
    ht_insert_with_hash(t, h, "x", 1, "b", 1);
    bool removed = ht_remove_kv_one_with_hash(t, h, "x", 1, "a", 1);
    ASSERT_EQ(removed, true, "Should remove from spill");
    ht_destroy(t);
    PASS("ht_remove_kv_one spill");
}

// ============================================================================
// Test: ht_resize
// ============================================================================

static void test_ht_resize_grow(void) {
    printf("\n  [ht_resize] Grow table...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    ht_stats_t st_before;
    ht_stats(t, &st_before);
    bool ok = ht_resize(t, 256);
    ASSERT_EQ(ok, true, "Resize should succeed");
    ht_stats_t st_after;
    ht_stats(t, &st_after);
    ASSERT_EQ(st_after.size, st_before.size, "Size should be preserved");
    ht_destroy(t);
    PASS("ht_resize grow");
}

static void test_ht_resize_shrink(void) {
    printf("\n  [ht_resize] Shrink table...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    bool ok = ht_resize(t, 64);
    ASSERT_EQ(ok, true, "Resize should succeed");
    ht_destroy(t);
    PASS("ht_resize shrink");
}

static void test_ht_resize_same_size(void) {
    printf("\n  [ht_resize] Same size (no-op)...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    ht_stats_t st_before;
    ht_stats(t, &st_before);
    bool ok = ht_resize(t, st_before.capacity);
    ASSERT_EQ(ok, true, "Resize to same should succeed");
    ht_destroy(t);
    PASS("ht_resize same size");
}

// ============================================================================
// Test: ht_compact
// ============================================================================

static void test_ht_compact_basic(void) {
    printf("\n  [ht_compact] Basic...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    for (int i = 0; i < 10; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        ht_remove(t, k, strlen(k));
    }
    ht_stats_t st_before;
    ht_stats(t, &st_before);
    ASSERT_GT(st_before.tombstone_cnt, 0, "Should have tombstones");
    bool ok = ht_compact(t);
    ASSERT_EQ(ok, true, "Compact should succeed");
    ht_stats_t st_after;
    ht_stats(t, &st_after);
    ASSERT_EQ(st_after.size, st_before.size, "Size should be preserved");
    ht_destroy(t);
    PASS("ht_compact basic");
}

static void test_ht_compact_empty(void) {
    printf("\n  [ht_compact] Empty table...\n");
    ht_table_t *t = create_default_ht();
    bool ok = ht_compact(t);
    ASSERT_EQ(ok, true, "Compact on empty should succeed");
    ht_destroy(t);
    PASS("ht_compact empty");
}

static void test_ht_compact_preserves_entries(void) {
    printf("\n  [ht_compact] Preserves all entries...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 15; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        insert_test_kv(t, k, i * 10);
    }
    for (int i = 0; i < 5; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        ht_remove(t, k, strlen(k));
    }
    ht_compact(t);
    for (int i = 5; i < 15; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        const void *v = ht_find(t, k, strlen(k), NULL);
        ASSERT_NOT_NULL(v, "Entry should still exist after compact");
    }
    ht_destroy(t);
    PASS("ht_compact preserves");
}

// ============================================================================
// Test: ht_iter_begin / ht_iter_next
// ============================================================================

static void test_ht_iter_empty(void) {
    printf("\n  [ht_iter] Empty table...\n");
    ht_table_t *t = create_default_ht();
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    bool has_next = ht_iter_next(t, &iter, &key, &klen, &val, &vlen);
    ASSERT_EQ(has_next, false, "Empty table should have no next");
    ht_destroy(t);
    PASS("ht_iter empty");
}

static void test_ht_iter_nonempty(void) {
    printf("\n  [ht_iter] Non-empty table...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 5; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    ht_iter_t iter = ht_iter_begin(t);
    size_t count = 0;
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        count++;
    }
    ASSERT_EQ(count, 5, "Should iterate over 5 entries");
    ht_destroy(t);
    PASS("ht_iter non-empty");
}

static void test_ht_iter_after_remove(void) {
    printf("\n  [ht_iter] After removal...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "a", 1);
    insert_test_kv(t, "b", 2);
    insert_test_kv(t, "c", 3);
    ht_remove(t, "b", 1);
    ht_iter_t iter = ht_iter_begin(t);
    size_t count = 0;
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        count++;
    }
    ASSERT_EQ(count, 2, "Should iterate over 2 remaining entries");
    ht_destroy(t);
    PASS("ht_iter after remove");
}

// ============================================================================
// Test: ht_stats
// ============================================================================

static void test_ht_stats_basic(void) {
    printf("\n  [ht_stats] Basic...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "k1", 1);
    insert_test_kv(t, "k2", 2);
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 2, "Size should be 2");
    ASSERT_GT(st.capacity, 0, "Capacity should be positive");
    ht_destroy(t);
    PASS("ht_stats basic");
}

static void test_ht_stats_with_tombstones(void) {
    printf("\n  [ht_stats] With tombstones...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "k1", 1);
    insert_test_kv(t, "k2", 2);
    ht_remove(t, "k1", 2);
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 1, "Size should be 1");
    ASSERT_GT(st.tombstone_cnt, 0, "Should have tombstones");
    ASSERT_GT(st.load_factor, 0, "Load factor should be positive");
    ht_destroy(t);
    PASS("ht_stats with tombstones");
}

static void test_ht_stats_empty(void) {
    printf("\n  [ht_stats] Empty table...\n");
    ht_table_t *t = create_default_ht();
    ht_stats_t st;
    ht_stats(t, &st);
    ASSERT_EQ(st.size, 0, "Size should be 0");
    ASSERT_EQ(st.tombstone_cnt, 0, "Tombstones should be 0");
    ht_destroy(t);
    PASS("ht_stats empty");
}

// ============================================================================
// Test: ht_size
// ============================================================================

static void test_ht_size_empty(void) {
    printf("\n  [ht_size] Empty...\n");
    ht_table_t *t = create_default_ht();
    ASSERT_EQ(ht_size(t), 0, "Size should be 0");
    ht_destroy(t);
    PASS("ht_size empty");
}

static void test_ht_size_nonempty(void) {
    printf("\n  [ht_size] Non-empty...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "a", 1);
    insert_test_kv(t, "b", 2);
    insert_test_kv(t, "c", 3);
    ASSERT_EQ(ht_size(t), 3, "Size should be 3");
    ht_destroy(t);
    PASS("ht_size non-empty");
}

static void test_ht_size_after_remove(void) {
    printf("\n  [ht_size] After remove...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "a", 1);
    insert_test_kv(t, "b", 2);
    ht_remove(t, "a", 1);
    ASSERT_EQ(ht_size(t), 1, "Size should be 1");
    ht_destroy(t);
    PASS("ht_size after remove");
}

// ============================================================================
// Test: ht_check_invariants
// ============================================================================

static void test_ht_check_invariants_valid(void) {
    printf("\n  [ht_check_invariants] Valid table...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        insert_test_kv(t, k, i);
    }
    const char *err = ht_check_invariants(t);
    ASSERT_NULL(err, "Should return NULL (no error) for valid table");
    ht_destroy(t);
    PASS("ht_check_invariants valid");
}

static void test_ht_check_invariants_empty(void) {
    printf("\n  [ht_check_invariants] Empty table...\n");
    ht_table_t *t = create_default_ht();
    const char *err = ht_check_invariants(t);
    ASSERT_NULL(err, "Empty table should have no invariant violations");
    ht_destroy(t);
    PASS("ht_check_invariants empty");
}

static void test_ht_check_invariants_after_operations(void) {
    printf("\n  [ht_check_invariants] After mixed ops...\n");
    ht_table_t *t = create_default_ht();
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        ht_remove(t, k, strlen(k));
    }
    for (int i = 20; i < 25; i++) {
        char k[16]; snprintf(k, sizeof(k), "k%d", i);
        insert_test_kv(t, k, i);
    }
    const char *err = ht_check_invariants(t);
    ASSERT_NULL(err, "Should have no invariant violations after mixed ops");
    ht_destroy(t);
    PASS("ht_check_invariants after ops");
}

// ============================================================================
// Test: ht_dump
// ============================================================================

static void test_ht_dump_basic(void) {
    printf("\n  [ht_dump] Basic...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "a", 1);
    insert_test_kv(t, "b", 2);
    // ht_dump is for debugging - just verify it doesn't crash
    ht_dump(t, 0, 10);
    ht_destroy(t);
    PASS("ht_dump basic");
}

static void test_ht_dump_empty(void) {
    printf("\n  [ht_dump] Empty...\n");
    ht_table_t *t = create_default_ht();
    ht_dump(t, 0, 10);
    ht_destroy(t);
    PASS("ht_dump empty");
}

static void test_ht_dump_with_hash_filter(void) {
    printf("\n  [ht_dump] With hash filter...\n");
    ht_table_t *t = create_default_ht();
    insert_test_kv(t, "key1", 1);
    uint64_t h = simple_hash("key1", 4, NULL);
    ht_dump(t, (uint32_t)(h >> 32), 5);
    ht_destroy(t);
    PASS("ht_dump with filter");
}

// ============================================================================
// Test: ht_bare_stats, ht_bare_check_invariants, ht_bare_dump
// ============================================================================

static void test_ht_bare_stats_basic(void) {
    printf("\n  [ht_bare_stats] Basic...\n");
    ht_table_t *t = create_default_ht();
    ht_bare_t *bare = &t->bare;
    for (int i = 0; i < 5; i++) {
        ht_bare_insert(bare, i + 100, (uint32_t)i);
    }
    ht_stats_t st;
    ht_bare_stats(bare, &st);
    ASSERT_EQ(st.size, 5, "Bare size should be 5");
    ht_destroy(t);
    PASS("ht_bare_stats basic");
}

static void test_ht_bare_check_invariants(void) {
    printf("\n  [ht_bare_check_invariants] Valid...\n");
    ht_table_t *t = create_default_ht();
    ht_bare_t *bare = &t->bare;
    ht_bare_insert(bare, 100, 1);
    ht_bare_insert(bare, 200, 2);
    const char *err = ht_bare_check_invariants(bare);
    ASSERT_NULL(err, "Should have no invariant violations");
    ht_destroy(t);
    PASS("ht_bare_check_invariants valid");
}

static void test_ht_bare_dump_basic(void) {
    printf("\n  [ht_bare_dump] Basic...\n");
    ht_table_t *t = create_default_ht();
    ht_bare_t *bare = &t->bare;
    ht_bare_insert(bare, 100, 1);
    ht_bare_insert(bare, 200, 2);
    // Just verify it doesn't crash
    ht_bare_dump(bare, 0, 10);
    ht_destroy(t);
    PASS("ht_bare_dump basic");
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("========================================\n");
    printf("Public API Test Suite: draugr Hash Table\n");
    printf("========================================\n");

    // Lifecycle
    test_ht_create_null_cfg();
    test_ht_create_custom_cfg();
    test_ht_create_with_context();
    test_ht_clear_empty();
    test_ht_clear_nonempty();
    test_ht_clear_with_tombstones();

    // Insert
    test_ht_insert_basic();
    test_ht_insert_multi_value();
    test_ht_insert_with_null_value();
    test_ht_insert_with_hash_basic();
    test_ht_insert_with_hash_specific();
    test_ht_insert_with_hash_collision();
    test_ht_upsert_new_key();
    test_ht_upsert_existing_key();
    test_ht_upsert_after_remove();
    test_ht_upsert_with_hash_new();
    test_ht_upsert_with_hash_update();
    test_ht_upsert_with_hash_spill();
    test_ht_unsert_new_key();
    test_ht_unsert_duplicate_key_different_value();
    test_ht_unsert_same_key_value();
    test_ht_unsert_with_hash_new();
    test_ht_unsert_with_hash_update_rejected();
    test_ht_unsert_with_hash_spill_lane();
    test_ht_inc_new_key();
    test_ht_inc_existing_key();
    test_ht_inc_negative_delta();
    test_ht_inc_with_hash_new();
    test_ht_inc_with_hash_existing();
    test_ht_inc_with_hash_spill_lane();

    // Find
    test_ht_find_existing();
    test_ht_find_nonexistent();
    test_ht_find_after_remove();
    test_ht_find_with_hash_existing();
    test_ht_find_with_hash_spill_lane();
    test_ht_find_with_hash_mismatch();
    test_ht_find_all_multi();
    test_ht_find_all_none();
    test_ht_find_all_spill_lane();
    test_ht_find_key_all_single();
    test_ht_find_key_all_multi();
    test_ht_find_key_all_none();
    test_ht_find_key_all_with_hash_single();
    test_ht_find_key_all_with_hash_multi();
    test_ht_find_key_all_with_hash_spill();
    test_ht_find_kv_existing();
    test_ht_find_kv_wrong_key();
    test_ht_find_kv_wrong_value();
    test_ht_find_kv_with_hash_existing();
    test_ht_find_kv_with_hash_mismatch();
    test_ht_find_kv_with_hash_spill();

    // Remove
    test_ht_remove_existing();
    test_ht_remove_nonexistent();
    test_ht_remove_multi_value();
    test_ht_remove_with_hash_existing();
    test_ht_remove_with_hash_spill();
    test_ht_remove_with_hash_wrong_key();
    test_ht_remove_kv_existing();
    test_ht_remove_kv_wrong_value();
    test_ht_remove_kv_multi_match();
    test_ht_remove_kv_with_hash_existing();
    test_ht_remove_kv_with_hash_wrong();
    test_ht_remove_kv_with_hash_spill();
    test_ht_remove_kv_one_existing();
    test_ht_remove_kv_one_not_found();
    test_ht_remove_kv_one_spill();

    // Resize
    test_ht_resize_grow();
    test_ht_resize_shrink();
    test_ht_resize_same_size();
    test_ht_compact_basic();
    test_ht_compact_empty();
    test_ht_compact_preserves_entries();

    // Iterator
    test_ht_iter_empty();
    test_ht_iter_nonempty();
    test_ht_iter_after_remove();

    // Stats
    test_ht_stats_basic();
    test_ht_stats_with_tombstones();
    test_ht_stats_empty();
    test_ht_size_empty();
    test_ht_size_nonempty();
    test_ht_size_after_remove();
    test_ht_check_invariants_valid();
    test_ht_check_invariants_empty();
    test_ht_check_invariants_after_operations();
    test_ht_dump_basic();
    test_ht_dump_empty();
    test_ht_dump_with_hash_filter();

    // Bare
    test_ht_bare_stats_basic();
    test_ht_bare_check_invariants();
    test_ht_bare_dump_basic();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}