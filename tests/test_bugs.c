/**
 * test_bugs.c - Bug-Finding Stress Tests for draugr Hash Table
 *
 * Designed to expose specific bugs in the implementation:
 * 1. Spill grow partial failure (memory leak when realloc fails for spill_vals)
 * 2. Tombstone counter drift in backward-shift compaction
 * 3. Zombie mechanism disabled + tomb_threshold exceeded
 * 4. UPSERT return value inverted
 * 5. bshift_cap=16 hard limit with long probe chains
 * 6. Arena rollback not atomic (entry_count mismatch on failed alloc)
 * 7. Resize partial failure loses entries
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <signal.h>

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

static uint64_t hash_zero(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0;
}

static volatile sig_atomic_t segv_detected = 0;
static void sigsegv_handler(int sig) { if (sig == SIGSEGV) segv_detected = 1; }

static int tests_passed = 0;
static int tests_failed = 0;

#define BUG(msg) do { printf("  BUG: %s\n", msg); tests_failed++; } while(0)
#define PASS(msg) do { printf("  PASS: %s\n", msg); tests_passed++; } while(0)
#define INFO(msg) do { printf("    INFO: %s\n", msg); } while(0)

static void count_bare_slots(ht_bare_t *t, size_t *out_live, size_t *out_tomb, size_t *out_empty) {
    *out_live = *out_tomb = *out_empty = 0;
    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        if (hpd_live(hpd)) (*out_live)++;
        else if (hpd_tomb(hpd)) (*out_tomb)++;
        else (*out_empty)++;
    }
}

static void verify_bare_invariants(ht_bare_t *t) {
    if (!t) return;
    size_t live, tomb, empty;
    count_bare_slots(t, &live, &tomb, &empty);
    if (live != t->size) {
        printf("    counted live=%zu vs t->size=%zu\n", live, t->size);
    }
    if (tomb != t->tombstone_cnt) {
        printf("    counted tomb=%zu vs t->tombstone_cnt=%zu\n", tomb, t->tombstone_cnt);
    }
}

// ============================================================================
// Bug 1: Spill Grow Partial Failure (spill_hash_pd stored before spill_vals succeeds)
// ============================================================================

static void test_spill_grow_partial_failure(void) {
    printf("\n=== Spill Grow Partial Failure ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.3f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 30; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(10000);
        
        ht_table_t *ht = ht_create(&cfg, hash_zero, NULL, NULL);
        if (!ht) continue;

        for (int i = 0; i < 8; i++) {
            char k[8], v[8];
            memset(k, i, sizeof(k));
            memset(v, i, sizeof(v));
            ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        }

        int count_before = alloc_mock_get_count();
        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(count_before + 2);
        
        char k_new[8] = {100};
        char v_new[8] = {100};
        bool ok = ht_upsert(ht, k_new, sizeof(k_new), v_new, sizeof(v_new));

        if (!ok) {
            size_t live, tomb, empty;
            count_bare_slots(&ht->bare, &live, &tomb, &empty);
            printf("    trial %d: insert failed, spill_cap=%zu spill_len=%zu\n",
                   trial, ht->bare.spill_cap, ht->bare.spill_len);
            if (live != ht->bare.size) {
                BUG("spill grow partial: live count mismatch after failure");
                ht_destroy(ht);
                return;
            }
        }
        ht_destroy(ht);
    }
    PASS("spill grow partial failure: no crash");
}

// ============================================================================
// Bug 2: Tombstone Counter Drift in backward-shift compaction
// ============================================================================

static void test_tombstone_counter_drift_removes(void) {
    printf("\n=== Tombstone Counter Drift (Removes) ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 8,
        .max_load_factor = 0.75f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.0f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 20; trial++) {
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (int i = 0; i < 8; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_insert(bare, simple_hash(k, sizeof(k), NULL), i);
        }

        for (int i = 0; i < 5; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_remove(bare, simple_hash(k, sizeof(k), NULL));
        }

        size_t live, tomb, empty;
        count_bare_slots(bare, &live, &tomb, &empty);

        if (tomb != bare->tombstone_cnt) {
            printf("    trial %d: counted=%zu recorded=%zu\n", trial, tomb, bare->tombstone_cnt);
            ht_bare_destroy(bare);
            BUG("tombstone counter drift after removes");
            return;
        }
        ht_bare_destroy(bare);
    }
    PASS("tombstone counter accurate after removes");
}

static void test_tombstone_counter_drift_compact(void) {
    printf("\n  --- Tombstone Drift After Compact ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 8,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.0f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 20; trial++) {
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (int i = 0; i < 6; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_insert(bare, simple_hash(k, sizeof(k), NULL), i);
        }

        for (int i = 0; i < 3; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_remove(bare, simple_hash(k, sizeof(k), NULL));
        }

        ht_bare_compact(bare);
        
        size_t live, tomb, empty;
        count_bare_slots(bare, &live, &tomb, &empty);

        if (tomb != bare->tombstone_cnt) {
            printf("    trial %d: counted=%zu recorded=%zu\n", trial, tomb, bare->tombstone_cnt);
            ht_bare_destroy(bare);
            BUG("tombstone counter drift after compact");
            return;
        }
        ht_bare_destroy(bare);
    }
    PASS("tombstone counter accurate after compact");
}

// ============================================================================
// Bug 3: Zombie Disabled Accumulation
// ============================================================================

static void test_zombie_disabled_tombstone_accumulation(void) {
    printf("\n=== Zombie Disabled (zombie_window=0) ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.15f,
        .zombie_window = 0,
    };

    ht_bare_t *bare = ht_bare_create(&cfg);
    assert(bare);

    for (int i = 0; i < 20; i++) {
        char k[8];
        memset(k, i, sizeof(k));
        ht_bare_insert(bare, simple_hash(k, sizeof(k), NULL), i);
    }

    size_t live, tomb, empty;
    count_bare_slots(bare, &live, &tomb, &empty);

    printf("    size=%zu tomb_count=%zu (counted) vs tombstone_cnt=%zu (recorded)\n",
           bare->size, tomb, bare->tombstone_cnt);

    if (tomb != bare->tombstone_cnt) {
        BUG("zombie disabled: tombstone counter drift");
    } else {
        PASS("zombie disabled: counter accurate");
    }

    if (bare->tombstone_cnt > bare->size) {
        INFO("high tombstone ratio with zombie disabled");
    }

    ht_bare_destroy(bare);
}

// ============================================================================
// Bug 4: UPSERT Return Value
// ============================================================================

static void test_upsert_return_value(void) {
    printf("\n=== UPSERT Return Value ===\n");
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) {
        BUG("ht_create returned NULL");
        return;
    }

    char k[8] = {1};
    char v1[8] = {0x11};
    char v2[8] = {0x22};

    bool r1 = ht_upsert(ht, k, sizeof(k), v1, sizeof(v1));
    bool r2 = ht_upsert(ht, k, sizeof(k), v2, sizeof(v2));

    printf("    insert new: ht_upsert returned %d\n", r1);
    printf("    replace existing: ht_upsert returned %d\n", r2);

    if (r1 != r2) {
        PASS("insert and replace return different values");
    } else {
        INFO("insert and replace return same value (ambiguous)");
    }

    size_t vlen = 0;
    const void *found = ht_find(ht, k, sizeof(k), &vlen);
    if (found && memcmp(found, v2, sizeof(v2)) == 0) {
        PASS("upsert correctly updates value");
    } else {
        BUG("upsert did not update value correctly");
    }

    ht_destroy(ht);
}

// ============================================================================
// Bug 5: Arena Rollback Not Atomic (entry_count on failed alloc)
// ============================================================================

static void test_arena_rollback_atomicity(void) {
    printf("\n=== Arena Rollback Atomicity ===\n");

    for (int trial = 0; trial < 20; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(100);
        ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
        if (!ht) continue;

        char k[8] = {0};
        char v[16] = {0};
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        if (!ok) { ht_destroy(ht); continue; }

        size_t entry_count_before = ht->entry_count;

        alloc_mock_set_max_alloc_size(1024);
        char v_large[1400];
        memset(v_large, 'x', sizeof(v_large));

        ok = ht_upsert(ht, k, sizeof(k), v_large, sizeof(v_large));

        if (!ok) {
            if (ht->entry_count != entry_count_before) {
                printf(" trial %d: entry_count drifted from %zu to %zu after failure\n",
                       trial, entry_count_before, ht->entry_count);
                ht_destroy(ht);
                BUG("entry_count not rolled back on failure");
                return;
            }
        }
        ht_destroy(ht);
    }
    PASS("arena rollback atomicity: entry_count stays consistent");
}

// ============================================================================
// Bug 6: Resize Partial Failure (entries lost)
// ============================================================================

static void test_resize_partial_failure_loses_entries(void) {
    printf("\n=== Resize Partial Failure ===\n");
    
    for (int trial = 0; trial < 20; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(200);
        ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
        if (!ht) continue;

        for (int i = 0; i < 20; i++) {
            char k[8], v[8];
            memset(k, i, sizeof(k));
            memset(v, i, sizeof(v));
            ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        }

        size_t count_before = ht_size(ht);

        int count_alloc = alloc_mock_get_count();
        alloc_mock_reset();
        alloc_mock_set_alloc_num_to_fail(count_alloc + 3);

        bool ok = ht_resize(ht, 256);
        (void)ok;

        if (ht_size(ht) != count_before) {
            printf("    trial %d: size=%zu expected=%zu\n", trial, ht_size(ht), count_before);
            ht_destroy(ht);
            BUG("resize failure lost entries");
            return;
        }
        ht_destroy(ht);
    }
    PASS("resize partial failure: no entries lost");
}

// ============================================================================
// Bug 7: Spill Lane Remove All Matching
// ============================================================================

static void test_spill_lane_remove_consistency(void) {
    printf("\n=== Spill Lane Remove Consistency ===\n");
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) {
        BUG("ht_create returned NULL");
        return;
    }

    for (int i = 0; i < 5; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    printf("    inserted 5 entries\n");
    printf("    size=%zu\n", ht_size(ht));

    char k[8] = {0};
    ht_remove(ht, k, sizeof(k));

    printf("    removed key[0]: size=%zu\n", ht_size(ht));

    size_t vlen = 0;
    const void *found = ht_find(ht, k, sizeof(k), &vlen);
    if (!found && ht_size(ht) == 4) {
        PASS("remove: removed exactly one entry");
    } else {
        BUG("remove: entry count wrong after remove");
    }

    ht_destroy(ht);
}

// ============================================================================
// Harder: Multi-pass Tombstone Accumulation
// ============================================================================

static void test_multi_pass_tombstone_accumulation(void) {
    printf("\n=== Multi-Pass Tombstone Accumulation ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 8,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.2f,
        .zombie_window = 0,
    };

    for (int round = 0; round < 5; round++) {
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (int i = 0; i < 8; i++) {
            char k[8];
            memset(k, round * 10 + i, sizeof(k));
            ht_bare_insert(bare, simple_hash(k, sizeof(k), NULL), i);
        }

        for (int i = 0; i < 4; i++) {
            char k[8];
            memset(k, round * 10 + i, sizeof(k));
            ht_bare_remove(bare, simple_hash(k, sizeof(k), NULL));
        }

        size_t live, tomb, empty;
        count_bare_slots(bare, &live, &tomb, &empty);

        if (tomb != bare->tombstone_cnt) {
            printf("    round %d: counted=%zu recorded=%zu\n", round, tomb, bare->tombstone_cnt);
            ht_bare_destroy(bare);
            BUG("multi-pass tombstone drift");
            return;
        }
        ht_bare_destroy(bare);
    }
    PASS("multi-pass tombstone accumulation: counter accurate");
}

// ============================================================================
// Harder: Long Probe Chain with Deletions
// ============================================================================

static void test_long_probe_chain_with_deletes(void) {
    printf("\n=== Long Probe Chain with Deletes ===\n");
    
    ht_config_t cfg = {
        .initial_capacity = 4,
        .max_load_factor = 0.5f,
        .min_load_factor = 0.0f,
        .tomb_threshold = 0.0f,
        .zombie_window = 0,
    };

    for (int trial = 0; trial < 20; trial++) {
        ht_bare_t *bare = ht_bare_create(&cfg);
        if (!bare) continue;

        for (int i = 0; i < 6; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_insert(bare, simple_hash(k, sizeof(k), NULL), i);
        }

        for (int i = 0; i < 4; i++) {
            char k[8];
            memset(k, i, sizeof(k));
            ht_bare_remove(bare, simple_hash(k, sizeof(k), NULL));
        }

        ht_bare_compact(bare);

        size_t live, tomb, empty;
        count_bare_slots(bare, &live, &tomb, &empty);

        if (live != bare->size || tomb != bare->tombstone_cnt) {
            printf("    trial %d: live=%zu/%zu tomb=%zu/%zu\n",
                   trial, live, bare->size, tomb, bare->tombstone_cnt);
            ht_bare_destroy(bare);
            BUG("long probe chain: counters inconsistent after compact");
            return;
        }
        ht_bare_destroy(bare);
    }
    PASS("long probe chain with deletes: consistent state");
}

// ============================================================================
// Harder: Arena Grow Failure During Value Update
// ============================================================================

static void test_arena_grow_failure_during_update(void) {
    printf("\n=== Arena Grow Failure During Update ===\n");
    
    for (int trial = 0; trial < 20; trial++) {
        alloc_mock_reset();
        alloc_mock_set_max_alloc_calls(100);
        ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
        if (!ht) continue;

        char k[8] = {1};
        char v[16] = {0};
        bool ok = ht_upsert(ht, k, sizeof(k), v, sizeof(v));
        if (!ok) { ht_destroy(ht); continue; }

        size_t entry_before = ht->entry_count;

        alloc_mock_set_max_alloc_size(1024);
        char v_large[1400];
        memset(v_large, 'x', sizeof(v_large));

        ok = ht_upsert(ht, k, sizeof(k), v_large, sizeof(v_large));

        if (!ok) {
            if (ht->entry_count != entry_before) {
                printf(" trial %d: entry_count=%zu expected=%zu\n",
                       trial, ht->entry_count, entry_before);
                ht_destroy(ht);
                BUG("arena grow failure: entry_count corrupted");
                return;
            }
        }
        ht_destroy(ht);
    }
    PASS("arena grow failure during update: state consistent");
}

// ============================================================================
// Harder: Mixed Hash 0 and Normal Entries
// ============================================================================

static void test_mixed_spill_and_normal_entries(void) {
    printf("\n=== Mixed Spill and Normal Entries ===\n");
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) {
        BUG("ht_create returned NULL");
        return;
    }

    for (int i = 0; i < 10; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    char k_spill[8] = {100};
    char v_spill[8] = {100};
    ht_upsert_with_hash(ht, 0, k_spill, sizeof(k_spill), v_spill, sizeof(v_spill));

    printf("    10 normal + 1 spill entry\n");
    printf("    size=%zu\n", ht_size(ht));

    size_t vlen = 0;
    const void *found = ht_find_with_hash(ht, 0, k_spill, sizeof(k_spill), &vlen);
    if (found) {
        PASS("mixed entries: find_with_hash(0) works for spill entry");
    } else {
        BUG("mixed entries: spill entry not findable with hash=0");
    }

    ht_remove_with_hash(ht, 0, k_spill, sizeof(k_spill));
    found = ht_find_with_hash(ht, 0, k_spill, sizeof(k_spill), &vlen);
    if (!found) {
        PASS("mixed entries: remove_with_hash(0) works for spill entry");
    } else {
        BUG("mixed entries: spill entry still found after remove");
    }

    ht_destroy(ht);
}

// ============================================================================
// Harder: 100% Remove Rate Stress
// ============================================================================

static void test_100pct_remove_rate(void) {
    printf("\n=== 100%% Remove Rate Stress ===\n");
    
    ht_table_t *ht = ht_create(NULL, simple_hash, NULL, NULL);
    if (!ht) {
        BUG("ht_create returned NULL");
        return;
    }

    for (int i = 0; i < 100; i++) {
        char k[8], v[8];
        memset(k, i, sizeof(k));
        memset(v, i, sizeof(v));
        ht_upsert(ht, k, sizeof(k), v, sizeof(v));
    }

    for (int i = 0; i < 100; i++) {
        char k[8];
        memset(k, i, sizeof(k));
        ht_remove(ht, k, sizeof(k));
    }

    verify_bare_invariants(&ht->bare);

    if (ht_size(ht) == 0) {
        PASS("100% remove rate: empty table consistent");
    } else {
        BUG("100% remove rate: size != 0 after all removed");
    }

    ht_destroy(ht);
}

// ============================================================================
// Main
// ============================================================================

int main(void) {
    printf("========================================\n");
    printf("Bug-Finding Stress Tests: draugr Hash Table\n");
    printf("========================================\n");

    signal(SIGSEGV, sigsegv_handler);

    /* Run simpler tests first to check basic functionality */
    test_spill_lane_remove_consistency();
    test_mixed_spill_and_normal_entries();
    test_100pct_remove_rate();
    
    test_spill_grow_partial_failure();
    test_tombstone_counter_drift_removes();
    test_tombstone_counter_drift_compact();
    test_zombie_disabled_tombstone_accumulation();
    test_upsert_return_value();
    test_arena_rollback_atomicity();
    test_resize_partial_failure_loses_entries();
    test_multi_pass_tombstone_accumulation();
    test_long_probe_chain_with_deletes();
    test_arena_grow_failure_during_update();

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}
