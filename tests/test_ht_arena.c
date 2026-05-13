/**
 * test_ht_arena.c — Tests for ht + external arena integration
 *
 * Exercises the ht_create_with_arena() path:
 * - Basic CRUD with arena-backed ht
 * - arena_contains for kv-ptrs
 * - Resize/compact with external arena
 * - Destroy does NOT free individual kv-ptrs (arena owns them)
 * - Clear calls arena_clear
 * - Upsert with arena (value size change does not free old kv_ptr)
 * - Multi-value insert/remove with arena
 * - Iterator over arena-backed ht
 * - Large number of entries to trigger slab/cold tiers
 */

#include "draugr/ht.h"
#include "draugr/ht_internal.h"
#include "draugr/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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

#define INV_CHECK(t, label) do { \
    const char *_inv_err = ht_check_invariants(t); \
    if (_inv_err) { \
        printf("  INVARIANT BROKEN at %s: %s\n", (label), _inv_err); \
        return; \
    } \
} while (0)

/* ─── Test 1: Basic CRUD ──────────────────────────────────────── */

static void test_arena_basic_crud(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);
    assert(t->allocator == a);

    ht_insert_result_t r = ht_upsert(t, "key1", 4, "val1", 4);
    assert(r == HT_INSERT_OK);
    INV_CHECK(t, "after insert");

    size_t vlen;
    const void *val = ht_find(t, "key1", 4, &vlen);
    assert(val != NULL);
    assert(vlen == 4);
    assert(memcmp(val, "val1", 4) == 0);

    size_t removed = ht_remove(t, "key1", 4);
    assert(removed == 1);

    val = ht_find(t, "key1", 4, &vlen);
    assert(val == NULL);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 2: arena_contains for kv-ptrs ──────────────────────── */

static void test_arena_contains_kvpairs(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_upsert(t, "alpha", 5, "data_alpha", 10);
    ht_upsert(t, "beta", 4, "data_beta", 9);
    ht_upsert(t, "gamma", 5, "data_gamma", 10);

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(t->entries[i].kv_ptr != NULL);
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 3: Resize with external arena ──────────────────────── */

static void test_arena_resize(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(64 * 1024);
    assert(a != NULL);

    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.75,
        .min_load_factor = 0.20,
        .tomb_threshold = 0.20,
        .zombie_window = 16,
    };

    ht_table_t *t = ht_create_with_arena(&cfg, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    char key[32], val[32];
    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(val, sizeof(val), "val_%d", i);
        ht_insert_result_t r = ht_upsert(t, key, strlen(key), val, strlen(val));
        assert(r == HT_INSERT_OK || r == HT_INSERT_UPDATE);
    }
    INV_CHECK(t, "after 100 inserts");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 100);
    assert(st.capacity > 16);

    for (int i = 0; i < 100; i++) {
        snprintf(key, sizeof(key), "key_%d", i);
        snprintf(val, sizeof(val), "val_%d", i);
        size_t vlen;
        const void *v = ht_find(t, key, strlen(key), &vlen);
        assert(v != NULL);
        assert(vlen == strlen(val));
        assert(memcmp(v, val, vlen) == 0);
    }

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(t->entries[i].kv_ptr != NULL);
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 4: Compact with external arena ─────────────────────── */

static void test_arena_compact(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(64 * 1024);
    assert(a != NULL);

    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .tomb_threshold = 0.20,
        .zombie_window = 16,
    };

    ht_table_t *t = ht_create_with_arena(&cfg, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    for (int i = 0; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%d", i);
        ht_upsert(t, key, strlen(key), "v", 1);
    }
    INV_CHECK(t, "after 50 inserts");

    for (int i = 0; i < 25; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
    }
    INV_CHECK(t, "after 25 removes");

    ht_stats_t st_before;
    ht_stats(t, &st_before);
    assert(st_before.tombstone_cnt > 0);

    bool ok = ht_compact(t);
    assert(ok);
    INV_CHECK(t, "after compact");

    ht_stats_t st_after;
    ht_stats(t, &st_after);
    assert(st_after.tombstone_cnt < st_before.tombstone_cnt);
    assert(st_after.size == 25);

    for (int i = 25; i < 50; i++) {
        char key[32];
        snprintf(key, sizeof(key), "k%d", i);
        size_t vlen;
        const void *v = ht_find(t, key, strlen(key), &vlen);
        assert(v != NULL);
    }

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(t->entries[i].kv_ptr != NULL);
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 5: Destroy does not free kv-ptrs (arena owns them) ── */

static void test_arena_destroy_no_double_free(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    for (int i = 0; i < 20; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(val, sizeof(val), "val%d", i);
        ht_upsert(t, key, strlen(key), val, strlen(val));
    }

    assert(t->entry_count == 20);

    ht_destroy(t);

    for (int i = 0; i < 20; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "key%d", i);
        snprintf(val, sizeof(val), "val%d", i);
        ht_table_t *t2 = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
        assert(t2 != NULL);
        ht_upsert(t2, key, strlen(key), val, strlen(val));
        ht_destroy(t2);
    }

    arena_destroy(a);
}

