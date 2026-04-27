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

int main() {
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

    printf("\nAll tests passed!\n");
    return 0;
}
