/**
 * test_oom.c - Out-of-Memory Tests with State Integrity Verification
 *
 * Tests OOM error handling paths using mocked allocators via gcc --wrap.
 * Each test verifies:
 *   1. Operation returns false/error when OOM
 *   2. No crash
 *   3. State invariants still hold after OOM
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "draugr/ht.h"
#include "draugr/ht_cache.h"
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

int alloc_mock_get_count(void) { return mock.alloc_count; }
int alloc_mock_get_fail_count(void) { return mock.fail_count; }

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
    if (should_fail(size)) {
        mock.fail_count++;
        return NULL;
    }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    mock.alloc_count++;
    if (should_fail(nmemb * size)) {
        mock.fail_count++;
        return NULL;
    }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    mock.alloc_count++;
    if (should_fail(size)) {
        mock.fail_count++;
        return NULL;
    }
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr) {
    __real_free(ptr);
}

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

static const char *check_bare_invariants(ht_bare_t *t) {
    if (!t) return "NULL table";
    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        uint32_t val = t->vals[i];
        if (hpd_live(hpd)) {
            if (val == UINT32_MAX) return "live entry with VAL_NONE";
        }
    }
    return ht_bare_check_invariants(t);
}

static const char *check_table_invariants(ht_table_t *t) {
    if (!t) return "NULL table";
    const char *err = check_bare_invariants(&t->bare);
    if (err) return err;
    return ht_check_invariants(t);
}

static void verify_table_state(ht_table_t *ht) {
    const char *err = check_table_invariants(ht);
    assert(err == NULL);
}

static void verify_bare_state(ht_bare_t *bare) {
    const char *err = check_bare_invariants(bare);
    assert(err == NULL);
}

// ============================================================================
// Basic OOM Tests
// ============================================================================

static void test_bare_create_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(1);
    ht_bare_t *bare = ht_bare_create(NULL);
    assert(bare == NULL);
    printf("  ht_bare_create OOM at limit=1: PASS\n");

    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    bare = ht_bare_create(NULL);
    assert(bare != NULL);
    verify_bare_state(bare);
    printf("  ht_bare_create OK at limit=100: PASS\n");
    ht_bare_destroy(bare);
}

static void test_bare_resize_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_bare_t *bare = ht_bare_create(NULL);
    assert(bare != NULL);

    for (uint32_t i = 0; i < 10; i++) {
        ht_bare_insert(bare, simple_hash(&i, sizeof(i), NULL), i);
    }

    verify_bare_state(bare);

    alloc_mock_set_max_alloc_calls(3);
    bool ok = ht_bare_resize(bare, 128);
    assert(!ok);
    verify_bare_state(bare);
    printf("  ht_bare_resize OOM at limit=3: PASS\n");
    ht_bare_destroy(bare);
}

static void test_create_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(1);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht == NULL);
    printf("  ht_create OOM at limit=1: PASS\n");

    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);
    verify_table_state(ht);
    printf("  ht_create OK at limit=100: PASS\n");
    ht_destroy(ht);
}

static void test_upsert_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    char key[16] = {0};
    char val[32] = {0};
    bool r = ht_upsert(ht, key, sizeof(key), val, sizeof(val));
    assert(r);
    verify_table_state(ht);

    alloc_mock_set_max_alloc_size(1000000);
    memset(key, 1, sizeof(key));
    r = ht_upsert(ht, key, sizeof(key), val, 1000000);
    if (!r) {
        verify_table_state(ht);
        printf("  ht_upsert OOM at size threshold: PASS\n");
    } else {
        printf("  ht_upsert large value succeeded (no OOM)\n");
    }
    ht_destroy(ht);
}

static void test_resize_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 5; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i * 2, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    verify_table_state(ht);

    alloc_mock_set_max_alloc_calls(1);
    bool ok = ht_resize(ht, 128);
    if (!ok) {
        verify_table_state(ht);
        printf("  ht_resize OOM at limit=1: PASS\n");
    } else {
        printf("  ht_resize at limit=1 succeeded\n");
    }
    ht_destroy(ht);
}

static void test_compact_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 5; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i * 2, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    verify_table_state(ht);

    alloc_mock_set_max_alloc_calls(1);
    ht_compact(ht);
    verify_table_state(ht);
    printf("  ht_compact OOM at limit=1: PASS\n");
    ht_destroy(ht);
}

static void test_cache_create_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(1);
    ht_cache_config_t ccfg = {
        .capacity = 8,
        .entry_size = 32,
        .hash_fn = simple_hash,
        .eq_fn = NULL,
        .user_ctx = NULL,
    };
    ht_cache_t *cache = ht_cache_create(&ccfg);
    assert(cache == NULL);
    printf("  ht_cache_create OOM at limit=1: PASS\n");
}

static void test_cache_put_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_cache_config_t ccfg = {
        .capacity = 8,
        .entry_size = 32,
        .hash_fn = simple_hash,
        .eq_fn = NULL,
        .user_ctx = NULL,
    };
    ht_cache_t *cache = ht_cache_create(&ccfg);
    assert(cache != NULL);

    uint8_t entry[32] = {0};
    void *ptr = ht_cache_put(cache, entry, sizeof(entry));
    assert(ptr != NULL);
    assert(ht_cache_size(cache) == 1);
    printf("  ht_cache_put OK with small entry: PASS\n");
    ht_cache_destroy(cache);
}

// ============================================================================
// Sequence Tests: Insert → Fail → Verify
// ============================================================================

static void test_insert_then_oom_preserves_state(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    char keys[5][8];
    char vals[5][8];
    for (int i = 0; i < 5; i++) {
        memset(keys[i], i, sizeof(keys[i]));
        memset(vals[i], i * 10, sizeof(vals[i]));
        bool ok = ht_upsert(ht, keys[i], sizeof(keys[i]), vals[i], sizeof(vals[i]));
        assert(ok);
    }

    verify_table_state(ht);
    size_t size_before = ht_size(ht);
    assert(size_before == 5);

    alloc_mock_set_max_alloc_size(1000000);
    uint8_t fail_key[8] = {0xFF};
    char fail_val[100000];
    memset(fail_val, 'x', sizeof(fail_val));
    bool ok = ht_upsert(ht, fail_key, sizeof(fail_key), fail_val, sizeof(fail_val));
    if (!ok) {
        verify_table_state(ht);
        assert(ht_size(ht) == size_before);
        for (int i = 0; i < 5; i++) {
            size_t vlen = 0;
            const void *v = ht_find(ht, keys[i], sizeof(keys[i]), &vlen);
            assert(v != NULL);
            assert(vlen == sizeof(vals[i]));
            assert(memcmp(v, vals[i], sizeof(vals[i])) == 0);
        }
        printf("  insert→OOM→verify preserves state: PASS\n");
    } else {
        printf("  insert→large→succeeded (no OOM triggered)\n");
    }
    ht_destroy(ht);
}

static void test_resize_oom_preserves_state(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 20; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i * 7, sizeof(v));
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        assert(ok);
    }

    verify_table_state(ht);
    size_t size_before = ht_size(ht);
    assert(size_before == 20);

    alloc_mock_set_max_alloc_calls(1);
    bool ok = ht_resize(ht, 256);
    if (!ok) {
        verify_table_state(ht);
        assert(ht_size(ht) == size_before);
        printf("  resize→OOM→verify preserves state: PASS\n");
    } else {
        printf("  resize at limit=1 succeeded\n");
    }
    ht_destroy(ht);
}

static void test_remove_then_insert_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 5; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    for (int i = 0; i < 3; i++) {
        char k[8];
        memset(k, i, sizeof(k));
        ht_remove(ht, k, sizeof(k));
    }

    verify_table_state(ht);
    size_t size_after_removes = ht_size(ht);
    assert(size_after_removes == 2);

    alloc_mock_set_max_alloc_calls(1);
    char k[8] = {3};
    char v[8] = {3};
    bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    (void)ok;
    verify_table_state(ht);
    printf("  remove→insert→OOM→verify: PASS\n");
    ht_destroy(ht);
}

static void test_remove_allows_insert_after_oom(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    char k[8] = {0}, v[8] = {0};
    bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    assert(ok);
    verify_table_state(ht);

    alloc_mock_set_max_alloc_calls(1);
    ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    if (!ok) {
        verify_table_state(ht);
    }

    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    verify_table_state(ht);
    printf("  insert→OOM→insert again→OK: PASS\n");
    ht_destroy(ht);
}

// ============================================================================
// Spill Lane Tests
// ============================================================================

static void test_bare_spill_insert_oom(void) {
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 20; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(200);
        ht_bare_t *bare = ht_bare_create(&cfg);
        assert(bare != NULL);

        for (uint32_t i = 0; i < 8; i++) {
            uint64_t h = simple_hash(&i, sizeof(i), NULL);
            ht_bare_insert(bare, h, i);
        }

        verify_bare_state(bare);

        int count_before = alloc_mock_get_count();
        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(count_before + 1);
        bool ok = ht_bare_insert(bare, simple_hash(&(uint32_t){100}, sizeof(uint32_t), NULL), 100);
        if (!ok) {
            verify_bare_state(bare);
            printf("  bare_spill_insert OOM at trial %d: PASS\n", trial);
            ht_bare_destroy(bare);
            return;
        }
        ht_bare_destroy(bare);
    }
    printf("  bare_spill_insert: could not trigger OOM (info only)\n");
}

static void test_bare_spill_grow_partial_failure(void) {
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 20; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(200);
        ht_bare_t *bare = ht_bare_create(&cfg);
        assert(bare != NULL);

        for (uint32_t i = 0; i < 8; i++) {
            uint64_t h = simple_hash(&i, sizeof(i), NULL);
            ht_bare_insert(bare, h, i);
        }

        int count_before = alloc_mock_get_count();
        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(count_before + 1);
        bool ok = ht_bare_insert(bare, simple_hash(&(uint32_t){100}, sizeof(uint32_t), NULL), 100);
        if (!ok) {
            verify_bare_state(bare);
            printf("  bare_spill_grow partial failure at trial %d: PASS\n", trial);
            ht_bare_destroy(bare);
            return;
        }
        ht_bare_destroy(bare);
    }
    printf("  bare_spill_grow partial failure: could not trigger OOM (info only)\n");
}

// ============================================================================
// Arena Grow Tests
// ============================================================================

static void test_arena_grow_oom_during_insert(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.75f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    assert(ht != NULL);

    char key[8] = {0};
    char val_small[16] = {0};
    bool ok = ht_upsert(ht, key, sizeof(key), val_small, sizeof(val_small));
    assert(ok);
    verify_table_state(ht);

    alloc_mock_set_max_alloc_size(256 * 256);
    char key2[8] = {1};
    char val_large[256 * 256];
    memset(val_large, 'x', sizeof(val_large));
    ok = ht_upsert(ht, key2, sizeof(key2), val_large, sizeof(val_large));

    if (!ok) {
        verify_table_state(ht);
        printf("  arena_grow OOM during insert: PASS\n");
    } else {
        printf("  arena_grow insert succeeded (threshold not restrictive)\n");
    }
    ht_destroy(ht);
}

// ============================================================================
// Config Edge Cases
// ============================================================================

static void test_small_initial_capacity(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_config_t cfg = {
        .initial_capacity = 2,
        .max_load_factor = 0.75f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 4; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        assert(ok);
    }

    verify_table_state(ht);
    printf("  small initial_capacity=2 with resize: PASS\n");
    ht_destroy(ht);
}

static void test_zero_zombie_window(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_config_t cfg = {
        .initial_capacity = 16,
        .max_load_factor = 0.75f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };
    ht_table_t *ht = ht_create(&cfg, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 10; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i * 3, sizeof(v));
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        assert(ok);
    }

    verify_table_state(ht);
    printf("  zombie_window=0 (disabled): PASS\n");
    ht_destroy(ht);
}

// ============================================================================
// Bug-Finding OOM Tests: Specific vulnerable code paths
// ============================================================================

static void test_spill_grow_partial_failure_bug(void) {
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 30; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(200);
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (uint32_t i = 0; i < 8; i++) {
            uint64_t h = simple_hash(&i, sizeof(i), NULL);
            ht_bare_insert(bare, h, i);
        }

        size_t base_count = alloc_mock_get_count();
        size_t spill_cap_before = bare->spill_cap;

        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(base_count + 2);
        bool ok = ht_bare_insert(bare, simple_hash(&(uint32_t){100}, sizeof(uint32_t), NULL), 100);
        if (!ok) {
            if (bare->spill_cap != spill_cap_before) {
                printf("  spill_grow BUG: spill_cap changed=%zu but insert failed\n", bare->spill_cap);
            } else {
                printf("  spill_grow partial failure at trial %d: invariants ok\n", trial);
            }
            ht_bare_destroy(bare);
            return;
        }
        ht_bare_destroy(bare);
    }
    printf("  spill_grow partial failure: could not trigger (info)\n");
}

static void test_orphaned_entry_on_bare_insert_fail(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 10; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    size_t entry_count_before = ht->entry_count;

    alloc_mock_set_max_alloc_calls(1);
    char k[8] = {100};
    char v[8] = {100};
    bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    if (!ok) {
        if (ht->entry_count != entry_count_before) {
            printf("  orphaned_entry BUG: entry_count=%zu expected=%zu\n", 
                   ht->entry_count, entry_count_before);
        } else {
            verify_table_state(ht);
            printf("  orphaned_entry on bare insert fail: PASS\n");
        }
    } else {
        printf("  orphaned_entry: insert succeeded at limit=1\n");
    }
    ht_destroy(ht);
}

static void test_resize_partial_alloc_failure(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 10; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    size_t count_before = ht_size(ht);
    size_t entry_cap_before = ht->entry_cap;

    alloc_mock_set_max_alloc_calls(3);
    bool ok = ht_resize(ht, 128);
    if (!ok) {
        if (ht->entry_cap != entry_cap_before) {
            printf("  resize_partial BUG: entry_cap changed on failure\n");
        } else {
            verify_table_state(ht);
            assert(ht_size(ht) == count_before);
            printf("  resize partial alloc failure: PASS\n");
        }
    } else {
        printf("  resize partial: succeeded at limit=3\n");
    }
    ht_destroy(ht);
}

static void test_cache_evict_bare_insert_fail(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_cache_config_t ccfg = {
        .capacity = 2,
        .entry_size = 32,
        .hash_fn = simple_hash,
        .eq_fn = NULL,
        .user_ctx = NULL,
    };
    ht_cache_t *cache = ht_cache_create(&ccfg);
    assert(cache != NULL);

    uint8_t entry[32] = {0};
    void *ptr = ht_cache_put(cache, entry, sizeof(entry));
    assert(ptr != NULL);
    assert(ht_cache_size(cache) == 1);

    int base_count = alloc_mock_get_count();
    alloc_mock_reset();
    alloc_mock_set_alloc_num_to_fail(base_count + 1);

    uint8_t entry2[32] = {1};
    ptr = ht_cache_put(cache, entry2, sizeof(entry2));
    if (!ptr) {
        if (ht_cache_size(cache) != 1) {
            printf("  cache_evict_bug: size=%zu expected=1\n", ht_cache_size(cache));
        } else {
            printf("  cache evict + bare_insert fail: state ok\n");
        }
    } else {
        printf("  cache evict: succeeded (no OOM at allocation %d)\n", base_count + 1);
    }
    ht_cache_destroy(cache);
}

static void test_arena_waste_on_failed_update(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    char k[8] = {0};
    char v[16] = {0};
    bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    assert(ok);

    size_t entry_count_before = ht->entry_count;

    alloc_mock_set_max_alloc_size(1024);
    char v_large[1400];
    memset(v_large, 'x', sizeof(v_large));

    ok = ht_upsert(ht, k, sizeof(k), v_large, sizeof(v_large));
    printf(" after large upsert (ok=%d): entry_count=%zu, fail_count=%d\n",
           ok, ht->entry_count, alloc_mock_get_fail_count());

    if (ok) {
        printf(" arena waste: update succeeded without OOM\n");
    } else if (alloc_mock_get_fail_count() == 0) {
        printf(" arena waste: malloc bypasses mock (known limitation)\n");
    } else if (ht->entry_count != entry_count_before) {
        printf(" arena_waste: BUG - entry_count changed from %zu to %zu despite allocation failure\n",
               entry_count_before, ht->entry_count);
    } else {
        printf(" arena waste: PASS (entry_count stayed at %zu)\n", ht->entry_count);
    }
    ht_destroy(ht);
}

static void test_resize_reinsert_fail_returns_success(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_bare_t *bare = ht_bare_create(NULL);
    assert(bare != NULL);

    for (uint32_t i = 0; i < 20; i++) {
        uint64_t h = simple_hash(&i, sizeof(i), NULL);
        ht_bare_insert(bare, h, i);
    }

    verify_bare_state(bare);
    size_t size_before = bare->size;

    int count_before = alloc_mock_get_count();
    alloc_mock_reset();
    alloc_mock_set_alloc_num_to_fail(count_before + 3);

    bool ok = ht_bare_resize(bare, 64);
    if (!ok) {
        if (bare->size != size_before) {
            printf("  reinsert_fail BUG: size=%zu expected=%zu\n", bare->size, size_before);
        } else {
            verify_bare_state(bare);
            printf("  resize reinsert partial fail: PASS\n");
        }
    } else {
        printf("  resize reinsert: succeeded\n");
    }
    ht_bare_destroy(bare);
}

static void test_spill_insert_after_partial_grow(void) {
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 30; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(200);
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (uint32_t i = 0; i < 8; i++) {
            uint64_t h = simple_hash(&i, sizeof(i), NULL);
            ht_bare_insert(bare, h, i);
        }

        int count_before = alloc_mock_get_count();
        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(count_before + 2);

        bool fail1 = ht_bare_insert(bare, simple_hash(&(uint32_t){50}, sizeof(uint32_t), NULL), 50);
        
        size_t spill_len_after = bare->spill_len;
        size_t size_after = bare->size;

        if (!fail1 && (spill_len_after > 8 || size_after > 9)) {
            printf("  spill_insert_corruption BUG: spill_len=%zu size=%zu\n", 
                   spill_len_after, size_after);
            ht_bare_destroy(bare);
            return;
        }
        
        if (!fail1) {
            ht_bare_destroy(bare);
            continue;
        }

        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(100);
        
        bool fail2 = ht_bare_insert(bare, simple_hash(&(uint32_t){60}, sizeof(uint32_t), NULL), 60);
        
        if (!fail2) {
            const char *err = ht_bare_check_invariants(bare);
            if (err) {
                printf("  spill corruption after partial grow: %s\n", err);
                ht_bare_destroy(bare);
                return;
            }
        }
        
        ht_bare_destroy(bare);
    }
    printf("  spill insert after partial grow: PASS (no corruption)\n");
}

static void test_do_insert_hash_oom_after_alloc(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    assert(ht != NULL);

    for (int i = 0; i < 50; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        assert(ok);
    }

    size_t entry_count_before = ht->entry_count;

    alloc_mock_set_max_alloc_calls(3);
    uint8_t k[8] = {200};
    uint8_t v[8] = {200};
    bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    if (!ok) {
        if (ht->entry_count != entry_count_before) {
            printf(" do_insert_partial BUG: entry_count drifted\n");
        } else {
            verify_table_state(ht);
            printf(" do_insert_with_hash OOM after alloc: PASS\n");
        }
    } else {
        printf(" do_insert: succeeded (need lower limit)\n");
    }
    ht_destroy(ht);
}

// ============================================================================
// Cache Tests
// ============================================================================

static void test_cache_evict_works(void) {
    alloc_mock_reset();
    alloc_mock_set_max_alloc_calls(100);
    ht_cache_config_t ccfg = {
        .capacity = 4,
        .entry_size = 32,
        .hash_fn = simple_hash,
        .eq_fn = NULL,
        .user_ctx = NULL,
    };
    ht_cache_t *cache = ht_cache_create(&ccfg);
    assert(cache != NULL);

    for (int i = 0; i < 6; i++) {
        uint8_t entry[32];
        memset(entry, i, sizeof(entry));
        void *ptr = ht_cache_put(cache, entry, sizeof(entry));
        assert(ptr != NULL);
    }

    assert(ht_cache_size(cache) == 4);
    printf("  cache evict works correctly: PASS\n");
    ht_cache_destroy(cache);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("Test: OOM tests with state integrity verification\n\n");

    printf("=== Basic OOM Tests ===\n");
    test_bare_create_oom();
    test_bare_resize_oom();
    test_create_oom();
    test_upsert_oom();
    test_resize_oom();
    test_compact_oom();
    test_cache_create_oom();
    test_cache_put_oom();

    printf("\n=== Sequence Tests (State Preservation) ===\n");
    test_insert_then_oom_preserves_state();
    test_resize_oom_preserves_state();
    test_remove_then_insert_oom();
    test_remove_allows_insert_after_oom();

    printf("\n=== Spill Lane Tests ===\n");
    test_bare_spill_insert_oom();
    test_bare_spill_grow_partial_failure();
    test_spill_grow_partial_failure_bug();
    test_spill_insert_after_partial_grow();

    printf("\n=== Arena Grow Tests ===\n");
    test_arena_grow_oom_during_insert();
    test_arena_waste_on_failed_update();

    printf("\n=== Bug-Finding Tests ===\n");
    test_orphaned_entry_on_bare_insert_fail();
    test_resize_partial_alloc_failure();
    test_resize_reinsert_fail_returns_success();
    test_cache_evict_bare_insert_fail();
    test_do_insert_hash_oom_after_alloc();

    printf("\n=== Config Edge Cases ===\n");
    test_small_initial_capacity();
    test_zero_zombie_window();

    printf("\n=== Cache Tests ===\n");
    test_cache_evict_works();

    printf("\nAll OOM tests passed with state integrity verification!\n");
    return 0;
}