/* ─── Test 6: Clear calls arena_clear ─────────────────────────── */

static void test_arena_clear(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    for (int i = 0; i < 30; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "ckey%d", i);
        snprintf(val, sizeof(val), "cval%d", i);
        ht_upsert(t, key, strlen(key), val, strlen(val));
    }
    assert(ht_size(t) == 30);

    ht_clear(t);

    assert(ht_size(t) == 0);
    assert(t->entry_count == 0);

    for (int i = 0; i < 10; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "new%d", i);
        snprintf(val, sizeof(val), "nval%d", i);
        ht_upsert(t, key, strlen(key), val, strlen(val));
    }
    assert(ht_size(t) == 10);

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(t->entries[i].kv_ptr != NULL);
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 7: Upsert with value size change (no free of old ptr) ─*/

static void test_arena_upsert_value_resize(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_upsert(t, "resize_key", 10, "short", 5);
    INV_CHECK(t, "after first upsert");

    size_t vlen;
    const void *v = ht_find(t, "resize_key", 10, &vlen);
    assert(v != NULL);
    assert(vlen == 5);
    assert(memcmp(v, "short", 5) == 0);

    ht_upsert(t, "resize_key", 10, "a_much_longer_value_here", 24);
    INV_CHECK(t, "after value resize upsert");

    v = ht_find(t, "resize_key", 10, &vlen);
    assert(v != NULL);
    assert(vlen == 24);
    assert(memcmp(v, "a_much_longer_value_here", 24) == 0);

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 8: Multi-value insert/remove with arena ────────────── */

static void test_arena_multi_value(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_insert(t, "multi", 5, "v1", 2);
    ht_insert(t, "multi", 5, "v2", 2);
    ht_insert(t, "multi", 5, "v3", 2);
    INV_CHECK(t, "after multi insert");

    assert(ht_size(t) == 3);

    size_t removed = ht_remove(t, "multi", 5);
    assert(removed == 3);
    assert(ht_size(t) == 0);

    ht_insert(t, "multi", 5, "v1", 2);
    ht_insert(t, "multi", 5, "v2", 2);

    size_t removed_kv = ht_remove_kv(t, "multi", 5, "v1", 2);
    assert(removed_kv == 1);
    assert(ht_size(t) == 1);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 9: Iterator over arena-backed ht ───────────────────── */

static void test_arena_iterator(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    for (int i = 0; i < 20; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "iter%d", i);
        snprintf(val, sizeof(val), "ival%d", i);
        ht_upsert(t, key, strlen(key), val, strlen(val));
    }

    int count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *k, *v;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &k, &klen, &v, &vlen)) {
        assert(k != NULL);
        assert(v != NULL);
        assert(klen > 0);
        count++;
    }
    assert(count == 20);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 10: Many entries to exercise slab/cold tiers ──────── */

static void test_arena_many_entries(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(1024 * 1024);
    assert(a != NULL);

    ht_config_t cfg = {
        .initial_capacity = 64,
        .max_load_factor = 0.75,
        .min_load_factor = 0.0,
        .tomb_threshold = 0.20,
        .zombie_window = 16,
    };

    ht_table_t *t = ht_create_with_arena(&cfg, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    const size_t N = 500;
    for (size_t i = 0; i < N; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "many_key_%04zu", i);
        snprintf(val, sizeof(val), "many_val_%04zu", i);
        ht_insert_result_t r = ht_upsert(t, key, strlen(key), val, strlen(val));
        assert(r == HT_INSERT_OK);
    }
    INV_CHECK(t, "after 500 inserts");
    assert(ht_size(t) == N);

    for (size_t i = 0; i < N; i++) {
        char key[64], val[64];
        snprintf(key, sizeof(key), "many_key_%04zu", i);
        snprintf(val, sizeof(val), "many_val_%04zu", i);
        size_t vlen;
        const void *v = ht_find(t, key, strlen(key), &vlen);
        assert(v != NULL);
        assert(vlen == strlen(val));
        assert(memcmp(v, val, vlen) == 0);
    }

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(t->entries[i].kv_ptr != NULL);
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 11: Arena invalidation — destroy arena after ht ──── */

static void test_arena_invalidation_order(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_upsert(t, "k1", 2, "v1", 2);
    ht_upsert(t, "k2", 2, "v2", 2);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 12: ht_create_with_arena vs ht_create comparison ── */

static void test_arena_vs_malloc_equivalence(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *ta = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    ht_table_t *tm = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(ta != NULL);
    assert(tm != NULL);

    for (int i = 0; i < 50; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "eqkey%d", i);
        snprintf(val, sizeof(val), "eqval%d", i);
        ht_upsert(ta, key, strlen(key), val, strlen(val));
        ht_upsert(tm, key, strlen(key), val, strlen(val));
    }

    assert(ht_size(ta) == ht_size(tm));

    for (int i = 0; i < 50; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "eqkey%d", i);
        snprintf(val, sizeof(val), "eqval%d", i);
        size_t va_len, vm_len;
        const void *va = ht_find(ta, key, strlen(key), &va_len);
        const void *vm = ht_find(tm, key, strlen(key), &vm_len);
        assert(va != NULL);
        assert(vm != NULL);
        assert(va_len == vm_len);
        assert(memcmp(va, vm, va_len) == 0);
    }

    ht_destroy(ta);
    ht_destroy(tm);
    arena_destroy(a);
}

