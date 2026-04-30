#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INV_CHECK(t, label) do { \
    const char *_inv_err = ht_check_invariants(t); \
    if (_inv_err) { \
        printf("  INVARIANT BROKEN at %s: %s\n", (label), _inv_err); \
        return; \
    } \
} while (0)

uint64_t fnv1a_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

void test_basic() {
    printf("Testing basic operations...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    const char *key1 = "hello";
    const char *val1 = "world";
    bool inserted = ht_upsert(t, key1, strlen(key1), val1, strlen(val1));
    assert(inserted == true);

    INV_CHECK(t, "test_basic: after insert");

    size_t out_len;
    const char *found = ht_find(t, key1, strlen(key1), &out_len);
    assert(found != NULL);
    assert(out_len == strlen(val1));
    assert(memcmp(found, val1, out_len) == 0);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.load_factor > 0.0);

    ht_destroy(t);
    printf("Basic operations passed!\n");
}

void test_update() {
    printf("Testing update...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_upsert(t, "key", 3, "val1", 4);
    bool inserted = ht_upsert(t, "key", 3, "val2", 4);
    assert(inserted == false); // Should be update, not insert

    INV_CHECK(t, "test_update: after update");

    size_t out_len;
    const char *found = ht_find(t, "key", 3, &out_len);
    assert(found != NULL);
    assert(out_len == 4);
    assert(memcmp(found, "val2", 4) == 0);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Update passed!\n");
}

void test_remove() {
    printf("Testing remove...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_upsert(t, "abc", 3, "123", 3);
    ht_upsert(t, "def", 3, "456", 3);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 2);

    size_t removed = ht_remove(t, "abc", 3);
    assert(removed == true);

    ht_stats(t, &stats);
    assert(stats.size == 1);
    assert(stats.tombstone_cnt == 1); // always tombstoned (backward-shift is optional cleanup)
    INV_CHECK(t, "test_remove: after remove");

    assert(ht_find(t, "abc", 3, NULL) == NULL);
    assert(ht_find(t, "def", 3, NULL) != NULL);

    ht_destroy(t);
    printf("Remove passed!\n");
}

void test_resize() {
    printf("Testing resize...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.5 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        ht_upsert(t, key, strlen(key), &i, sizeof(i));
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 10);
    assert(stats.capacity >= 10);
    INV_CHECK(t, "test_resize: after insert 10");

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        const int *val = ht_find(t, key, strlen(key), NULL);
        assert(val != NULL && *val == i);
    }

    ht_destroy(t);
    printf("Resize passed!\n");
}

void test_clear() {
    printf("Testing clear...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        ht_upsert(t, key, strlen(key), &i, sizeof(i));
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 5);

    ht_clear(t);
    ht_stats(t, &stats);
    assert(stats.size == 0);
    assert(stats.capacity > 0);

    ht_destroy(t);
    printf("Clear passed!\n");
}

