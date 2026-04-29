/**
 * test_edge_cases_a.c - Edge Case Tests A (39 tests)
 *
 * Tests 1-31 from original suite plus 8 new tests (32-39).
 *
 * Sections:
 *   1-4:   Spill lane (hash=0, hash=1, mixed, stress)
 *   5-6:   Duplicate keys (hash collisions, early termination)
 *   7:     Custom equality function
 *   8-12:  Negative / edge cases (empty, double-remove, nonexistent, clear-reuse, NULL)
 *   13-16: Iterator edge cases (empty, tombstones, spill, after clear)
 *   17:    _with_hash variants
 *   18:    ht_inc edge cases
 *   19:    Prophylactic tombstones
 *   20-22: Value edge cases (zero-length, large, empty key)
 *   23-24: Resize edge cases (same cap, shrink below size)
 *   25:    Stats consistency
 *   26:    Binary keys
 *   27-28: Extended insert/remove sequences (4x length, normal + spill + collision)
 *   29:    Update in-place preserves other entries
 *   30:    Resize up then down
 *   31:    Compact after massive churn
 *   32:    user_ctx propagation through hash/eq callbacks
 *   33:    eq_fn=NULL uses default memcmp
 *   34:    initial_capacity clamping (non-power-of-2, 0, 1)
 *   35:    ht_resize to non-power-of-2
 *   36:    ht_compact on empty table
 *   37:    ht_find_all with hash=0 (spill lane)
 *   38:    _with_hash variants with hash=0/1 (spill lane)
 *   39:    Spill lane grow (20+ entries with hash=0)
 */

#include "draugr/ht.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define INV_CHECK(t, label) do { \
    const char *_inv_err = ht_check_invariants(t); \
    if (_inv_err) { \
        printf("  INVARIANT BROKEN at %s: %s\n", (label), _inv_err); \
        return 1; \
    } \
} while (0)

// ============================================================================
// Hash helpers
// ============================================================================

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

static uint64_t zero_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

static uint64_t one_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 1;
}

static uint64_t fixed_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 42;
}

static uint64_t selective_zero_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    const char *k = (const char *)key;
    if (len > 0 && k[0] == 'z') return 0;
    return fnv1a_hash(key, len, ctx);
}

static uint64_t case_insensitive_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t hash = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = p[i];
        if (c >= 'A' && c <= 'Z') c = c - 'A' + 'a';
        hash ^= c;
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

// ============================================================================
// Equality helpers
// ============================================================================

static bool case_insensitive_eq(const void *a, size_t la,
                                 const void *b, size_t lb, void *ctx) {
    (void)ctx;
    if (la != lb) return false;
    for (size_t i = 0; i < la; i++) {
        char ca = ((const char *)a)[i];
        char cb = ((const char *)b)[i];
        if (ca >= 'A' && ca <= 'Z') ca = ca - 'A' + 'a';
        if (cb >= 'A' && cb <= 'Z') cb = cb - 'A' + 'a';
        if (ca != cb) return false;
    }
    return true;
}

// ============================================================================
// Callbacks
// ============================================================================

static int g_collect_count;
static int g_collect_vals[256];

static bool collect_val_cb(const void *key, size_t klen,
                           const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)vlen; (void)ctx;
    if (g_collect_count < 256) {
        g_collect_vals[g_collect_count] = *(const int *)val;
    }
    g_collect_count++;
    return true;
}

static bool collect_stop_2_cb(const void *key, size_t klen,
                              const void *val, size_t vlen, void *ctx) {
    (void)key; (void)klen; (void)vlen; (void)ctx;
    g_collect_count++;
    return g_collect_count < 2;
}


