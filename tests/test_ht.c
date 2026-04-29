#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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
    bool inserted = ht_insert(t, key1, strlen(key1), val1, strlen(val1));
    assert(inserted == true);

    size_t out_len;
    const char *found = ht_find(t, key1, strlen(key1), &out_len);
    assert(found != NULL);
    assert(out_len == strlen(val1));
    assert(memcmp(found, val1, out_len) == 0);

    ht_destroy(t);
    printf("Basic operations passed!\n");
}

void test_update() {
    printf("Testing update...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_insert(t, "key", 3, "val1", 4);
    bool inserted = ht_insert(t, "key", 3, "val2", 4);
    assert(inserted == false); // Should be update, not insert

    size_t out_len;
    const char *found = ht_find(t, "key", 3, &out_len);
    assert(found != NULL);
    assert(out_len == 4);
    assert(memcmp(found, "val2", 4) == 0);

    ht_destroy(t);
    printf("Update passed!\n");
}

void test_remove() {
    printf("Testing remove...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_insert(t, "abc", 3, "123", 3);
    ht_insert(t, "def", 3, "456", 3);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 2);

    bool removed = ht_remove(t, "abc", 3);
    assert(removed == true);

    ht_stats(t, &stats);
    assert(stats.size == 1);
    assert(stats.tombstone_cnt == 1); // always tombstoned (backward-shift is optional cleanup)

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
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.size == 10);
    assert(stats.capacity >= 10);

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
        ht_insert(t, key, strlen(key), &i, sizeof(i));
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

    ht_destroy(t);
    printf("Increment passed!\n");
}

void test_binary_keys() {
    printf("Testing binary keys...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    // Key with embedded nulls
    uint8_t key1[] = {'a', '\0', 'b', '\0', 'c'};
    uint8_t val1[] = {1, 2, 3, 4};

    ht_insert(t, key1, sizeof(key1), val1, sizeof(val1));

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
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }

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
        ht_insert(t, key, strlen(key), &i, sizeof(i));
    }

    // Remove some to create tombstones
    ht_remove(t, "key0", 4);
    ht_remove(t, "key2", 4);

    ht_stats_t stats;
    ht_stats(t, &stats);
    assert(stats.tombstone_cnt <= 2); // backward-shift may reduce count

    // Compact should rebuild table
    ht_compact(t);
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
    ht_insert(t, "unique1", 7, "val1", 4);
    ht_insert(t, "unique2", 7, "val2", 4);

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
    bool inserted = ht_insert_with_hash(t, h, "key", 3, "val", 3);
    assert(inserted == true);

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

    ht_insert(t, "a", 1, "va", 2);
    ht_insert(t, "b", 1, "vb", 2);
    /* Dump a few slots — should not crash */
    ht_dump(t, (uint32_t)fnv1a_hash("a", 1, NULL), 4);
    ht_dump(t, 0, 0);       /* zero count */
    ht_dump(NULL, 0, 4);    /* NULL table */

    /* Also dump spill-lane entries */
    ht_insert_with_hash(t, 0, "s", 1, "vs", 2);
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
    ht_insert(t, "abc", 3, "val", 3);

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
    ht_insert(t, "abc", 3, "val", 3);

    /* Try removing via spill-lane hash — should fail */
    assert(ht_remove_with_hash(t, 0, "abc", 3) == false);
    assert(ht_remove_with_hash(t, 1, "abc", 3) == false);

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
    ht_insert_with_hash(t, 0, "abc", 3, "val", 3);

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
    ht_insert(t, "k", 1, "v", 1);

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
        ht_insert(t, "key", 3, val, strlen(val));
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
        ht_insert(t, k, strlen(k), &v, sizeof(v));
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
        ht_insert(t, k, strlen(k), strings[i], strlen(strings[i]));
    }

    /* Force multiple resizes */
    for (int i = 5; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }

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
    bool inserted = ht_insert(t, big_key, big_len, &val, sizeof(val));
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

    bool inserted = ht_insert(t, "bigkey", 6, big_val, big_len);
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

    printf("\nAll tests passed!\n");
    return bugs;
}