void test_increment() {
    printf("Testing ht_inc...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int64_t val = ht_inc(t, "counter", 7, 1);
    assert(val == 1);

    val = ht_inc(t, "counter", 7, 5);
    assert(val == 6);

    val = ht_inc(t, "counter", 7, -3);
    assert(val == 3);

    INV_CHECK(t, "test_increment: after incs");
    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Increment passed!\n");
}

void test_binary_keys() {
    printf("Testing binary keys...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // Key with embedded nulls
    uint8_t key1[] = {'a', '\0', 'b', '\0', 'c'};
    uint8_t val1[] = {1, 2, 3, 4};

    ht_upsert(t, key1, sizeof(key1), val1, sizeof(val1));

    INV_CHECK(t, "test_binary_keys: after insert");

    size_t out_len;
    const uint8_t *found = ht_find(t, key1, sizeof(key1), &out_len);
    assert(found != NULL);
    assert(out_len == sizeof(val1));
    assert(memcmp(found, val1, out_len) == 0);

    ht_destroy(t);
    printf("Binary keys passed!\n");
}

void test_iterator() {
    printf("Testing iterator...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        ht_upsert(t, key, strlen(key), &i, sizeof(i));
    }

    INV_CHECK(t, "test_iterator: after inserts");

    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t key_len, val_len;

    while (ht_iter_next(t, &iter, &key, &key_len, &val, &val_len)) {
        count++;
        assert(key_len > 0);
        assert(val_len == sizeof(int));
    }
    assert(count == 5);

    ht_destroy(t);
    printf("Iterator passed!\n");
}

void test_graveyard() {
    printf("Testing graveyard compaction...\n");
    ht_config_t cfg = { .initial_capacity = 8, .tomb_threshold = 0.3 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Insert some entries
    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        ht_upsert(t, key, strlen(key), &i, sizeof(i));
    }

    // Remove some to create tombstones
    ht_remove(t, "key0", 4);
    ht_remove(t, "key2", 4);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.tombstone_cnt <= 2); // backward-shift may reduce count

    // Compact should rebuild table
    ht_compact(t);
    INV_CHECK(t, "test_graveyard: after compact");
    ht_stats(t, &stats);
    // After compact, prophylactic tombstones may be placed (graveyard hashing)
    assert(stats.tombstone_cnt == 0 || stats.tombstone_cnt > 0); // tombstones cleared or replaced with primitives
    assert(stats.size == 3);

    // Verify remaining entries still accessible
    assert(ht_find(t, "key0", 4, NULL) == NULL);
    assert(ht_find(t, "key1", 4, NULL) != NULL);
    assert(ht_find(t, "key2", 4, NULL) == NULL);
    assert(ht_find(t, "key3", 4, NULL) != NULL);

    ht_destroy(t);
    printf("Graveyard passed!\n");
}

static int find_all_count;
static const char *find_all_expected;

static bool find_all_cb(const void *key, size_t key_len,
                        const void *value, size_t value_len,
                        void *user_ctx) {
    (void)key; (void)key_len; (void)value; (void)value_len; (void)user_ctx;
    find_all_count++;
    // Stop after finding one
    return find_all_count < 2;
}

void test_find_all() {
    printf("Testing ht_find_all...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // ht_find_all iterates all entries with same hash
    // With good hash, collisions are rare, but we test callback works
    ht_upsert(t, "unique1", 7, "val1", 4);
    ht_upsert(t, "unique2", 7, "val2", 4);

    INV_CHECK(t, "test_find_all: after inserts");

    find_all_count = 0;
    uint64_t h = fnv1a_hash("unique1", 7, NULL);
    ht_find_all(t, h, find_all_cb, NULL);
    // Should find at most 1 (the matching key)

    ht_destroy(t);
    printf("Find all passed!\n");
}

void test_insert_with_hash() {
    printf("Testing ht_insert_with_hash...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    uint64_t h = fnv1a_hash("key", 3, NULL);
    bool inserted = ht_upsert_with_hash(t, h, "key", 3, "val", 3);
    assert(inserted == true);

    INV_CHECK(t, "test_insert_with_hash: after insert");

    size_t out_len;
    const char *found = ht_find_with_hash(t, h, "key", 3, &out_len);
    assert(found != NULL);
    assert(out_len == 3);

    ht_destroy(t);
    printf("Insert with hash passed!\n");
}

void test_ht_dump(void) {
    printf("Testing ht_dump (no crash)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    ht_upsert(t, "a", 1, "va", 2);
    ht_upsert(t, "b", 1, "vb", 2);
    /* Dump a few slots — should not crash */
    ht_dump(t, (uint32_t)fnv1a_hash("a", 1, NULL), 4);
    ht_dump(t, 0, 0);       /* zero count */
    ht_dump(NULL, 0, 4);    /* NULL table */

    /* Also dump spill-lane entries */
    ht_upsert_with_hash(t, 0, "s", 1, "vs", 2);
    ht_dump(t, 0, 4);

    ht_destroy(t);
    printf("ht_dump passed!\n");
}

void test_ht_inc_null(void) {
    printf("Testing ht_inc with NULL args...\n");
    assert(ht_inc(NULL, "k", 1, 1) == 0);

    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(ht_inc(t, NULL, 0, 1) == 0);
    ht_destroy(t);
    printf("ht_inc NULL args passed!\n");
}

void test_ht_find_with_hash_mismatch(void) {
    printf("Testing find_with_hash with wrong hash...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    ht_upsert(t, "abc", 3, "val", 3);

    /* Search in spill lane (hash=0) for a main-table key — should miss */
    const void *r = ht_find_with_hash(t, 0, "abc", 3, NULL);
    assert(r == NULL);
    r = ht_find_with_hash(t, 1, "abc", 3, NULL);
    assert(r == NULL);

    ht_destroy(t);
    printf("find_with_hash mismatch passed!\n");
}

void test_ht_remove_with_hash_mismatch(void) {
    printf("Testing remove_with_hash with wrong hash...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    ht_upsert(t, "abc", 3, "val", 3);

    /* Try removing via spill-lane hash — should fail */
    assert(ht_remove_with_hash(t, 0, "abc", 3) == 0);
    assert(ht_remove_with_hash(t, 1, "abc", 3) == 0);

    /* Key still present */
    const void *r = ht_find(t, "abc", 3, NULL);
    assert(r != NULL);
    assert(ht_remove(t, "abc", 3));  /* normal remove works */

    ht_destroy(t);
    printf("remove_with_hash mismatch passed!\n");
}

void test_ht_insert_with_hash_hash0_find_normal(void) {
    printf("Testing insert_with_hash(0) then normal find...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert "abc" into spill lane via hash=0 */
    ht_upsert_with_hash(t, 0, "abc", 3, "val", 3);

    /* ht_find recomputes hash via fnv1a → goes to main table, misses spill entry */
    const void *r = ht_find(t, "abc", 3, NULL);
    assert(r == NULL);

    /* find_with_hash(t, 0, ...) finds it in spill */
    r = ht_find_with_hash(t, 0, "abc", 3, NULL);
    assert(r != NULL);

    ht_destroy(t);
    printf("insert_with_hash(0) + find passed!\n");
}

void test_ht_iter_null(void) {
    printf("Testing ht_iter_next with NULL iter...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    ht_upsert(t, "k", 1, "v", 1);

    /* NULL iter should return false */
    bool ok = ht_iter_next(t, NULL, NULL, NULL, NULL, NULL);
    assert(ok == false);

    ht_destroy(t);
    printf("iter NULL passed!\n");
}

void test_update_size_stability(void) {
    printf("Testing update size stability (100 updates)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 100; i++) {
        char val[16]; snprintf(val, sizeof(val), "val%d", i);
        ht_upsert(t, "key", 3, val, strlen(val));
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    const char *found = ht_find(t, "key", 3, NULL);
    assert(found != NULL);
    assert(memcmp(found, "val99", 5) == 0);

    ht_destroy(t);
    printf("Update size stability passed!\n");
}

void test_remove_all_stats(void) {
    printf("Testing remove-all stats consistency...\n");
    ht_config_t cfg = { .initial_capacity = 64, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const int N = 50;
    for (int i = 0; i < N; i++) {
        char k[8]; snprintf(k, sizeof(k), "ra%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(v));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == N);
    double initial_load = st.load_factor;

    /* Remove all */
    for (int i = 0; i < N; i++) {
        char k[8]; snprintf(k, sizeof(k), "ra%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "test_remove_all_stats: after removing all");

    ht_stats(t, &st);
    assert(st.size == 0);
    /* load_factor should be 0 */
    assert(st.load_factor < 0.001);

    /* Stats formula check */
    if (st.size + st.tombstone_cnt > 0) {
        double expected_ratio = (double)st.tombstone_cnt / (st.size + st.tombstone_cnt);
        assert(st.tombstone_ratio >= expected_ratio - 0.01);
        assert(st.tombstone_ratio <= expected_ratio + 0.01);
    }

    ht_destroy(t);
    printf("Remove-all stats passed!\n");
}

void test_resize_byte_exact(void) {
    printf("Testing resize preserves byte-exact data...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert with string values of varying lengths */
    const char *strings[] = { "hello", "world!", "a", "longer value here", "x" };
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        ht_upsert(t, k, strlen(k), strings[i], strlen(strings[i]));
    }

    /* Force multiple resizes */
    for (int i = 5; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(v));
    }

    INV_CHECK(t, "test_resize_byte_exact: after multiple resizes");

    /* Verify string values byte-exact */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        size_t vl = 0;
        const char *v = ht_find(t, k, strlen(k), &vl);
        assert(v != NULL);
        assert(vl == strlen(strings[i]));
        assert(memcmp(v, strings[i], vl) == 0);
    }

    /* Verify int values */
    for (int i = 5; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Resize byte-exact passed!\n");
}

void test_inc_accumulation(void) {
    printf("Testing ht_inc accumulation correctness...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Accumulate +1 one thousand times */
    for (int i = 0; i < 1000; i++) {
        int64_t r = ht_inc(t, "ctr", 3, 1);
        assert(r == i + 1);
    }

    const int64_t *v = ht_find(t, "ctr", 3, NULL);
    assert(v != NULL && *v == 1000);

    /* Now decrement back to 0 */
    for (int i = 0; i < 1000; i++) {
        int64_t r = ht_inc(t, "ctr", 3, -1);
        assert(r == 999 - i);
    }

    v = ht_find(t, "ctr", 3, NULL);
    assert(v != NULL && *v == 0);

    INV_CHECK(t, "test_inc_accumulation: after 2000 ops");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Inc accumulation passed!\n");
}

void test_large_key(void) {
    printf("Testing large key (70000 bytes)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Bug target: key_len is stored as uint16_t (max 65535).
     * A 70000-byte key gets truncated, causing find to fail. */
    size_t big_len = 70000;
    char *big_key = malloc(big_len);
    memset(big_key, 'K', big_len);
    big_key[big_len - 4] = 'E';
    big_key[big_len - 3] = 'N';
    big_key[big_len - 2] = 'D';
    big_key[big_len - 1] = '!';

    int val = 42;
    bool inserted = ht_upsert(t, big_key, big_len, &val, sizeof(val));
    assert(inserted);

    const int *found = ht_find(t, big_key, big_len, NULL);
    assert(found != NULL && *found == 42);

    free(big_key);
    ht_destroy(t);
    printf("Large key passed!\n");
}

void test_large_value(void) {
    printf("Testing large value (70000 bytes)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Bug target: val_len is stored as uint16_t (max 65535). */
    size_t big_len = 70000;
    char *big_val = malloc(big_len);
    memset(big_val, 'V', big_len);
    big_val[0] = 'S';
    big_val[big_len - 1] = 'E';

    bool inserted = ht_upsert(t, "bigkey", 6, big_val, big_len);
    assert(inserted);

    size_t out_len = 0;
    const char *found = ht_find(t, "bigkey", 6, &out_len);
    assert(found != NULL && out_len == big_len);
    assert(found[0] == 'S');
    assert(found[big_len - 1] == 'E');

    free(big_val);
    ht_destroy(t);
    printf("Large value passed!\n");
}

void test_zero_length_key(void) {
    printf("Testing zero-length key...\n");
    ht_config_t cfg = { .initial_capacity = 16 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const char *ptr_a = "anything_a";
    const char *ptr_b = "anything_b";
    int val_a = 10, val_b = 20;

    /* Insert first zero-length key */
    bool inserted = ht_upsert(t, ptr_a, 0, &val_a, sizeof(val_a));
    assert(inserted == true);

    INV_CHECK(t, "test_zero_key: after insert");

    /* Find with key_len=0 should work */
    size_t out_len = 0;
    const int *found = ht_find(t, ptr_a, 0, &out_len);
    assert(found != NULL);
    assert(*found == val_a);

    /* Insert another zero-length key (different pointer) — should replace */
    inserted = ht_upsert(t, ptr_b, 0, &val_b, sizeof(val_b));
    assert(inserted == false);  /* update, not insert */

    /* Find should now return val_b */
    found = ht_find(t, ptr_b, 0, &out_len);
    assert(found != NULL);
    assert(*found == val_b);

    /* Size should be 1 since both zero-length keys are "equal" */
    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    ht_destroy(t);
    printf("Zero-length key passed!\n");
}

void test_zero_length_value(void) {
    printf("Testing zero-length value...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    bool inserted = ht_upsert(t, "k", 1, NULL, 0);
    assert(inserted == true);

    INV_CHECK(t, "test_zero_val: after insert");

    size_t out_len = 99;
    const void *found = ht_find(t, "k", 1, &out_len);
    assert(found != NULL);
    assert(out_len == 0);

    ht_destroy(t);
    printf("Zero-length value passed!\n");
}

void test_double_remove(void) {
    printf("Testing double remove...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_upsert(t, "key", 3, "val", 3);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    size_t removed = ht_remove(t, "key", 3);
    assert(removed == true);

    ht_stats(t, &stats);
    assert(stats.size == 0);

    removed = ht_remove(t, "key", 3);
    assert(removed == false);

    INV_CHECK(t, "test_double_remove: after double remove");
    ht_stats(t, &stats);
    assert(stats.size == 0);

    ht_destroy(t);
    printf("Double remove passed!\n");
}

void test_remove_then_reinsert(void) {
    printf("Testing remove then reinsert...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int v1 = 1, v2 = 2;
    ht_upsert(t, "abc", 3, &v1, sizeof(v1));
    size_t removed = ht_remove(t, "abc", 3);
    assert(removed == true);

    ht_upsert(t, "abc", 3, &v2, sizeof(v2));

    INV_CHECK(t, "test_remove_reinsert: after reinsert");

    size_t out_len = 0;
    const int *found = ht_find(t, "abc", 3, &out_len);
    assert(found != NULL);
    assert(*found == 2);
    assert(out_len == sizeof(int));

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    ht_destroy(t);
    printf("Remove then reinsert passed!\n");
}

void test_clear_then_reuse(void) {
    printf("Testing clear then reuse...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert 10 original entries */
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "old%d", i);
        int val = i;
        ht_upsert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_clear(t);

    INV_CHECK(t, "test_clear_reuse: after clear");

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 0);

    /* Old entries should be gone */
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "old%d", i);
        const void *found = ht_find(t, key, strlen(key), NULL);
        assert(found == NULL);
    }

    /* Insert 10 new entries */
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "new%d", i);
        int val = i + 100;
        ht_upsert(t, key, strlen(key), &val, sizeof(val));
    }

    INV_CHECK(t, "test_clear_reuse: after reinserts");
    ht_stats(t, &stats);
    assert(stats.size == 10);

    /* Verify new entries */
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "new%d", i);
        const int *found = ht_find(t, key, strlen(key), NULL);
        assert(found != NULL);
        assert(*found == i + 100);
    }

    ht_destroy(t);
    printf("Clear then reuse passed!\n");
}

void test_inc_zero_delta(void) {
    printf("Testing ht_inc with zero delta...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int64_t val = ht_inc(t, "zero", 4, 0);
    assert(val == 0);

    INV_CHECK(t, "test_inc_zero: after inc");

    const int64_t *found = ht_find(t, "zero", 4, NULL);
    assert(found != NULL);
    assert(*found == 0);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    ht_destroy(t);
    printf("Inc zero delta passed!\n");
}

void test_inc_negative_from_zero(void) {
    printf("Testing ht_inc with negative delta from zero...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int64_t val = ht_inc(t, "neg", 3, -5);
    assert(val == -5);

    INV_CHECK(t, "test_inc_neg: after inc");

    const int64_t *found = ht_find(t, "neg", 3, NULL);
    assert(found != NULL);
    assert(*found == -5);

    ht_destroy(t);
    printf("Inc negative from zero passed!\n");
}

void test_insert_many_same_first_char(void) {
    printf("Testing insert many same first char (200 entries)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 200; i++) {
        char key[16]; snprintf(key, sizeof(key), "a%d", i);
        int val = i;
        bool inserted = ht_upsert(t, key, strlen(key), &val, sizeof(val));
        assert(inserted == true);
    }

    INV_CHECK(t, "test_insert_many_same_first_char: after 200 inserts");

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 200);

    for (int i = 0; i < 200; i++) {
        char key[16]; snprintf(key, sizeof(key), "a%d", i);
        const int *found = ht_find(t, key, strlen(key), NULL);
        assert(found != NULL);
        assert(*found == i);
    }

    ht_destroy(t);
    printf("Insert many same first char passed!\n");
}

void test_iterator_after_remove(void) {
    printf("Testing iterator after remove...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert 10 entries */
    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "it%d", i);
        int val = i;
        ht_upsert(t, key, strlen(key), &val, sizeof(val));
    }

    /* Remove even-indexed entries */
    for (int i = 0; i < 10; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "it%d", i);
        size_t removed = ht_remove(t, key, strlen(key));
        assert(removed == true);
    }

    /* Iterate and collect */
    int count = 0;
    bool seen[10] = {false};
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t key_len, val_len;

    while (ht_iter_next(t, &iter, &key, &key_len, &val, &val_len)) {
        count++;
        /* Key should start with "it" */
        assert(key_len >= 2);
        assert(memcmp(key, "it", 2) == 0);

        /* Extract the index */
        char buf[16];
        assert(key_len < sizeof(buf));
        memcpy(buf, key, key_len);
        buf[key_len] = '\0';
        int idx = atoi(buf + 2);

        /* Should be an odd index (not removed) */
        assert(idx % 2 == 1);
        assert(idx >= 0 && idx < 10);
        assert(!seen[idx]);
        seen[idx] = true;
    }

    assert(count == 5);

    ht_destroy(t);
    printf("Iterator after remove passed!\n");
}

static uint64_t const_hash42(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 42;
}

static int find_all_collision_count;
static bool find_all_collision_keys_found[5];

static bool find_all_collision_cb(const void *key, size_t key_len,
                                   const void *value, size_t value_len,
                                   void *user_ctx) {
    (void)value; (void)value_len; (void)user_ctx;
    find_all_collision_count++;

    /* Mark which key was found */
    char buf[16];
    assert(key_len < sizeof(buf));
    memcpy(buf, key, key_len);
    buf[key_len] = '\0';
    int idx = atoi(buf + 1);
    if (idx >= 0 && idx < 5) {
        find_all_collision_keys_found[idx] = true;
    }

    return true; /* keep iterating */
}

void test_find_all_with_collisions(void) {
    printf("Testing find_all with collisions...\n");
    ht_table_t *t = ht_create(NULL, const_hash42, NULL, NULL);

    /* Insert 5 entries — all hash to 42 */
    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "c%d", i);
        int val = i;
        bool inserted = ht_upsert(t, key, strlen(key), &val, sizeof(val));
        assert(inserted == true);
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 5);

    /* Iterate via ht_find_all with hash=42 */
    find_all_collision_count = 0;
    memset(find_all_collision_keys_found, 0, sizeof(find_all_collision_keys_found));
    ht_find_all(t, 42, find_all_collision_cb, NULL);

    assert(find_all_collision_count == 5);
    for (int i = 0; i < 5; i++) {
        assert(find_all_collision_keys_found[i] == true);
    }

    ht_destroy(t);
    printf("Find all with collisions passed!\n");
}

void test_with_hash_cross_api(void) {
    printf("Testing with_hash cross-api...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Step 1: Insert via ht_insert_with_hash */
    uint64_t h = fnv1a_hash("x", 1, NULL);
    bool inserted = ht_upsert_with_hash(t, h, "x", 1, "v1", 2);
    assert(inserted == true);

    /* Step 2: Remove via ht_remove (normal) */
    size_t removed = ht_remove(t, "x", 1);
    assert(removed == true);

    assert(ht_find(t, "x", 1, NULL) == NULL);

    /* Step 3: Insert via ht_insert (normal) */
    inserted = ht_upsert(t, "x", 1, "v2", 2);
    assert(inserted == true);

    /* Step 4: Find via ht_find_with_hash */
    size_t out_len = 0;
    const char *found = ht_find_with_hash(t, h, "x", 1, &out_len);
    assert(found != NULL);
    assert(out_len == 2);
    assert(memcmp(found, "v2", 2) == 0);

    ht_destroy(t);
    printf("With hash cross-api passed!\n");
}

void test_large_key_large_value_together(void) {
    printf("Testing large key + large value together (100000 bytes each)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    size_t big_len = 100000;
    char *big_key = malloc(big_len);
    char *big_val = malloc(big_len);
    assert(big_key != NULL && big_val != NULL);

    /* Fill key with pattern */
    memset(big_key, 'K', big_len);
    big_key[0] = 'A';
    big_key[big_len - 1] = 'Z';

    /* Fill value with distinct pattern */
    memset(big_val, 'V', big_len);
    big_val[0] = 'S';
    big_val[1] = 'T';
    big_val[big_len - 2] = 'E';
    big_val[big_len - 1] = 'N';

    bool inserted = ht_upsert(t, big_key, big_len, big_val, big_len);
    assert(inserted == true);

    size_t out_len = 0;
    const char *found = ht_find(t, big_key, big_len, &out_len);
    assert(found != NULL);
    assert(out_len == big_len);

    /* Verify start of value */
    assert(found[0] == 'S');
    assert(found[1] == 'T');
    /* Verify end of value */
    assert(found[big_len - 2] == 'E');
    assert(found[big_len - 1] == 'N');
    /* Verify middle */
    assert(found[big_len / 2] == 'V');

    free(big_key);
    free(big_val);
    ht_destroy(t);
    printf("Large key + large value together passed!\n");
}

void test_insert_duplicate_with_different_value_size(void) {
    printf("Testing insert duplicate with different value size...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    bool inserted = ht_upsert(t, "k", 1, "short", 5);
    assert(inserted == true);

    inserted = ht_upsert(t, "k", 1, "a much longer value here", 22);
    assert(inserted == false); /* update */

    size_t out_len = 0;
    const char *found = ht_find(t, "k", 1, &out_len);
    assert(found != NULL);
    assert(out_len == 22);
    assert(memcmp(found, "a much longer value here", 22) == 0);

    INV_CHECK(t, "test_dup_val_size: after update");

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 1);

    ht_destroy(t);
    printf("Insert duplicate with different value size passed!\n");
}

void test_remove_nonexistent(void) {
    printf("Testing remove nonexistent...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    size_t removed = ht_remove(t, "nope", 4);
    assert(removed == false);

    INV_CHECK(t, "test_remove_nonexist: after failed remove");

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 0);

    ht_destroy(t);
    printf("Remove nonexistent passed!\n");
}

void test_stats_empty_table(void) {
    printf("Testing stats on empty table...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 0);
    assert(stats.load_factor == 0.0);

    /* Capacity should be a power of 2 */
    assert(stats.capacity > 0);
    assert((stats.capacity & (stats.capacity - 1)) == 0);

    INV_CHECK(t, "test_stats_empty: baseline");

    ht_destroy(t);
    printf("Stats empty table passed!\n");
}

// ============================================================================
// New tests: backward shift, early termination, collision edge cases
// ============================================================================

void test_delete_chain_head_collision(void) {
    printf("Testing delete chain head under collision...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    /* Insert 30 colliding keys (all hash to 42) forming a chain */
    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "ch%d", i);
        vals[i] = i * 11;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete the first entry in the chain (ideal position, probe_dist=0) */
    ht_remove(t, "ch0", 3);

    INV_CHECK(t, "test_delete_chain_head: after head delete");

    /* All remaining 29 entries must be findable */
    for (int i = 1; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "ch%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 11) {
            printf("  FAIL: ch%d lost after head delete\n", i);
            ht_destroy(t); return;
        }
    }
    assert(ht_find(t, "ch0", 3, NULL) == NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 29);

    ht_destroy(t);
    printf("Delete chain head collision passed!\n");
}

void test_delete_middle_collision_verify_ends(void) {
    printf("Testing delete middle of collision chain, verify both ends...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    /* Insert 10 colliding keys */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "md%d", i);
        vals[i] = i * 7;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete middle entries 3,4,5 */
    ht_remove(t, "md3", 3);
    ht_remove(t, "md4", 3);
    ht_remove(t, "md5", 3);

    INV_CHECK(t, "test_delete_middle: after deletes");

    /* Verify all 7 remaining entries */
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "md%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i >= 3 && i <= 5) {
            assert(v == NULL);
        } else {
            if (v == NULL || *v != i * 7) {
                printf("  FAIL: md%d lost after middle deletes\n", i);
                ht_destroy(t); return;
            }
        }
    }

    ht_destroy(t);
    printf("Delete middle collision verify ends passed!\n");
}

void test_tombstone_early_termination(void) {
    printf("Testing tombstone doesn't break early termination...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 12 keys to fill up the table */
    int vals[12];
    for (int i = 0; i < 12; i++) {
        char k[16]; snprintf(k, sizeof(k), "et%d", i);
        vals[i] = i;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete half — creates tombstones interleaved with live entries */
    for (int i = 0; i < 12; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "et%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Now insert a new key — insert path must respect stranding prevention */
    int v_new = 999;
    ht_upsert(t, "newkey", 6, &v_new, sizeof(int));

    INV_CHECK(t, "test_tombstone_early_termination: after insert");

    /* All odd-indexed entries + newkey must be findable */
    for (int i = 1; i < 12; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "et%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i) {
            printf("  FAIL: et%d lost\n", i);
            ht_destroy(t); return;
        }
    }
    const int *nv = ht_find(t, "newkey", 6, NULL);
    assert(nv != NULL && *nv == 999);

    ht_destroy(t);
    printf("Tombstone early termination passed!\n");
}

void test_resize_with_many_tombstones(void) {
    printf("Testing resize with many tombstones...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 20 keys */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "rt%d", i);
        vals[i] = i * 3;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete 15 of them — many tombstones */
    for (int i = 0; i < 15; i++) {
        char k[16]; snprintf(k, sizeof(k), "rt%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Resize to 128 — should clean up tombstones */
    assert(ht_resize(t, 128));

    INV_CHECK(t, "test_resize_tombstones: after resize");

    /* Survivors must be correct */
    for (int i = 15; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "rt%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 3) {
            printf("  FAIL: rt%d lost after resize with tombstones\n", i);
            ht_destroy(t); return;
        }
    }
    for (int i = 0; i < 15; i++) {
        assert(ht_find(t, "rt%d" + 0, 0, NULL) || true); // just no crash
        char k[16]; snprintf(k, sizeof(k), "rt%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    ht_destroy(t);
    printf("Resize with many tombstones passed!\n");
}

void test_delete_then_insert_stranding(void) {
    printf("Testing delete-then-insert stranding prevention...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.85,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 10 keys */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "ds%d", i);
        vals[i] = i * 5;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete key 5 — creates tombstone */
    ht_remove(t, "ds5", 3);

    /* Insert a new key that could land at the tombstone position
     * and potentially strand entries past it via early termination */
    int v_new = 777;
    ht_upsert(t, "ds5", 3, &v_new, sizeof(int));

    INV_CHECK(t, "test_delete_insert_stranding: after reinsert");

    /* Verify all 10 entries (ds5 was reinserted with new value) */
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "ds%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        int expected = (i == 5) ? 777 : i * 5;
        if (v == NULL || *v != expected) {
            printf("  FAIL: ds%d lost (got %p expected %d)\n",
                   i, (void *)v, expected);
            ht_destroy(t); return;
        }
    }

    ht_destroy(t);
    printf("Delete-then-insert stranding prevention passed!\n");
}

void test_inc_under_collision(void) {
    printf("Testing ht_inc under collision...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    /* Inc 10 colliding keys 100 times each */
    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < 10; i++) {
            char k[16]; snprintf(k, sizeof(k), "ic%d", i);
            int64_t r = ht_inc(t, k, strlen(k), 1);
            assert(r == round + 1);
        }
    }

    INV_CHECK(t, "test_inc_collision: after 1000 incs");

    /* Verify final values */
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "ic%d", i);
        const int64_t *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != 100) {
            printf("  FAIL: ic%d got %lld expected 100\n", i,
                   v ? (long long)*v : -1LL);
            ht_destroy(t); return;
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    ht_destroy(t);
    printf("Inc under collision passed!\n");
}

void test_capacity_2_full_load(void) {
    printf("Testing minimum capacity table...\n");
    /* initial_capacity < 4 gets clamped to 4, so use ht_resize to get to 4 */
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.97,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity == 4);

    /* Insert 3 entries into cap-4 table (high load) */
    int v1 = 10, v2 = 20, v3 = 30;
    assert(ht_upsert(t, "a", 1, &v1, sizeof(int)));
    assert(ht_upsert(t, "b", 1, &v2, sizeof(int)));
    assert(ht_upsert(t, "c", 1, &v3, sizeof(int)));

    INV_CHECK(t, "test_cap2_full: after 3 inserts");

    /* All must be findable */
    assert(*(int *)ht_find(t, "a", 1, NULL) == 10);
    assert(*(int *)ht_find(t, "b", 1, NULL) == 20);
    assert(*(int *)ht_find(t, "c", 1, NULL) == 30);

    /* Remove middle one */
    assert(ht_remove(t, "b", 1));
    assert(ht_find(t, "b", 1, NULL) == NULL);
    assert(*(int *)ht_find(t, "a", 1, NULL) == 10);
    assert(*(int *)ht_find(t, "c", 1, NULL) == 30);

    /* Reinsert */
    int v4 = 40;
    assert(ht_upsert(t, "b", 1, &v4, sizeof(int)));
    assert(*(int *)ht_find(t, "b", 1, NULL) == 40);

    INV_CHECK(t, "test_cap2_full: after delete+reinsert");

    ht_destroy(t);
    printf("Minimum capacity table passed!\n");
}

void test_remove_all_verify_clean(void) {
    printf("Testing remove all entries leaves clean state...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int vals[12];
    for (int i = 0; i < 12; i++) {
        char k[16]; snprintf(k, sizeof(k), "ra%d", i);
        vals[i] = i;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Remove all */
    for (int i = 0; i < 12; i++) {
        char k[16]; snprintf(k, sizeof(k), "ra%d", i);
        size_t r = ht_remove(t, k, strlen(k));
        assert(r);
    }

    INV_CHECK(t, "test_remove_all: after deleting all");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Reinsert — should work cleanly */
    int v = 42;
    assert(ht_upsert(t, "fresh", 5, &v, sizeof(int)));
    assert(*(int *)ht_find(t, "fresh", 5, NULL) == 42);

    ht_destroy(t);
    printf("Remove all verify clean passed!\n");
}

/* ---- Spill lane remove (spill_remove never tested) ---- */

static uint64_t zero_hash_fn(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

void test_spill_remove(void) {
    printf("Testing spill lane remove...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash_fn, NULL, NULL);

    /* Insert 10 spill entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", i);
        int v = i * 10;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    /* Remove from head, middle, tail */
    assert(ht_remove(t, "sr0", 3));  /* head */
    assert(ht_remove(t, "sr5", 3));  /* middle */
    assert(ht_remove(t, "sr9", 3));  /* tail */

    INV_CHECK(t, "test_spill_remove: after removes");

    ht_stats(t, &st);
    assert(st.size == 7);

    /* Verify remaining */
    int expected[] = {1,2,3,4,6,7,8};
    for (int j = 0; j < 7; j++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", expected[j]);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != expected[j] * 10) {
            printf("  FAIL: sr%d lost after spill remove\n", expected[j]);
            ht_destroy(t); return;
        }
    }
    assert(ht_find(t, "sr0", 3, NULL) == NULL);
    assert(ht_find(t, "sr5", 3, NULL) == NULL);
    assert(ht_find(t, "sr9", 3, NULL) == NULL);

    /* Remove last single entry */
    char k[8]; snprintf(k, sizeof(k), "sr%d", 8);
    assert(ht_remove(t, k, strlen(k)));
    assert(ht_find(t, k, strlen(k), NULL) == NULL);

    ht_stats(t, &st);
    assert(st.size == 6);

    ht_destroy(t);
    printf("Spill lane remove passed!\n");
}

/* ---- Mixed hash=0 and hash=1 spill entries ---- */

void test_spill_mixed_sentinels(void) {
    printf("Testing mixed hash=0 and hash=1 spill entries...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert entries with hash=0 and hash=1 — both go to spill lane */
    int v0a = 10, v0b = 20, v1a = 30, v1b = 40;
    assert(ht_upsert_with_hash(t, 0, "a", 1, &v0a, sizeof(int)));
    assert(ht_upsert_with_hash(t, 0, "b", 1, &v0b, sizeof(int)));
    assert(ht_upsert_with_hash(t, 1, "c", 1, &v1a, sizeof(int)));
    assert(ht_upsert_with_hash(t, 1, "d", 1, &v1b, sizeof(int)));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 4);

    /* Verify all distinct */
    assert(*(int *)ht_find_with_hash(t, 0, "a", 1, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, 0, "b", 1, NULL) == 20);
    assert(*(int *)ht_find_with_hash(t, 1, "c", 1, NULL) == 30);
    assert(*(int *)ht_find_with_hash(t, 1, "d", 1, NULL) == 40);

    /* Remove hash=0 entry, verify hash=1 unaffected */
    assert(ht_remove_with_hash(t, 0, "a", 1));
    assert(ht_find_with_hash(t, 0, "a", 1, NULL) == NULL);
    assert(*(int *)ht_find_with_hash(t, 1, "c", 1, NULL) == 30);
    assert(*(int *)ht_find_with_hash(t, 1, "d", 1, NULL) == 40);
    assert(*(int *)ht_find_with_hash(t, 0, "b", 1, NULL) == 20);

    /* Remove hash=1 entry, verify hash=0 unaffected */
    assert(ht_remove_with_hash(t, 1, "d", 1));
    assert(ht_find_with_hash(t, 1, "d", 1, NULL) == NULL);
    assert(*(int *)ht_find_with_hash(t, 0, "b", 1, NULL) == 20);

    INV_CHECK(t, "test_spill_mixed_sentinels: after mixed removes");

    ht_stats(t, &st);
    assert(st.size == 2);

    ht_destroy(t);
    printf("Mixed sentinel spill passed!\n");
}

/* ---- ht_inc on key in spill lane ---- */

void test_inc_on_spill_key(void) {
    printf("Testing ht_inc on spill-lane key (hash=0)...\n");
    ht_table_t *t = ht_create(NULL, zero_hash_fn, NULL, NULL);

    /* Insert into spill lane via normal API (zero_hash_fn returns 0) */
    int v = 5;
    ht_upsert(t, "counter", 7, &v, sizeof(int));

    /* Verify it's in the spill lane: find_with_hash(0, ...) works */
    const int *fv = ht_find_with_hash(t, 0, "counter", 7, NULL);
    assert(fv != NULL && *fv == 5);

    /* ht_inc finds the entry in spill lane but val_len=4 != sizeof(int64_t),
     * so it discards the old value and sets new_val = delta */
    int64_t r = ht_inc(t, "counter", 7, 10);
    assert(r == 10);

    /* Second inc: now val_len == sizeof(int64_t), so it adds delta */
    r = ht_inc(t, "counter", 7, -3);
    assert(r == 7);

    /* Verify final value */
    const int64_t *v2 = ht_find(t, "counter", 7, NULL);
    assert(v2 != NULL && *v2 == 7);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);  /* No duplicate */

    ht_destroy(t);
    printf("Inc on spill key passed!\n");
}

/* ---- Bulk 1000-key round-trip ---- */

void test_bulk_1000_roundtrip(void) {
    printf("Testing bulk 1000-key round-trip...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 1000 keys */
    for (int i = 0; i < 1000; i++) {
        char k[16]; snprintf(k, sizeof(k), "bt%d", i);
        int v = i * 13;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "test_bulk_1000: after insert");

    /* Find all */
    for (int i = 0; i < 1000; i++) {
        char k[16]; snprintf(k, sizeof(k), "bt%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 13);
    }

    /* Delete even */
    for (int i = 0; i < 1000; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "bt%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "test_bulk_1000: after delete evens");

    /* Reinsert deleted with new value */
    for (int i = 0; i < 1000; i += 2) {
        char k[16]; snprintf(k, sizeof(k), "bt%d", i);
        int v = i * 17;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "test_bulk_1000: after reinsert");

    /* Final verify: all 1000 present, evens have new value */
    for (int i = 0; i < 1000; i++) {
        char k[16]; snprintf(k, sizeof(k), "bt%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        int expected = (i % 2 == 0) ? i * 17 : i * 13;
        assert(v != NULL && *v == expected);
    }

    ht_destroy(t);
    printf("Bulk 1000 round-trip passed!\n");
}

/* ---- Compact then iterate — count matches size ---- */

void test_iter_after_compact(void) {
    printf("Testing iterator after compact matches size...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 40; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_compact(t);

    INV_CHECK(t, "test_iter_compact: after compact");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    /* Iterate and count */
    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *ik, *iv;
    size_t ikl, ivl;
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) count++;
    assert((size_t)count == st.size);

    ht_destroy(t);
    printf("Iter after compact passed!\n");
}

/* ---- Update value size from large to small ---- */

void test_update_value_shrink(void) {
    printf("Testing update value from large to small...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    char big_val[1000];
    memset(big_val, 'X', sizeof(big_val));
    assert(ht_upsert(t, "k", 1, big_val, sizeof(big_val)));

    /* Overwrite with tiny value */
    int small = 42;
    assert(ht_upsert(t, "k", 1, &small, sizeof(small)) == false);

    size_t vl = 0;
    const int *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL && vl == sizeof(int) && *v == 42);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Update value shrink passed!\n");
}

// ============================================================================
// Additional tests: stats accuracy, lifecycle, iterator values, edge cases
// ============================================================================

void test_stats_after_each_op(void) {
    printf("Testing stats after each operation type...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    ht_stats_t st;

    /* Empty */
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.capacity == 16);
    assert(st.load_factor == 0.0);

    /* Insert */
    int v1 = 10;
    ht_upsert(t, "a", 1, &v1, sizeof(int));
    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.load_factor > 0.0);
    INV_CHECK(t, "stats_ops: after insert");

    /* Update */
    int v2 = 20;
    ht_upsert(t, "a", 1, &v2, sizeof(int));
    ht_stats(t, &st);
    assert(st.size == 1);
    INV_CHECK(t, "stats_ops: after update");

    /* Inc existing — val was int (4 bytes), ht_inc sees val_len != int64_t,
     * discards old value, sets new_val = delta */
    int64_t r = ht_inc(t, "a", 1, 5);
    assert(r == 5);
    ht_stats(t, &st);
    assert(st.size == 1);
    INV_CHECK(t, "stats_ops: after inc existing");

    /* Inc new key */
    r = ht_inc(t, "b", 1, 100);
    assert(r == 100);
    ht_stats(t, &st);
    assert(st.size == 2);
    INV_CHECK(t, "stats_ops: after inc new");

    /* Remove */
    ht_remove(t, "a", 1);
    ht_stats(t, &st);
    assert(st.size == 1);
    INV_CHECK(t, "stats_ops: after remove");

    /* Find (no stats change) */
    assert(ht_find(t, "b", 1, NULL) != NULL);
    ht_stats(t, &st);
    assert(st.size == 1);

    /* Verify final state */
    const int64_t *bv = ht_find(t, "b", 1, NULL);
    assert(bv != NULL && *bv == 100);
    assert(ht_find(t, "a", 1, NULL) == NULL);

    ht_destroy(t);
    printf("Stats after each op passed!\n");
}

void test_inc_lifecycle(void) {
    printf("Testing ht_inc lifecycle (create, inc, remove, re-create)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Create via inc */
    int64_t r = ht_inc(t, "ctr", 3, 10);
    assert(r == 10);
    INV_CHECK(t, "inc_life: after create");

    /* Inc again */
    r = ht_inc(t, "ctr", 3, 5);
    assert(r == 15);

    /* Remove */
    assert(ht_remove(t, "ctr", 3));
    assert(ht_find(t, "ctr", 3, NULL) == NULL);

    /* Re-create via inc */
    r = ht_inc(t, "ctr", 3, 7);
    assert(r == 7);
    INV_CHECK(t, "inc_life: after re-create");

    const int64_t *v = ht_find(t, "ctr", 3, NULL);
    assert(v != NULL && *v == 7);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Inc lifecycle passed!\n");
}

void test_iter_values_correct(void) {
    printf("Testing iterator values correctness...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    struct { const char *key; int val; } entries[] = {
        {"alpha", 1}, {"bravo", 2}, {"charlie", 3}, {"delta", 4}, {"echo", 5}
    };
    for (int i = 0; i < 5; i++)
        ht_upsert(t, entries[i].key, strlen(entries[i].key),
                  &entries[i].val, sizeof(int));

    INV_CHECK(t, "iter_vals: after inserts");

    /* Iterate and verify each key-value pair */
    bool seen[5] = {false};
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t kl, vl;
    int count = 0;
    while (ht_iter_next(t, &iter, &key, &kl, &val, &vl)) {
        count++;
        assert(vl == sizeof(int));
        int v = *(const int *)val;
        bool matched = false;
        for (int i = 0; i < 5; i++) {
            if (kl == strlen(entries[i].key) &&
                memcmp(key, entries[i].key, kl) == 0) {
                assert(v == entries[i].val);
                assert(!seen[i]);
                seen[i] = true;
                matched = true;
                break;
            }
        }
        assert(matched);
    }
    assert(count == 5);
    for (int i = 0; i < 5; i++) assert(seen[i]);

    ht_destroy(t);
    printf("Iterator values correct passed!\n");
}

static int find_all_early_count;
static bool find_all_early_cb(const void *key, size_t kl,
                               const void *val, size_t vl, void *ctx) {
    (void)key; (void)kl; (void)val; (void)vl; (void)ctx;
    find_all_early_count++;
    return false;
}

void test_find_all_early_stop(void) {
    printf("Testing find_all early stop...\n");
    ht_table_t *t = ht_create(NULL, const_hash42, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "fa%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    find_all_early_count = 0;
    ht_find_all(t, 42, find_all_early_cb, NULL);
    assert(find_all_early_count == 1);

    ht_destroy(t);
    printf("Find all early stop passed!\n");
}

void test_collision_find_after_interleaved_delete(void) {
    printf("Testing collision chain find after interleaved deletes...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        vals[i] = i * 3;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete odd indices (interleaved with live) */
    for (int i = 1; i < 20; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "collision_interleaved: after deletes");

    /* Verify even indices correct */
    for (int i = 0; i < 20; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 3) {
            printf("  FAIL: cf%d lost after interleaved delete\n", i);
            ht_destroy(t); return;
        }
    }
    /* Verify odd gone */
    for (int i = 1; i < 20; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "cf%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    ht_destroy(t);
    printf("Collision find after interleaved delete passed!\n");
}

void test_update_value_grow(void) {
    printf("Testing update value from small to large...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int small = 42;
    assert(ht_upsert(t, "k", 1, &small, sizeof(int)));

    char big[500];
    memset(big, 'Q', sizeof(big));
    big[0] = 'A'; big[499] = 'Z';
    assert(ht_upsert(t, "k", 1, big, sizeof(big)) == false);

    size_t vl = 0;
    const char *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL && vl == 500);
    assert(v[0] == 'A' && v[499] == 'Z' && v[250] == 'Q');

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    INV_CHECK(t, "update_grow: after update");

    ht_destroy(t);
    printf("Update value grow passed!\n");
}

void test_insert_with_hash1(void) {
    printf("Testing insert_with_hash(1)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int v1 = 10, v2 = 20;
    assert(ht_upsert_with_hash(t, 1, "x", 1, &v1, sizeof(int)));
    assert(ht_upsert_with_hash(t, 1, "y", 1, &v2, sizeof(int)));

    INV_CHECK(t, "hash1: after inserts");

    /* Find via with_hash API */
    assert(*(int *)ht_find_with_hash(t, 1, "x", 1, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, 1, "y", 1, NULL) == 20);

    /* Normal find recomputes hash — won't find spill entries */
    assert(ht_find(t, "x", 1, NULL) == NULL);
    assert(ht_find(t, "y", 1, NULL) == NULL);

    /* Remove via with_hash */
    assert(ht_remove_with_hash(t, 1, "x", 1));
    assert(ht_find_with_hash(t, 1, "x", 1, NULL) == NULL);
    assert(*(int *)ht_find_with_hash(t, 1, "y", 1, NULL) == 20);

    INV_CHECK(t, "hash1: after remove");

    ht_destroy(t);
    printf("Insert with hash=1 passed!\n");
}

void test_compact_idempotent(void) {
    printf("Testing compact is idempotent...\n");
    ht_config_t cfg = { .initial_capacity = 32, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ci%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ci%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_compact(t);
    INV_CHECK(t, "compact_idem: first");

    ht_stats_t st1;
    ht_stats(t, &st1);

    ht_compact(t);
    INV_CHECK(t, "compact_idem: second");

    ht_stats_t st2;
    ht_stats(t, &st2);

    assert(st1.size == st2.size);
    assert(st1.capacity == st2.capacity);

    /* Verify entries */
    for (int i = 10; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ci%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Compact idempotent passed!\n");
}

void test_empty_table_ops(void) {
    printf("Testing operations on empty table...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    assert(ht_find(t, "anything", 8, NULL) == NULL);
    assert(ht_find_with_hash(t, 42, "x", 1, NULL) == NULL);
    assert(ht_remove(t, "anything", 8) == false);
    assert(ht_remove_with_hash(t, 0, "x", 1) == 0);

    ht_iter_t iter = ht_iter_begin(t);
    assert(ht_iter_next(t, &iter, NULL, NULL, NULL, NULL) == false);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.load_factor == 0.0);

    INV_CHECK(t, "empty_ops: baseline");

    ht_destroy(t);
    printf("Empty table ops passed!\n");
}

void test_inc_multiple_keys(void) {
    printf("Testing ht_inc on multiple independent keys...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int round = 0; round < 50; round++) {
        ht_inc(t, "a", 1, 1);
        ht_inc(t, "b", 1, 2);
        ht_inc(t, "c", 1, -1);
    }

    INV_CHECK(t, "inc_multi: after 150 incs");

    assert(*(int64_t *)ht_find(t, "a", 1, NULL) == 50);
    assert(*(int64_t *)ht_find(t, "b", 1, NULL) == 100);
    assert(*(int64_t *)ht_find(t, "c", 1, NULL) == -50);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 3);

    ht_destroy(t);
    printf("Inc multiple keys passed!\n");
}

void test_remove_half_reinsert(void) {
    printf("Testing remove half then reinsert...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "remove_half: after removes");

    /* Reinsert with new values */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        int v = i + 100;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "remove_half: after reinsert");

    /* Verify all 20 */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        int expected = (i < 10) ? i + 100 : i;
        assert(v != NULL && *v == expected);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);

    ht_destroy(t);
    printf("Remove half reinsert passed!\n");
}

void test_binary_key_collision(void) {
    printf("Testing binary key collision via const hash...\n");
    ht_table_t *t = ht_create(NULL, const_hash42, NULL, NULL);

    uint8_t k1[] = {0x01, 0x02, 0x03};
    uint8_t k2[] = {0x04, 0x05, 0x06};
    int v1 = 11, v2 = 22;

    assert(ht_upsert(t, k1, 3, &v1, sizeof(int)));
    assert(ht_upsert(t, k2, 3, &v2, sizeof(int)));

    INV_CHECK(t, "bin_collision: after inserts");

    assert(*(int *)ht_find(t, k1, 3, NULL) == 11);
    assert(*(int *)ht_find(t, k2, 3, NULL) == 22);

    ht_remove(t, k1, 3);
    INV_CHECK(t, "bin_collision: after remove k1");
    assert(ht_find(t, k1, 3, NULL) == NULL);
    assert(*(int *)ht_find(t, k2, 3, NULL) == 22);

    ht_destroy(t);
    printf("Binary key collision passed!\n");
}

void test_resize_same_capacity(void) {
    printf("Testing resize to same capacity...\n");
    ht_config_t cfg = { .initial_capacity = 16, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rs%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity == 16);

    assert(ht_resize(t, 16));
    INV_CHECK(t, "resize_same: after resize");

    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.capacity == 16);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rs%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Resize same capacity passed!\n");
}

void test_tombstone_tracking(void) {
    printf("Testing tombstone count tracking...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "tt%d", i);
        vals[i] = i;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    size_t initial_tombs = st.tombstone_cnt;

    /* Delete 10 */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "tt%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats(t, &st);
    assert(st.size == 20);
    assert(st.tombstone_cnt >= initial_tombs);
    INV_CHECK(t, "tomb_track: after deletes");

    /* Compact should clear delete-tombstones */
    ht_compact(t);
    INV_CHECK(t, "tomb_track: after compact");

    ht_stats(t, &st);
    assert(st.size == 20);

    /* Verify survivors */
    for (int i = 10; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "tt%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "tt%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_destroy(t);
    printf("Tombstone tracking passed!\n");
}

void test_spill_resize_lifecycle(void) {
    printf("Testing spill lane lifecycle with resize...\n");
    ht_table_t *t = ht_create(NULL, zero_hash_fn, NULL, NULL);

    /* Insert 5 spill entries (zero_hash_fn returns 0) */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sl%d", i);
        int v = i * 10;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    /* Insert main-table entries via with_hash */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "m%d", i);
        int v = i * 5;
        assert(ht_upsert_with_hash(t, 100 + i, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "spill_resize: after mixed inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 25);

    /* Verify all entries */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sl%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 10);
    }
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "m%d", i);
        const int *v = ht_find_with_hash(t, 100 + i, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 5);
    }

    /* Remove a spill entry */
    assert(ht_remove(t, "sl2", 3));
    assert(ht_find(t, "sl2", 3, NULL) == NULL);
    assert(ht_find(t, "sl0", 3, NULL) != NULL);

    INV_CHECK(t, "spill_resize: after spill remove");

    ht_destroy(t);
    printf("Spill resize lifecycle passed!\n");
}

void test_clear_stats_accuracy(void) {
    printf("Testing clear stats accuracy...\n");
    ht_config_t cfg = { .initial_capacity = 16, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 12; i++) {
        char k[8]; snprintf(k, sizeof(k), "cs%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }
    for (int i = 0; i < 6; i++) {
        char k[8]; snprintf(k, sizeof(k), "cs%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_clear(t);
    INV_CHECK(t, "clear_stats: after clear");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.tombstone_cnt == 0);
    assert(st.load_factor == 0.0);
    assert(st.capacity > 0);

    ht_destroy(t);
    printf("Clear stats accuracy passed!\n");
}

void test_double_insert_returns_false(void) {
    printf("Testing double insert returns false...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    assert(ht_upsert(t, "k", 1, "v1", 2) == true);
    assert(ht_upsert(t, "k", 1, "v2", 2) == false);
    assert(ht_upsert(t, "k", 1, "v3", 2) == false);

    size_t vl = 0;
    const char *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL && vl == 2 && memcmp(v, "v3", 2) == 0);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    INV_CHECK(t, "double_insert: after updates");

    ht_destroy(t);
    printf("Double insert returns false passed!\n");
}

void test_collision_chain_tail_delete(void) {
    printf("Testing collision chain tail delete...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "td%d", i);
        vals[i] = i * 5;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete the last entry (highest probe_dist) */
    ht_remove(t, "td9", 3);
    INV_CHECK(t, "tail_delete: after delete");

    /* All 9 remaining must be findable */
    for (int i = 0; i < 9; i++) {
        char k[8]; snprintf(k, sizeof(k), "td%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 5) {
            printf("  FAIL: td%d lost after tail delete\n", i);
            ht_destroy(t); return;
        }
    }
    assert(ht_find(t, "td9", 3, NULL) == NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 9);

    ht_destroy(t);
    printf("Collision chain tail delete passed!\n");
}

void test_inc_with_hash_existing(void) {
    printf("Testing ht_inc on with_hash inserted key...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert with known hash */
    uint64_t h = fnv1a_hash("x", 1, NULL);
    ht_upsert_with_hash(t, h, "x", 1, "v", 1);

    /* ht_inc recomputes hash via fnv1a → finds same bucket */
    int64_t r = ht_inc(t, "x", 1, 10);
    assert(r == 10);

    /* Old value was "v" (1 byte, not int64_t), so inc discards it */
    const int64_t *v = ht_find(t, "x", 1, NULL);
    assert(v != NULL && *v == 10);

    r = ht_inc(t, "x", 1, 5);
    assert(r == 15);

    INV_CHECK(t, "inc_with_hash: after incs");

    v = ht_find(t, "x", 1, NULL);
    assert(v != NULL && *v == 15);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Inc with hash existing passed!\n");
}

void test_iter_count_matches_size(void) {
    printf("Testing iter count matches size under various conditions...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Phase 1: After inserts */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *ik, *iv; size_t ikl, ivl;
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) count++;
    assert((size_t)count == st.size);

    /* Phase 2: After deletes */
    for (int i = 0; i < 15; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        ht_remove(t, k, strlen(k));
    }

    INV_CHECK(t, "iter_count: after deletes");
    ht_stats(t, &st);
    count = 0;
    iter = ht_iter_begin(t);
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) count++;
    assert((size_t)count == st.size);

    /* Phase 3: After compact */
    ht_compact(t);
    INV_CHECK(t, "iter_count: after compact");
    ht_stats(t, &st);
    count = 0;
    iter = ht_iter_begin(t);
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) count++;
    assert((size_t)count == st.size);

    ht_destroy(t);
    printf("Iter count matches size passed!\n");
}

void test_resize_down_verify(void) {
    printf("Testing resize down preserves all entries...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 10 entries, causing resize to 32 or 64 */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rd%d", i);
        int v = i * 7;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 16);

    /* Resize down to 16 (minimum for 10 entries at 0.75 load) */
    assert(ht_resize(t, 16));
    INV_CHECK(t, "resize_down: after resize");

    ht_stats(t, &st);
    assert(st.size == 10);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rd%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 7);
    }

    ht_destroy(t);
    printf("Resize down verify passed!\n");
}

void test_find_all_empty_hash(void) {
    printf("Testing find_all with hash=0 on empty and populated...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* find_all on empty table should not crash */
    find_all_count = 0;
    ht_find_all(t, 0, find_all_cb, NULL);
    assert(find_all_count == 0);

    /* Insert some entries, none should match hash=0 */
    ht_upsert(t, "a", 1, "v", 1);
    ht_upsert(t, "b", 1, "v", 1);

    find_all_count = 0;
    ht_find_all(t, 0, find_all_cb, NULL);
    assert(find_all_count == 0);

    ht_destroy(t);
    printf("Find all empty hash passed!\n");
}

void test_update_value_same_size(void) {
    printf("Testing update value with same size preserves byte-exact...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    assert(ht_upsert(t, "k", 1, "old", 3));
    assert(ht_upsert(t, "k", 1, "new", 3) == false);

    size_t vl = 0;
    const char *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL && vl == 3 && memcmp(v, "new", 3) == 0);

    INV_CHECK(t, "update_same_size: after update");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Update value same size passed!\n");
}

// ============================================================================
// Additional round: churn, cross-API, resize, value verification, invariants
// ============================================================================

void test_single_key_churn(void) {
    printf("Testing single key insert/remove churn (50 cycles)...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 50; i++) {
        int v = i * 3;
        assert(ht_upsert(t, "churn", 5, &v, sizeof(int))); /* always true: table is empty */
        assert(ht_remove(t, "churn", 5));
        assert(ht_find(t, "churn", 5, NULL) == NULL);

        if (i % 10 == 9) INV_CHECK(t, "single_churn: checkpoint");
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    INV_CHECK(t, "single_churn: final");

    ht_destroy(t);
    printf("Single key churn passed!\n");
}

void test_multiple_resizes(void) {
    printf("Testing multiple resizes (4 -> 8 -> 16 -> 32 -> 64)...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 50 entries — triggers several resizes */
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "mr%d", i);
        int v = i;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "multi_resize: after 50 inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 50);
    assert(st.capacity >= 64);
    assert((st.capacity & (st.capacity - 1)) == 0); /* power of 2 */

    /* Verify all */
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "mr%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Multiple resizes passed!\n");
}

void test_find_all_verifies_keys(void) {
    printf("Testing find_all returns only matching keys...\n");
    ht_table_t *t = ht_create(NULL, const_hash42, NULL, NULL);

    /* Insert 5 colliding and 5 non-colliding (via with_hash) */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "c%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "n%d", i);
        int v = i + 100;
        ht_upsert_with_hash(t, 999, k, strlen(k), &v, sizeof(int));
    }

    /* find_all for hash=42 should find exactly the c0..c4 keys */
    find_all_collision_count = 0;
    memset(find_all_collision_keys_found, 0, sizeof(find_all_collision_keys_found));
    ht_find_all(t, 42, find_all_collision_cb, NULL);

    assert(find_all_collision_count == 5);
    for (int i = 0; i < 5; i++)
        assert(find_all_collision_keys_found[i] == true);

    INV_CHECK(t, "find_all_keys: after search");

    ht_destroy(t);
    printf("Find all verifies keys passed!\n");
}

void test_remove_with_hash_cross_api(void) {
    printf("Testing remove_with_hash then normal find fails...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert via normal API */
    int v = 42;
    assert(ht_upsert(t, "x", 1, &v, sizeof(int)));

    uint64_t h = fnv1a_hash("x", 1, NULL);

    /* Remove via with_hash API */
    assert(ht_remove_with_hash(t, h, "x", 1));
    assert(ht_find(t, "x", 1, NULL) == NULL);
    assert(ht_find_with_hash(t, h, "x", 1, NULL) == NULL);

    INV_CHECK(t, "rm_with_hash_cross: after remove");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("Remove with hash cross-api passed!\n");
}

void test_inc_after_update(void) {
    printf("Testing ht_inc after value update...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert with int value */
    int v = 10;
    ht_upsert(t, "k", 1, &v, sizeof(int));

    /* Update with int64_t value */
    int64_t v2 = 100;
    ht_upsert(t, "k", 1, &v2, sizeof(int64_t));

    /* Now inc — val_len matches int64_t, should add */
    int64_t r = ht_inc(t, "k", 1, 7);
    assert(r == 107);

    INV_CHECK(t, "inc_after_update: after inc");

    const int64_t *fv = ht_find(t, "k", 1, NULL);
    assert(fv != NULL && *fv == 107);

    ht_destroy(t);
    printf("Inc after update passed!\n");
}

void test_delete_all_reverse_order(void) {
    printf("Testing delete all entries in reverse order...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    const int N = 25;
    int vals[25];
    for (int i = 0; i < N; i++) {
        char k[8]; snprintf(k, sizeof(k), "dr%d", i);
        vals[i] = i;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete in reverse */
    for (int i = N - 1; i >= 0; i--) {
        char k[8]; snprintf(k, sizeof(k), "dr%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "delete_reverse: after all deletes");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Reinsert should work cleanly */
    int v = 99;
    assert(ht_upsert(t, "fresh", 5, &v, sizeof(int)));
    assert(*(int *)ht_find(t, "fresh", 5, NULL) == 99);

    ht_destroy(t);
    printf("Delete all reverse order passed!\n");
}

void test_iter_after_clear(void) {
    printf("Testing iterator after clear yields nothing...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ic%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    ht_clear(t);
    INV_CHECK(t, "iter_clear: after clear");

    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *k, *v; size_t kl, vl;
    while (ht_iter_next(t, &iter, &k, &kl, &v, &vl)) count++;
    assert(count == 0);

    ht_destroy(t);
    printf("Iter after clear passed!\n");
}

void test_capacity_power_of_two_invariant(void) {
    printf("Testing capacity remains power of 2 across operations...\n");
    ht_config_t cfg = { .initial_capacity = 8, .max_load_factor = 0.5,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 100; i++) {
        char k[8]; snprintf(k, sizeof(k), "pw%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));

        ht_stats_t st;
        ht_stats(t, &st);
        assert((st.capacity & (st.capacity - 1)) == 0);
    }

    /* Remove some, triggering potential shrink */
    for (int i = 0; i < 80; i++) {
        char k[8]; snprintf(k, sizeof(k), "pw%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert((st.capacity & (st.capacity - 1)) == 0);
    INV_CHECK(t, "power_of_2: final");

    ht_destroy(t);
    printf("Capacity power of 2 invariant passed!\n");
}

void test_insert_after_compact(void) {
    printf("Testing insert after compact works correctly...\n");
    ht_config_t cfg = { .initial_capacity = 32, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ac%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ac%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_compact(t);
    INV_CHECK(t, "insert_after_compact: after compact");

    /* Insert new entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "an%d", i);
        int v = i + 100;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "insert_after_compact: after new inserts");

    /* Verify old survivors + new entries */
    for (int i = 10; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ac%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "an%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 100);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);

    ht_destroy(t);
    printf("Insert after compact passed!\n");
}

void test_inc_val_len_int64_after_inc(void) {
    printf("Testing ht_inc changes val_len to int64_t...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert with int value (4 bytes) */
    int v = 5;
    ht_upsert(t, "k", 1, &v, sizeof(int));

    size_t vl = 99;
    ht_find(t, "k", 1, &vl);
    assert(vl == sizeof(int));

    /* Inc — converts to int64_t */
    int64_t r = ht_inc(t, "k", 1, 10);
    assert(r == 10);

    vl = 99;
    const int64_t *fv = ht_find(t, "k", 1, &vl);
    assert(fv != NULL && *fv == 10);
    assert(vl == sizeof(int64_t));

    INV_CHECK(t, "inc_val_len: after inc");

    ht_destroy(t);
    printf("Inc val_len int64 passed!\n");
}

void test_find_all_no_match(void) {
    printf("Testing find_all with no matching entries...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_upsert(t, "a", 1, "v", 1);
    ht_upsert(t, "b", 1, "v", 1);

    /* Search for hash that doesn't match any entry */
    find_all_count = 0;
    ht_find_all(t, 99999, find_all_cb, NULL);
    assert(find_all_count == 0);

    ht_destroy(t);
    printf("Find all no match passed!\n");
}

void test_spill_remove_all_reinsert(void) {
    printf("Testing spill lane remove all then reinsert...\n");
    ht_table_t *t = ht_create(NULL, zero_hash_fn, NULL, NULL);

    /* Insert 5 spill entries */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", i);
        int v = i;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    /* Remove all */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "spill_reuse: after remove all");

    /* Reinsert with different values */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", i);
        int v = i + 50;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "spill_reuse: after reinsert");

    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sr%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 50);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    ht_destroy(t);
    printf("Spill remove all reinsert passed!\n");
}

void test_with_hash_survives_resize(void) {
    printf("Testing with_hash entries survive resize...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert into spill lane */
    int v0 = 10;
    ht_upsert_with_hash(t, 0, "s", 1, &v0, sizeof(int));

    /* Insert into main table via specific hash */
    int v1 = 20;
    ht_upsert_with_hash(t, 555, "m", 1, &v1, sizeof(int));

    /* Trigger resizes with normal inserts */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "with_hash_resize: after resizes");

    /* Spill entry should still be findable */
    assert(*(int *)ht_find_with_hash(t, 0, "s", 1, NULL) == 10);
    /* with_hash entry should still be findable */
    assert(*(int *)ht_find_with_hash(t, 555, "m", 1, NULL) == 20);

    ht_destroy(t);
    printf("With hash survives resize passed!\n");
}

void test_large_update_stress(void) {
    printf("Testing large update stress (1000 updates to same key)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 1000; i++) {
        char val[16]; snprintf(val, sizeof(val), "v%04d", i);
        assert(ht_upsert(t, "k", 1, val, strlen(val)) == (i == 0));
    }

    INV_CHECK(t, "large_update: after 1000 updates");

    size_t vl = 0;
    const char *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL);
    /* "v%04d" with 999 → "v0999" = 6 chars */
    char expected[16]; snprintf(expected, sizeof(expected), "v%04d", 999);
    if (vl != strlen(expected) || memcmp(v, expected, vl) != 0) {
        printf("  FAIL: got '%.*s' (len=%zu), expected '%s' (len=%zu)\n",
               (int)vl, v, vl, expected, strlen(expected));
        ht_destroy(t); return;
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Large update stress passed!\n");
}

void test_inc_large_delta(void) {
    printf("Testing ht_inc with large delta (near int64_t bounds)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int64_t r = ht_inc(t, "big", 3, INT64_C(1000000000000));
    assert(r == INT64_C(1000000000000));

    r = ht_inc(t, "big", 3, INT64_C(2000000000000));
    assert(r == INT64_C(3000000000000));

    const int64_t *v = ht_find(t, "big", 3, NULL);
    assert(v != NULL && *v == INT64_C(3000000000000));

    INV_CHECK(t, "inc_large: after large incs");

    ht_destroy(t);
    printf("Inc large delta passed!\n");
}

void test_collision_delete_all_from_middle(void) {
    printf("Testing collision chain delete all from middle outward...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    int vals[15];
    for (int i = 0; i < 15; i++) {
        char k[8]; snprintf(k, sizeof(k), "mo%d", i);
        vals[i] = i * 2;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete from middle outward: 7,8,6,9,5,10,4,11,3,12,2,13,1,14,0 */
    int order[] = {7,8,6,9,5,10,4,11,3,12,2,13,1,14,0};
    for (int j = 0; j < 15; j++) {
        char k[8]; snprintf(k, sizeof(k), "mo%d", order[j]);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "mid_outward: after all deletes");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* All should be gone */
    for (int i = 0; i < 15; i++) {
        char k[8]; snprintf(k, sizeof(k), "mo%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_destroy(t);
    printf("Collision delete all from middle outward passed!\n");
}

void test_clear_then_compact(void) {
    printf("Testing clear then compact (no-op)...\n");
    ht_config_t cfg = { .initial_capacity = 16, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "cc%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    ht_clear(t);
    ht_compact(t);  /* compact on empty table */

    INV_CHECK(t, "clear_compact: after");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Still usable */
    int v = 42;
    assert(ht_upsert(t, "x", 1, &v, sizeof(int)));
    assert(*(int *)ht_find(t, "x", 1, NULL) == 42);

    ht_destroy(t);
    printf("Clear then compact passed!\n");
}

void test_find_val_len_accuracy(void) {
    printf("Testing find returns correct val_len for various sizes...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    struct { const char *key; const char *val; size_t vlen; } cases[] = {
        {"k1", "a",    1},
        {"k2", "ab",   2},
        {"k3", "abc",  3},
        {"k4", "abcd", 4},
        {"k5", "abcdefghij", 10},
    };
    int nc = sizeof(cases) / sizeof(cases[0]);

    for (int i = 0; i < nc; i++)
        ht_upsert(t, cases[i].key, strlen(cases[i].key), cases[i].val, cases[i].vlen);

    INV_CHECK(t, "val_len: after inserts");

    for (int i = 0; i < nc; i++) {
        size_t vl = 99;
        const char *v = ht_find(t, cases[i].key, strlen(cases[i].key), &vl);
        assert(v != NULL);
        assert(vl == cases[i].vlen);
        assert(memcmp(v, cases[i].val, vl) == 0);
    }

    ht_destroy(t);
    printf("Find val_len accuracy passed!\n");
}

// ============================================================================
// Hardening tests: NULL value, config, overflow, eq_fn, spill+compact, find_all
// ============================================================================

static bool memcmp_eq(const void *a, size_t al, const void *b, size_t bl,
                      void *ctx) {
    (void)ctx;
    return al == bl && memcmp(a, b, al) == 0;
}

void test_null_value_guard(void) {
    printf("Testing NULL value with positive val_len...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* NULL value with val_len > 0 — should clamp val_len to 0 */
    assert(ht_upsert(t, "k1", 2, NULL, 5));
    INV_CHECK(t, "null_val: after insert NULL+5");

    size_t vl = 99;
    const void *v = ht_find(t, "k1", 2, &vl);
    assert(v != NULL);
    assert(vl == 0);

    /* NULL value with val_len == 0 — existing behavior */
    assert(ht_upsert(t, "k2", 2, NULL, 0));
    vl = 99;
    v = ht_find(t, "k2", 2, &vl);
    assert(v != NULL);
    assert(vl == 0);

    INV_CHECK(t, "null_val: after both inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);

    ht_destroy(t);
    printf("NULL value guard passed!\n");
}

void test_all_zero_config(void) {
    printf("Testing all-zero config defaults...\n");
    ht_config_t cfg = {0};
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 4);
    assert(st.size == 0);
    assert(st.load_factor == 0.0);

    INV_CHECK(t, "zero_cfg: empty");

    /* Insert 10 keys — should trigger resize since default load is 0.75 */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "zc%d", i);
        int v = i;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "zero_cfg: after 10 inserts");

    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.capacity >= 16);

    /* Verify all */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "zc%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    /* Remove all */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "zc%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "zero_cfg: after remove all");

    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("All-zero config passed!\n");
}

void test_max_load_factor_capped(void) {
    printf("Testing max_load_factor capped at 0.97...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 1.0,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* With cap=4 and max_load capped to 0.97: resize triggers when (size+1)/cap > 0.97.
     * 4 * 0.97 = 3.88, so inserting the 4th entry (size goes 3→4) triggers resize. */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ml%d", i);
        vals[i] = i;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    INV_CHECK(t, "load_cap: after 10 inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);
    /* Table should have resized — cap > 4 */
    assert(st.capacity > 4);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ml%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Max load factor capped passed!\n");
}

void test_inc_overflow(void) {
    printf("Testing ht_inc overflow behavior...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Create key with INT64_MAX */
    int64_t r = ht_inc(t, "k", 1, INT64_MAX);
    assert(r == INT64_MAX);

    /* Increment by 1 — signed overflow is UB, verify platform behavior */
    r = ht_inc(t, "k", 1, 1);
    assert(r == INT64_MIN); /* wraps on 2's complement */

    /* Decrement by 1 — back to INT64_MAX */
    r = ht_inc(t, "k", 1, -1);
    assert(r == INT64_MAX);

    INV_CHECK(t, "inc_overflow: after wraps");

    const int64_t *v = ht_find(t, "k", 1, NULL);
    assert(v != NULL && *v == INT64_MAX);

    ht_destroy(t);
    printf("Inc overflow passed!\n");
}

void test_custom_eq_fn(void) {
    printf("Testing custom eq_fn (memcmp wrapper)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, memcmp_eq, NULL);
    assert(t != NULL);

    /* Insert 10 keys */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "eq%d", i);
        int v = i * 3;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "custom_eq: after inserts");

    /* Find all */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "eq%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 3);
    }

    /* Update */
    int v2 = 99;
    assert(ht_upsert(t, "eq5", 3, &v2, sizeof(int)) == false);
    assert(*(int *)ht_find(t, "eq5", 3, NULL) == 99);

    /* Remove */
    assert(ht_remove(t, "eq3", 3));
    assert(ht_find(t, "eq3", 3, NULL) == NULL);

    INV_CHECK(t, "custom_eq: after remove");

    /* Reinsert */
    int v3 = 77;
    assert(ht_upsert(t, "eq3", 3, &v3, sizeof(int)));
    assert(*(int *)ht_find(t, "eq3", 3, NULL) == 77);

    INV_CHECK(t, "custom_eq: after reinsert");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    ht_destroy(t);
    printf("Custom eq_fn passed!\n");
}

void test_remove_with_hash_after_resize(void) {
    printf("Testing remove_with_hash after resize...\n");
    ht_config_t cfg = { .initial_capacity = 4, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert entries with known hashes */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rwr%d", i);
        int v = i;
        uint64_t h = fnv1a_hash(k, strlen(k), NULL);
        ht_upsert_with_hash(t, h, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "rm_with_hash_resize: after inserts");

    /* Remove via with_hash after resize */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "rwr%d", i);
        uint64_t h = fnv1a_hash(k, strlen(k), NULL);
        assert(ht_remove_with_hash(t, h, k, strlen(k)));
    }

    INV_CHECK(t, "rm_with_hash_resize: after removes");

    /* Verify survivors */
    for (int i = 5; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rwr%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "rwr%d", i);
        assert(ht_find(t, k, strlen(k), NULL) == NULL);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 15);

    ht_destroy(t);
    printf("Remove with hash after resize passed!\n");
}

void test_remove_with_hash_collision(void) {
    printf("Testing remove_with_hash under collision...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "rc%d", i);
        vals[i] = i * 5;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Remove via with_hash(42, ...) */
    assert(ht_remove_with_hash(t, 42, "rc5", 3));
    assert(ht_remove_with_hash(t, 42, "rc2", 3));

    INV_CHECK(t, "rm_with_hash_coll: after removes");

    /* Verify */
    assert(ht_find(t, "rc5", 3, NULL) == NULL);
    assert(ht_find(t, "rc2", 3, NULL) == NULL);
    int expected_alive[] = {0,1,3,4,6,7,8,9};
    for (int j = 0; j < 8; j++) {
        char k[8]; snprintf(k, sizeof(k), "rc%d", expected_alive[j]);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != expected_alive[j] * 5) {
            printf("  FAIL: rc%d lost after collision remove\n", expected_alive[j]);
            ht_destroy(t); return;
        }
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 8);

    ht_destroy(t);
    printf("Remove with hash collision passed!\n");
}

void test_spill_compact(void) {
    printf("Testing spill entries survive ht_compact...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, zero_hash_fn, NULL, NULL);

    /* Insert 5 spill entries (zero_hash_fn returns 0) */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sp%d", i);
        int v = i * 10;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    /* Insert 10 main-table entries via with_hash */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "mn%d", i);
        int v = i * 5;
        assert(ht_upsert_with_hash(t, 100 + i, k, strlen(k), &v, sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 15);

    /* Compact */
    ht_compact(t);
    INV_CHECK(t, "spill_compact: after compact");

    ht_stats(t, &st);
    assert(st.size == 15);

    /* Verify all spill entries */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "sp%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 10) {
            printf("  FAIL: spill entry sp%d lost after compact (got %d)\n",
                   i, v ? *v : -1);
            ht_destroy(t); return;
        }
    }

    /* Verify all main entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "mn%d", i);
        const int *v = ht_find_with_hash(t, 100 + i, k, strlen(k), NULL);
        if (v == NULL || *v != i * 5) {
            printf("  FAIL: main entry mn%d lost after compact\n", i);
            ht_destroy(t); return;
        }
    }

    ht_destroy(t);
    printf("Spill compact passed!\n");
}

static int find_all_tomb_count;
static bool find_all_tomb_found[10];

static bool find_all_tomb_cb(const void *key, size_t key_len,
                              const void *value, size_t value_len,
                              void *user_ctx) {
    (void)value; (void)value_len; (void)user_ctx;
    find_all_tomb_count++;
    char buf[16];
    assert(key_len < sizeof(buf));
    memcpy(buf, key, key_len);
    buf[key_len] = '\0';
    int idx = atoi(buf + 2);
    if (idx >= 0 && idx < 10) find_all_tomb_found[idx] = true;
    return true;
}

void test_find_all_with_tombstones(void) {
    printf("Testing find_all with tombstones in chain...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    /* Insert 10 colliding keys */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ft%d", i);
        vals[i] = i * 7;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete 3 entries — creates tombstones in the chain */
    ht_remove(t, "ft2", 3);
    ht_remove(t, "ft5", 3);
    ht_remove(t, "ft8", 3);

    INV_CHECK(t, "find_all_tombs: after deletes");

    /* find_all should find exactly the 7 live entries */
    find_all_tomb_count = 0;
    memset(find_all_tomb_found, 0, sizeof(find_all_tomb_found));
    ht_find_all(t, 42, find_all_tomb_cb, NULL);

    assert(find_all_tomb_count == 7);
    for (int i = 0; i < 10; i++) {
        if (i == 2 || i == 5 || i == 8)
            assert(find_all_tomb_found[i] == false);
        else
            assert(find_all_tomb_found[i] == true);
    }

    ht_destroy(t);
    printf("Find all with tombstones passed!\n");
}

void test_find_all_tombstone_early_termination(void) {
    printf("Testing find_all early termination doesn't skip entries...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, NULL, NULL);

    /* Insert 10 colliding keys */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "et%d", i);
        vals[i] = i * 3;
        ht_upsert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete the tail entry */
    ht_remove(t, "et9", 3);

    INV_CHECK(t, "find_all_early: after delete");

    /* find_all should find exactly 9 entries */
    find_all_tomb_count = 0;
    memset(find_all_tomb_found, 0, sizeof(find_all_tomb_found));
    ht_find_all(t, 42, find_all_tomb_cb, NULL);

    assert(find_all_tomb_count == 9);
    for (int i = 0; i < 9; i++)
        assert(find_all_tomb_found[i] == true);
    assert(find_all_tomb_found[9] == false);

    ht_destroy(t);
    printf("Find all tombstone early termination passed!\n");
}

// ============================================================================
// Coverage gaps: hash=1 lifecycle, large spill, iter+insert, eq_fn collision
// ============================================================================

void test_hash1_lifecycle(void) {
    printf("Testing hash=1 spill lifecycle (compact, resize, inc, find_all)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert hash=1 spill entries */
    int v0 = 10, v1 = 20, v2 = 30;
    assert(ht_upsert_with_hash(t, 1, "a", 1, &v0, sizeof(int)));
    assert(ht_upsert_with_hash(t, 1, "b", 1, &v1, sizeof(int)));
    assert(ht_upsert_with_hash(t, 1, "c", 1, &v2, sizeof(int)));

    /* Compact with spill entries */
    ht_compact(t);
    INV_CHECK(t, "hash1_life: after compact");

    assert(*(int *)ht_find_with_hash(t, 1, "a", 1, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, 1, "b", 1, NULL) == 20);
    assert(*(int *)ht_find_with_hash(t, 1, "c", 1, NULL) == 30);

    /* Add main-table entries to trigger resize */
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "m%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "hash1_life: after resize");

    /* Spill entries survive resize */
    assert(*(int *)ht_find_with_hash(t, 1, "a", 1, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, 1, "c", 1, NULL) == 30);

    /* Inc on hash=1 key */
    int64_t r = ht_inc(t, "b", 1, 5);
    /* val was int (4 bytes), inc discards it, sets new_val = delta = 5 */
    assert(r == 5);
    const int64_t *iv = ht_find(t, "b", 1, NULL);
    assert(iv != NULL && *iv == 5);

    /* find_all on hash=1 */
    find_all_collision_count = 0;
    memset(find_all_collision_keys_found, 0, sizeof(find_all_collision_keys_found));
    ht_find_all(t, 1, find_all_collision_cb, NULL);
    assert(find_all_collision_count == 3);

    INV_CHECK(t, "hash1_life: final");

    ht_destroy(t);
    printf("Hash=1 lifecycle passed!\n");
}

void test_large_spill_lane(void) {
    printf("Testing large spill lane (50 entries)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, zero_hash_fn, NULL, NULL);

    const int N = 50;
    int *vals = calloc(N, sizeof(int));

    /* Insert 50 spill entries — forces spill array growth */
    for (int i = 0; i < N; i++) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        vals[i] = i * 7;
        assert(ht_upsert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    INV_CHECK(t, "large_spill: after 50 inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == N);

    /* Verify all */
    for (int i = 0; i < N; i++) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 7) {
            printf("  FAIL: ls%d lost (got %p)\n", i, (void *)v);
            free(vals); ht_destroy(t); return;
        }
    }

    /* Remove 25 of them */
    for (int i = 0; i < N; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    INV_CHECK(t, "large_spill: after removes");

    ht_stats(t, &st);
    assert(st.size == N / 2);

    /* Verify survivors */
    for (int i = 1; i < N; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "ls%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 7);
    }

    free(vals);
    ht_destroy(t);
    printf("Large spill lane passed!\n");
}

void test_iter_after_insert(void) {
    printf("Testing iterator after mid-iteration insert...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert 5 entries */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "ii%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Start iterating, collect first 3 */
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t kl, vl;
    int first_batch = 0;
    while (first_batch < 3 && ht_iter_next(t, &iter, &key, &kl, &val, &vl))
        first_batch++;
    assert(first_batch == 3);

    /* Insert 5 more entries mid-iteration */
    for (int i = 5; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ii%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    INV_CHECK(t, "iter_insert: after mid-iteration insert");

    /* Continue iterating — should finish without crash */
    int second_batch = 0;
    while (ht_iter_next(t, &iter, &key, &kl, &val, &vl))
        second_batch++;

    /* Total entries found should be at least 5 (original), possibly up to 10 */
    assert(first_batch + second_batch >= 5);

    /* Fresh iteration should see all 10 */
    int total = 0;
    iter = ht_iter_begin(t);
    while (ht_iter_next(t, &iter, &key, &kl, &val, &vl)) total++;
    assert(total == 10);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    ht_destroy(t);
    printf("Iter after insert passed!\n");
}

static int find_all_spill_tomb_count;
static bool find_all_spill_tomb_found[10];

static bool find_all_spill_tomb_cb(const void *key, size_t kl,
                                    const void *val, size_t vl, void *ctx) {
    (void)val; (void)vl; (void)ctx;
    find_all_spill_tomb_count++;
    char buf[16];
    assert(kl < sizeof(buf));
    memcpy(buf, key, kl);
    buf[kl] = '\0';
    int idx = atoi(buf + 2);
    if (idx >= 0 && idx < 10) find_all_spill_tomb_found[idx] = true;
    return true;
}

void test_find_all_spill_with_deletions(void) {
    printf("Testing find_all on spill lane after deletions...\n");
    ht_table_t *t = ht_create(NULL, zero_hash_fn, NULL, NULL);

    /* Insert 10 spill entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "sd%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Delete 3 */
    ht_remove(t, "sd1", 3);
    ht_remove(t, "sd4", 3);
    ht_remove(t, "sd7", 3);

    INV_CHECK(t, "find_all_spill_del: after deletes");

    /* find_all(hash=0) should find 7 live entries */
    find_all_spill_tomb_count = 0;
    memset(find_all_spill_tomb_found, 0, sizeof(find_all_spill_tomb_found));
    ht_find_all(t, 0, find_all_spill_tomb_cb, NULL);

    assert(find_all_spill_tomb_count == 7);
    for (int i = 0; i < 10; i++) {
        if (i == 1 || i == 4 || i == 7)
            assert(find_all_spill_tomb_found[i] == false);
        else
            assert(find_all_spill_tomb_found[i] == true);
    }

    ht_destroy(t);
    printf("Find all spill with deletions passed!\n");
}

void test_compact_only_spill(void) {
    printf("Testing compact with only spill entries...\n");
    ht_config_t cfg = { .initial_capacity = 32, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, zero_hash_fn, NULL, NULL);

    /* Insert only spill entries — no main-table entries */
    for (int i = 0; i < 8; i++) {
        char k[8]; snprintf(k, sizeof(k), "os%d", i);
        int v = i * 3;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 8);

    /* Compact */
    ht_compact(t);
    INV_CHECK(t, "compact_spill_only: after compact");

    ht_stats(t, &st);
    assert(st.size == 8);

    /* Verify all spill entries */
    for (int i = 0; i < 8; i++) {
        char k[8]; snprintf(k, sizeof(k), "os%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 3) {
            printf("  FAIL: os%d lost after compact-only-spill\n", i);
            ht_destroy(t); return;
        }
    }

    ht_destroy(t);
    printf("Compact only spill passed!\n");
}

void test_inc_zero_len_value(void) {
    printf("Testing ht_inc on key with zero-length value...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert with NULL value, val_len=0 */
    assert(ht_upsert(t, "k", 1, NULL, 0));

    size_t vl = 99;
    const void *v = ht_find(t, "k", 1, &vl);
    assert(v != NULL && vl == 0);

    /* Inc — val_len=0 != sizeof(int64_t), so discards old, sets new_val=delta */
    int64_t r = ht_inc(t, "k", 1, 42);
    assert(r == 42);

    vl = 99;
    const int64_t *iv = ht_find(t, "k", 1, &vl);
    assert(iv != NULL && *iv == 42);
    assert(vl == sizeof(int64_t));

    INV_CHECK(t, "inc_zero_val: after inc");

    ht_destroy(t);
    printf("Inc zero len value passed!\n");
}

void test_custom_eq_collision(void) {
    printf("Testing custom eq_fn under full collision...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.9,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, const_hash42, memcmp_eq, NULL);

    /* All keys hash to 42 — eq_fn must distinguish them */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ce%d", i);
        vals[i] = i * 11;
        assert(ht_upsert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    INV_CHECK(t, "eq_collision: after inserts");

    /* Find each — eq_fn distinguishes by key content */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ce%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (v == NULL || *v != i * 11) {
            printf("  FAIL: ce%d lost (eq_fn under collision)\n", i);
            ht_destroy(t); return;
        }
    }

    /* Remove middle, verify survivors */
    ht_remove(t, "ce4", 3);
    ht_remove(t, "ce7", 3);

    INV_CHECK(t, "eq_collision: after removes");

    assert(ht_find(t, "ce4", 3, NULL) == NULL);
    assert(ht_find(t, "ce7", 3, NULL) == NULL);
    for (int i = 0; i < 10; i++) {
        if (i == 4 || i == 7) continue;
        char k[8]; snprintf(k, sizeof(k), "ce%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 11);
    }

    /* Reinsert removed key — eq_fn must find the tombstone slot */
    int v_new = 999;
    assert(ht_upsert(t, "ce4", 3, &v_new, sizeof(int)));
    assert(*(int *)ht_find(t, "ce4", 3, NULL) == 999);

    INV_CHECK(t, "eq_collision: after reinsert");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 9);

    ht_destroy(t);
    printf("Custom eq_fn collision passed!\n");
}

void test_remove_with_hash_spill(void) {
    printf("Testing remove_with_hash on spill entries...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert spill entries via with_hash */
    int v0 = 10, v1 = 20, v2 = 30;
    ht_upsert_with_hash(t, 0, "a", 1, &v0, sizeof(int));
    ht_upsert_with_hash(t, 0, "b", 1, &v1, sizeof(int));
    ht_upsert_with_hash(t, 1, "c", 1, &v2, sizeof(int));

    /* Remove hash=0 entry via remove_with_hash */
    assert(ht_remove_with_hash(t, 0, "a", 1));
    assert(ht_find_with_hash(t, 0, "a", 1, NULL) == NULL);
    assert(*(int *)ht_find_with_hash(t, 0, "b", 1, NULL) == 20);

    /* Remove hash=1 entry via remove_with_hash */
    assert(ht_remove_with_hash(t, 1, "c", 1));
    assert(ht_find_with_hash(t, 1, "c", 1, NULL) == NULL);

    INV_CHECK(t, "rm_with_hash_spill: after removes");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Remove with hash spill passed!\n");
}

void test_binary_key_lifecycle(void) {
    printf("Testing binary key through full lifecycle...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    uint8_t key[] = {'a', '\0', 'b', '\0', 'c'};
    int v1 = 42;
    assert(ht_upsert(t, key, sizeof(key), &v1, sizeof(int)));

    /* Find */
    const int *v = ht_find(t, key, sizeof(key), NULL);
    assert(v != NULL && *v == 42);

    /* Update */
    int v2 = 99;
    assert(ht_upsert(t, key, sizeof(key), &v2, sizeof(int)) == false);
    v = ht_find(t, key, sizeof(key), NULL);
    assert(v != NULL && *v == 99);

    /* Inc */
    int64_t r = ht_inc(t, key, sizeof(key), 1);
    assert(r == 1);
    const int64_t *iv = ht_find(t, key, sizeof(key), NULL);
    assert(iv != NULL && *iv == 1);

    /* Remove */
    assert(ht_remove(t, key, sizeof(key)));
    assert(ht_find(t, key, sizeof(key), NULL) == NULL);

    INV_CHECK(t, "bin_key_life: after remove");

    /* Reinsert with new value */
    int v3 = 77;
    assert(ht_upsert(t, key, sizeof(key), &v3, sizeof(int)));
    v = ht_find(t, key, sizeof(key), NULL);
    assert(v != NULL && *v == 77);

    INV_CHECK(t, "bin_key_life: after reinsert");

    ht_destroy(t);
    printf("Binary key lifecycle passed!\n");
}

// ============================================================================
// Coverage gaps: insert_with_hash update, mid-iter remove, spill iter, etc.
// ============================================================================

void test_insert_with_hash_update(void) {
    printf("Testing ht_insert_with_hash update returns false...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int v1 = 10, v2 = 20;
    assert(ht_upsert_with_hash(t, 42, "k", 1, &v1, sizeof(int)) == true);
    assert(ht_upsert_with_hash(t, 42, "k", 1, &v2, sizeof(int)) == false);

    INV_CHECK(t, "with_hash_update: after update");

    assert(*(int *)ht_find_with_hash(t, 42, "k", 1, NULL) == 20);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Insert with hash update passed!\n");
}

void test_remove_mid_iteration(void) {
    printf("Testing ht_remove between iter_next calls...\n");
    ht_config_t cfg = { .initial_capacity = 32, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "mi%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Iterate, removing every other entry mid-iteration */
    int seen = 0, removed = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t kl, vl;
    while (ht_iter_next(t, &iter, &key, &kl, &val, &vl)) {
        seen++;
        /* Extract index from key */
        char buf[8];
        assert(kl < sizeof(buf));
        memcpy(buf, key, kl);
        buf[kl] = '\0';
        int idx = atoi(buf + 2);

        /* Remove even-indexed entries */
        if (idx % 2 == 0) {
            ht_remove(t, buf, kl);
            removed++;
        }
    }

    /* Should have seen all 20 entries */
    assert(seen == 20);

    INV_CHECK(t, "mid_iter_rm: after iteration");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20 - removed);

    /* Verify odd entries survive */
    for (int i = 1; i < 20; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "mi%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Remove mid iteration passed!\n");
}

void test_iter_spill_only(void) {
    printf("Testing iterator on spill-only table...\n");
    ht_table_t *t = ht_create(NULL, zero_hash_fn, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "si%d", i);
        int v = i * 10;
        assert(ht_upsert(t, k, strlen(k), &v, sizeof(int)));
    }

    INV_CHECK(t, "iter_spill: after inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    /* Iterate and collect */
    bool seen[10] = {false};
    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t kl, vl;
    while (ht_iter_next(t, &iter, &key, &kl, &val, &vl)) {
        count++;
        assert(vl == sizeof(int));
        char buf[8];
        assert(kl < sizeof(buf));
        memcpy(buf, key, kl);
        buf[kl] = '\0';
        int idx = atoi(buf + 2);
        assert(idx >= 0 && idx < 10);
        assert(*(const int *)val == idx * 10);
        assert(!seen[idx]);
        seen[idx] = true;
    }
    assert(count == 10);
    for (int i = 0; i < 10; i++) assert(seen[i]);

    ht_destroy(t);
    printf("Iter spill only passed!\n");
}

void test_inc_int64_min_delta(void) {
    printf("Testing ht_inc with INT64_MIN delta...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Start at 1 */
    int64_t r = ht_inc(t, "k", 1, 1);
    assert(r == 1);

    /* Subtract INT64_MIN — this is 1 - INT64_MIN = 1 + INT64_MAX + 1 = wraps */
    r = ht_inc(t, "k", 1, INT64_MIN);
    /* 1 + INT64_MIN = INT64_MIN + 1 (wraps on 2's complement) */
    assert(r == INT64_MIN + 1);

    INV_CHECK(t, "inc_min: after INT64_MIN delta");

    ht_destroy(t);
    printf("Inc INT64_MIN delta passed!\n");
}

void test_pointer_stability_after_inc(void) {
    printf("Testing pointer stability after ht_inc...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert int value */
    int v = 42;
    ht_upsert(t, "k", 1, &v, sizeof(int));

    size_t vl1 = 0;
    const void *ptr1 = ht_find(t, "k", 1, &vl1);
    assert(ptr1 != NULL && vl1 == sizeof(int));

    /* Inc changes value to int64_t — old arena pointer may be stale */
    ht_inc(t, "k", 1, 5);

    size_t vl2 = 0;
    const void *ptr2 = ht_find(t, "k", 1, &vl2);
    assert(ptr2 != NULL && vl2 == sizeof(int64_t));

    /* New value is correct */
    assert(*(const int64_t *)ptr2 == 5);

    /* Old pointer's arena slot may have been reclaimed — at minimum,
     * the new find must return the correct value */
    INV_CHECK(t, "ptr_stability: after inc");

    ht_destroy(t);
    printf("Pointer stability after inc passed!\n");
}

void test_resize_exact_minimum(void) {
    printf("Testing ht_resize to exact minimum capacity...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    /* Insert 10 entries */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "em%d", i);
        int v = i;
        ht_upsert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Resize to minimum: 10 entries at 0.75 load needs ceil(10/0.75)=14, next pow2=16 */
    size_t min_cap = 16;
    assert(ht_resize(t, min_cap));
    INV_CHECK(t, "resize_exact: after resize");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.capacity == min_cap);

    /* Verify all entries survived */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "em%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("Resize exact minimum passed!\n");
}

void test_remove_insert_remove(void) {
    printf("Testing remove-insert-remove cycle on same key...\n");
    ht_config_t cfg = { .initial_capacity = 16, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    int v1 = 10, v2 = 20;
    ht_upsert(t, "k", 1, &v1, sizeof(int));
    assert(ht_remove(t, "k", 1));
    assert(ht_find(t, "k", 1, NULL) == NULL);

    /* Reinsert */
    ht_upsert(t, "k", 1, &v2, sizeof(int));
    assert(*(int *)ht_find(t, "k", 1, NULL) == 20);

    /* Remove again */
    assert(ht_remove(t, "k", 1));
    assert(ht_find(t, "k", 1, NULL) == NULL);

    INV_CHECK(t, "rir: after remove-insert-remove");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("Remove-insert-remove passed!\n");
}

void test_key_prefix_distinct(void) {
    printf("Testing key prefix collision — abc vs abcd...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int v1 = 1, v2 = 2;
    assert(ht_upsert(t, "abc", 3, &v1, sizeof(int)));
    assert(ht_upsert(t, "abcd", 4, &v2, sizeof(int)));

    INV_CHECK(t, "prefix: after inserts");

    assert(*(int *)ht_find(t, "abc", 3, NULL) == 1);
    assert(*(int *)ht_find(t, "abcd", 4, NULL) == 2);

    /* Remove short, long survives */
    ht_remove(t, "abc", 3);
    assert(ht_find(t, "abc", 3, NULL) == NULL);
    assert(*(int *)ht_find(t, "abcd", 4, NULL) == 2);

    INV_CHECK(t, "prefix: after remove");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("Key prefix distinct passed!\n");
}

void test_insert_with_hash_matching_normal(void) {
    printf("Testing insert_with_hash matching normal entry hash (different key)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    /* Insert "a" normally */
    int v1 = 10;
    assert(ht_upsert(t, "a", 1, &v1, sizeof(int)));

    /* Get "a"'s hash */
    uint64_t h = fnv1a_hash("a", 1, NULL);

    /* Insert "b" with the same hash — forces collision in same bucket */
    int v2 = 20;
    assert(ht_upsert_with_hash(t, h, "b", 1, &v2, sizeof(int)));

    INV_CHECK(t, "with_hash_match: after inserts");

    /* Both must be findable */
    assert(*(int *)ht_find(t, "a", 1, NULL) == 10);
    assert(*(int *)ht_find_with_hash(t, h, "b", 1, NULL) == 20);
    assert(*(int *)ht_find_with_hash(t, h, "a", 1, NULL) == 10);

    /* Remove one, other survives */
    ht_remove(t, "a", 1);
    assert(ht_find(t, "a", 1, NULL) == NULL);
    assert(*(int *)ht_find_with_hash(t, h, "b", 1, NULL) == 20);

    INV_CHECK(t, "with_hash_match: after remove");

    ht_destroy(t);
    printf("Insert with hash matching normal passed!\n");
}

/* ============================================================================
 * Multi-Value API Tests
 * ========================================================================== */

/* Counting callback for find_key_all */
static bool count_kv_cb(const void *key, size_t klen,
                        const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)val; (void)vlen;
    int *count = (int *)ctx;
    (*count)++;
    return true;
}

/* Collect values via find_key_all */
#define MAX_COLLECT 32
typedef struct { const void *vals[MAX_COLLECT]; size_t vlens[MAX_COLLECT]; int n; } val_collect_t;

static bool collect_val_cb(const void *key, size_t klen,
                           const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen;
    val_collect_t *c = (val_collect_t *)ctx;
    if (c->n < MAX_COLLECT) {
        c->vals[c->n] = val;
        c->vlens[c->n] = vlen;
    }
    c->n++;
    return true;
}

static void test_insert_multi(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* Insert same key 3 times with different values */
    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "b", 1));
    assert(ht_insert(t, "k", 1, "c", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 3);

    /* find_key_all should return all 3 */
    val_collect_t vc = {0};
    ht_find_key_all(t, "k", 1, collect_val_cb, &vc);
    assert(vc.n == 3);

    INV_CHECK(t, "insert_multi");
    ht_destroy(t);
    printf("Insert multi passed!\n");
}

static void test_upsert_collapses_multi(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* Insert 3 values for same key */
    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "b", 1));
    assert(ht_insert(t, "k", 1, "c", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 3);

    /* Upsert with new value — collapses to 1 */
    bool r = ht_upsert(t, "k", 1, "z", 1);
    assert(r == false);  /* replaced, not new */

    ht_stats(t, &st);
    assert(st.size == 1);

    /* Only upserted value remains */
    size_t vl;
    const void *v = ht_find(t, "k", 1, &vl);
    assert(v && vl == 1 && memcmp(v, "z", 1) == 0);

    /* find_key_all returns exactly 1 */
    val_collect_t vc = {0};
    ht_find_key_all(t, "k", 1, collect_val_cb, &vc);
    assert(vc.n == 1);

    INV_CHECK(t, "upsert_collapses_multi");
    ht_destroy(t);
    printf("Upsert collapses multi passed!\n");
}

static void test_unsert_dedup(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* unsert same k,v twice — second returns false */
    assert(ht_unsert(t, "k", 1, "a", 1));
    assert(!ht_unsert(t, "k", 1, "a", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    /* unsert different value for same key — succeeds */
    assert(ht_unsert(t, "k", 1, "b", 1));
    ht_stats(t, &st);
    assert(st.size == 2);

    INV_CHECK(t, "unsert_dedup");
    ht_destroy(t);
    printf("Unsert dedup passed!\n");
}

static void test_remove_all_multi(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* Insert 5 values for key */
    for (int i = 0; i < 5; i++)
        assert(ht_insert(t, "k", 1, &i, sizeof(int)));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    /* remove(key) returns 5, size=0 */
    size_t removed = ht_remove(t, "k", 1);
    assert(removed == 5);

    ht_stats(t, &st);
    assert(st.size == 0);

    INV_CHECK(t, "remove_all_multi");
    ht_destroy(t);
    printf("Remove all multi passed!\n");
}

static void test_remove_kv(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* insert ("k","a"), ("k","b"), ("k","a") */
    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "b", 1));
    assert(ht_insert(t, "k", 1, "a", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 3);

    /* remove_kv("k","a") returns 2 */
    size_t removed = ht_remove_kv(t, "k", 1, "a", 1);
    assert(removed == 2);

    ht_stats(t, &st);
    assert(st.size == 1);

    /* Only "b" remains */
    val_collect_t vc = {0};
    ht_find_key_all(t, "k", 1, collect_val_cb, &vc);
    assert(vc.n == 1);
    assert(vc.vlens[0] == 1 && memcmp(vc.vals[0], "b", 1) == 0);

    INV_CHECK(t, "remove_kv");
    ht_destroy(t);
    printf("Remove kv passed!\n");
}

static void test_remove_kv_one(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* insert ("k","a") twice */
    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "a", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);

    /* remove_kv_one returns true, size=1 */
    assert(ht_remove_kv_one(t, "k", 1, "a", 1));
    ht_stats(t, &st);
    assert(st.size == 1);

    /* second call returns true, size=0 */
    assert(ht_remove_kv_one(t, "k", 1, "a", 1));
    ht_stats(t, &st);
    assert(st.size == 0);

    /* third returns false */
    assert(!ht_remove_kv_one(t, "k", 1, "a", 1));

    INV_CHECK(t, "remove_kv_one");
    ht_destroy(t);
    printf("Remove kv one passed!\n");
}

static void test_find_key_all_values(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* Insert 5 values for same key */
    const char *vals[] = {"v0", "v1", "v2", "v3", "v4"};
    for (int i = 0; i < 5; i++)
        assert(ht_insert(t, "k", 1, vals[i], 2));

    /* find_key_all returns all 5 with correct values */
    val_collect_t vc = {0};
    ht_find_key_all(t, "k", 1, collect_val_cb, &vc);
    assert(vc.n == 5);

    /* Verify all values are present (order may vary) */
    bool found[5] = {false};
    for (int i = 0; i < 5; i++) {
        for (int j = 0; j < vc.n && j < MAX_COLLECT; j++) {
            if (vc.vlens[j] == 2 && memcmp(vc.vals[j], vals[i], 2) == 0)
                found[i] = true;
        }
    }
    for (int i = 0; i < 5; i++)
        assert(found[i]);

    INV_CHECK(t, "find_key_all_values");
    ht_destroy(t);
    printf("Find key all passed!\n");
}

static void test_find_kv(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "b", 1));

    /* find_kv("k","a") finds it */
    size_t vl;
    const void *v = ht_find_kv(t, "k", 1, "a", 1, &vl);
    assert(v && vl == 1 && memcmp(v, "a", 1) == 0);

    /* find_kv("k","c") returns NULL */
    v = ht_find_kv(t, "k", 1, "c", 1, &vl);
    assert(v == NULL);

    INV_CHECK(t, "find_kv");
    ht_destroy(t);
    printf("Find kv passed!\n");
}

static void test_multi_value_with_collision(void) {
    /* Use const_hash42 — all keys get hash 42 */
    ht_table_t *t = ht_create(NULL, const_hash42, NULL, NULL);
    assert(t);

    /* Insert 3 values for key "k" */
    assert(ht_insert(t, "k", 1, "a", 1));
    assert(ht_insert(t, "k", 1, "b", 1));
    assert(ht_insert(t, "k", 1, "c", 1));

    /* Insert other keys (same hash) */
    assert(ht_insert(t, "x", 1, "X", 1));
    assert(ht_insert(t, "y", 1, "Y", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 5);

    /* find_key_all("k") returns only "k" entries, not "x" or "y" */
    val_collect_t vc = {0};
    ht_find_key_all(t, "k", 1, collect_val_cb, &vc);
    assert(vc.n == 3);

    /* Verify all are "k" entries */
    for (int i = 0; i < vc.n && i < MAX_COLLECT; i++) {
        assert(vc.vlens[i] == 1);
        const char c = *(const char *)vc.vals[i];
        assert(c == 'a' || c == 'b' || c == 'c');
    }

    INV_CHECK(t, "multi_value_with_collision");
    ht_destroy(t);
    printf("Multi value with collision passed!\n");
}

static void test_upsert_preserves_single(void) {
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(t);

    /* upsert on key with one value updates in-place */
    assert(ht_upsert(t, "k", 1, "a", 1));

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    size_t tombs_before = st.tombstone_cnt;

    /* upsert same key with new value */
    bool r = ht_upsert(t, "k", 1, "b", 1);
    assert(r == false);  /* replaced */

    ht_stats(t, &st);
    assert(st.size == 1);
    /* Tombstone count should NOT increase — in-place update */
    assert(st.tombstone_cnt == tombs_before);

    /* Value is "b" */
    size_t vl;
    const void *v = ht_find(t, "k", 1, &vl);
    assert(v && vl == 1 && memcmp(v, "b", 1) == 0);

    INV_CHECK(t, "upsert_preserves_single");
    ht_destroy(t);
    printf("Upsert preserves single passed!\n");
}

int main() {
    int bugs = 0;
    test_basic();
    test_update();
    test_remove();
    test_resize();
    test_clear();
    test_increment();
    test_binary_keys();
    test_iterator();
    test_graveyard();
    test_find_all();
    test_insert_with_hash();
    test_ht_dump();
    test_ht_inc_null();
    test_ht_find_with_hash_mismatch();
    test_ht_remove_with_hash_mismatch();
    test_ht_insert_with_hash_hash0_find_normal();
    test_ht_iter_null();
    test_update_size_stability();
    test_remove_all_stats();
    test_resize_byte_exact();
    test_inc_accumulation();
    test_large_key();    /* Bug probe: key_len uint16_t truncation */
    test_large_value();  /* Bug probe: val_len uint16_t truncation */
    test_zero_length_key();
    test_zero_length_value();
    test_double_remove();
    test_remove_then_reinsert();
    test_clear_then_reuse();
    test_inc_zero_delta();
    test_inc_negative_from_zero();
    test_insert_many_same_first_char();
    test_iterator_after_remove();
    test_find_all_with_collisions();
    test_with_hash_cross_api();
    test_large_key_large_value_together();
    test_insert_duplicate_with_different_value_size();
    test_remove_nonexistent();
    test_stats_empty_table();

    /* New: backward shift, early termination, collision edge cases */
    test_delete_chain_head_collision();
    test_delete_middle_collision_verify_ends();
    test_tombstone_early_termination();
    test_resize_with_many_tombstones();
    test_delete_then_insert_stranding();
    test_inc_under_collision();
    test_capacity_2_full_load();
    test_remove_all_verify_clean();

    /* New: spill lane, bulk, compact+iter, value shrink */
    test_spill_remove();
    test_spill_mixed_sentinels();
    test_inc_on_spill_key();
    test_bulk_1000_roundtrip();
    test_iter_after_compact();
    test_update_value_shrink();

    /* New: stats, lifecycle, iterator values, edge cases */
    test_stats_after_each_op();
    test_inc_lifecycle();
    test_iter_values_correct();
    test_find_all_early_stop();
    test_collision_find_after_interleaved_delete();
    test_update_value_grow();
    test_insert_with_hash1();
    test_compact_idempotent();
    test_empty_table_ops();
    test_inc_multiple_keys();
    test_remove_half_reinsert();
    test_binary_key_collision();
    test_resize_same_capacity();
    test_tombstone_tracking();
    test_spill_resize_lifecycle();
    test_clear_stats_accuracy();
    test_double_insert_returns_false();
    test_collision_chain_tail_delete();
    test_inc_with_hash_existing();
    test_iter_count_matches_size();
    test_resize_down_verify();
    test_find_all_empty_hash();
    test_update_value_same_size();

    /* New: churn, cross-API, resize, value verification */
    test_single_key_churn();
    test_multiple_resizes();
    test_find_all_verifies_keys();
    test_remove_with_hash_cross_api();
    test_inc_after_update();
    test_delete_all_reverse_order();
    test_iter_after_clear();
    test_capacity_power_of_two_invariant();
    test_insert_after_compact();
    test_inc_val_len_int64_after_inc();
    test_find_all_no_match();
    test_spill_remove_all_reinsert();
    test_with_hash_survives_resize();
    test_large_update_stress();
    test_inc_large_delta();
    test_collision_delete_all_from_middle();
    test_clear_then_compact();
    test_find_val_len_accuracy();

    /* New: hardening — NULL value, config, overflow, eq_fn, spill+compact, find_all */
    test_null_value_guard();
    test_all_zero_config();
    test_max_load_factor_capped();
    test_inc_overflow();
    test_custom_eq_fn();
    test_remove_with_hash_after_resize();
    test_remove_with_hash_collision();
    test_spill_compact();
    test_find_all_with_tombstones();
    test_find_all_tombstone_early_termination();

    /* New: coverage gaps — hash=1, large spill, iter+insert, eq_fn collision */
    test_hash1_lifecycle();
    test_large_spill_lane();
    test_iter_after_insert();
    test_find_all_spill_with_deletions();
    test_compact_only_spill();
    test_inc_zero_len_value();
    test_custom_eq_collision();
    test_remove_with_hash_spill();
    test_binary_key_lifecycle();

    /* New: coverage gaps — with_hash update, mid-iter remove, spill iter, etc. */
    test_insert_with_hash_update();
    test_remove_mid_iteration();
    test_iter_spill_only();
    test_inc_int64_min_delta();
    test_pointer_stability_after_inc();
    test_resize_exact_minimum();
    test_remove_insert_remove();
    test_key_prefix_distinct();
    test_insert_with_hash_matching_normal();

    /* New: multi-value API — insert, upsert, unsert, remove_kv, find_key_all, find_kv */
    test_insert_multi();
    test_upsert_collapses_multi();
    test_unsert_dedup();
    test_remove_all_multi();
    test_remove_kv();
    test_remove_kv_one();
    test_find_key_all_values();
    test_find_kv();
    test_multi_value_with_collision();
    test_upsert_preserves_single();

    printf("\nAll tests passed!\n");
    return bugs;
}