/* ─── Test 13: Resize up then down with arena ──────────────── */

static void test_arena_resize_up_and_down(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(64 * 1024);
    assert(a != NULL);

    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.75,
        .min_load_factor = 0.20,
        .tomb_threshold = 0.20,
        .zombie_window = 16,
    };

    ht_table_t *t = ht_create_with_arena(&cfg, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    for (int i = 0; i < 80; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "rud%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        ht_upsert(t, key, strlen(key), val, strlen(val));
    }
    INV_CHECK(t, "after 80 inserts");

    for (int i = 0; i < 60; i++) {
        char key[32];
        snprintf(key, sizeof(key), "rud%d", i);
        ht_remove(t, key, strlen(key));
    }
    INV_CHECK(t, "after 60 removes");

    bool ok = ht_compact(t);
    assert(ok);
    INV_CHECK(t, "after compact");

    for (int i = 60; i < 80; i++) {
        char key[32], val[32];
        snprintf(key, sizeof(key), "rud%d", i);
        snprintf(val, sizeof(val), "v%d", i);
        size_t vlen;
        const void *v = ht_find(t, key, strlen(key), &vlen);
        assert(v != NULL);
        assert(memcmp(v, val, vlen) == 0);
    }

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 14: Inc with arena ──────────────────────────────── */

static void test_arena_inc(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    int64_t val = ht_inc(t, "counter", 7, 10);
    assert(val == 10);

    val = ht_inc(t, "counter", 7, 5);
    assert(val == 15);

    val = ht_inc(t, "counter", 7, -3);
    assert(val == 12);

    size_t vlen;
    const void *v = ht_find(t, "counter", 7, &vlen);
    assert(v != NULL);
    assert(vlen == sizeof(int64_t));
    assert(*(const int64_t *)v == 12);

    for (size_t i = 0; i < t->entry_count; i++) {
        assert(arena_contains(a, t->entries[i].kv_ptr));
    }

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 15: Unsert with arena ───────────────────────────── */

static void test_arena_unsert(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_insert_result_t r = ht_unsert(t, "ukey", 4, "uval1", 5);
    assert(r == HT_INSERT_OK);

    r = ht_unsert(t, "ukey", 4, "uval1", 5);
    assert(r == HT_INSERT_UPDATE);

    r = ht_unsert(t, "ukey", 4, "uval2", 5);
    assert(r == HT_INSERT_OK);

    assert(ht_size(t) == 2);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 16: find_key_all with arena ─────────────────────── */

static bool find_key_all_cb(const void *key, size_t key_len,
                            const void *value, size_t value_len, void *ctx) {
    (void)key; (void)key_len; (void)value; (void)value_len;
    int *cnt = (int *)ctx;
    (*cnt)++;
    return true;
}

static void test_arena_find_key_all(void) {
    printf("  %s\n", __func__);
    struct arena *a = arena_create(4096);
    assert(a != NULL);

    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, a);
    assert(t != NULL);

    ht_insert(t, "fk", 2, "a", 1);
    ht_insert(t, "fk", 2, "b", 1);
    ht_insert(t, "fk", 2, "c", 1);

    int found = 0;
    ht_find_key_all(t, "fk", 2, find_key_all_cb, &found);
    assert(found == 3);

    ht_destroy(t);
    arena_destroy(a);
}

/* ─── Test 17: ht_create_with_arena(NULL arena) falls back to malloc ── */

static void test_arena_null_fallback(void) {
    printf("  %s\n", __func__);
    ht_table_t *t = ht_create_with_arena(NULL, fnv1a_hash, NULL, NULL, NULL);
    assert(t != NULL);
    assert(t->allocator == NULL);

    ht_upsert(t, "nf", 2, "nfval", 5);
    size_t vlen;
    const void *v = ht_find(t, "nf", 2, &vlen);
    assert(v != NULL);
    assert(vlen == 5);
    assert(memcmp(v, "nfval", 5) == 0);

    ht_destroy(t);
}

/* ─── Main ────────────────────────────────────────────────────── */

int main(void) {
    printf("=== ht + arena integration tests ===\n");

    test_arena_basic_crud();
    test_arena_contains_kvpairs();
    test_arena_resize();
    test_arena_compact();
    test_arena_destroy_no_double_free();
    test_arena_clear();
    test_arena_upsert_value_resize();
    test_arena_multi_value();
    test_arena_iterator();
    test_arena_many_entries();
    test_arena_invalidation_order();
    test_arena_vs_malloc_equivalence();
    test_arena_resize_up_and_down();
    test_arena_inc();
    test_arena_unsert();
    test_arena_find_key_all();
    test_arena_null_fallback();

    printf("\nAll ht+arena tests passed!\n");
    return 0;
}