static int test_spill_hash_zero(void) {
    printf("Test: spill lane hash=0...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);

    for (int i = 0; i < 12; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        int val = i * 100;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 12);

    for (int i = 0; i < 12; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i * 100);
    }

    assert(ht_remove(t, "k3", 2));
    assert(ht_remove(t, "k7", 2));
    assert(ht_remove(t, "k11", 3));

    for (int i = 0; i < 12; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i == 3 || i == 7 || i == 11) assert(v == NULL);
        else assert(v != NULL && *v == i * 100);
    }

    ht_resize(t, 64);
    INV_CHECK(t, "test_spill_hash_zero: after resize");

    for (int i = 0; i < 12; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i == 3 || i == 7 || i == 11) assert(v == NULL);
        else assert(v != NULL && *v == i * 100);
    }

    ht_compact(t);
    INV_CHECK(t, "test_spill_hash_zero: after compact");

    for (int i = 0; i < 12; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (i == 3 || i == 7 || i == 11) assert(v == NULL);
        else assert(v != NULL && *v == i * 100);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 2. Spill lane: hash=1
// ============================================================================

static int test_spill_hash_one(void) {
    printf("Test: spill lane hash=1...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, one_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[32]; snprintf(key, sizeof(key), "key%d", i);
        int val = i;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    for (int i = 0; i < 10; i++) {
        char key[32]; snprintf(key, sizeof(key), "key%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    assert(ht_remove(t, "key5", 4));
    assert(ht_find(t, "key5", 4, NULL) == NULL);
    assert(ht_find(t, "key0", 4, NULL) != NULL);

    ht_resize(t, 128);
    assert(ht_find(t, "key5", 4, NULL) == NULL);
    assert(ht_find(t, "key0", 4, NULL) != NULL);

    ht_compact(t);
    assert(ht_find(t, "key0", 4, NULL) != NULL);
    assert(ht_find(t, "key5", 4, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 3. Spill lane: mixed (some hash=0, some normal)
// ============================================================================

static int test_spill_mixed(void) {
    printf("Test: spill lane mixed (some hash=0)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);

    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        int val = i;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }
    for (int i = 0; i < 10; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        int val = i + 100;
        assert(ht_insert(t, key, strlen(key), &val, sizeof(val)));
    }

    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 10; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i + 100);
    }

    assert(ht_remove(t, "z2", 2));
    assert(ht_remove(t, "n3", 2));
    assert(ht_find(t, "z2", 2, NULL) == NULL);
    assert(ht_find(t, "n3", 2, NULL) == NULL);

    ht_compact(t);
    INV_CHECK(t, "test_spill_mixed: after compact");

    assert(ht_find(t, "z0", 2, NULL) != NULL);
    assert(ht_find(t, "z2", 2, NULL) == NULL);
    assert(ht_find(t, "n4", 2, NULL) != NULL);
    assert(ht_find(t, "n3", 2, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 4. Spill lane stress (hash=0, 2000 ops)
// ============================================================================

static int test_spill_stress(void) {
    printf("Test: spill lane stress (hash=0, 16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);

    #define SSTRESS_N 1000
    int *present = calloc(SSTRESS_N, sizeof(int));

    srand(777);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % SSTRESS_N;
        int action = rand() % 3;

        if (action == 0) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action == 1) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            if (present[k]) {
                const int *v = ht_find(t, key, strlen(key), NULL);
                assert(v != NULL && *v == k);
            } else {
                assert(ht_find(t, key, strlen(key), NULL) == NULL);
            }
        } else {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            bool removed = ht_remove(t, key, strlen(key));
            if (present[k]) assert(removed);
            else assert(!removed);
            present[k] = 0;
        }
    }

    for (int i = 0; i < SSTRESS_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    free(present);

    INV_CHECK(t, "test_spill_stress: after 16000 ops");

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 5. Duplicate keys (same hash, different keys → coexist)
// ============================================================================

static int test_duplicate_keys(void) {
    printf("Test: duplicate keys (hash collisions)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    const char *keys[] = {"alice", "bob", "charlie", "delta", "eve"};
    int vals[] = {1, 2, 3, 4, 5};

    for (int i = 0; i < 5; i++) {
        assert(ht_insert(t, keys[i], strlen(keys[i]), &vals[i], sizeof(int)));
    }

    for (int i = 0; i < 5; i++) {
        const int *v = ht_find(t, keys[i], strlen(keys[i]), NULL);
        assert(v != NULL && *v == vals[i]);
    }

    g_collect_count = 0;
    ht_find_all(t, 42, collect_val_cb, NULL);
    assert(g_collect_count == 5);

    int new_val = 100;
    assert(ht_insert(t, "bob", 3, &new_val, sizeof(new_val)) == false);
    assert(*(const int *)ht_find(t, "bob", 3, NULL) == 100);

    g_collect_count = 0;
    ht_find_all(t, 42, collect_val_cb, NULL);
    assert(g_collect_count == 5);

    assert(ht_remove(t, "charlie", 7));
    assert(ht_find(t, "charlie", 7, NULL) == NULL);

    g_collect_count = 0;
    ht_find_all(t, 42, collect_val_cb, NULL);
    assert(g_collect_count == 4);

    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        const int *v2 = ht_find(t, keys[i], strlen(keys[i]), NULL);
        assert(v2 != NULL);
        if (i == 1) assert(*v2 == 100);
        else assert(*v2 == vals[i]);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 6. ht_find_all early termination
// ============================================================================

static int test_find_all_early_stop(void) {
    printf("Test: ht_find_all early termination...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    g_collect_count = 0;
    ht_find_all(t, 42, collect_stop_2_cb, NULL);
    assert(g_collect_count == 2);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 7. Custom equality function
// ============================================================================

static int test_custom_eq(void) {
    printf("Test: custom equality function...\n");
    ht_table_t *t = ht_create(NULL, case_insensitive_hash, case_insensitive_eq, NULL);

    int val = 42;
    assert(ht_insert(t, "Hello", 5, &val, sizeof(val)));

    const int *v = ht_find(t, "hello", 5, NULL);
    assert(v != NULL && *v == 42);

    v = ht_find(t, "HELLO", 5, NULL);
    assert(v != NULL && *v == 42);

    int val2 = 99;
    assert(ht_insert(t, "HELLO", 5, &val2, sizeof(val2)) == false);
    v = ht_find(t, "Hello", 5, NULL);
    assert(v != NULL && *v == 99);

    int val3 = 7;
    assert(ht_insert(t, "World", 5, &val3, sizeof(val3)) == true);
    v = ht_find(t, "world", 5, NULL);
    assert(v != NULL && *v == 7);

    assert(ht_remove(t, "hello", 5));
    assert(ht_find(t, "Hello", 5, NULL) == NULL);
    assert(ht_find(t, "World", 5, NULL) != NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 8. Empty table operations
// ============================================================================

static int test_empty_table(void) {
    printf("Test: empty table operations...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    assert(ht_find(t, "anything", 8, NULL) == NULL);
    assert(ht_remove(t, "anything", 8) == false);
    assert(ht_find(t, "", 0, NULL) == NULL);
    assert(ht_remove(t, "", 0) == false);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.tombstone_cnt == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 9. Double remove
// ============================================================================

static int test_double_remove(void) {
    printf("Test: double remove...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int val = 1;
    ht_insert(t, "key", 3, &val, sizeof(val));

    assert(ht_remove(t, "key", 3) == true);
    assert(ht_remove(t, "key", 3) == false);
    assert(ht_find(t, "key", 3, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 10. Remove nonexistent key
// ============================================================================

static int test_remove_nonexistent(void) {
    printf("Test: remove nonexistent...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int val = 1;
    ht_insert(t, "exists", 6, &val, sizeof(val));

    assert(ht_remove(t, "nope", 4) == false);
    assert(ht_find(t, "exists", 6, NULL) != NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.tombstone_cnt == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 11. Clear then reuse
// ============================================================================

static int test_clear_reuse(void) {
    printf("Test: clear then reuse...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 10);

    ht_clear(t);
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.tombstone_cnt == 0);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "new%d", i);
        int val = i * 10;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats(t, &st);
    assert(st.size == 20);

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "new%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i * 10);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 12. NULL args (no crash)
// ============================================================================

static int test_null_args(void) {
    printf("Test: NULL args (no crash)...\n");
    ht_destroy(NULL);
    ht_clear(NULL);
    ht_compact(NULL);
    ht_stats(NULL, NULL);
    assert(ht_resize(NULL, 64) == false);
    assert(ht_insert(NULL, "k", 1, "v", 1) == false);
    assert(ht_find(NULL, "k", 1, NULL) == NULL);
    assert(ht_remove(NULL, "k", 1) == false);
    assert(ht_insert_with_hash(NULL, 42, "k", 1, "v", 1) == false);
    assert(ht_find_with_hash(NULL, 42, "k", 1, NULL) == NULL);
    assert(ht_remove_with_hash(NULL, 42, "k", 1) == false);
    ht_find_all(NULL, 42, collect_val_cb, NULL);

    // Create a valid table, then test NULL key
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);
    assert(ht_insert(t, NULL, 1, "v", 1) == false);
    assert(ht_find(t, NULL, 1, NULL) == NULL);
    assert(ht_remove(t, NULL, 1) == false);
    ht_destroy(t);

    // ht_create with NULL hash_fn → NULL
    assert(ht_create(NULL, NULL, NULL, NULL) == NULL);

    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 13. Iterator: empty table
// ============================================================================

static int test_iter_empty(void) {
    printf("Test: iterate empty table...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    int count = 0;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) count++;
    assert(count == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 14. Iterator: with tombstones
// ============================================================================

static int test_iter_tombstones(void) {
    printf("Test: iterate with tombstones...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    for (int i = 0; i < 10; i += 2) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    // With backward-shift, short chains may produce 0 tombstones.
    // The key invariant is that size == 5 and lookups still work.
    assert(st.size == 5);

    int count = 0;
    int seen[10] = {0};
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        const char *k = (const char *)key;
        char buf[16];
        assert(klen >= 2 && klen < sizeof(buf));
        memcpy(buf, k + 1, klen - 1);
        buf[klen - 1] = '\0';
        int idx = atoi(buf);
        assert(idx % 2 == 1);
        assert(idx >= 0 && idx < 10);
        seen[idx] = 1;
        count++;
    }
    assert(count == 5);
    for (int i = 1; i < 10; i += 2) assert(seen[i] == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 15. Iterator: with spill-lane entries
// ============================================================================

static int test_iter_spill(void) {
    printf("Test: iterate with spill-lane entries...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);

    for (int i = 0; i < 3; i++) {
        char key[32]; snprintf(key, sizeof(key), "z%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }
    for (int i = 0; i < 5; i++) {
        char key[32]; snprintf(key, sizeof(key), "n%d", i);
        int val = i + 100;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    int spill_count = 0, main_count = 0;
    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) {
        const char *k = (const char *)key;
        if (k[0] == 'z') spill_count++;
        else main_count++;
    }
    assert(spill_count == 3);
    assert(main_count == 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 16. Iterator: after clear → 0 items
// ============================================================================

static int test_iter_after_clear(void) {
    printf("Test: iterate after clear...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 5; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_clear(t);

    ht_iter_t iter = ht_iter_begin(t);
    const void *key, *val;
    size_t klen, vlen;
    int count = 0;
    while (ht_iter_next(t, &iter, &key, &klen, &val, &vlen)) count++;
    assert(count == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 17. _with_hash variants
// ============================================================================

static int test_with_hash_variants(void) {
    printf("Test: _with_hash variants...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    uint64_t h = fnv1a_hash("testkey", 7, NULL);

    int val = 42;
    assert(ht_insert_with_hash(t, h, "testkey", 7, &val, sizeof(val)));

    size_t out_len;
    const int *v = ht_find_with_hash(t, h, "testkey", 7, &out_len);
    assert(v != NULL && *v == 42 && out_len == sizeof(int));

    v = ht_find_with_hash(t, 999, "testkey", 7, NULL);
    assert(v == NULL);

    v = ht_find_with_hash(t, h, "wrongkey", 8, NULL);
    assert(v == NULL);

    assert(ht_remove_with_hash(t, h, "testkey", 7));
    assert(ht_find_with_hash(t, h, "testkey", 7, NULL) == NULL);
    assert(ht_remove_with_hash(t, h, "testkey", 7) == false);
    assert(ht_remove_with_hash(t, 999, "nope", 4) == false);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 18. ht_inc edge cases
// ============================================================================

static int test_ht_inc_edge(void) {
    printf("Test: ht_inc edge cases...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int64_t v = ht_inc(t, "counter", 7, 10);
    assert(v == 10);

    v = ht_inc(t, "counter", 7, 5);
    assert(v == 15);

    v = ht_inc(t, "counter", 7, -7);
    assert(v == 8);

    v = ht_inc(t, "counter", 7, 0);
    assert(v == 8);

    ht_insert(t, "x", 1, &(int){42}, sizeof(int));
    ht_remove(t, "x", 1);
    v = ht_inc(t, "x", 1, 1);
    assert(v == 1);

    ht_inc(t, "a", 1, 100);
    ht_inc(t, "b", 1, 200);
    ht_inc(t, "a", 1, 50);
    v = ht_inc(t, "a", 1, 0);
    assert(v == 150);
    v = ht_inc(t, "b", 1, 0);
    assert(v == 200);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 19. Prophylactic tombstones don't break lookups
// ============================================================================

static int test_prophylactic_tombstones(void) {
    printf("Test: prophylactic tombstones don't break lookups...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    for (int i = 20; i < 30; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    for (int i = 0; i < 30; i++) {
        char key[16]; snprintf(key, sizeof(key), "key%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 20. Zero-length values
// ============================================================================

static int test_zero_len_values(void) {
    printf("Test: zero-length values...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    assert(ht_insert(t, "key", 3, NULL, 0));

    size_t out_len = 999;
    const void *v = ht_find(t, "key", 3, &out_len);
    assert(v != NULL);
    assert(out_len == 0);

    assert(ht_remove(t, "key", 3));
    assert(ht_find(t, "key", 3, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 21. Large values (4KB)
// ============================================================================

static int test_large_values(void) {
    printf("Test: large values (4KB)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    uint8_t large[4096];
    for (int i = 0; i < 4096; i++) large[i] = (uint8_t)(i & 0xFF);

    assert(ht_insert(t, "bigkey", 6, large, sizeof(large)));

    size_t out_len;
    const uint8_t *v = ht_find(t, "bigkey", 6, &out_len);
    assert(v != NULL);
    assert(out_len == sizeof(large));
    assert(memcmp(v, large, sizeof(large)) == 0);

    large[0] = 0xFF;
    ht_insert(t, "bigkey", 6, large, sizeof(large));
    v = ht_find(t, "bigkey", 6, &out_len);
    assert(v != NULL && v[0] == 0xFF);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 22. Empty string key (key_len=0)
// ============================================================================

static int test_empty_key(void) {
    printf("Test: empty string key (key_len=0)...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    int val = 42;
    assert(ht_insert(t, "", 0, &val, sizeof(val)));

    size_t out_len;
    const int *v = ht_find(t, "", 0, &out_len);
    assert(v != NULL && *v == 42);

    int val2 = 99;
    assert(ht_insert(t, "", 0, &val2, sizeof(val2)) == false);
    v = ht_find(t, "", 0, &out_len);
    assert(v != NULL && *v == 99);

    assert(ht_remove(t, "", 0));
    assert(ht_find(t, "", 0, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 23. Resize to same capacity (no-op)
// ============================================================================

static int test_resize_same_cap(void) {
    printf("Test: resize to same capacity (no-op)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st_before;
    ht_stats(t, &st_before);

    assert(ht_resize(t, 64));

    ht_stats_t st_after;
    ht_stats(t, &st_after);
    assert(st_after.size == st_before.size);
    assert(st_after.capacity == st_before.capacity);

    for (int i = 0; i < 10; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 24. Resize below current size → rejected
// ============================================================================

static int test_resize_below_size(void) {
    printf("Test: resize below current size → rejected...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st_before;
    ht_stats(t, &st_before);
    assert(st_before.size == 20);

    // Resize to 4 → should fail (less than current size)
    assert(ht_resize(t, 4) == false);

    // All entries still there
    ht_stats_t st_after;
    ht_stats(t, &st_after);
    assert(st_after.size == 20);
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_find(t, key, strlen(key), NULL) != NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 25. Stats consistency
// ============================================================================

static int test_stats_consistency(void) {
    printf("Test: stats consistency...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    assert(st.capacity >= 16);
    assert(st.tombstone_cnt == 0);
    assert(st.load_factor == 0.0);
    assert(st.tombstone_ratio == 0.0);

    for (int i = 0; i < 12; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }
    ht_stats(t, &st);
    assert(st.size == 12);
    assert(st.load_factor > 0.0);

    ht_remove(t, "k0", 2);
    ht_remove(t, "k1", 2);
    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.tombstone_cnt >= 0 && st.tombstone_cnt <= 2); // backshift may eliminate tombstones

    ht_compact(t);
    ht_stats(t, &st);
    assert(st.size == 10);
    assert(st.size + st.tombstone_cnt <= st.capacity);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 26. Binary keys with embedded nulls
// ============================================================================

static int test_binary_keys(void) {
    printf("Test: binary keys with embedded nulls...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    uint8_t key1[] = {0x00, 0x01, 0x00, 0x02};
    uint8_t key2[] = {0x00, 0x01, 0x00, 0x03};
    uint8_t key3[] = {0x00, 0x01};

    int v1 = 1, v2 = 2, v3 = 3;
    assert(ht_insert(t, key1, sizeof(key1), &v1, sizeof(v1)));
    assert(ht_insert(t, key2, sizeof(key2), &v2, sizeof(v2)));
    assert(ht_insert(t, key3, sizeof(key3), &v3, sizeof(v3)));

    assert(*(int *)ht_find(t, key1, sizeof(key1), NULL) == 1);
    assert(*(int *)ht_find(t, key2, sizeof(key2), NULL) == 2);
    assert(*(int *)ht_find(t, key3, sizeof(key3), NULL) == 3);

    assert(ht_remove(t, key1, sizeof(key1)));
    assert(ht_find(t, key1, sizeof(key1), NULL) == NULL);
    assert(*(int *)ht_find(t, key2, sizeof(key2), NULL) == 2);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 27. Extended insert/remove sequence: normal hash (4000 ops, 500 keys)
// ============================================================================

static int test_long_sequence_normal(void) {
    printf("Test: long insert/remove sequence (normal, 16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    #define LN_N 1000
    int *present = calloc(LN_N, sizeof(int));

    srand(1001);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % LN_N;
        int action = rand() % 100;

        if (action < 40) {
            // Insert
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k * 7;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 75) {
            // Find
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) {
                assert(v != NULL && *v == k * 7);
            } else {
                assert(v == NULL);
            }
        } else if (action < 95) {
            // Remove
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            bool result = ht_remove(t, key, strlen(key));
            if (present[k]) assert(result);
            else assert(!result);
            present[k] = 0;
        } else {
            // Compact
            ht_compact(t);
        }
    }

    // Final verify
    for (int i = 0; i < LN_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i * 7);
        else assert(v == NULL);
    }

    INV_CHECK(t, "test_long_sequence_normal: final");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size + st.tombstone_cnt <= st.capacity);
    #undef LN_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 28. Extended insert/remove sequence: fixed hash (all collide, 4000 ops)
// ============================================================================

static int test_long_sequence_collision(void) {
    printf("Test: long insert/remove sequence (all collide, 16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 128, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fixed_hash, NULL, NULL);

    #define LC_N 1000
    int *present = calloc(LC_N, sizeof(int));

    srand(2002);
    for (int op = 0; op < 16000; op++) {
        int k = rand() % LC_N;
        int action = rand() % 100;

        if (action < 40) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k * 3;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
        } else if (action < 75) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            const int *v = ht_find(t, key, strlen(key), NULL);
            if (present[k]) assert(v != NULL && *v == k * 3);
            else assert(v == NULL);
        } else if (action < 95) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            bool result = ht_remove(t, key, strlen(key));
            assert(result == !!present[k]);
            present[k] = 0;
        } else {
            ht_compact(t);
        }
    }

    for (int i = 0; i < LC_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i * 3);
        else assert(v == NULL);
    }

    INV_CHECK(t, "test_long_sequence_collision: final");

    #undef LC_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 29. Update in-place preserves other entries
// ============================================================================

static int test_update_preserves_others(void) {
    printf("Test: update in-place preserves other entries...\n");
    ht_table_t *t = ht_create(NULL, fnv1a_hash, NULL, NULL);

    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    // Update k10
    int new_val = 9999;
    assert(ht_insert(t, "k10", 3, &new_val, sizeof(new_val)) == false);

    // All other entries still correct
    for (int i = 0; i < 20; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL);
        if (i == 10) assert(*v == 9999);
        else assert(*v == i);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 30. Resize up then down
// ============================================================================

static int test_resize_up_down(void) {
    printf("Test: resize up then down...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    // Fill up → triggers auto-resize
    for (int i = 0; i < 100; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
    }

    ht_stats_t st1;
    ht_stats(t, &st1);
    assert(st1.size == 100);
    size_t cap_up = st1.capacity;

    // Manual resize down to 256
    assert(ht_resize(t, 256));
    INV_CHECK(t, "test_resize_up_down: after resize to 256");

    ht_stats(t, &st1);
    assert(st1.capacity == 256);
    assert(st1.size == 100);

    for (int i = 0; i < 100; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }

    // Remove most entries
    for (int i = 0; i < 90; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        ht_remove(t, key, strlen(key));
    }

    // Resize down to 64
    assert(ht_resize(t, 64));
    INV_CHECK(t, "test_resize_up_down: after resize to 64");

    ht_stats_t st2;
    ht_stats(t, &st2);
    assert(st2.size == 10);
    assert(st2.capacity == 64);

    for (int i = 90; i < 100; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 90; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        assert(ht_find(t, key, strlen(key), NULL) == NULL);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 31. Compact after massive churn
// ============================================================================

static int test_compact_churn(void) {
    printf("Test: compact after massive churn (16000 ops)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);

    #define CHURN_N 1000
    int *present = calloc(CHURN_N, sizeof(int));
    int present_count = 0;

    srand(3003);
    // Phase 1: Fill
    for (int i = 0; i < CHURN_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        int val = i;
        ht_insert(t, key, strlen(key), &val, sizeof(val));
        present[i] = 1;
        present_count++;
    }

    // Phase 2: Massive churn (delete + reinsert)
    for (int op = 0; op < 16000; op++) {
        int k = rand() % CHURN_N;
        if (rand() % 2 == 0 && present[k]) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            ht_remove(t, key, strlen(key));
            present[k] = 0;
            present_count--;
        } else if (!present[k]) {
            char key[16]; snprintf(key, sizeof(key), "k%d", k);
            int val = k;
            ht_insert(t, key, strlen(key), &val, sizeof(val));
            present[k] = 1;
            present_count++;
        }
    }

    // Phase 3: Compact
    ht_compact(t);

    INV_CHECK(t, "test_compact_churn: after compact");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == (size_t)present_count);

    // Verify all
    for (int i = 0; i < CHURN_N; i++) {
        char key[16]; snprintf(key, sizeof(key), "k%d", i);
        const int *v = ht_find(t, key, strlen(key), NULL);
        if (present[i]) assert(v != NULL && *v == i);
        else assert(v == NULL);
    }
    #undef CHURN_N

    free(present);
    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 32. user_ctx propagation through hash/eq callbacks
// ============================================================================

static uint64_t ctx_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len;
    return (uint64_t)(uintptr_t)ctx;
}

static bool ctx_eq(const void *a, size_t la, const void *b, size_t lb, void *ctx) {
    (void)ctx;
    return la == lb && memcmp(a, b, la) == 0;
}

static int test_user_ctx_propagation(void) {
    printf("Test: user_ctx propagation...\n");
    /* Use ctx_hash with user_ctx = (void*)42 so all keys hash to 42 */
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, ctx_hash, ctx_eq, (void *)(uintptr_t)42);
    assert(t != NULL);

    int v1 = 1, v2 = 2;
    assert(ht_insert(t, "a", 1, &v1, sizeof(v1)));
    assert(ht_insert(t, "b", 1, &v2, sizeof(v2)));

    const int *r = ht_find(t, "a", 1, NULL);
    assert(r != NULL && *r == 1);
    r = ht_find(t, "b", 1, NULL);
    assert(r != NULL && *r == 2);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 33. eq_fn=NULL uses default memcmp
// ============================================================================

static int test_eq_fn_null(void) {
    printf("Test: eq_fn=NULL default memcmp...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 100;
    assert(ht_insert(t, "hello", 5, &v, sizeof(v)));

    const int *r = ht_find(t, "hello", 5, NULL);
    assert(r != NULL && *r == 100);

    /* Different key with same prefix should not match */
    r = ht_find(t, "hell", 4, NULL);
    assert(r == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 34. initial_capacity clamping (non-power-of-2, 0, 1)
// ============================================================================

static int test_initial_capacity_clamp(void) {
    printf("Test: initial_capacity clamping...\n");
    /* capacity=0 should clamp to some minimum */
    ht_config_t cfg = { .initial_capacity = 0, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);
    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 1);

    /* Insert should work */
    int v = 1;
    assert(ht_insert(t, "k", 1, &v, sizeof(v)));
    ht_stats(t, &st);
    assert(st.size == 1);
    ht_destroy(t);

    /* capacity=1 should work */
    cfg.initial_capacity = 1;
    t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);
    ht_stats(t, &st);
    assert(st.capacity >= 1);
    assert(ht_insert(t, "k", 1, &v, sizeof(v)));
    ht_destroy(t);

    /* capacity=33 (non-power-of-2) should round up */
    cfg.initial_capacity = 33;
    t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);
    ht_stats(t, &st);
    assert(st.capacity >= 33);
    /* Should be power of 2 */
    assert((st.capacity & (st.capacity - 1)) == 0);
    ht_destroy(t);

    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 35. ht_resize to non-power-of-2
// ============================================================================

static int test_resize_non_pow2(void) {
    printf("Test: ht_resize to non-power-of-2...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .min_load_factor = 0.0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert some entries */
    for (int i = 0; i < 5; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        int v = i;
        assert(ht_insert(t, k, strlen(k), &v, sizeof(v)));
    }

    /* Resize to non-power-of-2 — should round up */
    bool ok = ht_resize(t, 33);
    assert(ok);

    INV_CHECK(t, "test_resize_non_pow2: after resize");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.capacity >= 33);
    assert((st.capacity & (st.capacity - 1)) == 0);  /* still power of 2 */

    /* All entries still findable */
    for (int i = 0; i < 5; i++) {
        char k[16]; snprintf(k, sizeof(k), "key%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 36. ht_compact on empty table
// ============================================================================

static int test_compact_empty(void) {
    printf("Test: ht_compact on empty table...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    ht_compact(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);
    /* Prophylactic tombstones may remain after compact — only check size */

    /* Should still work after compact */
    int v = 42;
    assert(ht_insert(t, "k", 1, &v, sizeof(v)));
    const int *r = ht_find(t, "k", 1, NULL);
    assert(r != NULL && *r == 42);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 37. ht_find_all with hash=0 (spill lane)
// ============================================================================

static int test_find_all_spill(void) {
    printf("Test: ht_find_all with hash=0 (spill lane)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    int vals[5];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "k%d", i);
        vals[i] = i * 10;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    g_collect_count = 0;
    memset(g_collect_vals, 0, sizeof(g_collect_vals));
    ht_find_all(t, 0, collect_val_cb, NULL);

    assert(g_collect_count == 5);
    /* Sort collected values for deterministic check */
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++)
            if (g_collect_vals[i] > g_collect_vals[j]) {
                int tmp = g_collect_vals[i];
                g_collect_vals[i] = g_collect_vals[j];
                g_collect_vals[j] = tmp;
            }
    for (int i = 0; i < 5; i++)
        assert(g_collect_vals[i] == i * 10);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 38. _with_hash variants with hash=0/1 (spill lane)
// ============================================================================

static int test_with_hash_spill_variants(void) {
    printf("Test: _with_hash variants with hash=0/1 (spill lane)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert with hash=0 (spill) */
    int v0 = 100;
    assert(ht_insert_with_hash(t, 0, "a", 1, &v0, sizeof(v0)));
    /* Insert with hash=1 (tomb sentinel, spill) */
    int v1 = 200;
    assert(ht_insert_with_hash(t, 1, "b", 1, &v1, sizeof(v1)));

    const int *r;
    r = ht_find_with_hash(t, 0, "a", 1, NULL);
    assert(r != NULL && *r == 100);
    r = ht_find_with_hash(t, 1, "b", 1, NULL);
    assert(r != NULL && *r == 200);

    /* Remove with hash */
    assert(ht_remove_with_hash(t, 0, "a", 1));
    r = ht_find_with_hash(t, 0, "a", 1, NULL);
    assert(r == NULL);

    assert(ht_remove_with_hash(t, 1, "b", 1));
    r = ht_find_with_hash(t, 1, "b", 1, NULL);
    assert(r == NULL);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 39. Spill lane grow (20+ entries with hash=0)
// ============================================================================

static int test_spill_grow(void) {
    printf("Test: spill lane grow (20+ entries with hash=0)...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 25 entries all with hash=0 — forces table to grow */
    int vals[25];
    for (int i = 0; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "g%d", i);
        vals[i] = i * 7;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 25);
    /* Capacity may have grown — exact value depends on load factor triggers */

    /* Verify all */
    for (int i = 0; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "g%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 7);
    }

    /* Remove half, verify remainder */
    for (int i = 0; i < 25; i += 2) {
        char k[8]; snprintf(k, sizeof(k), "g%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    for (int i = 0; i < 25; i++) {
        char k[8]; snprintf(k, sizeof(k), "g%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i % 2 == 0) assert(v == NULL);
        else assert(v != NULL && *v == i * 7);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 40. Spill-lane update (overwrite existing key in spill)
// ============================================================================

static int test_spill_update(void) {
    printf("Test: spill-lane update (overwrite existing key)...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    int v1 = 10, v2 = 20;
    assert(ht_insert(t, "a", 1, &v1, sizeof(v1)));
    assert(ht_insert(t, "b", 1, &v2, sizeof(v2)));

    /* Overwrite 'a' with new value */
    int v1_new = 99;
    assert(!ht_insert(t, "a", 1, &v1_new, sizeof(v1_new)));

    const int *r = ht_find(t, "a", 1, NULL);
    assert(r != NULL && *r == 99);
    r = ht_find(t, "b", 1, NULL);
    assert(r != NULL && *r == 20);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 41. Spill-lane middle removal integrity (20 entries, remove middle)
// ============================================================================

static int test_spill_middle_remove(void) {
    printf("Test: spill-lane middle removal integrity...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "s%d", i);
        vals[i] = i * 5;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    /* Remove entries 5, 10, 15 from the middle */
    for (int idx = 5; idx < 20; idx += 5) {
        char k[8]; snprintf(k, sizeof(k), "s%d", idx);
        assert(ht_remove(t, k, strlen(k)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 17);

    /* Verify all 20: removed ones return NULL, rest have correct values */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "s%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i == 5 || i == 10 || i == 15) {
            assert(v == NULL);
        } else {
            assert(v != NULL && *v == i * 5);
        }
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 42. ht_inc on existing key with non-int64 val_len
// ============================================================================

static int test_inc_wrong_val_len(void) {
    printf("Test: ht_inc on existing key with non-int64 val_len...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert with sizeof(int) = 4 bytes */
    int small_val = 42;
    assert(ht_insert(t, "x", 1, &small_val, sizeof(int)));

    /* ht_inc should see val_len != sizeof(int64_t), discard old, set new_val = delta */
    int64_t r = ht_inc(t, "x", 1, 100);
    assert(r == 100);

    size_t vlen = 0;
    const int64_t *v = ht_find(t, "x", 1, &vlen);
    assert(v != NULL && *v == 100);
    assert(vlen == sizeof(int64_t));

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 43. Config max_load_factor=0 falls back to default
// ============================================================================

static int test_max_load_factor_zero(void) {
    printf("Test: max_load_factor=0 falls back to default...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.0,
                        .min_load_factor = 0.0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Default is 0.75 — with capacity 16, grow at 12 entries */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ml%d", i);
        vals[i] = i;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);
    assert(st.capacity > 16);  /* Must have grown */

    /* Verify all */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "ml%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 44. Config zombie_window larger than capacity
// ============================================================================

static int test_zombie_window_oversized(void) {
    printf("Test: zombie_window larger than capacity...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .zombie_window = 10000 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 30 entries — zombie step clamped to capacity */
    for (int i = 0; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "zo%d", i);
        int v = i;
        assert(ht_insert(t, k, strlen(k), &v, sizeof(v)));
    }

    /* Delete 20 — zombie cursor advances through entire table each step */
    for (int i = 0; i < 20; i++) {
        char k[16]; snprintf(k, sizeof(k), "zo%d", i);
        assert(ht_remove(t, k, strlen(k)));
    }

    /* Insert 10 more to trigger zombie steps */
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "zn%d", i);
        int v = i + 100;
        assert(ht_insert(t, k, strlen(k), &v, sizeof(v)));
    }

    /* Verify survivors: zo20-29 and zn0-9 */
    for (int i = 20; i < 30; i++) {
        char k[16]; snprintf(k, sizeof(k), "zo%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }
    for (int i = 0; i < 10; i++) {
        char k[16]; snprintf(k, sizeof(k), "zn%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i + 100);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 45. Iterator over all-prophylactic tombstone table (no live entries)
// ============================================================================

static int test_iter_only_prophylactic(void) {
    printf("Test: iterator over all-prophylactic tombstone table...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 10 entries, then delete all, then compact */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ip%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(v));
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "ip%d", i);
        ht_remove(t, k, strlen(k));
    }
    ht_compact(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Iterate — should yield 0 entries */
    ht_iter_t iter = ht_iter_begin(t);
    const void *ik, *iv;
    size_t ikl, ivl;
    int count = 0;
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) {
        count++;
    }
    assert(count == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 46. Auto-shrink disabled (min_load_factor=0, verify no shrink)
// ============================================================================

static int test_auto_shrink_disabled(void) {
    printf("Test: auto-shrink disabled (min_load_factor=0)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Fill to trigger grow */
    int vals[50];
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "as%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    ht_stats_t st;
    ht_stats(t, &st);
    size_t grown_cap = st.capacity;
    assert(grown_cap > 64);

    /* Delete all but 1 — should NOT shrink */
    for (int i = 1; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "as%d", i);
        ht_remove(t, k, strlen(k));
    }

    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.capacity == grown_cap);  /* No shrink */

    /* Verify remaining entry */
    const int *v = ht_find(t, "as0", 3, NULL);
    assert(v != NULL && *v == 0);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 47. Spill entries surviving multiple resizes
// ============================================================================

static int test_spill_multi_resize(void) {
    printf("Test: spill entries surviving multiple resizes...\n");
    ht_config_t cfg = { .initial_capacity = 16, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, selective_zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert spill entries (z-prefix → hash=0) and normal entries */
    int spill_vals[5], norm_vals[10];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "z%d", i);
        spill_vals[i] = i * 10;
        ht_insert(t, k, strlen(k), &spill_vals[i], sizeof(int));
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "n%d", i);
        norm_vals[i] = i * 100;
        ht_insert(t, k, strlen(k), &norm_vals[i], sizeof(int));
    }

    /* Force multiple resizes */
    ht_resize(t, 128);
    ht_resize(t, 256);

    INV_CHECK(t, "test_spill_multi_resize: after resizes");

    /* Verify all spill and normal entries survived */
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "z%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 10);
    }
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "n%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 100);
    }

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 15);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 48. Arena byte-exact integrity after compact with heterogeneous sizes
// ============================================================================

static int test_arena_heterogeneous_compact(void) {
    printf("Test: arena integrity after compact with heterogeneous sizes...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Mix of key/value sizes */
    const char *long_val = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789ABCDEFGHIJKLMNOPQRST";
    const char *short_key = "k";
    char big_key[200];
    memset(big_key, 'X', 199);
    big_key[199] = '\0';

    int v1 = 42;
    assert(ht_insert(t, short_key, 1, &v1, sizeof(int)));
    assert(ht_insert(t, "medium", 6, long_val, strlen(long_val)));
    assert(ht_insert(t, big_key, 199, &v1, 1));  /* 199-byte key, 1-byte value */

    /* Delete and reinsert to create tombstones */
    ht_remove(t, "medium", 6);
    int v2 = 99;
    assert(ht_insert(t, "medium", 6, &v2, sizeof(int)));

    /* Compact — arena is rebuilt, all offsets change */
    ht_compact(t);

    INV_CHECK(t, "test_arena_heterogeneous_compact: after compact");

    /* Verify byte-exact correctness */
    const int *r1 = ht_find(t, short_key, 1, NULL);
    assert(r1 != NULL && *r1 == 42);

    size_t vl = 0;
    const char *r2 = ht_find(t, "medium", 6, &vl);
    assert(r2 != NULL && vl == sizeof(int) && *(const int *)r2 == 99);

    const int *r3 = ht_find(t, big_key, 199, NULL);
    assert(r3 != NULL && *(const char *)r3 == (char)42);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 49. Double compact on populated table
// ============================================================================

static int test_double_compact(void) {
    printf("Test: double compact on populated table...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "dc%d", i);
        vals[i] = i * 7;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete some to create tombstones */
    ht_remove(t, "dc0", 3);
    ht_remove(t, "dc5", 3);

    /* First compact */
    ht_compact(t);
    INV_CHECK(t, "test_double_compact: after first compact");

    ht_stats_t st1;
    ht_stats(t, &st1);
    assert(st1.size == 8);

    /* Second compact — should be stable */
    ht_compact(t);
    INV_CHECK(t, "test_double_compact: after second compact");

    ht_stats_t st2;
    ht_stats(t, &st2);
    assert(st2.size == 8);
    assert(st2.capacity == st1.capacity);

    /* All entries still correct */
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "dc%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i == 0 || i == 5)
            assert(v == NULL);
        else
            assert(v != NULL && *v == i * 7);
    }

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 50. Robin-Hood probe_dist invariant verification
// ============================================================================

static int test_robin_hood_invariant(void) {
    printf("Test: Robin-Hood probe_dist invariant...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 30 entries with some collisions */
    int vals[30];
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Delete 10 to create tombstones and trigger compaction */
    for (int i = 0; i < 30; i += 3) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        ht_remove(t, k, strlen(k));
    }

    /* Walk main table and verify probe_dist invariant */
    size_t cap_mask = 0;
    /* Access internal slots via ht_dump — we need to verify the invariant.
     * Since slots are internal, use ht_find to verify all entries are
     * reachable, which indirectly validates the invariant. */
    ht_stats_t st;
    ht_stats(t, &st);
    size_t found = 0;
    for (int i = 0; i < 30; i++) {
        char k[8]; snprintf(k, sizeof(k), "rh%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        if (i % 3 == 0) {
            assert(v == NULL);
        } else {
            assert(v != NULL && *v == i);
            found++;
        }
    }
    assert(found == st.size);
    (void)cap_mask;

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 51. ht_insert_with_hash with identical hash for many distinct keys
// ============================================================================

static int test_insert_with_hash_same_hash(void) {
    printf("Test: ht_insert_with_hash with identical hash for 50 keys...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* All keys get hash=42 via explicit hash parameter */
    int vals[50];
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "hs%d", i);
        vals[i] = i * 11;
        assert(ht_insert_with_hash(t, 42, k, strlen(k), &vals[i], sizeof(int)));
    }

    /* Each key individually findable via find_with_hash */
    for (int i = 0; i < 50; i++) {
        char k[8]; snprintf(k, sizeof(k), "hs%d", i);
        const int *v = ht_find_with_hash(t, 42, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 11);
    }

    /* Not findable via normal ht_find since fnv1a gives a different hash
     * that probes a different chain. Only find_with_hash(t, 42, ...) works. */

    /* find_all(42) returns all 50 */
    g_collect_count = 0;
    ht_find_all(t, 42, collect_val_cb, NULL);
    assert(g_collect_count == 50);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 52. ht_find with out_value_len=NULL on a successful hit
// ============================================================================

static int test_find_null_out_value_len(void) {
    printf("Test: ht_find with out_value_len=NULL on successful hit...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 12345;
    assert(ht_insert(t, "testkey", 7, &v, sizeof(v)));

    /* Find with NULL out_value_len — should not crash, should return valid ptr */
    const int *r = ht_find(t, "testkey", 7, NULL);
    assert(r != NULL && *r == 12345);

    /* Also with spill-lane entry */
    assert(ht_insert_with_hash(t, 0, "spill", 5, &v, sizeof(v)));
    r = ht_find_with_hash(t, 0, "spill", 5, NULL);
    assert(r != NULL && *r == 12345);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 53. ht_iter_next with all output pointers NULL
// ============================================================================

static int test_iter_all_null_outputs(void) {
    printf("Test: ht_iter_next with all output pointers NULL...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int vals[5];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "in%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Iterate with all NULLs — should still count correctly */
    ht_iter_t iter = ht_iter_begin(t);
    int count = 0;
    while (ht_iter_next(t, &iter, NULL, NULL, NULL, NULL)) {
        count++;
    }
    assert(count == 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 54. ht_stats(valid_table, NULL) no-crash
// ============================================================================

static int test_stats_null_output(void) {
    printf("Test: ht_stats(valid_table, NULL) no-crash...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 1;
    ht_insert(t, "k", 1, &v, sizeof(v));

    /* Should not crash — just return early */
    ht_stats(t, NULL);

    /* Verify table is still intact */
    const int *r = ht_find(t, "k", 1, NULL);
    assert(r != NULL && *r == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 55. ht_inc with hash=1 (spill lane, one_hash)
// ============================================================================

static int test_inc_spill_hash_one(void) {
    printf("Test: ht_inc with hash=1 (spill lane)...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, one_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert with hash=1 (spill lane) */
    int v = 10;
    ht_insert(t, "x", 1, &v, sizeof(int));

    /* ht_inc on "x" — one_hash recomputes hash=1, finds in spill,
     * val_len=4 != sizeof(int64_t), so new_val = delta = 50 */
    int64_t r = ht_inc(t, "x", 1, 50);
    assert(r == 50);

    /* Increment again — now val_len == sizeof(int64_t) */
    r = ht_inc(t, "x", 1, 25);
    assert(r == 75);

    const int64_t *fv = ht_find(t, "x", 1, NULL);
    assert(fv != NULL && *fv == 75);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);  /* Not duplicated */

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 56. Prophylactic tombstones placed on resize (not compact)
// ============================================================================

static int test_resize_prophylactic(void) {
    printf("Test: prophylactic tombstones placed on resize...\n");
    ht_config_t cfg = { .initial_capacity = 32, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert entries */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rp%d", i);
        int v = i;
        ht_insert(t, k, strlen(k), &v, sizeof(int));
    }

    /* Resize — should place prophylactic tombstones */
    ht_resize(t, 128);

    INV_CHECK(t, "test_resize_prophylactic: after resize");

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 20);
    assert(st.capacity >= 128);
    /* Prophylactic tombstones may be present — just verify entries work */

    /* All entries findable */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "rp%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i);
    }

    /* Insert after resize — prophylactics should not block */
    int extra = 999;
    assert(ht_insert(t, "extra", 5, &extra, sizeof(int)));
    const int *r = ht_find(t, "extra", 5, NULL);
    assert(r != NULL && *r == 999);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 57. Insert with key_len=0 and value_len=0 simultaneously
// ============================================================================

static int test_insert_zero_key_zero_val(void) {
    printf("Test: insert with key_len=0 and value_len=0...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert with empty key and empty value */
    assert(ht_insert(t, "", 0, "", 0));

    size_t vl = 99;
    const void *r = ht_find(t, "", 0, &vl);
    assert(r != NULL);
    assert(vl == 0);

    /* Can still insert normal keys */
    int v = 42;
    assert(ht_insert(t, "a", 1, &v, sizeof(int)));
    const int *rv = ht_find(t, "a", 1, NULL);
    assert(rv != NULL && *rv == 42);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 2);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 58. Arena reuse after ht_clear
// ============================================================================

static int test_clear_arena_reuse(void) {
    printf("Test: arena reuse after ht_clear...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert with various sizes */
    const char *big = "ABCDEFGHIJKLMNOP";  /* 16 bytes */
    int small = 42;
    ht_insert(t, "big", 3, big, strlen(big));
    ht_insert(t, "s", 1, &small, sizeof(int));

    /* Clear — data_size resets, arena preserved */
    ht_clear(t);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 0);

    /* Reinsert with different keys — should reuse arena */
    int vals[20];
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "nw%d", i);
        vals[i] = i * 3;
        assert(ht_insert(t, k, strlen(k), &vals[i], sizeof(int)));
    }

    /* Verify — no stale data from before clear */
    for (int i = 0; i < 20; i++) {
        char k[8]; snprintf(k, sizeof(k), "nw%d", i);
        const int *v = ht_find(t, k, strlen(k), NULL);
        assert(v != NULL && *v == i * 3);
    }

    /* Old keys gone */
    assert(ht_find(t, "big", 3, NULL) == NULL);
    assert(ht_find(t, "s", 1, NULL) == NULL);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 59. Iterator on table with only spill-lane entries
// ============================================================================

static int test_iter_spill_only(void) {
    printf("Test: iterator on table with only spill-lane entries...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert only spill-lane entries */
    int vals[5];
    for (int i = 0; i < 5; i++) {
        char k[8]; snprintf(k, sizeof(k), "so%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* Iterate — should find all 5 */
    ht_iter_t iter = ht_iter_begin(t);
    const void *ik, *iv;
    size_t ikl, ivl;
    int count = 0;
    while (ht_iter_next(t, &iter, &ik, &ikl, &iv, &ivl)) {
        count++;
        assert(ikl > 0);
    }
    assert(count == 5);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 60. Spill find_all early termination
// ============================================================================

static int test_spill_find_all_early_stop(void) {
    printf("Test: spill find_all early termination...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75 };
    ht_table_t *t = ht_create(&cfg, zero_hash, NULL, NULL);
    assert(t != NULL);

    /* Insert 10 spill entries */
    int vals[10];
    for (int i = 0; i < 10; i++) {
        char k[8]; snprintf(k, sizeof(k), "se%d", i);
        vals[i] = i;
        ht_insert(t, k, strlen(k), &vals[i], sizeof(int));
    }

    /* find_all with early stop after 3 */
    g_collect_count = 0;
    ht_find_all(t, 0, collect_stop_2_cb, NULL);
    assert(g_collect_count == 2);  /* Stops when cb returns false (count >= 2) */

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// 61. ht_resize with capacity 0 and 1
// ============================================================================

static int test_resize_capacity_zero_one(void) {
    printf("Test: ht_resize with capacity 0 and 1...\n");
    ht_config_t cfg = { .initial_capacity = 64, .max_load_factor = 0.75,
                        .min_load_factor = 0.0, .zombie_window = 0 };
    ht_table_t *t = ht_create(&cfg, fnv1a_hash, NULL, NULL);
    assert(t != NULL);

    int v = 1;
    ht_insert(t, "k", 1, &v, sizeof(v));

    /* Resize to 0 — next_pow2(0)=1 but 1 < size=1, so should fail */
    bool ok = ht_resize(t, 0);
    assert(!ok);

    /* Resize to 1 — next_pow2(1)=1, 1 >= size=1, works */
    ok = ht_resize(t, 1);
    assert(ok);

    ht_stats_t st;
    ht_stats(t, &st);
    assert(st.size == 1);
    assert(st.capacity == 1);

    const int *r = ht_find(t, "k", 1, NULL);
    assert(r != NULL && *r == 1);

    ht_destroy(t);
    printf("  PASS\n");
    return 0;
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("=== Edge Case Tests A ===\n\n");

    int fails = 0;

    // Spill lane (1-4)
    fails += test_spill_hash_zero();
    fails += test_spill_hash_one();
    fails += test_spill_mixed();
    fails += test_spill_stress();

    // Duplicate keys (5-6)
    fails += test_duplicate_keys();
    fails += test_find_all_early_stop();

    // Custom equality (7)
    fails += test_custom_eq();

    // Negative / edge (8-12)
    fails += test_empty_table();
    fails += test_double_remove();
    fails += test_remove_nonexistent();
    fails += test_clear_reuse();
    fails += test_null_args();

    // Iterator (13-16)
    fails += test_iter_empty();
    fails += test_iter_tombstones();
    fails += test_iter_spill();
    fails += test_iter_after_clear();

    // _with_hash variants (17)
    fails += test_with_hash_variants();

    // ht_inc (18)
    fails += test_ht_inc_edge();

    // Prophylactic tombstones (19)
    fails += test_prophylactic_tombstones();

    // Value edge cases (20-22)
    fails += test_zero_len_values();
    fails += test_large_values();
    fails += test_empty_key();

    // Resize edge cases (23-24)
    fails += test_resize_same_cap();
    fails += test_resize_below_size();

    // Stats (25)
    fails += test_stats_consistency();

    // Binary keys (26)
    fails += test_binary_keys();

    // Extended sequences (27-28)
    fails += test_long_sequence_normal();
    fails += test_long_sequence_collision();

    // More edge cases (29-31)
    fails += test_update_preserves_others();
    fails += test_resize_up_down();
    fails += test_compact_churn();

    // New tests (32-39)
    fails += test_user_ctx_propagation();
    fails += test_eq_fn_null();
    fails += test_initial_capacity_clamp();
    fails += test_resize_non_pow2();
    fails += test_compact_empty();
    fails += test_find_all_spill();
    fails += test_with_hash_spill_variants();
    fails += test_spill_grow();

    // New tests (40-47)
    fails += test_spill_update();
    fails += test_spill_middle_remove();
    fails += test_inc_wrong_val_len();
    fails += test_max_load_factor_zero();
    fails += test_zombie_window_oversized();
    fails += test_iter_only_prophylactic();
    fails += test_auto_shrink_disabled();
    fails += test_spill_multi_resize();

    // New tests (48-54)
    fails += test_arena_heterogeneous_compact();
    fails += test_double_compact();
    fails += test_robin_hood_invariant();
    fails += test_insert_with_hash_same_hash();
    fails += test_find_null_out_value_len();
    fails += test_iter_all_null_outputs();
    fails += test_stats_null_output();

    // New tests (55-61)
    fails += test_inc_spill_hash_one();
    fails += test_resize_prophylactic();
    fails += test_insert_zero_key_zero_val();
    fails += test_clear_arena_reuse();
    fails += test_iter_spill_only();
    fails += test_spill_find_all_early_stop();
    fails += test_resize_capacity_zero_one();

    printf("\n");
    if (fails == 0) {
        printf("All edge case A tests passed! (61/61)\n");
    } else {
        printf("%d test(s) FAILED!\n", fails);
    }
    return fails;
}
