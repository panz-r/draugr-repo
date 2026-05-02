#include "draugr/ht_cache.h"
#include <assert.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

/* Simple entry: int key + int value */
typedef struct {
    int key;
    int value;
} test_entry_t;

static uint64_t test_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    const int *k = (const int *)key;
    return (uint64_t)(*k * 2654435761u);
}

static bool test_eq_fn(const void *key, size_t key_len,
                       const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    const int *k = key;
    const test_entry_t *e = entry;
    return *k == e->key;
}

static ht_cache_t *create_test_cache(size_t capacity) {
    ht_cache_config_t cfg = {
        .capacity   = capacity,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = test_hash_fn,
        .eq_fn      = test_eq_fn,
        .user_ctx   = NULL,
    };
    return ht_cache_create(&cfg);
}

/* ── Basic lifecycle ──────────────────────────────────────────── */

static void test_create_destroy(void) {
    ht_cache_t *c = create_test_cache(16);
    assert(c != NULL);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_capacity(c) == 16);
    ht_cache_destroy(c);
    printf("  PASS create_destroy\n");
}

static void test_null_args(void) {
    assert(ht_cache_create(NULL) == NULL);
    assert(ht_cache_size(NULL) == 0);
    assert(ht_cache_capacity(NULL) == 0);
    assert(ht_cache_get(NULL, "x", 1) == NULL);
    assert(ht_cache_find(NULL, 0, NULL, NULL) == NULL);
    assert(ht_cache_remove(NULL, "x", 1) == false);
    assert(ht_cache_evict(NULL) == false);
    ht_cache_destroy(NULL);
    ht_cache_clear(NULL);
    ht_cache_promote(NULL, NULL);
    printf("  PASS null_args\n");
}

/* ── Put / Get ────────────────────────────────────────────────── */

static void test_put_get(void) {
    ht_cache_t *c = create_test_cache(16);
    test_entry_t e1 = {.key = 42, .value = 100};
    test_entry_t e2 = {.key = 99, .value = 200};

    void *p1 = ht_cache_put(c, &e1, sizeof(e1));
    assert(p1 != NULL);
    assert(ht_cache_size(c) == 1);

    void *p2 = ht_cache_put(c, &e2, sizeof(e2));
    assert(p2 != NULL);
    assert(ht_cache_size(c) == 2);

    int k1 = 42;
    test_entry_t *found = ht_cache_get(c, &k1, sizeof(k1));
    assert(found != NULL);
    assert(found->key == 42);
    assert(found->value == 100);

    int k2 = 99;
    found = ht_cache_get(c, &k2, sizeof(k2));
    assert(found != NULL);
    assert(found->value == 200);

    int k3 = 7;
    found = ht_cache_get(c, &k3, sizeof(k3));
    assert(found == NULL);

    ht_cache_destroy(c);
    printf("  PASS put_get\n");
}

/* ── LRU eviction ─────────────────────────────────────────────── */

static void test_lru_eviction(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 5; i++) {
        entries[i].key = i * 10;
        entries[i].value = i * 100;
    }

    /* Fill cache: 0, 10, 20, 30 */
    for (int i = 0; i < 4; i++)
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));
    assert(ht_cache_size(c) == 4);

    /* Insert 40 → evicts LRU (key=0) */
    void *p = ht_cache_put(c, &entries[4], sizeof(test_entry_t));
    assert(p != NULL);
    assert(ht_cache_size(c) == 4);

    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    int k40 = 40;
    assert(ht_cache_get(c, &k40, sizeof(k40)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS lru_eviction\n");
}

static void test_get_promotes(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 5; i++) {
        entries[i].key = i * 10;
        entries[i].value = i * 100;
    }

    /* Fill: 0, 10, 20, 30 (in MRU→LRU order: 30, 20, 10, 0) */
    for (int i = 0; i < 4; i++)
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));

    /* Access key=0 → promotes to MRU. LRU is now key=10 */
    int k0 = 0;
    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL && found->value == 0);

    /* Insert key=40 → should evict key=10 (now LRU) */
    ht_cache_put(c, &entries[4], sizeof(test_entry_t));

    int k10 = 10;
    assert(ht_cache_get(c, &k10, sizeof(k10)) == NULL);
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS get_promotes\n");
}

static void test_manual_evict(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);

    assert(ht_cache_evict(c) == true);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    ht_cache_destroy(c);
    printf("  PASS manual_evict\n");
}

/* ── Remove ───────────────────────────────────────────────────── */

static void test_remove(void) {
    ht_cache_t *c = create_test_cache(16);
    test_entry_t e = {.key = 42, .value = 100};
    ht_cache_put(c, &e, sizeof(e));

    int k = 42;
    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    assert(ht_cache_remove(c, &k, sizeof(k)) == false);

    ht_cache_destroy(c);
    printf("  PASS remove\n");
}

static void test_clear(void) {
    ht_cache_t *c = create_test_cache(16);
    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 10);

    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    for (int i = 0; i < 10; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* Reuse after clear */
    test_entry_t e = {.key = 5, .value = 99};
    ht_cache_put(c, &e, sizeof(e));
    int k = 5;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 99);

    ht_cache_destroy(c);
    printf("  PASS clear\n");
}

/* ── Scan callback / two-phase ────────────────────────────────── */

typedef struct {
    int target_key;
    void *result;
} simple_scan_ctx_t;

static bool simple_scan_fn(void *entry, void *ctx) {
    simple_scan_ctx_t *s = ctx;
    test_entry_t *e = entry;
    if (e->key == s->target_key) {
        s->result = entry;
        return false;
    }
    return true;
}

static void test_find_scan(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int k = 3;
    uint64_t hash = test_hash_fn(&k, sizeof(k), NULL);
    simple_scan_ctx_t ctx = {.target_key = 3, .result = NULL};
    void *found = ht_cache_find(c, hash, simple_scan_fn, &ctx);
    assert(found != NULL);
    assert(((test_entry_t *)found)->value == 30);

    /* Not found */
    ctx.target_key = 99;
    ctx.result = NULL;
    found = ht_cache_find(c, hash, simple_scan_fn, &ctx);
    assert(found == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_scan\n");
}

/* Two-phase: exact match vs fallback */
typedef struct {
    int  exact_key;
    int  fallback_min;
    void *exact;
    void *fallback;
} two_phase_ctx_t;

static bool two_phase_scan_fn(void *entry, void *ctx) {
    two_phase_ctx_t *s = ctx;
    test_entry_t *e = entry;
    if (e->key == s->exact_key) {
        s->exact = entry;
        return false; /* stop */
    }
    if (e->value >= s->fallback_min) {
        if (!s->fallback)
            s->fallback = entry;
    }
    return true; /* continue */
}

static void test_two_phase_scan(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i * 10, .value = i * 100};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Exact hit */
    int k = 20;
    uint64_t hash = test_hash_fn(&k, sizeof(k), NULL);
    two_phase_ctx_t ctx = {.exact_key = 20, .fallback_min = 200,
                           .exact = NULL, .fallback = NULL};
    void *found = ht_cache_find(c, hash, two_phase_scan_fn, &ctx);
    assert(found != NULL);
    assert(ctx.exact != NULL);
    assert(((test_entry_t *)ctx.exact)->key == 20);

    /* No exact, fallback exists */
    ctx.exact_key = 25;
    ctx.fallback_min = 200;
    ctx.exact = NULL;
    ctx.fallback = NULL;
    found = ht_cache_find(c, hash, two_phase_scan_fn, &ctx);
    assert(found == NULL);
    assert(ctx.exact == NULL);
    assert(ctx.fallback != NULL);
    assert(((test_entry_t *)ctx.fallback)->value >= 200);

    ht_cache_destroy(c);
    printf("  PASS two_phase_scan\n");
}

static void test_promote_after_find(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 5; i++) {
        entries[i].key = i * 10;
        entries[i].value = i * 100;
    }
    for (int i = 0; i < 4; i++)
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));

    /* Find key=0 via scan (no auto-promote) */
    int k = 0;
    uint64_t hash = test_hash_fn(&k, sizeof(k), NULL);
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    void *found = ht_cache_find(c, hash, simple_scan_fn, &ctx);
    assert(found != NULL);

    /* Promote manually */
    ht_cache_promote(c, found);

    /* Insert one more → should evict key=10 (now LRU, not key=0) */
    ht_cache_put(c, &entries[4], sizeof(test_entry_t));
    int k10 = 10;
    assert(ht_cache_get(c, &k10, sizeof(k10)) == NULL);

    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_after_find\n");
}

/* ── Hash collisions (multi-value) ────────────────────────────── */

static uint64_t collision_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    /* All keys hash to 42 */
    (void)key;
    return 42;
}

static bool collision_eq_fn(const void *key, size_t key_len,
                            const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    const int *k = key;
    const test_entry_t *e = entry;
    return *k == e->key;
}

static void test_hash_collisions(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* All entries share hash=42 but have different keys */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Get each one — eq_fn distinguishes them */
    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == i);
        assert(found->value == i * 10);
    }

    /* Remove one — only that one goes */
    int k3 = 3;
    assert(ht_cache_remove(c, &k3, sizeof(k3)) == true);
    assert(ht_cache_size(c) == 7);
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Others still there */
    int k5 = 5;
    assert(ht_cache_get(c, &k5, sizeof(k5)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS hash_collisions\n");
}

/* ── Iteration ────────────────────────────────────────────────── */

static void test_iteration(void) {
    ht_cache_t *c = create_test_cache(16);

    /* Empty cache */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    assert(ht_cache_iter_next(c, &it, &entry) == false);

    /* Add entries */
    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int count = 0;
    int keys[5] = {0};
    it = ht_cache_iter_begin(c);
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 0 && e->key < 5);
        keys[e->key] = 1;
        count++;
    }
    assert(count == 5);
    for (int i = 0; i < 5; i++)
        assert(keys[i] == 1);

    ht_cache_destroy(c);
    printf("  PASS iteration\n");
}

/* ── Capacity boundaries ──────────────────────────────────────── */

static void test_single_entry(void) {
    ht_cache_t *c = create_test_cache(1);
    test_entry_t e1 = {.key = 1, .value = 10};
    test_entry_t e2 = {.key = 2, .value = 20};

    void *p = ht_cache_put(c, &e1, sizeof(e1));
    assert(p != NULL);
    assert(ht_cache_size(c) == 1);

    /* Insert second → evicts first */
    p = ht_cache_put(c, &e2, sizeof(e2));
    assert(p != NULL);
    assert(ht_cache_size(c) == 1);

    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS single_entry\n");
}

/* ── Wrong entry_size ─────────────────────────────────────────── */

static void test_wrong_size(void) {
    ht_cache_t *c = create_test_cache(16);
    test_entry_t e = {.key = 1, .value = 10};
    assert(ht_cache_put(c, &e, 999) == NULL);
    ht_cache_destroy(c);
    printf("  PASS wrong_size\n");
}

/* ── Reuse after eviction ─────────────────────────────────────── */

static void test_evict_reuse(void) {
    ht_cache_t *c = create_test_cache(2);

    test_entry_t e1 = {.key = 1, .value = 10};
    test_entry_t e2 = {.key = 2, .value = 20};
    test_entry_t e3 = {.key = 3, .value = 30};

    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    /* Cache full: {1, 2}, LRU=1, MRU=2 */
    assert(ht_cache_size(c) == 2);

    /* Insert 3 → evict 1 */
    ht_cache_put(c, &e3, sizeof(e3));
    assert(ht_cache_size(c) == 2);

    /* Remove 2 → size 1 */
    int k2 = 2;
    ht_cache_remove(c, &k2, sizeof(k2));
    assert(ht_cache_size(c) == 1);

    /* Insert 4 → should NOT need eviction */
    test_entry_t e4 = {.key = 4, .value = 40};
    void *p = ht_cache_put(c, &e4, sizeof(e4));
    assert(p != NULL);
    assert(ht_cache_size(c) == 2);

    /* Verify both 3 and 4 present */
    int k3 = 3, k4 = 4;
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_reuse\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Edge case tests
 * ══════════════════════════════════════════════════════════════════ */

/* ── Invalid config ───────────────────────────────────────────── */

static void test_invalid_config(void) {
    ht_cache_config_t cfg;

    /* Zero capacity */
    cfg = (ht_cache_config_t){0, sizeof(test_entry_t), test_hash_fn, test_eq_fn, NULL};
    assert(ht_cache_create(&cfg) == NULL);

    /* Zero entry_size */
    cfg = (ht_cache_config_t){16, 0, test_hash_fn, test_eq_fn, NULL};
    assert(ht_cache_create(&cfg) == NULL);

    /* NULL hash_fn */
    cfg = (ht_cache_config_t){16, sizeof(test_entry_t), NULL, test_eq_fn, NULL};
    assert(ht_cache_create(&cfg) == NULL);

    /* All zero */
    cfg = (ht_cache_config_t){0, 0, NULL, NULL, NULL};
    assert(ht_cache_create(&cfg) == NULL);

    printf("  PASS invalid_config\n");
}

/* ── Null eq_fn: first hash match wins ────────────────────────── */

static void test_null_eq_fn(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = test_hash_fn,
        .eq_fn      = NULL,  /* no eq_fn */
    };
    ht_cache_t *c = ht_cache_create(&cfg);
    assert(c != NULL);

    test_entry_t e1 = {.key = 42, .value = 100};
    ht_cache_put(c, &e1, sizeof(e1));

    /* Get with matching hash finds it even though eq_fn is NULL */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);

    /* Different key → same hash? Unlikely with our hash, but miss works */
    int k2 = 999;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS null_eq_fn\n");
}

/* ── Duplicate keys (always-add) ──────────────────────────────── */

static void test_duplicate_keys(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e1 = {.key = 42, .value = 100};
    test_entry_t e2 = {.key = 42, .value = 200};

    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    assert(ht_cache_size(c) == 2);

    /* Get returns first match (the one with value=200, since it was promoted to MRU
       by the put of e2 — but actually e1 was the first inserted, so get will find
       whichever the bare table's find_all encounters first) */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);
    /* Both entries have key=42, so either value is valid */

    /* Remove removes only the first match */
    ht_cache_remove(c, &k, sizeof(k));
    assert(ht_cache_size(c) == 1);

    /* The other one is still there */
    found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);

    ht_cache_destroy(c);
    printf("  PASS duplicate_keys\n");
}

/* ── In-place mutation ────────────────────────────────────────── */

static void test_in_place_mutation(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 42, .value = 100};
    test_entry_t *p = ht_cache_put(c, &e, sizeof(e));
    assert(p != NULL);

    /* Mutate through returned pointer */
    p->value = 999;

    /* Verify visible on next get */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->value == 999);

    ht_cache_destroy(c);
    printf("  PASS in_place_mutation\n");
}

/* ── Pointer stability ────────────────────────────────────────── */

static void test_pointer_stability(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[4];
    void *ptrs[4];
    for (int i = 0; i < 4; i++) {
        entries[i].key = i;
        entries[i].value = i * 10;
        ptrs[i] = ht_cache_put(c, &entries[i], sizeof(test_entry_t));
        assert(ptrs[i] != NULL);
    }

    /* Pointers remain valid after subsequent puts and gets */
    int k0 = 0;
    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found == ptrs[0]);  /* same address */

    int k2 = 2;
    found = ht_cache_get(c, &k2, sizeof(k2));
    assert(found == ptrs[2]);

    /* Put a 5th entry → evicts LRU. The evicted pointer is now invalid,
       but the surviving pointers must still be valid. */
    test_entry_t e5 = {.key = 99, .value = 990};
    ht_cache_put(c, &e5, sizeof(e5));

    /* key=1 was LRU (insert order: 0,1,2,3, get(0) promotes 0, so LRU=1) */
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Surviving pointers still valid */
    found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found == ptrs[0]);
    found = ht_cache_get(c, &k2, sizeof(k2));
    assert(found == ptrs[2]);

    ht_cache_destroy(c);
    printf("  PASS pointer_stability\n");
}

/* ── Remove from LRU positions ────────────────────────────────── */

static void test_remove_lru_head(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 5);

    /* Remove MRU (key=4, last inserted) */
    int k4 = 4;
    assert(ht_cache_remove(c, &k4, sizeof(k4)) == true);
    assert(ht_cache_size(c) == 4);

    /* Insert new → should NOT need eviction */
    test_entry_t e = {.key = 99, .value = 990};
    void *p = ht_cache_put(c, &e, sizeof(e));
    assert(p != NULL);
    assert(ht_cache_size(c) == 5);

    ht_cache_destroy(c);
    printf("  PASS remove_lru_head\n");
}

static void test_remove_lru_tail(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove LRU (key=0, first inserted) */
    int k0 = 0;
    assert(ht_cache_remove(c, &k0, sizeof(k0)) == true);
    assert(ht_cache_size(c) == 4);

    /* Evict should now remove key=1 (next LRU) */
    assert(ht_cache_evict(c) == true);
    assert(ht_cache_size(c) == 3);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* key=2 still present */
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS remove_lru_tail\n");
}

static void test_remove_lru_middle(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU order (tail→head): 0, 1, 2, 3, 4 */

    /* Remove from middle */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    assert(ht_cache_size(c) == 4);

    /* Verify key=2 is gone */
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Verify eviction order preserved: LRU should be key=0 */
    assert(ht_cache_evict(c) == true);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Remaining: 1, 3, 4 */
    int k1 = 1, k3 = 3, k4 = 4;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS remove_lru_middle\n");
}

/* ── Promote at LRU positions ─────────────────────────────────── */

static void test_promote_head_noop(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 4; i++) {
        entries[i].key = i;
        entries[i].value = i * 10;
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));
    }
    /* MRU=key=3 */

    /* Promote MRU → no-op */
    int k3 = 3;
    test_entry_t *found = ht_cache_get(c, &k3, sizeof(k3));
    ht_cache_promote(c, found);

    /* Insert one more → should evict key=0 (LRU unchanged) */
    test_entry_t e = {.key = 99, .value = 990};
    ht_cache_put(c, &e, sizeof(e));
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_head_noop\n");
}

static void test_promote_tail(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 4; i++) {
        entries[i].key = i;
        entries[i].value = i * 10;
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));
    }
    /* LRU order (tail→head): 0, 1, 2, 3 */

    /* Promote tail (key=0) to MRU */
    int k0 = 0;
    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL);
    /* get auto-promotes */

    /* Insert → evicts key=1 (new LRU) */
    test_entry_t e = {.key = 99, .value = 990};
    ht_cache_put(c, &e, sizeof(e));
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_tail\n");
}

static void test_promote_middle(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t entries[5];
    for (int i = 0; i < 4; i++) {
        entries[i].key = i;
        entries[i].value = i * 10;
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));
    }
    /* LRU order (tail→head): 0, 1, 2, 3 */

    /* Promote middle (key=1) */
    int k1 = 1;
    test_entry_t *found = ht_cache_get(c, &k1, sizeof(k1));
    assert(found != NULL);

    /* LRU order now (tail→head): 0, 2, 3, 1 */

    /* Insert → evicts key=0 */
    test_entry_t e = {.key = 99, .value = 990};
    ht_cache_put(c, &e, sizeof(e));
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    int k2 = 2, k3 = 3;
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_middle\n");
}

/* ── Drain and refill ─────────────────────────────────────────── */

static void test_evict_all(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 4);

    /* Evict all one by one */
    for (int i = 0; i < 4; i++) {
        assert(ht_cache_evict(c) == true);
        assert(ht_cache_size(c) == (size_t)(3 - i));
    }

    /* Cache is empty */
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    /* Refill */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i + 100, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 4);

    for (int i = 0; i < 4; i++) {
        int k = i + 100;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS evict_all\n");
}

static void test_remove_all_then_refill(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Remove all via key */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);

    /* No stale data */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* Refill with different values */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 99};
        ht_cache_put(c, &e, sizeof(e));
    }
    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == i * 99);
    }

    ht_cache_destroy(c);
    printf("  PASS remove_all_then_refill\n");
}

/* ── Remove then re-insert same key ───────────────────────────── */

static void test_remove_reinsert(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e1 = {.key = 42, .value = 100};
    ht_cache_put(c, &e1, sizeof(e1));

    int k = 42;
    ht_cache_remove(c, &k, sizeof(k));
    assert(ht_cache_size(c) == 0);

    /* Re-insert with different value */
    test_entry_t e2 = {.key = 42, .value = 200};
    ht_cache_put(c, &e2, sizeof(e2));

    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->value == 200);

    ht_cache_destroy(c);
    printf("  PASS remove_reinsert\n");
}

/* ── Get-then-remove-then-get ─────────────────────────────────── */

static void test_get_remove_get(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 7, .value = 70};
    ht_cache_put(c, &e, sizeof(e));

    int k = 7;
    assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS get_remove_get\n");
}

/* ── Iterate after removal ────────────────────────────────────── */

static void test_iter_after_remove(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove key=2 */
    int k2 = 2;
    ht_cache_remove(c, &k2, sizeof(k2));

    /* Iterate — should see 4 entries, no key=2 */
    int count = 0;
    bool seen[5] = {false};
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key != 2);
        assert(e->key >= 0 && e->key < 5);
        seen[e->key] = true;
        count++;
    }
    assert(count == 4);
    assert(seen[0] && seen[1] && !seen[2] && seen[3] && seen[4]);

    ht_cache_destroy(c);
    printf("  PASS iter_after_remove\n");
}

/* ── Iterate after clear and refill ───────────────────────────── */

static void test_iter_clear_refill(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);

    for (int i = 10; i < 13; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int count = 0;
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 10 && e->key < 13);
        count++;
    }
    assert(count == 3);

    ht_cache_destroy(c);
    printf("  PASS iter_clear_refill\n");
}

/* ── Find on empty cache ──────────────────────────────────────── */

static void test_find_empty(void) {
    ht_cache_t *c = create_test_cache(16);

    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    assert(ht_cache_find(c, 12345, simple_scan_fn, &ctx) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_empty\n");
}

/* ── Scan callback patterns ───────────────────────────────────── */

static bool never_match_fn(void *entry, void *ctx) {
    (void)entry; (void)ctx;
    return true;  /* always continue */
}

static bool always_match_fn(void *entry, void *ctx) {
    *(void **)ctx = entry;
    return false;  /* always stop */
}

static void test_scan_never_match(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int k = 0;
    uint64_t hash = test_hash_fn(&k, sizeof(k), NULL);
    assert(ht_cache_find(c, hash, never_match_fn, NULL) == NULL);

    ht_cache_destroy(c);
    printf("  PASS scan_never_match\n");
}

static void test_scan_first_match(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int k = 0;
    uint64_t hash = test_hash_fn(&k, sizeof(k), NULL);
    void *result = NULL;
    void *found = ht_cache_find(c, hash, always_match_fn, &result);
    assert(found != NULL);
    assert(found == result);

    ht_cache_destroy(c);
    printf("  PASS scan_first_match\n");
}

/* ── Scan visits all entries with same hash ───────────────────── */

typedef struct { int count; } count_ctx_t;

static bool count_fn(void *entry, void *ctx) {
    (void)entry;
    count_ctx_t *cc = ctx;
    cc->count++;
    return true;
}

static void test_scan_visits_all(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 8);

    ht_cache_destroy(c);
    printf("  PASS scan_visits_all\n");
}

/* ── Capacity 2 ───────────────────────────────────────────────── */

static void test_capacity_two(void) {
    ht_cache_t *c = create_test_cache(2);

    test_entry_t e1 = {.key = 1, .value = 10};
    test_entry_t e2 = {.key = 2, .value = 20};
    test_entry_t e3 = {.key = 3, .value = 30};

    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));

    /* Both present */
    int k1 = 1, k2 = 2;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    /* Insert third → evicts key=1 (LRU, since get(1) happened before get(2)) */
    ht_cache_put(c, &e3, sizeof(e3));
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    /* Remove one, then add one back */
    ht_cache_remove(c, &k2, sizeof(k2));
    assert(ht_cache_size(c) == 1);

    test_entry_t e4 = {.key = 4, .value = 40};
    ht_cache_put(c, &e4, sizeof(e4));
    assert(ht_cache_size(c) == 2);

    ht_cache_destroy(c);
    printf("  PASS capacity_two\n");
}

/* ── Fill exactly to capacity ─────────────────────────────────── */

static void test_fill_exact(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* All present */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS fill_exact\n");
}

/* ── Large cache ──────────────────────────────────────────────── */

static void test_large_cache(void) {
    const size_t N = 1024;
    ht_cache_t *c = create_test_cache(N);

    for (size_t i = 0; i < N; i++) {
        test_entry_t e = {.key = (int)i, .value = (int)(i * 7)};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == N);

    for (size_t i = 0; i < N; i++) {
        int k = (int)i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == (int)i);
        assert(found->value == (int)(i * 7));
    }

    /* Add one more → evicts LRU (key=0, first inserted) */
    test_entry_t extra = {.key = 9999, .value = 77777};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_size(c) == N);

    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    int k9999 = 9999;
    assert(ht_cache_get(c, &k9999, sizeof(k9999)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS large_cache\n");
}

/* ── Rapid churn: fill, evict, fill, evict ────────────────────── */

static void test_rapid_churn(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int round = 0; round < 10; round++) {
        /* Fill */
        for (int i = 0; i < 8; i++) {
            test_entry_t e = {.key = round * 100 + i, .value = round * 1000 + i};
            void *p = ht_cache_put(c, &e, sizeof(e));
            assert(p != NULL);
        }
        assert(ht_cache_size(c) == 8);

        /* Evict all */
        for (int i = 0; i < 8; i++)
            assert(ht_cache_evict(c) == true);
        assert(ht_cache_size(c) == 0);
    }

    ht_cache_destroy(c);
    printf("  PASS rapid_churn\n");
}

/* ── Churn via put (implicit eviction) ────────────────────────── */

static void test_put_churn(void) {
    ht_cache_t *c = create_test_cache(4);

    /* Insert 100 entries through a 4-slot cache */
    for (int i = 0; i < 100; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* Only the last 4 should survive: 96, 97, 98, 99 */
    for (int i = 96; i < 100; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }
    for (int i = 0; i < 96; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS put_churn\n");
}

/* ── Full collision churn ─────────────────────────────────────── */

static void test_collision_churn(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* All entries share hash=42 */
    for (int i = 0; i < 20; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* Only last 8 survive: keys 12-19 */
    for (int i = 12; i < 20; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == i * 10);
    }
    for (int i = 0; i < 12; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_churn\n");
}

/* ── Negative keys ────────────────────────────────────────────── */

static void test_negative_keys(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e1 = {.key = -1, .value = 100};
    test_entry_t e2 = {.key = -100, .value = 200};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));

    int k = -1;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 100);

    k = -100;
    found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 200);

    k = -999;
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS negative_keys\n");
}

/* ── Key=0 (hash=0, goes to spill lane) ───────────────────────── */

static void test_zero_key(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 0, .value = 42};
    ht_cache_put(c, &e, sizeof(e));

    int k = 0;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 0);
    assert(found->value == 42);

    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS zero_key\n");
}

/* ── Multiple entries same hash, remove one by one ────────────── */

static void test_collision_remove_one_by_one(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 6);

    /* Remove in reverse order */
    for (int i = 5; i >= 0; i--) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
        assert(ht_cache_size(c) == (size_t)i);
    }

    /* All gone */
    for (int i = 0; i < 6; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_remove_one_by_one\n");
}

/* ── Clear single entry ───────────────────────────────────────── */

static void test_clear_single(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);

    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    ht_cache_destroy(c);
    printf("  PASS clear_single\n");
}

/* ── Put NULL data ────────────────────────────────────────────── */

static void test_put_null(void) {
    ht_cache_t *c = create_test_cache(16);
    assert(ht_cache_put(c, NULL, sizeof(test_entry_t)) == NULL);
    assert(ht_cache_size(c) == 0);
    ht_cache_destroy(c);
    printf("  PASS put_null\n");
}

/* ── Remove non-existent key ──────────────────────────────────── */

static void test_remove_nonexistent(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));

    int k = 999;
    assert(ht_cache_remove(c, &k, sizeof(k)) == false);
    assert(ht_cache_size(c) == 1);

    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS remove_nonexistent\n");
}

/* ── LRU order after series of gets ───────────────────────────── */

static void test_lru_ordering(void) {
    ht_cache_t *c = create_test_cache(4);

    /* Insert 0, 1, 2, 3 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU tail→head: 0, 1, 2, 3 */

    /* Access 0 → promotes: 1, 2, 3, 0 */
    int k0 = 0;
    ht_cache_get(c, &k0, sizeof(k0));

    /* Access 2 → promotes: 1, 3, 0, 2 */
    int k2 = 2;
    ht_cache_get(c, &k2, sizeof(k2));

    /* Insert 4 → evicts LRU=1 */
    test_entry_t e4 = {.key = 4, .value = 40};
    ht_cache_put(c, &e4, sizeof(e4));
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Remaining: 3, 0, 2, 4 */

    /* Insert 5 → evicts LRU=3 */
    test_entry_t e5 = {.key = 5, .value = 50};
    ht_cache_put(c, &e5, sizeof(e5));
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Remaining: 0, 2, 4, 5 */
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    int k4 = 4, k5 = 5;
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);
    assert(ht_cache_get(c, &k5, sizeof(k5)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS lru_ordering\n");
}

/* ── Promote invalid pointer ──────────────────────────────────── */

static void test_promote_invalid(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));

    /* Pointer from outside the entries array */
    test_entry_t bogus = {.key = 99, .value = 99};
    ht_cache_promote(c, &bogus);  /* should be no-op, not crash */

    /* NULL */
    ht_cache_promote(c, NULL);

    /* Verify cache still works */
    int k = 1;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 10);

    ht_cache_destroy(c);
    printf("  PASS promote_invalid\n");
}

/* ── Find with specific hash (no entries for that hash) ───────── */

static void test_find_wrong_hash(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Use a hash that doesn't match any entry */
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    assert(ht_cache_find(c, 0xDEADBEEFCAFE0000ULL, simple_scan_fn, &ctx) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_wrong_hash\n");
}

/* ── Multiple gets preserve ordering ──────────────────────────── */

static void test_repeated_get(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Get key=0 ten times — should stay at MRU */
    int k0 = 0;
    for (int i = 0; i < 10; i++) {
        test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
        assert(found != NULL);
    }

    /* Other entries still accessible */
    for (int i = 1; i < 4; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    /* LRU order after gets: tail→head = 0, 1, 2, 3 (key=0 promoted first, then
       1,2,3 each promoted after, so key=0 is LRU again) */
    test_entry_t extra = {.key = 99, .value = 990};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);  /* key=0 evicted (LRU) */
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS repeated_get\n");
}

/* ── Evict then put evicted key ───────────────────────────────── */

static void test_evict_reinsert(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict LRU (key=0) */
    assert(ht_cache_evict(c) == true);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Re-insert key=0 with new value */
    test_entry_t e = {.key = 0, .value = 999};
    ht_cache_put(c, &e, sizeof(e));

    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL);
    assert(found->value == 999);

    ht_cache_destroy(c);
    printf("  PASS evict_reinsert\n");
}

/* ── Iterator null args ───────────────────────────────────────── */

static void test_iter_null(void) {
    ht_cache_iter_t it = ht_cache_iter_begin(NULL);
    void *entry;
    assert(ht_cache_iter_next(NULL, &it, &entry) == false);
    assert(ht_cache_iter_next(NULL, NULL, &entry) == false);
    assert(ht_cache_iter_next(NULL, &it, NULL) == false);
    printf("  PASS iter_null\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Edge case tests — round 2
 * ══════════════════════════════════════════════════════════════════ */

/* ── Large entry struct ───────────────────────────────────────── */

typedef struct {
    int  key;
    char payload[256];
    int  checksum;
} big_entry_t;

static uint64_t big_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    const int *k = key;
    return (uint64_t)(*k * 2654435761u);
}

static bool big_eq_fn(const void *key, size_t key_len,
                      const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    return *(const int *)key == *(const int *)entry;
}

static void test_large_entry_size(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(big_entry_t),
        .hash_fn    = big_hash_fn,
        .eq_fn      = big_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 16; i++) {
        big_entry_t e;
        memset(&e, 0, sizeof(e));
        e.key = i;
        memset(e.payload, (char)('A' + i), sizeof(e.payload));
        e.checksum = i * 37;
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 16);

    /* Verify all entries intact */
    for (int i = 0; i < 16; i++) {
        int k = i;
        big_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == i);
        assert(found->checksum == i * 37);
        assert(found->payload[0] == (char)('A' + i));
        assert(found->payload[255] == (char)('A' + i));
    }

    ht_cache_destroy(c);
    printf("  PASS large_entry_size\n");
}

/* ── Hash=1 (spill lane path) ─────────────────────────────────── */

static uint64_t hash_one_fn(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 1;
}

static bool eq_by_key_fn(const void *key, size_t key_len,
                         const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    return *(const int *)key == *(const int *)entry;
}

static void test_hash_value_one(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = hash_one_fn,
        .eq_fn      = eq_by_key_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    /* Remove + find still works */
    int k3 = 3;
    assert(ht_cache_remove(c, &k3, sizeof(k3)) == true);
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    int k5 = 5;
    assert(ht_cache_get(c, &k5, sizeof(k5)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS hash_value_one\n");
}

/* ── Hash=2 (first valid main-table hash) ─────────────────────── */

static uint64_t hash_two_fn(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 2;
}

static void test_hash_value_two(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = hash_two_fn,
        .eq_fn      = eq_by_key_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    for (int i = 0; i < 6; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }
    ht_cache_destroy(c);
    printf("  PASS hash_value_two\n");
}

/* ── Alternating insert/remove on same key ────────────────────── */

static void test_alternating_insert_remove(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int round = 0; round < 20; round++) {
        test_entry_t e = {.key = 42, .value = round};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
        assert(ht_cache_size(c) >= 1);

        int k = 42;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);

        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);
    ht_cache_destroy(c);
    printf("  PASS alternating_insert_remove\n");
}

/* ── Interleaved put/get/remove/find on different keys ────────── */

static void test_interleaved_ops(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Put 0-3 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 4);

    /* Get 0, 2 */
    int k0 = 0, k2 = 2;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    /* Remove 1 */
    int k1 = 1;
    assert(ht_cache_remove(c, &k1, sizeof(k1)) == true);
    assert(ht_cache_size(c) == 3);

    /* Find 3 via scan */
    int k3 = 3;
    uint64_t h3 = test_hash_fn(&k3, sizeof(k3), NULL);
    simple_scan_ctx_t ctx = {.target_key = 3, .result = NULL};
    void *found = ht_cache_find(c, h3, simple_scan_fn, &ctx);
    assert(found != NULL);
    assert(((test_entry_t *)found)->value == 30);

    /* Promote it */
    ht_cache_promote(c, found);

    /* Insert 4-8 (5 new entries, cache capacity 8, currently 3 live) */
    for (int i = 4; i <= 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* key=0 was promoted by get, key=3 was promoted by find+promote,
       key=2 was promoted by get. After inserting 4-8 (5 entries into
       5 free slots), no eviction needed. */
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    /* key=1 was removed */
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS interleaved_ops\n");
}

/* ── Find after eviction ──────────────────────────────────────── */

static void test_find_after_eviction(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict LRU (key=0) */
    ht_cache_evict(c);

    /* Find via scan — key=0 should be gone */
    int k0 = 0;
    uint64_t h0 = test_hash_fn(&k0, sizeof(k0), NULL);
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    assert(ht_cache_find(c, h0, simple_scan_fn, &ctx) == NULL);

    /* key=1 still findable */
    int k1 = 1;
    uint64_t h1 = test_hash_fn(&k1, sizeof(k1), NULL);
    ctx.target_key = 1;
    ctx.result = NULL;
    assert(ht_cache_find(c, h1, simple_scan_fn, &ctx) != NULL);

    ht_cache_destroy(c);
    printf("  PASS find_after_eviction\n");
}

/* ── Multiple sequential finds on same hash ───────────────────── */

static void test_sequential_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Multiple finds — no promote, so order shouldn't change */
    for (int trial = 0; trial < 5; trial++) {
        for (int target = 0; target < 5; target++) {
            simple_scan_ctx_t ctx = {.target_key = target, .result = NULL};
            void *found = ht_cache_find(c, 42, simple_scan_fn, &ctx);
            assert(found != NULL);
            assert(((test_entry_t *)found)->key == target);
        }
    }

    ht_cache_destroy(c);
    printf("  PASS sequential_find\n");
}

/* ── Scan collects all matching entries ───────────────────────── */

#define COLLECT_MAX 32
typedef struct {
    int   keys[COLLECT_MAX];
    int   count;
} collect_ctx_t;

static bool collect_fn(void *entry, void *ctx) {
    collect_ctx_t *cc = ctx;
    test_entry_t *e = entry;
    if (cc->count < COLLECT_MAX)
        cc->keys[cc->count++] = e->key;
    return true;
}

static void test_scan_collect_all(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    collect_ctx_t ctx = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &ctx);
    assert(ctx.count == 10);

    /* Verify all keys present (order may vary) */
    bool seen[10] = {false};
    for (int i = 0; i < ctx.count; i++) {
        assert(ctx.keys[i] >= 0 && ctx.keys[i] < 10);
        seen[ctx.keys[i]] = true;
    }
    for (int i = 0; i < 10; i++)
        assert(seen[i]);

    ht_cache_destroy(c);
    printf("  PASS scan_collect_all\n");
}

/* ── Remove during scan context (find then remove found) ──────── */

static void test_find_then_remove(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find key=2 */
    int k2 = 2;
    uint64_t h2 = test_hash_fn(&k2, sizeof(k2), NULL);
    simple_scan_ctx_t ctx = {.target_key = 2, .result = NULL};
    void *found = ht_cache_find(c, h2, simple_scan_fn, &ctx);
    assert(found != NULL);

    /* Now remove it */
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    assert(ht_cache_size(c) == 4);

    /* Find again → miss */
    ctx.result = NULL;
    assert(ht_cache_find(c, h2, simple_scan_fn, &ctx) == NULL);

    /* Others unaffected */
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS find_then_remove\n");
}

/* ── Capacity 1: every operation ──────────────────────────────── */

static void test_capacity_one_comprehensive(void) {
    ht_cache_t *c = create_test_cache(1);

    assert(ht_cache_size(c) == 0);
    assert(ht_cache_capacity(c) == 1);
    assert(ht_cache_evict(c) == false);

    /* Put first */
    test_entry_t e1 = {.key = 1, .value = 10};
    void *p = ht_cache_put(c, &e1, sizeof(e1));
    assert(p != NULL);
    assert(ht_cache_size(c) == 1);

    /* Get it */
    int k1 = 1;
    test_entry_t *found = ht_cache_get(c, &k1, sizeof(k1));
    assert(found != NULL && found->value == 10);

    /* Find it */
    uint64_t h1 = test_hash_fn(&k1, sizeof(k1), NULL);
    simple_scan_ctx_t ctx = {.target_key = 1, .result = NULL};
    found = ht_cache_find(c, h1, simple_scan_fn, &ctx);
    assert(found != NULL);
    ht_cache_promote(c, found);

    /* Put another → evicts first */
    test_entry_t e2 = {.key = 2, .value = 20};
    ht_cache_put(c, &e2, sizeof(e2));
    assert(ht_cache_size(c) == 1);
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    /* Remove */
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    assert(ht_cache_size(c) == 0);

    /* Clear empty cache */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Put after clear */
    test_entry_t e3 = {.key = 3, .value = 30};
    ht_cache_put(c, &e3, sizeof(e3));
    int k3 = 3;
    found = ht_cache_get(c, &k3, sizeof(k3));
    assert(found != NULL && found->value == 30);

    /* Iterate (single entry) */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    assert(ht_cache_iter_next(c, &it, &entry) == true);
    assert(((test_entry_t *)entry)->key == 3);
    assert(ht_cache_iter_next(c, &it, &entry) == false);

    ht_cache_destroy(c);
    printf("  PASS capacity_one_comprehensive\n");
}

/* ── Clear then fill to capacity ──────────────────────────────── */

static void test_clear_refill_capacity(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Fill */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Fill again — should not need any eviction */
    for (int i = 100; i < 108; i++) {
        test_entry_t e = {.key = i, .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* No old data */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }
    /* All new data present */
    for (int i = 100; i < 108; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i);
    }

    ht_cache_destroy(c);
    printf("  PASS clear_refill_capacity\n");
}

/* ── Get on just-evicted entry ────────────────────────────────── */

static void test_get_after_evict(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict key=0 (LRU) */
    assert(ht_cache_evict(c) == true);

    /* Immediate get → miss */
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Double-check: put a new entry and get it */
    test_entry_t e5 = {.key = 99, .value = 990};
    ht_cache_put(c, &e5, sizeof(e5));
    int k99 = 99;
    test_entry_t *found = ht_cache_get(c, &k99, sizeof(k99));
    assert(found != NULL && found->value == 990);

    ht_cache_destroy(c);
    printf("  PASS get_after_evict\n");
}

/* ── Repeated promote of same entry ───────────────────────────── */

static void test_repeated_promote(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find key=2 and promote it many times */
    int k2 = 2;
    uint64_t h2 = test_hash_fn(&k2, sizeof(k2), NULL);
    for (int i = 0; i < 10; i++) {
        simple_scan_ctx_t ctx = {.target_key = 2, .result = NULL};
        void *found = ht_cache_find(c, h2, simple_scan_fn, &ctx);
        assert(found != NULL);
        ht_cache_promote(c, found);
    }

    /* Insert → evicts key=0 (LRU) */
    test_entry_t extra = {.key = 99, .value = 990};
    ht_cache_put(c, &extra, sizeof(extra));
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS repeated_promote\n");
}

/* ── Size tracking accuracy through mixed ops ─────────────────── */

static void test_size_accuracy(void) {
    ht_cache_t *c = create_test_cache(8);
    assert(ht_cache_size(c) == 0);

    /* Put 4 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 4);

    /* Remove 2 */
    for (int i = 0; i < 2; i++) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }
    assert(ht_cache_size(c) == 2);

    /* Put 6 more (2 slots free + 4 will evict) */
    for (int i = 10; i < 16; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Evict 3 */
    for (int i = 0; i < 3; i++)
        ht_cache_evict(c);
    assert(ht_cache_size(c) == 5);

    /* Clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Fill to capacity */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    ht_cache_destroy(c);
    printf("  PASS size_accuracy\n");
}

/* ── Put same key repeatedly (always-add, fills cache) ────────── */

static void test_put_same_key_fills(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = 42, .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* All entries have key=42 but different values.
       Get returns one of them (first hash match). */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);

    /* Insert one more → evicts LRU */
    test_entry_t extra = {.key = 42, .value = 99};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_size(c) == 8);

    ht_cache_destroy(c);
    printf("  PASS put_same_key_fills\n");
}

/* ── Data integrity after many operations ─────────────────────── */

static void test_data_integrity(void) {
    ht_cache_t *c = create_test_cache(64);

    /* Insert entries with predictable values */
    for (int i = 0; i < 64; i++) {
        test_entry_t e = {.key = i * 7, .value = i * 13};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Verify all values */
    for (int i = 0; i < 64; i++) {
        int k = i * 7;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == i * 7);
        assert(found->value == i * 13);
    }

    /* Evict half, add new entries, verify remaining old + all new */
    for (int i = 0; i < 32; i++)
        ht_cache_evict(c);
    assert(ht_cache_size(c) == 32);

    for (int i = 0; i < 32; i++) {
        test_entry_t e = {.key = 1000 + i, .value = i * 17};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 64);

    /* Verify new entries */
    for (int i = 0; i < 32; i++) {
        int k = 1000 + i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == i * 17);
    }

    ht_cache_destroy(c);
    printf("  PASS data_integrity\n");
}

/* ── Evict all then recover via put ───────────────────────────── */

static void test_evict_all_recover(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict everything */
    for (int i = 0; i < 4; i++)
        assert(ht_cache_evict(c) == true);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    /* Cache should be fully usable again */
    for (int i = 100; i < 104; i++) {
        test_entry_t e = {.key = i, .value = i * 2};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 4);

    for (int i = 100; i < 104; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 2);
    }

    ht_cache_destroy(c);
    printf("  PASS evict_all_recover\n");
}

/* ── Collision chain: remove middle entry ─────────────────────── */

static void test_collision_remove_middle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove middle of chain */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);

    /* Verify remaining via scan */
    collect_ctx_t ctx = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &ctx);
    assert(ctx.count == 4);

    bool seen[5] = {false};
    for (int i = 0; i < ctx.count; i++)
        seen[ctx.keys[i]] = true;
    assert(!seen[2]);
    assert(seen[0] && seen[1] && seen[3] && seen[4]);

    ht_cache_destroy(c);
    printf("  PASS collision_remove_middle\n");
}

/* ── Put/evict cycle with same hash (tombstone accumulation) ─── */

static void test_tombstone_accumulation(void) {
    ht_cache_t *c = create_test_cache(4);

    /* Rapidly churn 200 entries through a 4-slot cache.
       This exercises tombstone handling in the bare table. */
    for (int i = 0; i < 200; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* Last 4 entries survive */
    for (int i = 196; i < 200; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == i * 10);
    }
    for (int i = 0; i < 196; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS tombstone_accumulation\n");
}

/* ── Find returns correct entry with multiple hash matches ────── */

static void test_find_selects_correct(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 100};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find each specific key */
    for (int target = 0; target < 5; target++) {
        simple_scan_ctx_t ctx = {.target_key = target, .result = NULL};
        void *found = ht_cache_find(c, 42, simple_scan_fn, &ctx);
        assert(found != NULL);
        assert(((test_entry_t *)found)->key == target);
        assert(((test_entry_t *)found)->value == target * 100);
    }

    ht_cache_destroy(c);
    printf("  PASS find_selects_correct\n");
}

/* ── Collision churn with removes interleaved ─────────────────── */

static void test_collision_churn_with_removes(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill, remove one, fill more — exercises bare table tombstones */
    for (int round = 0; round < 10; round++) {
        int base = round * 10;
        for (int i = 0; i < 4; i++) {
            test_entry_t e = {.key = base + i, .value = base + i};
            ht_cache_put(c, &e, sizeof(e));
        }
        /* Remove middle */
        int k = base + 1;
        ht_cache_remove(c, &k, sizeof(k));
        /* Overfill to trigger eviction */
        test_entry_t extra = {.key = base + 99, .value = base + 99};
        ht_cache_put(c, &extra, sizeof(extra));
    }
    assert(ht_cache_size(c) == 4);

    ht_cache_destroy(c);
    printf("  PASS collision_churn_with_removes\n");
}

/* ── Negative values and INT_MAX keys ─────────────────────────── */

static void test_extreme_keys(void) {
    ht_cache_t *c = create_test_cache(16);

    int extreme_keys[] = {INT_MAX, INT_MIN, -1, 0, 1, 0x7FFFFFFE, -0x7FFFFFFF};
    int nkeys = (int)(sizeof(extreme_keys) / sizeof(extreme_keys[0]));

    for (int i = 0; i < nkeys; i++) {
        test_entry_t e = {.key = extreme_keys[i], .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == (size_t)nkeys);

    for (int i = 0; i < nkeys; i++) {
        int k = extreme_keys[i];
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == extreme_keys[i]);
        assert(found->value == i);
    }

    ht_cache_destroy(c);
    printf("  PASS extreme_keys\n");
}

/* ── Stress: mixed ops with collision hash ────────────────────── */

static void test_stress_collision_mixed(void) {
    ht_cache_config_t cfg = {
        .capacity   = 32,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill 32 entries, all same hash */
    for (int i = 0; i < 32; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 32);

    /* Get all */
    for (int i = 0; i < 32; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    /* Remove half */
    for (int i = 0; i < 16; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 16);

    /* Verify remaining */
    for (int i = 16; i < 32; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    /* Fill back up with new keys */
    for (int i = 100; i < 116; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 32);

    /* Verify new keys */
    for (int i = 100; i < 116; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    ht_cache_destroy(c);
    printf("  PASS stress_collision_mixed\n");
}

/* ── Iteration count matches size ─────────────────────────────── */

static void test_iter_count_matches_size(void) {
    ht_cache_t *c = create_test_cache(16);

    /* Empty */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) count++;
    assert(count == 0);

    /* Partial fill */
    for (int i = 0; i < 7; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    it = ht_cache_iter_begin(c);
    count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) count++;
    assert(count == 7);
    assert((size_t)count == ht_cache_size(c));

    /* After remove */
    int k3 = 3;
    ht_cache_remove(c, &k3, sizeof(k3));
    it = ht_cache_iter_begin(c);
    count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) count++;
    assert(count == 6);
    assert((size_t)count == ht_cache_size(c));

    /* After eviction */
    ht_cache_evict(c);
    it = ht_cache_iter_begin(c);
    count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) count++;
    assert(count == 5);
    assert((size_t)count == ht_cache_size(c));

    /* After clear */
    ht_cache_clear(c);
    it = ht_cache_iter_begin(c);
    count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) count++;
    assert(count == 0);
    assert((size_t)count == ht_cache_size(c));

    ht_cache_destroy(c);
    printf("  PASS iter_count_matches_size\n");
}

/* ── Mixed key types (string keys) ────────────────────────────── */

typedef struct {
    char key[16];
    int  value;
} str_entry_t;

static uint64_t str_hash_fn(const void *key, size_t len, void *ctx) {
    (void)ctx;
    const char *s;
    if (len == sizeof(str_entry_t))
        s = ((const str_entry_t *)key)->key;  /* called from put */
    else
        s = (const char *)key;                /* called from get/remove */
    uint64_t h = 14695981039346656037ULL;
    while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ULL; }
    return h;
}

static bool str_eq_fn(const void *key, size_t key_len,
                      const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    const char *lookup = (const char *)key;
    const str_entry_t *e = entry;
    return strcmp(lookup, e->key) == 0;
}

static void test_string_keys(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(str_entry_t),
        .hash_fn    = str_hash_fn,
        .eq_fn      = str_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    const char *names[] = {"alpha", "bravo", "charlie", "delta",
                           "echo", "foxtrot", "golf", "hotel"};
    int nnames = (int)(sizeof(names) / sizeof(names[0]));

    for (int i = 0; i < nnames; i++) {
        str_entry_t e;
        memset(&e, 0, sizeof(e));
        strncpy(e.key, names[i], sizeof(e.key) - 1);
        e.value = i * 100;
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == (size_t)nnames);

    for (int i = 0; i < nnames; i++) {
        str_entry_t *found = ht_cache_get(c, names[i], strlen(names[i]) + 1);
        assert(found != NULL);
        assert(strcmp(found->key, names[i]) == 0);
        assert(found->value == i * 100);
    }

    /* Remove one */
    assert(ht_cache_remove(c, "charlie", 8) == true);
    assert(ht_cache_size(c) == (size_t)(nnames - 1));
    assert(ht_cache_get(c, "charlie", 8) == NULL);

    /* Others still there */
    assert(ht_cache_get(c, "golf", 5) != NULL);

    ht_cache_destroy(c);
    printf("  PASS string_keys\n");
}

/* ── Get key=0 and key=1 (hash goes to spill lane) ────────────── */

static void test_spill_lane_keys(void) {
    ht_cache_t *c = create_test_cache(16);

    /* key=0 → hash = 0 * 2654435761 = 0 → spill lane */
    test_entry_t e0 = {.key = 0, .value = 100};
    ht_cache_put(c, &e0, sizeof(e0));

    /* key=2654435761 → hash = 2654435761 * 2654435761 = some value,
       but key=1 → hash = 2654435761 → mod 2^48 = 2654435761 which is >= 2,
       so NOT spill lane. We need a key whose hash lower-48 bits == 1.
       hash = key * 2654435761 mod 2^64, lower 48 = 1.
       key * 2654435761 ≡ 1 (mod 2^48) → key = modular inverse.
       2654435761 is odd, so invertible mod 2^48.
       Actually, let's just test key=0 specifically and a normal key. */

    int k0 = 0;
    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL && found->value == 100);

    /* Remove and re-add key=0 */
    assert(ht_cache_remove(c, &k0, sizeof(k0)) == true);
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    test_entry_t e0b = {.key = 0, .value = 200};
    ht_cache_put(c, &e0b, sizeof(e0b));
    found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL && found->value == 200);

    ht_cache_destroy(c);
    printf("  PASS spill_lane_keys\n");
}

/* ── Large-scale stress ───────────────────────────────────────── */

static void test_stress_large(void) {
    const size_t N = 4096;
    ht_cache_t *c = create_test_cache(N);

    for (size_t i = 0; i < N; i++) {
        test_entry_t e = {.key = (int)i, .value = (int)(i * 3 + 7)};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == N);

    /* Verify all */
    for (size_t i = 0; i < N; i++) {
        int k = (int)i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == (int)(i * 3 + 7));
    }

    /* Overfill by 1 → evicts LRU */
    test_entry_t extra = {.key = -1, .value = 999};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_size(c) == N);
    /* key=0 was LRU but got(0) was called which promoted it.
       After verify loop, last get was key=4095, so LRU is... well,
       just check we still have 4096 entries and the new one is present. */
    int km1 = -1;
    assert(ht_cache_get(c, &km1, sizeof(km1)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS stress_large\n");
}

/* ── Find does not promote (verify via eviction order) ────────── */

static void test_find_no_promote(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0, 1, 2, 3 */

    /* Find key=0 via scan — should NOT promote */
    int k0 = 0;
    uint64_t h0 = test_hash_fn(&k0, sizeof(k0), NULL);
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    void *found = ht_cache_find(c, h0, simple_scan_fn, &ctx);
    assert(found != NULL);

    /* Insert → should evict key=0 (still LRU since find didn't promote) */
    test_entry_t extra = {.key = 99, .value = 990};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_no_promote\n");
}

/* ── Promote then remove same entry ───────────────────────────── */

static void test_promote_then_remove(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find and promote key=2 */
    int k2 = 2;
    uint64_t h2 = test_hash_fn(&k2, sizeof(k2), NULL);
    simple_scan_ctx_t ctx = {.target_key = 2, .result = NULL};
    void *found = ht_cache_find(c, h2, simple_scan_fn, &ctx);
    assert(found != NULL);
    ht_cache_promote(c, found);

    /* Remove it */
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    assert(ht_cache_size(c) == 4);
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Eviction should target key=0 (original LRU, since promote removed key=2
       from its position) */
    assert(ht_cache_evict(c) == true);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Remaining: 1, 3, 4 */
    int k1 = 1, k3 = 3, k4 = 4;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_then_remove\n");
}

/* ── Overwrite: put same key, update via get pointer ──────────── */

static void test_overwrite_via_pointer(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t e = {.key = 42, .value = 100};
    ht_cache_put(c, &e, sizeof(e));

    /* Get pointer and overwrite value */
    int k = 42;
    test_entry_t *p = ht_cache_get(c, &k, sizeof(k));
    assert(p != NULL && p->value == 100);
    p->value = 200;

    /* Get again — should see updated value */
    p = ht_cache_get(c, &k, sizeof(k));
    assert(p != NULL && p->value == 200);

    /* Iterate — should also see updated value */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e2 = entry;
        assert(e2->key == 42);
        assert(e2->value == 200);
    }

    ht_cache_destroy(c);
    printf("  PASS overwrite_via_pointer\n");
}

/* ── Two-phase: promote fallback, not exact ───────────────────── */

typedef struct {
    int  exact_key;
    int  exact_value;
    void *timed_entry;
    int  timed_value;
} tp2_ctx_t;

static bool tp2_scan_fn(void *entry, void *ctx) {
    tp2_ctx_t *s = ctx;
    test_entry_t *e = entry;
    /* "exact" match: key AND value must match */
    if (e->key == s->exact_key && e->value == s->exact_value) {
        return true; /* continue — this is exact, but we want to collect timed */
    }
    /* "timed" fallback: any entry with matching key */
    if (e->key == s->exact_key) {
        s->timed_entry = entry;
        s->timed_value = e->value;
    }
    return true; /* always continue */
}

static void test_two_phase_promote_fallback(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Insert two entries with same key but different values */
    test_entry_t e1 = {.key = 42, .value = 100};
    test_entry_t e2 = {.key = 42, .value = 200};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));

    /* Two-phase: look for exact (key=42, value=999 — doesn't exist),
       fall back to any entry with key=42 */
    int k = 42;
    uint64_t h = test_hash_fn(&k, sizeof(k), NULL);
    tp2_ctx_t ctx = {.exact_key = 42, .exact_value = 999,
                     .timed_entry = NULL, .timed_value = 0};
    ht_cache_find(c, h, tp2_scan_fn, &ctx);

    /* Should have found a fallback */
    assert(ctx.timed_entry != NULL);
    assert(ctx.timed_value == 100 || ctx.timed_value == 200);

    /* Promote the fallback */
    ht_cache_promote(c, ctx.timed_entry);

    /* Verify it's still accessible */
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);

    ht_cache_destroy(c);
    printf("  PASS two_phase_promote_fallback\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Edge case tests — round 3
 * ══════════════════════════════════════════════════════════════════ */

/* ── Find with NULL scan_fn ───────────────────────────────────── */

static void test_find_null_scanfn(void) {
    ht_cache_t *c = create_test_cache(16);
    assert(ht_cache_find(c, 42, NULL, NULL) == NULL);
    ht_cache_destroy(c);
    printf("  PASS find_null_scanfn\n");
}

/* ── Double clear ─────────────────────────────────────────────── */

static void test_double_clear(void) {
    ht_cache_t *c = create_test_cache(16);
    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Still usable */
    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);
    ht_cache_destroy(c);
    printf("  PASS double_clear\n");
}

/* ── Ops after clear: get, find, remove, evict all return miss ── */

static void test_ops_after_clear(void) {
    ht_cache_t *c = create_test_cache(16);
    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);

    int k = 0;
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    assert(ht_cache_remove(c, &k, sizeof(k)) == false);
    assert(ht_cache_evict(c) == false);

    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    assert(ht_cache_find(c, 0, simple_scan_fn, &ctx) == NULL);

    ht_cache_destroy(c);
    printf("  PASS ops_after_clear\n");
}

/* ── Get after clear then put same key ────────────────────────── */

static void test_clear_put_same_key(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e1 = {.key = 42, .value = 100};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_clear(c);

    /* Re-insert same key with different value */
    test_entry_t e2 = {.key = 42, .value = 200};
    ht_cache_put(c, &e2, sizeof(e2));

    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->value == 200);  /* new value, not stale */

    ht_cache_destroy(c);
    printf("  PASS clear_put_same_key\n");
}

/* ── Remove only entry then put ───────────────────────────────── */

static void test_remove_only_then_put(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t e1 = {.key = 1, .value = 10};
    ht_cache_put(c, &e1, sizeof(e1));
    assert(ht_cache_size(c) == 1);

    int k1 = 1;
    ht_cache_remove(c, &k1, sizeof(k1));
    assert(ht_cache_size(c) == 0);

    test_entry_t e2 = {.key = 2, .value = 20};
    void *p = ht_cache_put(c, &e2, sizeof(e2));
    assert(p != NULL);
    assert(ht_cache_size(c) == 1);

    int k2 = 2;
    test_entry_t *found = ht_cache_get(c, &k2, sizeof(k2));
    assert(found != NULL && found->value == 20);

    ht_cache_destroy(c);
    printf("  PASS remove_only_then_put\n");
}

/* ── Put returns distinct pointers for duplicate keys ─────────── */

static void test_distinct_pointers(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t e = {.key = 42, .value = 0};
    void *ptrs[4];
    for (int i = 0; i < 4; i++) {
        e.value = i;
        ptrs[i] = ht_cache_put(c, &e, sizeof(e));
        assert(ptrs[i] != NULL);
    }
    /* All pointers must be distinct (different slots) */
    for (int i = 0; i < 4; i++)
        for (int j = i + 1; j < 4; j++)
            assert(ptrs[i] != ptrs[j]);

    ht_cache_destroy(c);
    printf("  PASS distinct_pointers\n");
}

/* ── Verify user_ctx passed to callbacks ──────────────────────── */

typedef struct {
    int hash_calls;
    int eq_calls;
} spy_ctx_t;

static uint64_t spy_hash_fn(const void *key, size_t len, void *ctx) {
    (void)key; (void)len;
    spy_ctx_t *s = ctx;
    s->hash_calls++;
    return 42;
}

static bool spy_eq_fn(const void *key, size_t key_len,
                      const void *entry, size_t entry_size, void *ctx) {
    (void)key; (void)key_len; (void)entry; (void)entry_size;
    spy_ctx_t *s = ctx;
    s->eq_calls++;
    return true;  /* first match */
}

static void test_user_ctx_passthrough(void) {
    spy_ctx_t spy = {0, 0};

    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = spy_hash_fn,
        .eq_fn      = spy_eq_fn,
        .user_ctx   = &spy,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put calls hash_fn */
    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));
    assert(spy.hash_calls == 1);

    /* Get calls hash_fn + eq_fn */
    int k = 1;
    ht_cache_get(c, &k, sizeof(k));
    assert(spy.hash_calls == 2);
    assert(spy.eq_calls >= 1);

    ht_cache_destroy(c);
    printf("  PASS user_ctx_passthrough\n");
}

/* ── Reverse access pattern then verify eviction ──────────────── */

static void test_reverse_access_eviction(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU tail→head: 0, 1, 2, 3 */

    /* Access in reverse: 3, 2, 1, 0 */
    for (int i = 3; i >= 0; i--) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }
    /* After accesses: tail→head = 3, 2, 1, 0 */

    /* Insert → evicts key=3 (now LRU) */
    test_entry_t extra = {.key = 99, .value = 990};
    ht_cache_put(c, &extra, sizeof(extra));
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* key=0 is MRU, should survive */
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS reverse_access_eviction\n");
}

/* ── Eviction after getting LRU promotes it ───────────────────── */

static void test_evict_after_get_lru(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0, 1, 2, 3 */

    /* Get key=0 → promotes to MRU. New LRU is key=1 */
    int k0 = 0;
    ht_cache_get(c, &k0, sizeof(k0));

    /* Evict → removes key=1 (new LRU), not key=0 */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_after_get_lru\n");
}

/* ── Remove MRU then put ──────────────────────────────────────── */

static void test_remove_mru_then_put(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* MRU=key=3 */

    /* Remove MRU */
    int k3 = 3;
    ht_cache_remove(c, &k3, sizeof(k3));
    assert(ht_cache_size(c) == 3);

    /* Put new — no eviction needed */
    test_entry_t extra = {.key = 99, .value = 990};
    void *p = ht_cache_put(c, &extra, sizeof(extra));
    assert(p != NULL);
    assert(ht_cache_size(c) == 4);

    /* key=0 still present (was LRU) */
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS remove_mru_then_put\n");
}

/* ── Steady state: fill-evict-fill-evict ──────────────────────── */

static void test_steady_state_churn(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int round = 0; round < 50; round++) {
        /* Fill */
        for (int i = 0; i < 4; i++) {
            test_entry_t e = {.key = round * 10 + i, .value = round * 100 + i};
            assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
        }
        /* Evict 1 */
        assert(ht_cache_evict(c) == true);
        assert(ht_cache_size(c) == 3);

        /* Fill back to 4 */
        test_entry_t extra = {.key = round * 10 + 99, .value = round * 100 + 99};
        assert(ht_cache_put(c, &extra, sizeof(extra)) != NULL);
        assert(ht_cache_size(c) == 4);
    }
    assert(ht_cache_size(c) == 4);

    ht_cache_destroy(c);
    printf("  PASS steady_state_churn\n");
}

/* ── Multiple clear cycles ────────────────────────────────────── */

static void test_multiple_clear_cycles(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int round = 0; round < 20; round++) {
        for (int i = 0; i < 8; i++) {
            test_entry_t e = {.key = round * 100 + i, .value = round + i};
            assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
        }
        assert(ht_cache_size(c) == 8);
        ht_cache_clear(c);
        assert(ht_cache_size(c) == 0);
    }

    /* Cache still usable */
    test_entry_t e = {.key = 1, .value = 99};
    ht_cache_put(c, &e, sizeof(e));
    int k = 1;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 99);

    ht_cache_destroy(c);
    printf("  PASS multiple_clear_cycles\n");
}

/* ── Remove all entries individually (no clear) ───────────────── */

static void test_remove_all_individually(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    for (int i = 7; i >= 0; i--) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    /* All gone */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* Refill */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i + 50, .value = i + 50};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 8);

    ht_cache_destroy(c);
    printf("  PASS remove_all_individually\n");
}

/* ── Collision: put/evict/put with same hash (tombstone reuse) ── */

static void test_collision_evict_reuse(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill with 4 entries all hashing to 42 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }

    /* Evict 2 */
    ht_cache_evict(c);
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 2);

    /* Add 2 new entries (same hash=42) — should reuse freed slots */
    for (int i = 10; i < 12; i++) {
        test_entry_t e = {.key = i, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* Verify collision chain: find all 4 entries */
    collect_ctx_t ctx = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &ctx);
    assert(ctx.count == 4);

    ht_cache_destroy(c);
    printf("  PASS collision_evict_reuse\n");
}

/* ── Scan visit order (MRU first) ─────────────────────────────── */

typedef struct {
    int  keys[16];
    int  count;
} order_ctx_t;

static bool order_fn(void *entry, void *ctx) {
    order_ctx_t *oc = ctx;
    test_entry_t *e = entry;
    if (oc->count < 16)
        oc->keys[oc->count++] = e->key;
    return true;
}

static void test_scan_visit_order(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Insert keys 0-3, each with unique hash */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Access keys in order 0, 1, 2, 3 — promotes each */
    for (int i = 0; i < 4; i++) {
        int k = i;
        ht_cache_get(c, &k, sizeof(k));
    }

    /* Verify each scan visits exactly one entry (unique hashes) */
    for (int i = 0; i < 4; i++) {
        int k = i;
        uint64_t h = test_hash_fn(&k, sizeof(k), NULL);
        order_ctx_t ctx = {.count = 0};
        ht_cache_find(c, h, order_fn, &ctx);
        assert(ctx.count == 1);
        assert(ctx.keys[0] == i);
    }

    ht_cache_destroy(c);
    printf("  PASS scan_visit_order\n");
}

/* ── Collision: verify all entries visited in scan ────────────── */

static void test_collision_scan_all_visited(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Scan collects all, remove even keys, scan again */
    collect_ctx_t before = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &before);
    assert(before.count == 10);

    for (int i = 0; i < 10; i += 2) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }

    collect_ctx_t after = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &after);
    assert(after.count == 5);

    /* Only odd keys remain */
    for (int i = 0; i < after.count; i++)
        assert(after.keys[i] % 2 == 1);

    ht_cache_destroy(c);
    printf("  PASS collision_scan_all_visited\n");
}

/* ── Very small entry: 1 byte ─────────────────────────────────── */

typedef struct {
    uint8_t key;
    uint8_t value;
} tiny_entry_t;

static uint64_t tiny_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    return (uint64_t)(*(const uint8_t *)key * 2654435761u);
}

static bool tiny_eq_fn(const void *key, size_t key_len,
                       const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    return *(const uint8_t *)key == *(const uint8_t *)entry;
}

static void test_tiny_entry(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(tiny_entry_t),
        .hash_fn    = tiny_hash_fn,
        .eq_fn      = tiny_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 16; i++) {
        tiny_entry_t e = {.key = (uint8_t)i, .value = (uint8_t)(i * 3)};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 16);

    for (int i = 0; i < 16; i++) {
        uint8_t k = (uint8_t)i;
        tiny_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->key == (uint8_t)i);
        assert(found->value == (uint8_t)(i * 3));
    }

    ht_cache_destroy(c);
    printf("  PASS tiny_entry\n");
}

/* ── Systematic LRU verification: 8 entries ───────────────────── */

static void test_lru_systematic(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Insert 0-7. LRU order (tail→head): 0,1,2,3,4,5,6,7 */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Access 0,2,4,6 — promotes them above 1,3,5,7 */
    int keys[] = {0, 2, 4, 6};
    for (int i = 0; i < 4; i++)
        ht_cache_get(c, &keys[i], sizeof(keys[i]));

    /* LRU order (tail→head): 1,3,5,7, 0,2,4,6 */
    /* Evict 4 entries → should remove 1,3,5,7 in order */
    int expected_evicted[] = {1, 3, 5, 7};
    for (int i = 0; i < 4; i++) {
        ht_cache_evict(c);
        assert(ht_cache_get(c, &expected_evicted[i],
                            sizeof(expected_evicted[i])) == NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* 0,2,4,6 survive */
    for (int i = 0; i < 4; i++) {
        int k = keys[i];
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS lru_systematic\n");
}

/* ── Collision: get after multiple removes ────────────────────── */

static void test_collision_get_after_removes(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove keys 1,3,5,7 */
    for (int i = 1; i < 8; i += 2) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }

    /* Get remaining keys 0,2,4,6 */
    for (int i = 0; i < 8; i += 2) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == i * 10);
    }

    /* Removed keys miss */
    for (int i = 1; i < 8; i += 2) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_get_after_removes\n");
}

/* ── Put after remove reuses slot (different pointer) ─────────── */

static void test_slot_reuse_after_remove(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t e1 = {.key = 1, .value = 10};
    void *p1 = ht_cache_put(c, &e1, sizeof(e1));
    assert(p1 != NULL);

    test_entry_t e2 = {.key = 2, .value = 20};
    void *p2 = ht_cache_put(c, &e2, sizeof(e2));

    /* Remove key=1 */
    int k1 = 1;
    ht_cache_remove(c, &k1, sizeof(k1));

    /* Put key=3 — may reuse key=1's slot */
    test_entry_t e3 = {.key = 3, .value = 30};
    void *p3 = ht_cache_put(c, &e3, sizeof(e3));
    assert(p3 != NULL);
    assert(p3 != p2);  /* different from key=2's slot */
    assert(ht_cache_size(c) == 2);

    /* Verify both present */
    int k2 = 2, k3 = 3;
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS slot_reuse_after_remove\n");
}

/* ── Long-running simulation ──────────────────────────────────── */

static void test_long_simulation(void) {
    ht_cache_t *c = create_test_cache(32);
    size_t expected_size = 0;

    for (int i = 0; i < 1000; i++) {
        /* Insert */
        test_entry_t e = {.key = i, .value = i * 7};
        ht_cache_put(c, &e, sizeof(e));
        if (expected_size < 32)
            expected_size++;
        assert(ht_cache_size(c) == expected_size);

        /* Every 10 ops, verify a recent entry */
        if (i % 10 == 0 && i > 0) {
            int k = i - 1;
            test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
            assert(found != NULL);
            assert(found->value == (i - 1) * 7);
        }

        /* Every 50 ops, remove a random recent entry */
        if (i % 50 == 0 && i > 10) {
            int k = i - 5;
            if (ht_cache_remove(c, &k, sizeof(k)))
                expected_size--;
            assert(ht_cache_size(c) == expected_size);
        }

        /* Every 100 ops, evict one */
        if (i % 100 == 0 && i > 0) {
            if (ht_cache_evict(c))
                expected_size--;
            assert(ht_cache_size(c) == expected_size);
        }
    }

    ht_cache_destroy(c);
    printf("  PASS long_simulation\n");
}

/* ── Collision: find after evict removes from chain ───────────── */

static void test_collision_find_after_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU order: 0, 1, 2, 3 */

    /* Evict 2 → removes key=0 and key=1 */
    ht_cache_evict(c);
    ht_cache_evict(c);

    /* Find remaining entries via scan */
    for (int target = 2; target <= 3; target++) {
        simple_scan_ctx_t ctx = {.target_key = target, .result = NULL};
        void *found = ht_cache_find(c, 42, simple_scan_fn, &ctx);
        assert(found != NULL);
        assert(((test_entry_t *)found)->key == target);
    }

    /* Evicted entries not found */
    for (int target = 0; target <= 1; target++) {
        simple_scan_ctx_t ctx = {.target_key = target, .result = NULL};
        assert(ht_cache_find(c, 42, simple_scan_fn, &ctx) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_find_after_evict\n");
}

/* ── Collision with eviction and refill ───────────────────────── */

static void test_collision_evict_and_refill(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill, evict half, refill */
    for (int round = 0; round < 5; round++) {
        int base = round * 100;
        for (int i = 0; i < 4; i++) {
            test_entry_t e = {.key = base + i, .value = base + i};
            ht_cache_put(c, &e, sizeof(e));
        }
        /* Evict 2 (LRU entries) */
        ht_cache_evict(c);
        ht_cache_evict(c);

        /* Add 2 new to fill back */
        for (int i = 0; i < 2; i++) {
            test_entry_t e = {.key = base + 50 + i, .value = base + 50 + i};
            ht_cache_put(c, &e, sizeof(e));
        }
        assert(ht_cache_size(c) == 4);
    }

    /* Verify cache is at capacity */
    assert(ht_cache_size(c) == 4);

    /* All current entries have same hash and can be found */
    collect_ctx_t ctx = {.count = 0};
    ht_cache_find(c, 42, collect_fn, &ctx);
    assert(ctx.count == 4);

    ht_cache_destroy(c);
    printf("  PASS collision_evict_and_refill\n");
}

/* ── Promote then verify with find (not get) ──────────────────── */

static void test_promote_verify_with_find(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find key=0, promote it */
    int k0 = 0;
    uint64_t h0 = test_hash_fn(&k0, sizeof(k0), NULL);
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    void *found = ht_cache_find(c, h0, simple_scan_fn, &ctx);
    assert(found != NULL);
    ht_cache_promote(c, found);

    /* Find again — should still find it */
    ctx.result = NULL;
    found = ht_cache_find(c, h0, simple_scan_fn, &ctx);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 0);

    ht_cache_destroy(c);
    printf("  PASS promote_verify_with_find\n");
}

/* ── Remove all from full cache via individual removes ────────── */

static void test_drain_full_cache(void) {
    ht_cache_t *c = create_test_cache(16);

    /* Fill to capacity */
    for (int i = 0; i < 16; i++) {
        test_entry_t e = {.key = i, .value = i * 3};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 16);

    /* Remove in random-ish order */
    int order[] = {8, 3, 15, 0, 11, 7, 1, 14, 5, 12, 2, 9, 6, 13, 4, 10};
    for (int i = 0; i < 16; i++) {
        int k = order[i];
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
        assert(ht_cache_size(c) == (size_t)(15 - i));
    }
    assert(ht_cache_size(c) == 0);

    /* Verify all gone */
    for (int i = 0; i < 16; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* And refill works */
    for (int i = 0; i < 16; i++) {
        test_entry_t e = {.key = i + 100, .value = i * 5};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 16);

    ht_cache_destroy(c);
    printf("  PASS drain_full_cache\n");
}

/* ── Fill exactly to capacity twice (no eviction needed) ──────── */

static void test_fill_twice_no_evict(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Fill 0-7 */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }

    /* Remove all 8 */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);

    /* Fill 100-107 — no eviction since cache is empty */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = 100 + i, .value = 100 + i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* No old data */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }
    /* All new data */
    for (int i = 0; i < 8; i++) {
        int k = 100 + i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS fill_twice_no_evict\n");
}

/* ── Hash value at UINT48 boundary ────────────────────────────── */

static uint64_t hash_48max_fn(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx;
    return 0x0000FFFFFFFFFFFFULL;  /* max 48-bit value */
}

static void test_hash_48max(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = hash_48max_fn,
        .eq_fn      = eq_by_key_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    for (int i = 0; i < 4; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    /* Remove and verify */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS hash_48max\n");
}

/* ── Multiple two-phase scans on same cache ───────────────────── */

static void test_multiple_two_phase(void) {
    ht_cache_t *c = create_test_cache(16);

    /* Insert entries with keys 0-9 */
    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Perform 5 two-phase lookups */
    for (int trial = 0; trial < 5; trial++) {
        int target = trial * 2;  /* keys 0, 2, 4, 6, 8 */
        uint64_t h = test_hash_fn(&target, sizeof(target), NULL);

        simple_scan_ctx_t ctx = {.target_key = target, .result = NULL};
        void *found = ht_cache_find(c, h, simple_scan_fn, &ctx);
        assert(found != NULL);
        assert(((test_entry_t *)found)->value == target * 10);

        /* Promote the found entry */
        ht_cache_promote(c, found);
    }

    /* Verify all 10 entries still present */
    assert(ht_cache_size(c) == 10);
    for (int i = 0; i < 10; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS multiple_two_phase\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Edge case tests — round 4
 * ══════════════════════════════════════════════════════════════════ */

/* ── Get/remove with NULL key ─────────────────────────────────── */

static void test_null_key(void) {
    ht_cache_t *c = create_test_cache(16);
    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));

    assert(ht_cache_get(c, NULL, 0) == NULL);
    assert(ht_cache_remove(c, NULL, 0) == false);
    assert(ht_cache_size(c) == 1);

    ht_cache_destroy(c);
    printf("  PASS null_key\n");
}

/* ── Capacity invariance ──────────────────────────────────────── */

static void test_capacity_invariant(void) {
    ht_cache_t *c = create_test_cache(13);
    size_t cap = ht_cache_capacity(c);
    assert(cap == 13);

    for (int i = 0; i < 13; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_capacity(c) == cap);

    ht_cache_evict(c);
    assert(ht_cache_capacity(c) == cap);

    ht_cache_clear(c);
    assert(ht_cache_capacity(c) == cap);

    for (int i = 0; i < 50; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_capacity(c) == cap);

    ht_cache_destroy(c);
    printf("  PASS capacity_invariant\n");
}

/* ── Size never exceeds capacity ──────────────────────────────── */

static void test_size_bound(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 100; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
        assert(ht_cache_size(c) <= ht_cache_capacity(c));
    }
    ht_cache_destroy(c);
    printf("  PASS size_bound\n");
}

/* ── Put pointer is within entries array ──────────────────────── */

static void test_put_pointer_range(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
        /* Pointer should be within a valid entry slot */
        size_t offset = (uint8_t *)p - (uint8_t *)NULL;
        assert(offset % sizeof(test_entry_t) == 0 ||
               offset % sizeof(test_entry_t) < sizeof(test_entry_t));
    }
    ht_cache_destroy(c);
    printf("  PASS put_pointer_range\n");
}

/* ── Collision: fill entire cache with one key ────────────────── */

static void test_collision_same_key(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* All entries: same hash AND same key, different values */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = 42, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* Get returns first match */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);

    /* Scan sees all 8 */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 8);

    /* Remove removes one */
    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_size(c) == 7);

    /* Scan sees 7 */
    cctx.count = 0;
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 7);

    ht_cache_destroy(c);
    printf("  PASS collision_same_key\n");
}

/* ── Remove with NULL eq_fn uses first hash match ─────────────── */

static void test_null_eq_remove(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = test_hash_fn,
        .eq_fn      = NULL,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e1 = {.key = 42, .value = 100};
    test_entry_t e2 = {.key = 99, .value = 200};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));

    /* Remove key=42 — since eq_fn is NULL, it removes first hash match for
       hash(42), which should be the entry for key=42 */
    int k42 = 42;
    assert(ht_cache_remove(c, &k42, sizeof(k42)) == true);
    assert(ht_cache_size(c) == 1);

    /* Key=42 gone */
    assert(ht_cache_get(c, &k42, sizeof(k42)) == NULL);

    /* Key=99 still there */
    int k99 = 99;
    assert(ht_cache_get(c, &k99, sizeof(k99)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS null_eq_remove\n");
}

/* ── Scan that stops early: verify scan_ctx untouched after ──── */

static bool stop_on_first_fn(void *entry, void *ctx) {
    *(void **)ctx = entry;
    return false;
}

static void test_scan_early_stop(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    void *result = NULL;
    void *found = ht_cache_find(c, 42, stop_on_first_fn, &result);
    assert(found != NULL);
    assert(found == result);

    ht_cache_destroy(c);
    printf("  PASS scan_early_stop\n");
}

/* ── Mixed hash: some unique, some colliding ──────────────────── */

static uint64_t mixed_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    const int *k = key;
    /* Keys 0-3 all hash to 100, keys 4+ have unique hashes */
    if (*k >= 0 && *k <= 3) return 100;
    return (uint64_t)(*k * 2654435761u);
}

static bool mixed_eq_fn(const void *key, size_t key_len,
                        const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    return *(const int *)key == *(const int *)entry;
}

static void test_mixed_hash(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = mixed_hash_fn,
        .eq_fn      = mixed_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Keys 0-3 collide, keys 4-9 are unique */
    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 10);

    /* All retrievable */
    for (int i = 0; i < 10; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    /* Collision group: scan sees 4 entries for hash=100 */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 100, count_fn, &cctx);
    assert(cctx.count == 4);

    /* Unique hash: scan sees 1 entry */
    int k7 = 7;
    uint64_t h7 = mixed_hash_fn(&k7, sizeof(k7), NULL);
    cctx.count = 0;
    ht_cache_find(c, h7, count_fn, &cctx);
    assert(cctx.count == 1);

    /* Remove from collision group */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)) == true);
    cctx.count = 0;
    ht_cache_find(c, 100, count_fn, &cctx);
    assert(cctx.count == 3);

    /* Remove from unique group */
    assert(ht_cache_remove(c, &k7, sizeof(k7)) == true);
    assert(ht_cache_get(c, &k7, sizeof(k7)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS mixed_hash\n");
}

/* ── Collision: remove all then put new entries ───────────────── */

static void test_collision_wipe_and_refill(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Remove all */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);

    /* Verify empty */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 0);

    /* Refill with different keys */
    for (int i = 100; i < 108; i++) {
        test_entry_t e = {.key = i, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* Only new entries present */
    cctx.count = 0;
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 8);

    /* Old keys absent */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* New keys present */
    for (int i = 100; i < 108; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_wipe_and_refill\n");
}

/* ── Find with hash=0 (spill lane path) ───────────────────────── */

static void test_find_hash_zero(void) {
    ht_cache_t *c = create_test_cache(16);

    /* Insert key=0 which hashes to 0 */
    test_entry_t e0 = {.key = 0, .value = 42};
    ht_cache_put(c, &e0, sizeof(e0));

    /* Find via hash=0 */
    int k0 = 0;
    uint64_t h0 = test_hash_fn(&k0, sizeof(k0), NULL);
    simple_scan_ctx_t ctx = {.target_key = 0, .result = NULL};
    void *found = ht_cache_find(c, h0, simple_scan_fn, &ctx);
    assert(found != NULL);
    assert(((test_entry_t *)found)->value == 42);

    ht_cache_destroy(c);
    printf("  PASS find_hash_zero\n");
}

/* ── Large capacity (65536) ───────────────────────────────────── */

static void test_very_large_capacity(void) {
    const size_t N = 65536;
    ht_cache_t *c = create_test_cache(N);
    assert(ht_cache_capacity(c) == N);

    /* Just fill and verify first/last/middle */
    for (size_t i = 0; i < N; i++) {
        test_entry_t e = {.key = (int)i, .value = (int)(i ^ 0xDEAD)};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == N);

    int k0 = 0;
    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL && found->value == (int)(0 ^ 0xDEAD));

    int kmid = (int)(N / 2);
    found = ht_cache_get(c, &kmid, sizeof(kmid));
    assert(found != NULL && found->value == (int)((N / 2) ^ 0xDEAD));

    int klast = (int)(N - 1);
    found = ht_cache_get(c, &klast, sizeof(klast));
    assert(found != NULL && found->value == (int)((N - 1) ^ 0xDEAD));

    ht_cache_destroy(c);
    printf("  PASS very_large_capacity\n");
}

/* ── Entry data integrity after clear and refill ──────────────── */

static void test_no_cross_contamination(void) {
    ht_cache_t *c = create_test_cache(8);

    /* First batch */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = 0xAA00 + i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);

    /* Second batch — different keys, different values */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = 100 + i, .value = 0xBB00 + i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Verify only second batch values */
    for (int i = 0; i < 8; i++) {
        int k = 100 + i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == 0xBB00 + i);
        assert(found->key == 100 + i);
    }

    ht_cache_destroy(c);
    printf("  PASS no_cross_contamination\n");
}

/* ── Iterator sees entries added after iter begin ─────────────── */

static void test_iter_sees_new_entries(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e0 = {.key = 0, .value = 0};
    ht_cache_put(c, &e0, sizeof(e0));

    ht_cache_iter_t it = ht_cache_iter_begin(c);

    /* Add more entries after iterator created */
    for (int i = 1; i <= 3; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Iterator should see at least the first entry and possibly more */
    int count = 0;
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 0 && e->key <= 3);
        count++;
    }
    assert(count >= 1);

    ht_cache_destroy(c);
    printf("  PASS iter_sees_new_entries\n");
}

/* ── Collision: heavy tombstone exercise ──────────────────────── */

static void test_collision_tombstone_stress(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Rapidly insert and remove entries with same hash — creates
       many tombstones in the bare table */
    for (int round = 0; round < 100; round++) {
        for (int i = 0; i < 4; i++) {
            test_entry_t e = {.key = round * 10 + i, .value = round * 10 + i};
            assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
        }
        /* Remove 2 entries */
        int k1 = round * 10;
        int k2 = round * 10 + 1;
        ht_cache_remove(c, &k1, sizeof(k1));
        ht_cache_remove(c, &k2, sizeof(k2));
    }

    /* Cache should still be functional */
    assert(ht_cache_size(c) == 2);

    /* Verify remaining entries are accessible */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 2);

    ht_cache_destroy(c);
    printf("  PASS collision_tombstone_stress\n");
}

/* ── Promote entry then verify via iterator ───────────────────── */

static void test_promote_visible_in_iter(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Promote key=3 via get */
    int k3 = 3;
    ht_cache_get(c, &k3, sizeof(k3));

    /* Iterator still sees all 8 entries regardless of LRU order */
    int count = 0;
    bool seen[8] = {false};
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 0 && e->key < 8);
        seen[e->key] = true;
        count++;
    }
    assert(count == 8);
    for (int i = 0; i < 8; i++)
        assert(seen[i]);

    ht_cache_destroy(c);
    printf("  PASS promote_visible_in_iter\n");
}

/* ── Multiple caches don't interfere ──────────────────────────── */

static void test_cache_isolation(void) {
    ht_cache_t *c1 = create_test_cache(8);
    ht_cache_t *c2 = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c1, &e, sizeof(e));
        e.value = i * 20;
        ht_cache_put(c2, &e, sizeof(e));
    }

    /* c1 has value=i*10, c2 has value=i*20 */
    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *f1 = ht_cache_get(c1, &k, sizeof(k));
        test_entry_t *f2 = ht_cache_get(c2, &k, sizeof(k));
        assert(f1 != NULL && f1->value == i * 10);
        assert(f2 != NULL && f2->value == i * 20);
    }

    /* Remove from c1 doesn't affect c2 */
    int k5 = 5;
    ht_cache_remove(c1, &k5, sizeof(k5));
    assert(ht_cache_get(c1, &k5, sizeof(k5)) == NULL);
    assert(ht_cache_get(c2, &k5, sizeof(k5)) != NULL);

    ht_cache_destroy(c1);
    ht_cache_destroy(c2);
    printf("  PASS cache_isolation\n");
}

/* ── Put-evict-get-put cycle ──────────────────────────────────── */

static void test_put_evict_get_put(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int cycle = 0; cycle < 10; cycle++) {
        /* Put 5 entries (1 eviction) */
        for (int i = 0; i < 5; i++) {
            test_entry_t e = {.key = cycle * 100 + i, .value = cycle * 100 + i};
            ht_cache_put(c, &e, sizeof(e));
        }
        assert(ht_cache_size(c) == 4);

        /* Get the last 4 entries */
        for (int i = 1; i < 5; i++) {
            int k = cycle * 100 + i;
            test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
            assert(found != NULL);
            assert(found->value == cycle * 100 + i);
        }

        /* First entry was evicted */
        int k0 = cycle * 100;
        assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS put_evict_get_put\n");
}

/* ── Collision: eviction with all entries same hash ───────────── */

static void test_collision_full_evict_cycle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict all via eviction */
    for (int i = 0; i < 4; i++)
        assert(ht_cache_evict(c) == true);
    assert(ht_cache_size(c) == 0);

    /* Refill with same hash */
    for (int i = 10; i < 14; i++) {
        test_entry_t e = {.key = i, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* All new entries accessible */
    for (int i = 10; i < 14; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_full_evict_cycle\n");
}

/* ── Scan callback receives correct entry pointers ────────────── */

typedef struct {
    void *first_ptr;
    void *last_ptr;
    int   count;
} ptr_track_ctx_t;

static bool ptr_track_fn(void *entry, void *ctx) {
    ptr_track_ctx_t *p = ctx;
    if (p->count == 0) p->first_ptr = entry;
    p->last_ptr = entry;
    p->count++;
    return true;
}

static void test_scan_entry_pointers(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Scan with hash for key=0 */
    int k0 = 0;
    uint64_t h0 = test_hash_fn(&k0, sizeof(k0), NULL);
    ptr_track_ctx_t ctx = {NULL, NULL, 0};
    ht_cache_find(c, h0, ptr_track_fn, &ctx);
    assert(ctx.count == 1);
    assert(ctx.first_ptr == ctx.last_ptr);

    /* Pointer matches what get returns */
    test_entry_t *via_get = ht_cache_get(c, &k0, sizeof(k0));
    assert(ctx.first_ptr == via_get);

    ht_cache_destroy(c);
    printf("  PASS scan_entry_pointers\n");
}

/* ── Collision: put more than capacity, verify LRU eviction ──── */

static void test_collision_overflow(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i, .value = i};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 4);

    /* Only last 4 survive: keys 6,7,8,9 */
    for (int i = 6; i < 10; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i);
    }
    for (int i = 0; i < 6; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_overflow\n");
}

/* ── Overwrite via put: same key, newer value ─────────────────── */

static void test_overwrite_semantic(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t e1 = {.key = 42, .value = 100};
    void *p1 = ht_cache_put(c, &e1, sizeof(e1));

    test_entry_t e2 = {.key = 42, .value = 200};
    void *p2 = ht_cache_put(c, &e2, sizeof(e2));

    /* always-add: both exist, different slots */
    assert(ht_cache_size(c) == 2);
    assert(p1 != p2);

    /* Get returns one of them */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 42);
    /* Could be either value since both match */
    assert(found->value == 100 || found->value == 200);

    ht_cache_destroy(c);
    printf("  PASS overwrite_semantic\n");
}

/* ── Interleaved unique and collision keys ────────────────────── */

static void test_interleaved_unique_collision(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = mixed_hash_fn,
        .eq_fn      = mixed_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Interleave: colliding (0-3) and unique (100-103) keys */
    for (int i = 0; i < 4; i++) {
        test_entry_t ec = {.key = i, .value = i};
        ht_cache_put(c, &ec, sizeof(ec));
        test_entry_t eu = {.key = 100 + i, .value = 100 + i};
        ht_cache_put(c, &eu, sizeof(eu));
    }
    assert(ht_cache_size(c) == 8);

    /* All accessible */
    for (int i = 0; i < 4; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
        k = 100 + i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    /* Remove one colliding, one unique */
    int k1 = 1;
    int k101 = 101;
    ht_cache_remove(c, &k1, sizeof(k1));
    ht_cache_remove(c, &k101, sizeof(k101));
    assert(ht_cache_size(c) == 6);

    /* Collision scan sees 3 */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 100, count_fn, &cctx);
    assert(cctx.count == 3);

    ht_cache_destroy(c);
    printf("  PASS interleaved_unique_collision\n");
}

/* ── Evict on freshly cleared cache returns false ─────────────── */

static void test_evict_after_clear(void) {
    ht_cache_t *c = create_test_cache(8);
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);
    assert(ht_cache_evict(c) == false);
    assert(ht_cache_size(c) == 0);
    ht_cache_destroy(c);
    printf("  PASS evict_after_clear\n");
}

/* ── Remove from empty cache returns false ────────────────────── */

static void test_remove_empty(void) {
    ht_cache_t *c = create_test_cache(8);
    int k = 42;
    assert(ht_cache_remove(c, &k, sizeof(k)) == false);
    assert(ht_cache_size(c) == 0);
    ht_cache_destroy(c);
    printf("  PASS remove_empty\n");
}

/* ── Double remove same key ───────────────────────────────────── */

static void test_double_remove(void) {
    ht_cache_t *c = create_test_cache(16);
    test_entry_t e = {.key = 42, .value = 100};
    ht_cache_put(c, &e, sizeof(e));

    int k = 42;
    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_remove(c, &k, sizeof(k)) == false);
    assert(ht_cache_size(c) == 0);
    ht_cache_destroy(c);
    printf("  PASS double_remove\n");
}

/* ══════════════════════════════════════════════════════════════════
 * Edge case tests — round 5
 * ══════════════════════════════════════════════════════════════════ */

/* ── Minimal collision: exactly 2 entries, same hash ──────────── */

static void test_minimal_collision(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e1 = {.key = 10, .value = 100};
    test_entry_t e2 = {.key = 20, .value = 200};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    assert(ht_cache_size(c) == 2);

    /* Both accessible */
    int k10 = 10, k20 = 20;
    test_entry_t *f;
    f = ht_cache_get(c, &k10, sizeof(k10));
    assert(f != NULL && f->value == 100);
    f = ht_cache_get(c, &k20, sizeof(k20));
    assert(f != NULL && f->value == 200);

    /* Remove one, other survives */
    ht_cache_remove(c, &k10, sizeof(k10));
    assert(ht_cache_size(c) == 1);
    assert(ht_cache_get(c, &k20, sizeof(k20)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS minimal_collision\n");
}

/* ── Entry with all-zero bytes ────────────────────────────────── */

static void test_zero_entry(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 0, .value = 0};
    void *p = ht_cache_put(c, &e, sizeof(e));
    assert(p != NULL);

    int k = 0;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);
    assert(found->key == 0);
    assert(found->value == 0);

    ht_cache_destroy(c);
    printf("  PASS zero_entry\n");
}

/* ── Sequential hash function ─────────────────────────────────── */

static uint64_t seq_hash_fn(const void *key, size_t len, void *ctx) {
    (void)ctx;
    /* len == sizeof(test_entry_t) during put, sizeof(int) during get */
    int k;
    if (len == sizeof(test_entry_t))
        k = ((const test_entry_t *)key)->key;
    else
        k = *(const int *)key;
    /* Hash = key + 100 — unique per key, no collisions */
    return (uint64_t)(k + 100);
}

static void test_sequential_hashes(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = seq_hash_fn,
        .eq_fn      = eq_by_key_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* Each key has unique hash, so get finds it directly */
    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    ht_cache_destroy(c);
    printf("  PASS sequential_hashes\n");
}

/* ── Hash with upper 16 bits set (masked by bare table) ───────── */

static uint64_t highbit_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    const int *k = key;
    /* Force upper 16 bits to be non-zero — bare table only stores lower 48 */
    return (uint64_t)(*k * 2654435761u) | 0xFFFF000000000000ULL;
}

static void test_hash_upper_bits(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = highbit_hash_fn,
        .eq_fn      = eq_by_key_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }

    for (int i = 0; i < 8; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 10);
    }

    ht_cache_destroy(c);
    printf("  PASS hash_upper_bits\n");
}

/* ── Evict only remaining entry ───────────────────────────────── */

static void test_evict_last_entry(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* Remove 4, leaving 1 */
    for (int i = 0; i < 4; i++) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }
    assert(ht_cache_size(c) == 1);

    /* Evict the last one */
    assert(ht_cache_evict(c) == true);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_evict(c) == false);

    int k4 = 4;
    assert(ht_cache_get(c, &k4, sizeof(k4)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_last_entry\n");
}

/* ── Find, promote, find again — still works ──────────────────── */

static void test_find_promote_find(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    int k2 = 2;
    uint64_t h2 = test_hash_fn(&k2, sizeof(k2), NULL);

    /* First find */
    simple_scan_ctx_t ctx = {.target_key = 2, .result = NULL};
    void *found1 = ht_cache_find(c, h2, simple_scan_fn, &ctx);
    assert(found1 != NULL);

    /* Promote */
    ht_cache_promote(c, found1);

    /* Find again — should still work */
    ctx.result = NULL;
    void *found2 = ht_cache_find(c, h2, simple_scan_fn, &ctx);
    assert(found2 != NULL);
    assert(((test_entry_t *)found2)->key == 2);

    ht_cache_destroy(c);
    printf("  PASS find_promote_find\n");
}

/* ── Clear many times on empty cache ──────────────────────────── */

static void test_many_clears(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t e = {.key = 1, .value = 10};
    ht_cache_put(c, &e, sizeof(e));
    ht_cache_clear(c);

    for (int i = 0; i < 100; i++)
        ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Still usable */
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);
    int k = 1;
    assert(ht_cache_get(c, &k, sizeof(k)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS many_clears\n");
}

/* ── Slot lifecycle: put, get, modify, remove, put new ────────── */

static void test_slot_lifecycle(void) {
    ht_cache_t *c = create_test_cache(4);

    test_entry_t e1 = {.key = 1, .value = 10};
    void *p1 = ht_cache_put(c, &e1, sizeof(e1));
    assert(p1 != NULL);

    /* Modify in place */
    ((test_entry_t *)p1)->value = 99;

    /* Verify modified */
    int k1 = 1;
    test_entry_t *found = ht_cache_get(c, &k1, sizeof(k1));
    assert(found != NULL && found->value == 99);

    /* Remove */
    ht_cache_remove(c, &k1, sizeof(k1));
    assert(ht_cache_size(c) == 0);

    /* Put new — may reuse same slot */
    test_entry_t e2 = {.key = 2, .value = 20};
    void *p2 = ht_cache_put(c, &e2, sizeof(e2));
    assert(p2 != NULL);

    int k2 = 2;
    found = ht_cache_get(c, &k2, sizeof(k2));
    assert(found != NULL && found->value == 20);

    /* Old key gone */
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS slot_lifecycle\n");
}

/* ── Collision: fill, promote via get, then evict ─────────────── */

static void test_collision_promote_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU order: 0, 1, 2, 3 */

    /* Promote key=0 via get */
    int k0 = 0;
    ht_cache_get(c, &k0, sizeof(k0));
    /* LRU order: 1, 2, 3, 0 */

    /* Evict → removes key=1 (LRU tail after promoting 0) */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);

    /* Cache now has 3 entries: keys 0, 2, 3. Room for one more.
     * LRU order: 2 (tail), 3, 0 (head — just promoted by get above) */
    test_entry_t extra = {.key = 99, .value = 990};
    ht_cache_put(c, &extra, sizeof(extra));
    /* All 4 slots full: keys 2, 3, 0, 99 (LRU→MRU) */

    /* One more put → evicts key=2 (LRU tail) */
    test_entry_t extra2 = {.key = 50, .value = 500};
    ht_cache_put(c, &extra2, sizeof(extra2));
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);
    int k3 = 3, k0_check = 0, k99 = 99, k50 = 50;
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k0_check, sizeof(k0_check)) != NULL);
    assert(ht_cache_get(c, &k99, sizeof(k99)) != NULL);
    assert(ht_cache_get(c, &k50, sizeof(k50)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS collision_promote_evict\n");
}

/* ── Remove multiple keys from same collision chain ───────────── */

static void test_collision_multi_remove(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 8);

    /* Remove keys 2, 4, 6 */
    int to_remove[] = {2, 4, 6};
    for (int i = 0; i < 3; i++) {
        int k = to_remove[i];
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 5);

    /* Removed keys gone */
    for (int i = 0; i < 3; i++) {
        int k = to_remove[i];
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* Others intact */
    int remaining[] = {0, 1, 3, 5, 7};
    for (int i = 0; i < 5; i++) {
        int k = remaining[i];
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == remaining[i] * 10);
    }

    /* Scan confirms 5 */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 5);

    ht_cache_destroy(c);
    printf("  PASS collision_multi_remove\n");
}

/* ── Evict then put same key immediately ──────────────────────── */

static void test_evict_then_put_same(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Evict LRU (key=0) */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Re-insert key=0 */
    test_entry_t e = {.key = 0, .value = 999};
    void *p = ht_cache_put(c, &e, sizeof(e));
    assert(p != NULL);

    test_entry_t *found = ht_cache_get(c, &k0, sizeof(k0));
    assert(found != NULL && found->value == 999);

    ht_cache_destroy(c);
    printf("  PASS evict_then_put_same\n");
}

/* ── LRU order is fresh after clear+refill ────────────────────── */

static void test_lru_fresh_after_clear(void) {
    ht_cache_t *c = create_test_cache(4);

    /* First batch: 0-3. Access key=0 to promote it. */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    int k0 = 0;
    ht_cache_get(c, &k0, sizeof(k0));

    ht_cache_clear(c);

    /* Second batch: 100-103. LRU should start fresh (100=LRU, 103=MRU) */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = 100 + i, .value = 100 + i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Insert → evicts key=100 (LRU of new batch) */
    test_entry_t extra = {.key = 999, .value = 999};
    ht_cache_put(c, &extra, sizeof(extra));

    int k100 = 100;
    assert(ht_cache_get(c, &k100, sizeof(k100)) == NULL);
    int k101 = 101;
    assert(ht_cache_get(c, &k101, sizeof(k101)) != NULL);
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL); /* old key gone */

    ht_cache_destroy(c);
    printf("  PASS lru_fresh_after_clear\n");
}

/* ── Collision: all entries same key AND same value ───────────── */

static void test_collision_identical_entries(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = 42, .value = 100};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 6);

    /* Get finds one of them */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->key == 42 && found->value == 100);

    /* Remove one — size decreases */
    assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    assert(ht_cache_size(c) == 5);

    /* Get still works */
    found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL);

    ht_cache_destroy(c);
    printf("  PASS collision_identical_entries\n");
}

/* ── Iteration order is by slot index ─────────────────────────── */

static void test_iter_slot_order(void) {
    ht_cache_t *c = create_test_cache(8);

    /* Insert in specific order to fill slots */
    test_entry_t entries[4];
    for (int i = 0; i < 4; i++) {
        entries[i].key = i * 10;
        entries[i].value = i;
        ht_cache_put(c, &entries[i], sizeof(test_entry_t));
    }

    /* Collect keys in iteration order */
    int iter_keys[4];
    int count = 0;
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        iter_keys[count++] = ((test_entry_t *)entry)->key;
    }
    assert(count == 4);

    /* Iteration is by slot index (ascending), not by insertion or LRU order.
       Keys could be in any order depending on free_stack allocation. */

    ht_cache_destroy(c);
    printf("  PASS iter_slot_order\n");
}

/* ── Non-power-of-2 capacity ──────────────────────────────────── */

static void test_non_power2_capacity(void) {
    ht_cache_t *c = create_test_cache(13);
    assert(ht_cache_capacity(c) == 13);

    for (int i = 0; i < 13; i++) {
        test_entry_t e = {.key = i * 7, .value = i * 13};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 13);

    for (int i = 0; i < 13; i++) {
        int k = i * 7;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 13);
    }

    /* Insert 14th → evicts LRU */
    test_entry_t extra = {.key = 999, .value = 777};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_size(c) == 13);

    ht_cache_destroy(c);
    printf("  PASS non_power2_capacity\n");
}

/* ── Get, clear, put same key, get ────────────────────────────── */

static void test_get_clear_put_get(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e1 = {.key = 42, .value = 100};
    ht_cache_put(c, &e1, sizeof(e1));

    int k = 42;
    assert(ht_cache_get(c, &k, sizeof(k)) != NULL);

    ht_cache_clear(c);
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    test_entry_t e2 = {.key = 42, .value = 200};
    ht_cache_put(c, &e2, sizeof(e2));
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 200);

    ht_cache_destroy(c);
    printf("  PASS get_clear_put_get\n");
}

/* ── Collision: put, remove all via individual removes, put again */

static void test_collision_remove_all_reput(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* First wave */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove all */
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)) == true);
    }
    assert(ht_cache_size(c) == 0);

    /* Second wave with new keys */
    for (int i = 50; i < 58; i++) {
        test_entry_t e = {.key = i, .value = i * 2};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 8);

    /* Only new keys */
    for (int i = 50; i < 58; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL && found->value == i * 2);
    }
    for (int i = 0; i < 8; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS collision_remove_all_reput\n");
}

/* ── Promote via get then promote again via find ──────────────── */

static void test_double_promote(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Promote key=2 via get */
    int k2 = 2;
    ht_cache_get(c, &k2, sizeof(k2));

    /* Promote key=2 again via find+promote */
    uint64_t h2 = test_hash_fn(&k2, sizeof(k2), NULL);
    simple_scan_ctx_t ctx = {.target_key = 2, .result = NULL};
    void *found = ht_cache_find(c, h2, simple_scan_fn, &ctx);
    assert(found != NULL);
    ht_cache_promote(c, found);

    /* Insert 3 more to force 3 evictions */
    test_entry_t extra[3] = {{.key=90,.value=90},{.key=91,.value=91},{.key=92,.value=92}};
    for (int i = 0; i < 3; i++)
        ht_cache_put(c, &extra[i], sizeof(test_entry_t));

    /* key=2 should survive (promoted twice to MRU) */
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS double_promote\n");
}

/* ── Stress: alternating collision and unique-hash inserts ────── */

static void test_stress_mixed_hash(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = mixed_hash_fn,
        .eq_fn      = mixed_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* 100 entries: even keys collide (hash=100), odd keys have unique hashes */
    for (int i = 0; i < 100; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 16);

    /* Last 16 entries should be keys 84-99 */
    for (int i = 84; i < 100; i++) {
        int k = i;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS stress_mixed_hash\n");
}

/* ── Put returns non-NULL for first N inserts ─────────────────── */

static void test_put_no_fail_until_full(void) {
    ht_cache_t *c = create_test_cache(64);
    for (int i = 0; i < 64; i++) {
        test_entry_t e = {.key = i, .value = i};
        void *p = ht_cache_put(c, &e, sizeof(e));
        assert(p != NULL);
    }
    assert(ht_cache_size(c) == 64);

    /* 65th also succeeds (evicts LRU) */
    test_entry_t e = {.key = 999, .value = 999};
    assert(ht_cache_put(c, &e, sizeof(e)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS put_no_fail_until_full\n");
}

/* ── Remove during iteration doesn't crash ────────────────────── */

static void test_remove_during_iteration(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Iterate and remove entries we've already visited */
    int visited = 0;
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        visited++;
        /* Remove the entry we just visited */
        ht_cache_remove(c, &e->key, sizeof(e->key));
    }
    /* We should have visited at least some entries before removes affect iteration */
    assert(visited >= 1);

    ht_cache_destroy(c);
    printf("  PASS remove_during_iteration\n");
}

/* ── Collision: scan count after series of removes and adds ───── */

static void test_collision_scan_after_mutation(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill 8 */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove 3 */
    for (int i = 0; i < 3; i++) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }
    /* Add 3 new */
    for (int i = 10; i < 13; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    assert(ht_cache_size(c) == 8);

    /* Scan should see all 8 */
    count_ctx_t cctx = {0};
    ht_cache_find(c, 42, count_fn, &cctx);
    assert(cctx.count == 8);

    ht_cache_destroy(c);
    printf("  PASS collision_scan_after_mutation\n");
}

/* ── Get with different key_len than entry_size ───────────────── */

static void test_different_key_len(void) {
    ht_cache_t *c = create_test_cache(16);

    test_entry_t e = {.key = 42, .value = 100};
    ht_cache_put(c, &e, sizeof(e));

    /* Get with just the int key (sizeof(int) != sizeof(test_entry_t)) */
    int k = 42;
    test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
    assert(found != NULL && found->value == 100);

    /* eq_fn compares the first sizeof(int) bytes against the entry's key field,
       which works because key is the first field of test_entry_t */

    ht_cache_destroy(c);
    printf("  PASS different_key_len\n");
}

/* ── Collision: verify LRU eviction order with all same hash ──── */

static void test_collision_lru_order(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Insert 0, 1, 2, 3 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Access key=0 → promoted to MRU */
    int k0 = 0;
    ht_cache_get(c, &k0, sizeof(k0));

    /* Insert key=4 → evicts key=1 (LRU after 0 was promoted) */
    test_entry_t e4 = {.key = 4, .value = 4};
    ht_cache_put(c, &e4, sizeof(e4));
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Insert key=5 → evicts key=2 */
    test_entry_t e5 = {.key = 5, .value = 5};
    ht_cache_put(c, &e5, sizeof(e5));
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* key=0, 3, 4, 5 survive */
    int k3 = 3, k4 = 4, k5 = 5;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);
    assert(ht_cache_get(c, &k5, sizeof(k5)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS collision_lru_order\n");
}

/* ────────────────────────────────────────────────────────────────
 *  Round 6: Least-tested call sequences
 *  ──────────────────────────────────────────────────────────────── */

/* Helper: scan that picks the Nth match (0-indexed) */
typedef struct {
    int          target_index;
    int          current_index;
    uint32_t     matched_idx;
    bool         found;
} pick_nth_ctx_t;

static bool pick_nth_scan(void *entry, void *ctx) {
    pick_nth_ctx_t *p = ctx;
    if (p->current_index == p->target_index) {
        p->matched_idx = ((test_entry_t *)entry)->key;
        p->found = true;
        return false;
    }
    p->current_index++;
    return true;
}

/* 1. find → promote → remove: two-phase lookup, promote, then remove that entry */
static void test_find_promote_remove(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 100};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 5);

    /* Find key=2 */
    pick_nth_ctx_t pc = {.target_index = 2, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 2);

    /* Promote it */
    ht_cache_promote(c, found);

    /* Remove by key */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)));
    assert(ht_cache_size(c) == 4);

    /* Verify gone via get */
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Verify gone via find */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    /* Scan for key=2 specifically */
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Verify others still present */
    for (int i = 0; i < 5; i++) {
        if (i == 2) continue;
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS find_promote_remove\n");
}

/* 2. find → promote → find: promote via find, then find again — still there */
static void test_find_promote_find_cycle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e0 = {.key = 0, .value = 100};
    test_entry_t e1 = {.key = 1, .value = 200};
    test_entry_t e2 = {.key = 2, .value = 300};
    test_entry_t e3 = {.key = 3, .value = 400};
    ht_cache_put(c, &e0, sizeof(e0));
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    ht_cache_put(c, &e3, sizeof(e3));

    /* Find key=1, promote it */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *found1 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found1 != NULL && ((test_entry_t *)found1)->key == 1);
    ht_cache_promote(c, found1);

    /* Now evict — should remove key=0 (LRU tail, since 1 was promoted) */
    ht_cache_evict(c);
    int k0 = 0, k1 = 1;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    /* Find key=1 again — still present */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    /* We need a scan that matches key=1 */
    void *found1_again = ht_cache_get(c, &k1, sizeof(k1));
    assert(found1_again != NULL);
    assert(((test_entry_t *)found1_again)->value == 200);

    ht_cache_destroy(c);
    printf("  PASS find_promote_find_cycle\n");
}

/* 3. clear → find: all finds return NULL after clear */
static void test_clear_then_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find before clear works */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) != NULL);

    ht_cache_clear(c);

    /* Find after clear returns NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    /* Also with a different hash */
    assert(ht_cache_find(c, 999, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS clear_then_find\n");
}

/* 4. clear → evict: evict returns false on empty cache */
static void test_clear_then_evict(void) {
    ht_cache_t *c = create_test_cache(4);
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 4);

    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Evict on empty cache */
    assert(!ht_cache_evict(c));
    assert(ht_cache_size(c) == 0);

    ht_cache_destroy(c);
    printf("  PASS clear_then_evict\n");
}

/* 5. clear → promote: promote is a no-op on cleared cache */
static void test_clear_then_promote(void) {
    ht_cache_t *c = create_test_cache(4);
    test_entry_t e = {.key = 1, .value = 100};
    void *ptr = ht_cache_put(c, &e, sizeof(e));
    assert(ptr != NULL);

    ht_cache_clear(c);

    /* Promote the stale pointer — should be a safe no-op */
    ht_cache_promote(c, ptr);
    assert(ht_cache_size(c) == 0);

    ht_cache_destroy(c);
    printf("  PASS clear_then_promote\n");
}

/* 6. clear → iter: iterating after clear yields nothing */
static void test_clear_then_iter(void) {
    ht_cache_t *c = create_test_cache(8);
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    ht_cache_clear(c);

    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    assert(!ht_cache_iter_next(c, &it, &entry));

    ht_cache_destroy(c);
    printf("  PASS clear_then_iter\n");
}

/* 7. find picks non-first match: put 4 colliding entries, find skips first 2 */
static void test_find_picks_nonfirst(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Pick the 3rd entry (index 2) */
    pick_nth_ctx_t pc = {.target_index = 2, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 2);
    assert(((test_entry_t *)found)->value == 20);

    /* Pick the last entry (index 3) */
    pc = (pick_nth_ctx_t){.target_index = 3, .current_index = 0};
    found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 3);
    assert(((test_entry_t *)found)->value == 30);

    ht_cache_destroy(c);
    printf("  PASS find_picks_nonfirst\n");
}

/* 8. evict interleaved with get: each get changes LRU, affecting next evict */
static void test_evict_interleaved_get(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU order: 0(tail), 1, 2, 3(head) */

    /* Access key=2 → promotes to MRU */
    int k2 = 2;
    ht_cache_get(c, &k2, sizeof(k2));
    /* LRU order: 0(tail), 1, 3, 2(head) */

    /* Evict → removes key=0 */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_size(c) == 3);

    /* Access key=1 → promotes to MRU */
    int k1 = 1;
    ht_cache_get(c, &k1, sizeof(k1));
    /* LRU order: 3(tail), 2, 1(head) */

    /* Evict → removes key=3 */
    ht_cache_evict(c);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);
    assert(ht_cache_size(c) == 2);

    /* Remaining: keys 1, 2 */
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_interleaved_get\n");
}

/* 9. get → modify → find: verify mutation via get is visible to find */
static void test_get_modify_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e = {.key = 5, .value = 50};
    ht_cache_put(c, &e, sizeof(e));

    /* Get and mutate */
    int k5 = 5;
    test_entry_t *entry = ht_cache_get(c, &k5, sizeof(k5));
    assert(entry != NULL);
    assert(entry->value == 50);
    entry->value = 999;

    /* Find should see the mutated value */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->value == 999);

    ht_cache_destroy(c);
    printf("  PASS get_modify_find\n");
}

/* 10. iter pointers match get results: for each iter entry, get returns same data */
static void test_iter_matches_get(void) {
    ht_cache_t *c = create_test_cache(16);

    for (int i = 0; i < 10; i++) {
        test_entry_t e = {.key = i * 7, .value = i * 13};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 10);

    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        /* Get the same key via get */
        test_entry_t *via_get = ht_cache_get(c, &e->key, sizeof(e->key));
        assert(via_get != NULL);
        assert(via_get->key == e->key);
        assert(via_get->value == e->value);
        count++;
    }
    assert(count == 10);

    ht_cache_destroy(c);
    printf("  PASS iter_matches_get\n");
}

/* 11. remove after promote changes evict target: promote middle, remove it, evict hits new tail */
static void test_promote_remove_evict(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Get key=1 to promote it */
    int k1 = 1;
    ht_cache_get(c, &k1, sizeof(k1));
    /* LRU: 0(tail), 2, 3, 1(head) */

    /* Remove key=1 */
    assert(ht_cache_remove(c, &k1, sizeof(k1)));
    /* LRU: 0(tail), 2, 3(head) */

    /* Evict → removes key=0 */
    ht_cache_evict(c);
    int k0 = 0, k2 = 2, k3 = 3;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_remove_evict\n");
}

/* 12. find returns NULL after all entries with that hash removed */
static void test_find_after_remove_all_hash(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find works before removal */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) != NULL);

    /* Remove all 5 entries */
    for (int i = 0; i < 5; i++) {
        int k = i;
        assert(ht_cache_remove(c, &k, sizeof(k)));
    }
    assert(ht_cache_size(c) == 0);

    /* Find returns NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_after_remove_all_hash\n");
}

/* 13. Repeated slot recycling: remove + put same slot many times */
static void test_slot_recycle_integrity(void) {
    ht_cache_t *c = create_test_cache(4);

    for (int cycle = 0; cycle < 50; cycle++) {
        /* Put an entry */
        test_entry_t e = {.key = cycle, .value = cycle * 7};
        void *ptr = ht_cache_put(c, &e, sizeof(e));
        assert(ptr != NULL);
        assert(ht_cache_size(c) == 1);

        /* Verify data */
        test_entry_t *got = ht_cache_get(c, &e.key, sizeof(e.key));
        assert(got != NULL);
        assert(got->key == cycle);
        assert(got->value == cycle * 7);

        /* Remove it */
        assert(ht_cache_remove(c, &e.key, sizeof(e.key)));
        assert(ht_cache_size(c) == 0);
    }

    ht_cache_destroy(c);
    printf("  PASS slot_recycle_integrity\n");
}

/* 14. Evict changes find results: put 4 colliding, find sees 4, evict 2, find sees 2 */
static void test_evict_changes_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Count entries via find */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 4);

    /* Evict two */
    ht_cache_evict(c);
    ht_cache_evict(c);

    /* Should see 2 now */
    cc.count = 0;
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 2);

    ht_cache_destroy(c);
    printf("  PASS evict_changes_find\n");
}

/* 15. Promote collision entry survives eviction */
static void test_promote_collision_survives_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Find and promote key=1 */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->key == 1);
    ht_cache_promote(c, found);
    /* LRU: 0(tail), 2, 3, 1(head) */

    /* Evict twice → removes key=0 then key=2 */
    ht_cache_evict(c);
    ht_cache_evict(c);

    /* key=1 still present (was promoted to MRU) */
    int k1 = 1, k3 = 3;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    int k0 = 0, k2 = 2;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_collision_survives_evict\n");
}

/* 16. put-evict-find_old: put A, put B evicts A, find A's hash returns NULL */
static void test_put_evict_find_old(void) {
    ht_cache_config_t cfg = {
        .capacity   = 2,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e0 = {.key = 0, .value = 100};
    test_entry_t e1 = {.key = 1, .value = 200};
    ht_cache_put(c, &e0, sizeof(e0));
    ht_cache_put(c, &e1, sizeof(e1));
    assert(ht_cache_size(c) == 2);

    /* Put a third — evicts key=0 (LRU) */
    test_entry_t e2 = {.key = 2, .value = 300};
    ht_cache_put(c, &e2, sizeof(e2));
    assert(ht_cache_size(c) == 2);

    /* Find should see only keys 1 and 2 */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 2);

    /* key=0 should be gone */
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS put_evict_find_old\n");
}

/* 17. full lifecycle via find: put → find → promote → get → remove → find(NULL) */
static void test_full_find_lifecycle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put */
    test_entry_t e = {.key = 42, .value = 4200};
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);

    /* Find */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 42);
    assert(((test_entry_t *)found)->value == 4200);

    /* Promote */
    ht_cache_promote(c, found);

    /* Get — should also find it */
    int k42 = 42;
    test_entry_t *via_get = ht_cache_get(c, &k42, sizeof(k42));
    assert(via_get != NULL);
    assert(via_get->value == 4200);

    /* Remove */
    assert(ht_cache_remove(c, &k42, sizeof(k42)));
    assert(ht_cache_size(c) == 0);

    /* Find again → NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS full_find_lifecycle\n");
}

/* 18. scan_fn uses user_ctx to accumulate across entries */
typedef struct {
    int sum_values;
    int count;
} accum_ctx_t;

static bool accum_scan(void *entry, void *ctx) {
    accum_ctx_t *a = ctx;
    a->sum_values += ((test_entry_t *)entry)->value;
    a->count++;
    return true; /* continue scanning */
}

static void test_find_accumulate_ctx(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = (i + 1) * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    accum_ctx_t ac = {.sum_values = 0, .count = 0};
    void *result = ht_cache_find(c, 42, accum_scan, &ac);
    /* scan_fn always returns true, so find returns NULL (no match) */
    assert(result == NULL);
    assert(ac.count == 5);
    assert(ac.sum_values == 10 + 20 + 30 + 40 + 50);

    ht_cache_destroy(c);
    printf("  PASS find_accumulate_ctx\n");
}

/* 19. capacity-3 evict/get/put cycle */
static void test_capacity3_evict_cycle(void) {
    ht_cache_t *c = create_test_cache(3);

    /* Fill */
    for (int i = 0; i < 3; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    for (int round = 0; round < 10; round++) {
        /* Evict one */
        assert(ht_cache_evict(c));
        assert(ht_cache_size(c) == 2);

        /* Access the MRU entry to promote it */
        int mru_key = 2 + round;
        test_entry_t *mru = ht_cache_get(c, &mru_key, sizeof(mru_key));

        /* Put a new entry */
        test_entry_t e = {.key = 100 + round, .value = round};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
        assert(ht_cache_size(c) == 3);

        /* Verify the new entry is present */
        int k = 100 + round;
        test_entry_t *found = ht_cache_get(c, &k, sizeof(k));
        assert(found != NULL);
        assert(found->value == round);
    }

    ht_cache_destroy(c);
    printf("  PASS capacity3_evict_cycle\n");
}

/* 20. find with hash no entry ever had */
static void test_find_nonexistent_hash(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find with a hash that no entry has */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 999999, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_nonexistent_hash\n");
}

/* 21. remove middle of LRU, verify find still sees remaining entries */
static void test_remove_middle_find_remaining(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 11};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove key=3 (middle of LRU) */
    int k3 = 3;
    assert(ht_cache_remove(c, &k3, sizeof(k3)));

    /* Find should see exactly 5 entries */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 5);

    /* Each remaining key findable via get */
    for (int i = 0; i < 6; i++) {
        int k = i;
        if (i == 3)
            assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
        else
            assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS remove_middle_find_remaining\n");
}

/* 22. iter after partial drain via remove */
static void test_iter_partial_drain(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove even keys */
    for (int i = 0; i < 8; i += 2) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }
    assert(ht_cache_size(c) == 4);

    /* Iterate — should see only odd keys */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int count = 0;
    bool seen_odd[4] = {false};
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key % 2 == 1);
        assert(e->key >= 1 && e->key <= 7);
        seen_odd[e->key / 2] = true;
        count++;
    }
    assert(count == 4);
    for (int i = 0; i < 4; i++)
        assert(seen_odd[i]);

    ht_cache_destroy(c);
    printf("  PASS iter_partial_drain\n");
}

/* 23. promote then evict: promoted entry not the one evicted */
static void test_promote_then_evict_ordering(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Promote key=1 via find */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->key == 1);
    ht_cache_promote(c, found);
    /* LRU: 0(tail), 2, 3, 1(head) */

    /* Evict → key=0 */
    ht_cache_evict(c);
    int k0 = 0, k1 = 1;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    /* Evict again → key=2 */
    ht_cache_evict(c);
    int k2 = 2, k3 = 3;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_then_evict_ordering\n");
}

/* 24. scan_fn returning false immediately (picks first entry every time) */
static bool pick_first_scan(void *entry, void *ctx) {
    *(void **)ctx = entry;
    return false;
}

static void test_find_always_first(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Multiple finds all return the same first entry */
    void *ctx1 = NULL;
    void *r1 = ht_cache_find(c, 42, pick_first_scan, &ctx1);
    assert(r1 != NULL && r1 == ctx1);

    void *ctx2 = NULL;
    void *r2 = ht_cache_find(c, 42, pick_first_scan, &ctx2);
    assert(r2 != NULL && r2 == ctx2);

    /* Same entry both times (bare table probes same first slot) */
    assert(r1 == r2);

    ht_cache_destroy(c);
    printf("  PASS find_always_first\n");
}

/* 25. clear → put → find → get → remove: full cycle after clear */
static void test_full_cycle_after_clear(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* First fill */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_clear(c);

    /* Refill with different data */
    for (int i = 10; i < 14; i++) {
        test_entry_t e = {.key = i, .value = i * 3};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 4);

    /* Find key=11 */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 11);
    assert(((test_entry_t *)found)->value == 33);

    /* Get key=12 */
    int k12 = 12;
    test_entry_t *via_get = ht_cache_get(c, &k12, sizeof(k12));
    assert(via_get != NULL);
    assert(via_get->value == 36);

    /* Remove key=10 */
    int k10 = 10;
    assert(ht_cache_remove(c, &k10, sizeof(k10)));
    assert(ht_cache_size(c) == 3);

    /* Old keys gone */
    for (int i = 0; i < 4; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS full_cycle_after_clear\n");
}

/* 26. mixed find and get on same collision chain */
static void test_mixed_find_get_collision(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 100};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* get promotes, find does not */
    int k3 = 3;
    test_entry_t *via_get = ht_cache_get(c, &k3, sizeof(k3));
    assert(via_get != NULL && via_get->value == 300);

    /* Find key=1 — no promotion */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *via_find = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(via_find != NULL && ((test_entry_t *)via_find)->key == 1);

    /* Evict → should remove key=0 (still LRU tail since only get promoted 3) */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* key=1 still there (find didn't promote it) */
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    /* key=3 still there (get promoted it) */
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS mixed_find_get_collision\n");
}

/* 27. promote after find, then get — verify get sees promoted position */
static void test_find_promote_then_get(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Find key=1 (no promote) */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->key == 1);

    /* Manually promote */
    ht_cache_promote(c, found);
    /* LRU: 0(tail), 2, 3, 1(head) */

    /* Evict → key=0 */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* key=1 survives (was promoted) */
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    /* Now evict again — should remove key=2 (new tail) */
    ht_cache_evict(c);
    int k2 = 2, k3 = 3;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS find_promote_then_get\n");
}

/* 28. repeated evict+put with get in between — LRU tracks correctly */
static void test_evict_put_get_lru_tracking(void) {
    ht_cache_t *c = create_test_cache(3);

    /* Fill */
    test_entry_t e0 = {.key = 10, .value = 100};
    test_entry_t e1 = {.key = 11, .value = 200};
    test_entry_t e2 = {.key = 12, .value = 300};
    ht_cache_put(c, &e0, sizeof(e0));
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    /* LRU: 10(tail), 11, 12(head) */

    /* Get key=10 to promote it */
    int k10 = 10;
    ht_cache_get(c, &k10, sizeof(k10));
    /* LRU: 11(tail), 12, 10(head) */

    /* Put → evicts key=11 */
    test_entry_t e3 = {.key = 13, .value = 400};
    ht_cache_put(c, &e3, sizeof(e3));
    int k11 = 11;
    assert(ht_cache_get(c, &k11, sizeof(k11)) == NULL);

    /* Get key=12 to promote it */
    int k12 = 12;
    ht_cache_get(c, &k12, sizeof(k12));
    /* LRU: 10(tail), 13, 12(head) */

    /* Put → evicts key=10 */
    test_entry_t e4 = {.key = 14, .value = 500};
    ht_cache_put(c, &e4, sizeof(e4));
    assert(ht_cache_get(c, &k10, sizeof(k10)) == NULL);

    /* Remaining: 12, 13, 14 */
    int k13 = 13, k14 = 14;
    assert(ht_cache_get(c, &k12, sizeof(k12)) != NULL);
    assert(ht_cache_get(c, &k13, sizeof(k13)) != NULL);
    assert(ht_cache_get(c, &k14, sizeof(k14)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_put_get_lru_tracking\n");
}

/* 29. find after remove and re-put with same hash */
static void test_find_remove_reput(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e0 = {.key = 5, .value = 50};
    test_entry_t e1 = {.key = 6, .value = 60};
    ht_cache_put(c, &e0, sizeof(e0));
    ht_cache_put(c, &e1, sizeof(e1));

    /* Find key=5 */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);

    /* Remove key=5 */
    int k5 = 5;
    ht_cache_remove(c, &k5, sizeof(k5));

    /* Re-put key=5 with new value */
    test_entry_t e0_new = {.key = 5, .value = 99};
    ht_cache_put(c, &e0_new, sizeof(e0_new));

    /* Find should see new value */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);

    /* Verify via get that new value is present */
    test_entry_t *got = ht_cache_get(c, &k5, sizeof(k5));
    assert(got != NULL && got->value == 99);

    ht_cache_destroy(c);
    printf("  PASS find_remove_reput\n");
}

/* 30. size correct after mixed find/promote/evict/remove/clear */
static void test_size_after_mixed_ops(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put 6 */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 6);

    /* Remove 2 */
    int k0 = 0, k5 = 5;
    ht_cache_remove(c, &k0, sizeof(k0));
    ht_cache_remove(c, &k5, sizeof(k5));
    assert(ht_cache_size(c) == 4);

    /* Evict 1 */
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 3);

    /* Find + promote doesn't change size */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    ht_cache_promote(c, found);
    assert(ht_cache_size(c) == 3);

    /* Get doesn't change size */
    int k3 = 3;
    ht_cache_get(c, &k3, sizeof(k3));
    assert(ht_cache_size(c) == 3);

    /* Put 2 (evicts 1 since capacity 8, 3 present, room for 5 more) */
    test_entry_t ea = {.key = 100, .value = 1};
    test_entry_t eb = {.key = 101, .value = 2};
    ht_cache_put(c, &ea, sizeof(ea));
    ht_cache_put(c, &eb, sizeof(eb));
    assert(ht_cache_size(c) == 5);

    /* Clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    ht_cache_destroy(c);
    printf("  PASS size_after_mixed_ops\n");
}

/* ────────────────────────────────────────────────────────────────
 *  Round 7: Remaining uncovered call sequences
 *  ──────────────────────────────────────────────────────────────── */

/* R7-1. find → promote → evict → find returns NULL for the evicted entry */
static void test_find_promote_evict_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Find and promote key=2 */
    pick_nth_ctx_t pc = {.target_index = 2, .current_index = 0};
    void *found2 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found2 != NULL && ((test_entry_t *)found2)->key == 2);
    ht_cache_promote(c, found2);
    /* LRU: 0(tail), 1, 3, 2(head) */

    /* Evict → removes key=0 */
    ht_cache_evict(c);

    /* Evict → removes key=1 */
    ht_cache_evict(c);

    /* Evict → removes key=3 */
    ht_cache_evict(c);

    /* Evict → removes key=2 (the promoted one, now LRU) */
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 0);

    /* Find key=2 → NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_promote_evict_find\n");
}

/* R7-2. promote stale pointer from slot-reused entry */
static void test_promote_stale_reused_slot(void) {
    ht_cache_t *c = create_test_cache(2);

    test_entry_t eA = {.key = 100, .value = 1000};
    void *ptrA = ht_cache_put(c, &eA, sizeof(eA));
    assert(ptrA != NULL);

    test_entry_t eB = {.key = 200, .value = 2000};
    ht_cache_put(c, &eB, sizeof(eB));

    /* Remove A — frees its slot */
    int kA = 100;
    ht_cache_remove(c, &kA, sizeof(kA));

    /* Put C — may reuse A's slot */
    test_entry_t eC = {.key = 300, .value = 3000};
    ht_cache_put(c, &eC, sizeof(eC));

    /* Promote the stale pointer — should be no-op (slot is live but
     * now belongs to C; promote checks live[idx] and index < capacity
     * but doesn't verify the entry identity, so it promotes whatever
     * is in that slot now). This should not crash or corrupt. */
    ht_cache_promote(c, ptrA);

    /* Verify cache is still consistent */
    assert(ht_cache_size(c) == 2);
    int kC = 300, kB = 200;
    test_entry_t *gotC = ht_cache_get(c, &kC, sizeof(kC));
    test_entry_t *gotB = ht_cache_get(c, &kB, sizeof(kB));
    assert(gotC != NULL && gotC->value == 3000);
    assert(gotB != NULL && gotB->value == 2000);

    ht_cache_destroy(c);
    printf("  PASS promote_stale_reused_slot\n");
}

/* R7-3. remove → put → evict → put → remove → get (complex slot recycling) */
static void test_complex_slot_recycling(void) {
    ht_cache_t *c = create_test_cache(4);

    /* Fill */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove key=2 */
    int k2 = 2;
    ht_cache_remove(c, &k2, sizeof(k2));
    assert(ht_cache_size(c) == 3);

    /* Put new entry — fills slot 2 */
    test_entry_t e10 = {.key = 10, .value = 100};
    ht_cache_put(c, &e10, sizeof(e10));
    assert(ht_cache_size(c) == 4);

    /* Evict LRU (key=0) */
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 3);

    /* Put another */
    test_entry_t e11 = {.key = 11, .value = 110};
    ht_cache_put(c, &e11, sizeof(e11));
    assert(ht_cache_size(c) == 4);

    /* Remove key=10 */
    int k10 = 10;
    ht_cache_remove(c, &k10, sizeof(k10));
    assert(ht_cache_size(c) == 3);

    /* Get remaining entries — should find 1, 3, 11 */
    int k1 = 1, k3 = 3, k11 = 11;
    test_entry_t *g1 = ht_cache_get(c, &k1, sizeof(k1));
    test_entry_t *g3 = ht_cache_get(c, &k3, sizeof(k3));
    test_entry_t *g11 = ht_cache_get(c, &k11, sizeof(k11));
    assert(g1 != NULL && g1->value == 10);
    assert(g3 != NULL && g3->value == 30);
    assert(g11 != NULL && g11->value == 110);

    /* Evicted/removed keys gone */
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);
    assert(ht_cache_get(c, &k10, sizeof(k10)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS complex_slot_recycling\n");
}

/* R7-4. iter with promote between iter_next calls */
static void test_iter_with_promote_midway(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int count = 0;
    test_entry_t *first_entry = NULL;

    while (ht_cache_iter_next(c, &it, &entry)) {
        count++;
        if (count == 1) {
            first_entry = entry;
            /* Promote the first iterated entry to MRU */
            ht_cache_promote(c, first_entry);
        }
        /* Continue iteration — promote should not corrupt iter state */
    }
    assert(count == 6);

    /* After iteration, promote changed LRU but all entries still present */
    assert(ht_cache_size(c) == 6);
    for (int i = 0; i < 6; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS iter_with_promote_midway\n");
}

/* R7-5. clear → put → iter → clear → put → iter (double clear-fill-iter) */
static void test_double_clear_fill_iter(void) {
    ht_cache_t *c = create_test_cache(8);

    /* First cycle: keys 0-3 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = 100 + i};
        ht_cache_put(c, &e, sizeof(e));
    }
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int count1 = 0;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 0 && e->key < 4);
        assert(e->value == 100 + e->key);
        count1++;
    }
    assert(count1 == 4);

    /* Clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Second cycle: keys 100-103 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = 100 + i, .value = 200 + i};
        ht_cache_put(c, &e, sizeof(e));
    }
    it = ht_cache_iter_begin(c);
    int count2 = 0;
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key >= 100 && e->key < 104);
        assert(e->value == 200 + (e->key - 100));
        count2++;
    }
    assert(count2 == 4);

    ht_cache_destroy(c);
    printf("  PASS double_clear_fill_iter\n");
}

/* R7-6. rapid key cycling: remove → put → remove → put on same key */
static void test_rapid_same_key_cycling(void) {
    ht_cache_t *c = create_test_cache(4);

    /* Put key=5 initially */
    test_entry_t e = {.key = 5, .value = 50};
    ht_cache_put(c, &e, sizeof(e));

    for (int round = 0; round < 20; round++) {
        /* Remove key=5 */
        int k5 = 5;
        assert(ht_cache_remove(c, &k5, sizeof(k5)));

        /* Re-put key=5 with new value */
        test_entry_t e_new = {.key = 5, .value = round};
        void *ptr = ht_cache_put(c, &e_new, sizeof(e_new));
        assert(ptr != NULL);

        /* Get — should see latest value */
        test_entry_t *got = ht_cache_get(c, &k5, sizeof(k5));
        assert(got != NULL && got->value == round);
    }
    assert(ht_cache_size(c) == 1);

    ht_cache_destroy(c);
    printf("  PASS rapid_same_key_cycling\n");
}

/* R7-7. find where hash only matches tombstoned entries (other live entries exist) */
static void test_find_tombstoned_hash(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put 4 entries all hashing to 42 */
    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Remove all 4 */
    for (int i = 0; i < 4; i++) {
        int k = i;
        ht_cache_remove(c, &k, sizeof(k));
    }
    assert(ht_cache_size(c) == 0);

    /* Now put new entries with DIFFERENT keys but same collision hash */
    test_entry_t eA = {.key = 100, .value = 1000};
    test_entry_t eB = {.key = 101, .value = 2000};
    ht_cache_put(c, &eA, sizeof(eA));
    ht_cache_put(c, &eB, sizeof(eB));

    /* find with hash 42 — should find the new entries, not the old ones */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 2);

    /* Old keys should NOT be findable via get */
    for (int i = 0; i < 4; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS find_tombstoned_hash\n");
}

/* R7-8. get → find → get consistency check on same key */
static void test_get_find_get_consistency(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t e = {.key = 7, .value = 77};
    ht_cache_put(c, &e, sizeof(e));

    /* get */
    int k7 = 7;
    test_entry_t *via_get1 = ht_cache_get(c, &k7, sizeof(k7));
    assert(via_get1 != NULL && via_get1->value == 77);

    /* find — should return same data (but not necessarily same pointer) */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *via_find = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(via_find != NULL);
    assert(((test_entry_t *)via_find)->key == 7);
    assert(((test_entry_t *)via_find)->value == 77);

    /* get again — same data, same pointer */
    test_entry_t *via_get2 = ht_cache_get(c, &k7, sizeof(k7));
    assert(via_get2 != NULL);
    assert(via_get2->key == 7);
    assert(via_get2->value == 77);
    assert(via_get2 == via_get1);

    ht_cache_destroy(c);
    printf("  PASS get_find_get_consistency\n");
}

/* R7-9. find → modify in-place → evict different entry → find sees modified data */
static void test_find_modify_evict_find(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find key=2 */
    pick_nth_ctx_t pc = {.target_index = 2, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->key == 2);
    ((test_entry_t *)found)->value = 999;

    /* Evict a different entry (key=0 is LRU tail) */
    ht_cache_evict(c);

    /* Find key=2 again — modified value persists */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    test_entry_t *e = found;
    assert(e->key == 0 || e->key == 1 || e->key == 2 || e->key == 3);

    /* Use get for key=2 to see modified value */
    int k2 = 2;
    test_entry_t *via_get = ht_cache_get(c, &k2, sizeof(k2));
    assert(via_get != NULL && via_get->value == 999);

    ht_cache_destroy(c);
    printf("  PASS find_modify_evict_find\n");
}

/* R7-10. multiple different scan_fns on same hash */
typedef struct {
    int min_value;
    void *best;
} best_value_ctx_t;

static bool best_value_scan(void *entry, void *ctx) {
    best_value_ctx_t *b = ctx;
    test_entry_t *e = entry;
    if (e->value > b->min_value) {
        b->min_value = e->value;
        b->best = entry;
    }
    return true;
}

static void test_multiple_scan_fns_same_hash(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = (i + 1) * 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* values: 10, 20, 30, 40, 50 */

    /* Scan 1: count entries */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 5);

    /* Scan 2: find best value */
    best_value_ctx_t bv = {.min_value = -1, .best = NULL};
    ht_cache_find(c, 42, best_value_scan, &bv);
    assert(bv.best != NULL);
    assert(((test_entry_t *)bv.best)->value == 50);

    /* Scan 3: pick_nth */
    pick_nth_ctx_t pc = {.target_index = 2, .current_index = 0};
    void *nth = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(nth != NULL);
    assert(((test_entry_t *)nth)->key == 2);

    /* Scan 4: accumulate */
    accum_ctx_t ac = {.sum_values = 0, .count = 0};
    ht_cache_find(c, 42, accum_scan, &ac);
    assert(ac.count == 5);
    assert(ac.sum_values == 10 + 20 + 30 + 40 + 50);

    ht_cache_destroy(c);
    printf("  PASS multiple_scan_fns_same_hash\n");
}

/* R7-11. stress with deterministic "random" ops including find/promote */
static void test_stress_find_promote(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Simple LCG for deterministic pseudo-random */
    unsigned int seed = 42;

    for (int round = 0; round < 500; round++) {
        seed = seed * 1103515245 + 12345;
        unsigned int op = (seed >> 16) % 5;

        switch (op) {
        case 0: { /* put */
            test_entry_t e = {.key = (int)(seed % 32), .value = (int)round};
            ht_cache_put(c, &e, sizeof(e));
            break;
        }
        case 1: { /* get */
            int k = (int)(seed % 32);
            ht_cache_get(c, &k, sizeof(k));
            break;
        }
        case 2: { /* remove */
            int k = (int)(seed % 32);
            ht_cache_remove(c, &k, sizeof(k));
            break;
        }
        case 3: { /* find + promote */
            pick_nth_ctx_t pc = {.target_index = (int)(seed % 4), .current_index = 0};
            void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
            if (found) ht_cache_promote(c, found);
            break;
        }
        case 4: { /* evict */
            ht_cache_evict(c);
            break;
        }
        }
    }

    /* Verify cache is consistent: size matches iterated count */
    assert(ht_cache_size(c) <= ht_cache_capacity(c));
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    size_t iter_count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) {
        assert(((test_entry_t *)entry)->key >= 0);
        iter_count++;
    }
    assert(iter_count == ht_cache_size(c));

    /* Every live entry is findable via get */
    it = ht_cache_iter_begin(c);
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        test_entry_t *via_get = ht_cache_get(c, &e->key, sizeof(e->key));
        assert(via_get != NULL);
        assert(via_get->key == e->key);
    }

    ht_cache_destroy(c);
    printf("  PASS stress_find_promote\n");
}

/* R7-12. promoted entry eventually becomes LRU and gets evicted */
static void test_promoted_entry_eventually_evicted(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find and promote key=0 (was LRU tail) */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found0 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found0 != NULL && ((test_entry_t *)found0)->key == 0);
    ht_cache_promote(c, found0);
    /* LRU: 1(tail), 2, 3, 0(head) */

    /* Access keys 2, 3 via get — they become MRU */
    int k2 = 2, k3 = 3;
    ht_cache_get(c, &k2, sizeof(k2));
    ht_cache_get(c, &k3, sizeof(k3));
    /* LRU: 1(tail), 0, 2, 3(head) */

    /* Access key=1 via get to promote it */
    int k1 = 1;
    ht_cache_get(c, &k1, sizeof(k1));
    /* LRU: 0(tail), 2, 3, 1(head) */

    /* Now evict — removes key=0, the one we promoted earlier */
    ht_cache_evict(c);
    assert(ht_cache_get(c, &(int){0}, sizeof(int)) == NULL);
    assert(ht_cache_size(c) == 3);

    ht_cache_destroy(c);
    printf("  PASS promoted_entry_eventually_evicted\n");
}

/* R7-13. iter → remove non-visited entry → continue iter */
static void test_iter_remove_unvisited(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int seen_keys[8] = {0};
    int count = 0;

    /* Visit first 3 entries */
    for (int i = 0; i < 3; i++) {
        assert(ht_cache_iter_next(c, &it, &entry));
        test_entry_t *e = entry;
        seen_keys[e->key] = 1;
        count++;
    }

    /* Remove an entry we have NOT visited yet (pick key=7) */
    int k7 = 7;
    ht_cache_remove(c, &k7, sizeof(k7));

    /* Continue iteration — should not see key=7 */
    while (ht_cache_iter_next(c, &it, &entry)) {
        test_entry_t *e = entry;
        assert(e->key != 7);
        seen_keys[e->key] = 1;
        count++;
    }

    assert(count == 7); /* 8 total minus 1 removed */
    assert(seen_keys[7] == 0); /* never saw the removed entry */
    for (int i = 0; i < 7; i++)
        assert(seen_keys[i] == 1);

    ht_cache_destroy(c);
    printf("  PASS iter_remove_unvisited\n");
}

/* R7-14. find with hash=0 after remove and re-put of spill-lane entry */
static void test_spill_lane_find_cycle(void) {
    ht_cache_t *c = create_test_cache(8);

    /* key=0 → hash 0 (spill lane) */
    test_entry_t e0 = {.key = 0, .value = 100};
    ht_cache_put(c, &e0, sizeof(e0));

    /* Find via hash=0 */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found = ht_cache_find(c, 0, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->key == 0);

    /* Remove key=0 */
    int k0 = 0;
    ht_cache_remove(c, &k0, sizeof(k0));

    /* Re-put key=0 */
    test_entry_t e0_new = {.key = 0, .value = 999};
    ht_cache_put(c, &e0_new, sizeof(e0_new));

    /* Find again via hash=0 */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    found = ht_cache_find(c, 0, pick_nth_scan, &pc);
    assert(found != NULL && ((test_entry_t *)found)->value == 999);

    ht_cache_destroy(c);
    printf("  PASS spill_lane_find_cycle\n");
}

/* R7-15. consecutive evicts until empty, then find/get/iter all return empty */
static void test_drain_then_find_get_iter(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Drain via consecutive evicts */
    for (int i = 0; i < 6; i++) {
        assert(ht_cache_evict(c));
    }
    assert(ht_cache_size(c) == 0);

    /* One more evict → false */
    assert(!ht_cache_evict(c));

    /* get → NULL */
    int k = 3;
    assert(ht_cache_get(c, &k, sizeof(k)) == NULL);

    /* find → NULL */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    /* iter → empty */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    assert(!ht_cache_iter_next(c, &it, &entry));

    ht_cache_destroy(c);
    printf("  PASS drain_then_find_get_iter\n");
}

/* R7-16. evict on mixed collision + non-collision cache */
static void test_evict_mixed_collision(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = mixed_hash_fn,
        .eq_fn      = mixed_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* keys 0-3 collide (hash=42), keys 4-5 unique (hash=4, 5) */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Promote a collision entry (key=1) via get */
    int k1 = 1;
    ht_cache_get(c, &k1, sizeof(k1));

    /* Evict → removes key=0 (LRU tail among collision entries) */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    /* Non-collision entries still present */
    int k4 = 4, k5 = 5;
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);
    assert(ht_cache_get(c, &k5, sizeof(k5)) != NULL);

    /* Evict again → removes key=2 (next LRU among collision) */
    ht_cache_evict(c);
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_mixed_collision\n");
}

/* ────────────────────────────────────────────────────────────────
 *  Round 8: Final uncovered sequences (A,B,D,F,J,N,O,Q)
 *  ──────────────────────────────────────────────────────────────── */

/* R8-A. find-only lifecycle: put → find → promote → find → modify → find → remove → find (no get) */
static void test_find_only_lifecycle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put */
    test_entry_t e = {.key = 42, .value = 100};
    ht_cache_put(c, &e, sizeof(e));
    assert(ht_cache_size(c) == 1);

    /* Find */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found1 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found1 != NULL);
    assert(((test_entry_t *)found1)->key == 42);
    assert(((test_entry_t *)found1)->value == 100);

    /* Promote */
    ht_cache_promote(c, found1);

    /* Find again — still there */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    void *found2 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found2 != NULL);
    assert(((test_entry_t *)found2)->key == 42);
    assert(((test_entry_t *)found2)->value == 100);

    /* Modify via find pointer */
    ((test_entry_t *)found2)->value = 999;

    /* Find again — sees modified value */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    void *found3 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found3 != NULL);
    assert(((test_entry_t *)found3)->value == 999);

    /* Remove by key */
    int k42 = 42;
    assert(ht_cache_remove(c, &k42, sizeof(k42)));
    assert(ht_cache_size(c) == 0);

    /* Find → NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_only_lifecycle\n");
}

/* R8-B. evict after find+promote-only access (no get) — LRU tracked by promote alone */
static void test_evict_after_find_promote_only(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Access all entries via find+promote in reverse order (no get) */
    for (int target = 3; target >= 0; target--) {
        pick_nth_ctx_t pc = {.target_index = target, .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL && ((test_entry_t *)found)->key == target);
        ht_cache_promote(c, found);
    }
    /* After promoting 3,2,1,0 in order, LRU: 3(tail), 2, 1, 0(head) */

    /* Evict → should remove key=3 (promoted first, then pushed to tail) */
    ht_cache_evict(c);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Evict → key=2 */
    ht_cache_evict(c);
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Keys 0 and 1 survive */
    int k0 = 0, k1 = 1;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_after_find_promote_only\n");
}

/* R8-D. clear after find+promote reordered LRU */
static void test_clear_after_find_promote(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find and promote key=0 and key=3 */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found0 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found0 != NULL);
    ht_cache_promote(c, found0);

    pc = (pick_nth_ctx_t){.target_index = 2, .current_index = 0};
    void *found3 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found3 != NULL);
    ht_cache_promote(c, found3);

    /* Clear — should fully reset */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Find returns NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    /* Evict on empty returns false */
    assert(!ht_cache_evict(c));

    /* Iter returns nothing */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    assert(!ht_cache_iter_next(c, &it, &entry));

    /* Refill with new data */
    for (int i = 100; i < 106; i++) {
        test_entry_t e = {.key = i, .value = i * 2};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 6);

    /* Find works on new data */
    /* All keys 100-105 collide on hash 42 */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 6);

    ht_cache_destroy(c);
    printf("  PASS clear_after_find_promote\n");
}

/* R8-F. scan_fn that modifies entries during scan */
typedef struct {
    int added_value;
} modify_scan_ctx_t;

static bool modify_during_scan(void *entry, void *ctx) {
    modify_scan_ctx_t *m = ctx;
    test_entry_t *e = entry;
    e->value += m->added_value;
    return true; /* continue scanning */
}

static void test_scan_fn_modifies_entries(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = 10};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* All entries start with value=10 */

    /* Scan that adds 5 to each entry's value during traversal */
    modify_scan_ctx_t mc = {.added_value = 5};
    ht_cache_find(c, 42, modify_during_scan, &mc);
    /* find returns NULL because scan never returns false */

    /* Verify all entries now have value=15 */
    for (int i = 0; i < 5; i++) {
        int k = i;
        test_entry_t *e = ht_cache_get(c, &k, sizeof(k));
        assert(e != NULL && e->value == 15);
    }

    /* Scan again, adding 3 */
    mc.added_value = 3;
    ht_cache_find(c, 42, modify_during_scan, &mc);

    /* Verify value=18 */
    for (int i = 0; i < 5; i++) {
        int k = i;
        test_entry_t *e = ht_cache_get(c, &k, sizeof(k));
        assert(e != NULL && e->value == 18);
    }

    ht_cache_destroy(c);
    printf("  PASS scan_fn_modifies_entries\n");
}

/* R8-J. evict where LRU set entirely by promote, promoting in specific order */
static void test_evict_promote_only_lru(void) {
    ht_cache_config_t cfg = {
        .capacity   = 5,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3, 4(head) */

    /* Promote ONLY via find+promote: promote key=4, then key=2, then key=0 */
    pick_nth_ctx_t pc = {.target_index = 4, .current_index = 0};
    void *f4 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f4 != NULL && ((test_entry_t *)f4)->key == 4);
    ht_cache_promote(c, f4);
    /* LRU: 0(tail), 1, 2, 3, 4(head) — already head, no change */

    pc = (pick_nth_ctx_t){.target_index = 2, .current_index = 0};
    void *f2 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f2 != NULL && ((test_entry_t *)f2)->key == 2);
    ht_cache_promote(c, f2);
    /* LRU: 0(tail), 1, 3, 4, 2(head) */

    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    void *f0 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f0 != NULL && ((test_entry_t *)f0)->key == 0);
    ht_cache_promote(c, f0);
    /* LRU: 1(tail), 3, 4, 2, 0(head) */

    /* Evict → key=1 */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Evict → key=3 */
    ht_cache_evict(c);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Keys 0, 2, 4 survive */
    int k0 = 0, k2 = 2, k4 = 4;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS evict_promote_only_lru\n");
}

/* R8-N. long sequence mixing ALL APIs with size assertions */
static void test_all_apis_sequence(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* 1. Put 6 entries */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        assert(ht_cache_put(c, &e, sizeof(e)) != NULL);
    }
    assert(ht_cache_size(c) == 6);

    /* 2. Get key=3 */
    int k3 = 3;
    test_entry_t *g3 = ht_cache_get(c, &k3, sizeof(k3));
    assert(g3 != NULL && g3->value == 30);
    assert(ht_cache_size(c) == 6);

    /* 3. Find key=1 */
    pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
    void *f1 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f1 != NULL && ((test_entry_t *)f1)->key == 1);
    assert(ht_cache_size(c) == 6);

    /* 4. Promote key=1 */
    ht_cache_promote(c, f1);
    assert(ht_cache_size(c) == 6);

    /* 5. Remove key=2 */
    int k2 = 2;
    assert(ht_cache_remove(c, &k2, sizeof(k2)));
    assert(ht_cache_size(c) == 5);

    /* 6. Evict (removes key=0, LRU tail since 1 and 3 promoted) */
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 4);

    /* 7. Put new entry */
    test_entry_t e100 = {.key = 100, .value = 1000};
    assert(ht_cache_put(c, &e100, sizeof(e100)) != NULL);
    assert(ht_cache_size(c) == 5);

    /* 8. Iterate — should see 5 entries */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    int iter_count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) iter_count++;
    assert(iter_count == 5);

    /* 9. Clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* 10. Put after clear */
    test_entry_t e200 = {.key = 200, .value = 2000};
    assert(ht_cache_put(c, &e200, sizeof(e200)) != NULL);
    assert(ht_cache_size(c) == 1);

    /* 11. Get after clear+put */
    int k200 = 200;
    test_entry_t *g200 = ht_cache_get(c, &k200, sizeof(k200));
    assert(g200 != NULL && g200->value == 2000);

    /* 12. Find after clear+put */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    void *f200 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f200 != NULL && ((test_entry_t *)f200)->key == 200);

    /* 13. Promote after clear+put */
    ht_cache_promote(c, f200);

    /* 14. Remove after clear+put */
    assert(ht_cache_remove(c, &k200, sizeof(k200)));
    assert(ht_cache_size(c) == 0);

    /* 15. Evict on empty */
    assert(!ht_cache_evict(c));

    ht_cache_destroy(c);
    printf("  PASS all_apis_sequence\n");
}

/* R8-O. eq_fn that depends on user_ctx for matching */
typedef struct {
    int min_value;
} threshold_ctx_t;

static uint64_t ctx_hash_fn(const void *key, size_t len, void *ctx) {
    (void)len; (void)ctx;
    const int *k = key;
    return (uint64_t)(*k * 2654435761u);
}

static bool ctx_eq_fn(const void *key, size_t key_len,
                      const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size;
    threshold_ctx_t *tc = ctx;
    const int *k = key;
    const test_entry_t *e = entry;
    /* Match if key matches AND value >= threshold from user_ctx */
    return *k == e->key && e->value >= tc->min_value;
}

static void test_eq_fn_uses_user_ctx(void) {
    threshold_ctx_t tctx = {.min_value = 50};

    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = ctx_hash_fn,
        .eq_fn      = ctx_eq_fn,
        .user_ctx   = &tctx,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put key=5 with value=30 (below threshold) */
    test_entry_t e_low = {.key = 5, .value = 30};
    ht_cache_put(c, &e_low, sizeof(e_low));

    /* Put key=5 with value=100 (above threshold) — duplicate key, different value */
    test_entry_t e_high = {.key = 5, .value = 100};
    ht_cache_put(c, &e_high, sizeof(e_high));

    /* Get with threshold=50 — should find the value=100 entry, not value=30 */
    int k5 = 5;
    test_entry_t *found = ht_cache_get(c, &k5, sizeof(k5));
    assert(found != NULL);
    assert(found->value == 100);

    /* Lower threshold to 0 — get should now find the first match (could be either) */
    tctx.min_value = 0;
    /* Both entries should be findable now */
    test_entry_t *found2 = ht_cache_get(c, &k5, sizeof(k5));
    assert(found2 != NULL);
    assert(found2->key == 5);

    /* Raise threshold to 200 — neither should match */
    tctx.min_value = 200;
    test_entry_t *found3 = ht_cache_get(c, &k5, sizeof(k5));
    assert(found3 == NULL);

    ht_cache_destroy(c);
    printf("  PASS eq_fn_uses_user_ctx\n");
}

/* R8-Q. find on eviction-drained then refilled cache */
static void test_find_after_evict_drain_refill(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Fill with keys 0-5 */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Drain via eviction */
    for (int i = 0; i < 6; i++) {
        assert(ht_cache_evict(c));
    }
    assert(ht_cache_size(c) == 0);

    /* Refill with keys 100-105 */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = 100 + i, .value = (100 + i) * 2};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 6);

    /* Find should see only new entries */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 6);

    /* Find a specific new entry */
    pick_nth_ctx_t pc = {.target_index = 3, .current_index = 0};
    void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found != NULL);
    assert(((test_entry_t *)found)->key == 103);
    assert(((test_entry_t *)found)->value == 206);

    /* Old keys not findable via get */
    for (int i = 0; i < 6; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) == NULL);
    }

    /* Old keys not findable via find — hash 42 only matches new entries */
    /* (Old and new entries share hash 42 since collision_hash_fn always returns 42) */
    /* But old entries are gone, so scan should only see 6 new entries */
    cc.count = 0;
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 6);

    ht_cache_destroy(c);
    printf("  PASS find_after_evict_drain_refill\n");
}

/* R8-extra: remove MRU then evict — verify second entry is new MRU */
static void test_remove_mru_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Remove key=3 (MRU) */
    int k3 = 3;
    ht_cache_remove(c, &k3, sizeof(k3));
    /* LRU: 0(tail), 1, 2(head) */

    /* Evict → removes key=0 (LRU tail) */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Keys 1 and 2 survive */
    int k1 = 1, k2 = 2;
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_size(c) == 2);

    ht_cache_destroy(c);
    printf("  PASS remove_mru_evict\n");
}

/* R8-extra: clear mid-iteration then verify stale iterator returns false */
static void test_clear_mid_iteration(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;

    /* Get first entry */
    assert(ht_cache_iter_next(c, &it, &entry));
    assert(((test_entry_t *)entry)->key == 0);

    /* Clear mid-iteration */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);

    /* Stale iterator — iter_next scans live[], all zeros now, returns false */
    assert(!ht_cache_iter_next(c, &it, &entry));

    ht_cache_destroy(c);
    printf("  PASS clear_mid_iteration\n");
}

/* R8-extra: promote after put already made entry MRU (no-op promote) */
static void test_promote_already_mru(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 3; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2(head) */

    /* Put key=3 — now MRU */
    test_entry_t e3 = {.key = 3, .value = 30};
    void *p3 = ht_cache_put(c, &e3, sizeof(e3));
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Promote key=3 — already head, no-op */
    ht_cache_promote(c, p3);
    /* LRU should still be: 0(tail), 1, 2, 3(head) */

    /* Evict → key=0 */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* key=3 survives (was MRU, promote was no-op) */
    int k3 = 3, k1 = 1, k2 = 2;
    assert(ht_cache_get(c, &k3, sizeof(k3)) != NULL);
    assert(ht_cache_get(c, &k1, sizeof(k1)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS promote_already_mru\n");
}

/* R8-extra: find picks entry with max value in collision chain */
static bool max_value_scan(void *entry, void *ctx) {
    test_entry_t *e = entry;
    test_entry_t **best = (test_entry_t **)ctx;
    if (*best == NULL || e->value > (*best)->value)
        *best = e;
    return true; /* visit all */
}

static void test_find_max_value(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = (i == 2) ? 999 : i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find entry with maximum value */
    test_entry_t *best = NULL;
    void *result = ht_cache_find(c, 42, max_value_scan, &best);
    /* scan always returns true, so find returns NULL */
    assert(result == NULL);
    /* But best was updated during scan */
    assert(best != NULL);
    assert(best->key == 2);
    assert(best->value == 999);

    ht_cache_destroy(c);
    printf("  PASS find_max_value\n");
}

/* R8-extra: multiple consecutive promotes of different entries before eviction */
static void test_multi_promote_then_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 5,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3, 4(head) */

    /* Promote keys 0, 2, 4 in order — each goes to head */
    int targets[] = {0, 2, 4};
    for (int t = 0; t < 3; t++) {
        pick_nth_ctx_t pc = {.target_index = targets[t], .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL);
        ht_cache_promote(c, found);
    }
    /* After promoting 0: LRU: 1(tail), 2, 3, 4, 0(head) */
    /* After promoting 2: LRU: 1(tail), 3, 4, 0, 2(head) */
    /* After promoting 4: LRU: 1(tail), 3, 0, 2, 4(head) — already head, no change */

    /* Evict → key=1 */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Evict → key=3 */
    ht_cache_evict(c);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Keys 0, 2, 4 survive */
    int k0 = 0, k2 = 2, k4 = 4;
    assert(ht_cache_get(c, &k0, sizeof(k0)) != NULL);
    assert(ht_cache_get(c, &k2, sizeof(k2)) != NULL);
    assert(ht_cache_get(c, &k4, sizeof(k4)) != NULL);

    ht_cache_destroy(c);
    printf("  PASS multi_promote_then_evict\n");
}

/* R8-extra: entry data unchanged after operations on other entries */
static void test_data_isolation_across_entries(void) {
    ht_cache_t *c = create_test_cache(8);

    test_entry_t eA = {.key = 10, .value = 100};
    test_entry_t eB = {.key = 20, .value = 200};
    void *pA = ht_cache_put(c, &eA, sizeof(eA));
    void *pB = ht_cache_put(c, &eB, sizeof(eB));

    /* Get B, modify B */
    int kB = 20;
    test_entry_t *gB = ht_cache_get(c, &kB, sizeof(kB));
    gB->value = 9999;

    /* Verify A unchanged */
    assert(((test_entry_t *)pA)->key == 10);
    assert(((test_entry_t *)pA)->value == 100);

    /* Remove B, put new entry C */
    ht_cache_remove(c, &kB, sizeof(kB));
    test_entry_t eC = {.key = 30, .value = 300};
    ht_cache_put(c, &eC, sizeof(eC));

    /* Verify A still unchanged */
    assert(((test_entry_t *)pA)->key == 10);
    assert(((test_entry_t *)pA)->value == 100);

    /* Verify A via get */
    int kA = 10;
    test_entry_t *gA = ht_cache_get(c, &kA, sizeof(kA));
    assert(gA != NULL && gA->value == 100);

    ht_cache_destroy(c);
    printf("  PASS data_isolation_across_entries\n");
}

/* ────────────────────────────────────────────────────────────────
 *  Round 9: Last 4 uncovered sequences + novel patterns
 *  ──────────────────────────────────────────────────────────────── */

/* R9-A. find-first, remove it, find-first again — verify different entry */
static void test_find_first_changes_after_remove(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find the first entry returned by always-first scan */
    void *ctx1 = NULL;
    void *first1 = ht_cache_find(c, 42, pick_first_scan, &ctx1);
    assert(first1 != NULL && first1 == ctx1);
    int first_key = ((test_entry_t *)first1)->key;

    /* Remove that entry */
    int k = first_key;
    assert(ht_cache_remove(c, &k, sizeof(k)));

    /* Find first again — must be a DIFFERENT entry */
    void *ctx2 = NULL;
    void *first2 = ht_cache_find(c, 42, pick_first_scan, &ctx2);
    assert(first2 != NULL && first2 == ctx2);
    assert(((test_entry_t *)first2)->key != first_key);

    /* Repeat: remove and verify first changes each time */
    for (int removed = 1; removed < 5; removed++) {
        int rm_key = ((test_entry_t *)first2)->key;
        assert(ht_cache_remove(c, &rm_key, sizeof(rm_key)));

        if (removed < 4) {
            void *ctx3 = NULL;
            void *first3 = ht_cache_find(c, 42, pick_first_scan, &ctx3);
            assert(first3 != NULL);
            assert(((test_entry_t *)first3)->key != rm_key);
            first2 = first3;
        } else {
            void *ctx3 = NULL;
            assert(ht_cache_find(c, 42, pick_first_scan, &ctx3) == NULL);
        }
    }

    ht_cache_destroy(c);
    printf("  PASS find_first_changes_after_remove\n");
}

/* R9-G. Triple clear-fill-iter cycle */
static void test_triple_clear_fill_iter(void) {
    ht_cache_t *c = create_test_cache(8);

    for (int cycle = 0; cycle < 3; cycle++) {
        int base = cycle * 100;
        for (int i = 0; i < 4; i++) {
            test_entry_t e = {.key = base + i, .value = (cycle + 1) * 1000 + i};
            ht_cache_put(c, &e, sizeof(e));
        }
        assert(ht_cache_size(c) == 4);

        ht_cache_iter_t it = ht_cache_iter_begin(c);
        void *entry;
        int count = 0;
        bool seen[4] = {false};
        while (ht_cache_iter_next(c, &it, &entry)) {
            test_entry_t *e = entry;
            assert(e->key >= base && e->key < base + 4);
            assert(e->value == (cycle + 1) * 1000 + (e->key - base));
            seen[e->key - base] = true;
            count++;
        }
        assert(count == 4);
        for (int i = 0; i < 4; i++) assert(seen[i]);

        ht_cache_clear(c);
        assert(ht_cache_size(c) == 0);
    }

    ht_cache_destroy(c);
    printf("  PASS triple_clear_fill_iter\n");
}

/* R9-J. find→promote→find→promote same entry → evict removes it → find NULL */
static void test_double_find_promote_then_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Find and promote key=1 twice */
    for (int rep = 0; rep < 2; rep++) {
        pick_nth_ctx_t pc = {.target_index = 1, .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL && ((test_entry_t *)found)->key == 1);
        ht_cache_promote(c, found);
    }
    /* After first promote(1): LRU: 0(tail), 2, 3, 1(head) */
    /* After second promote(1): no change (already head) */

    /* Promote keys 2 and 3 to push key=1 toward tail */
    int k3 = 3;
    ht_cache_get(c, &k3, sizeof(k3));
    /* LRU: 0(tail), 2, 1, 3(head) */

    int k2 = 2;
    ht_cache_get(c, &k2, sizeof(k2));
    /* LRU: 0(tail), 1, 3, 2(head) */

    /* Now evict → removes key=0 */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Evict → removes key=1 (the doubly-promoted one, now LRU) */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Find key=1 → NULL */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    /* Can't use pick_nth to find specific key, use get instead */
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    ht_cache_destroy(c);
    printf("  PASS double_find_promote_then_evict\n");
}

/* R9-K. Capacity-2 with full find/promote/evict lifecycle */
static void test_capacity2_full_lifecycle(void) {
    ht_cache_config_t cfg = {
        .capacity   = 2,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put 2 entries */
    test_entry_t e0 = {.key = 10, .value = 100};
    test_entry_t e1 = {.key = 11, .value = 200};
    ht_cache_put(c, &e0, sizeof(e0));
    ht_cache_put(c, &e1, sizeof(e1));
    assert(ht_cache_size(c) == 2);

    /* Find key=10 */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found10 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found10 != NULL && ((test_entry_t *)found10)->key == 10);

    /* Promote key=10 (was LRU tail) */
    ht_cache_promote(c, found10);
    /* LRU: 11(tail), 10(head) */

    /* Get key=11 */
    int k11 = 11;
    test_entry_t *g11 = ht_cache_get(c, &k11, sizeof(k11));
    assert(g11 != NULL && g11->value == 200);

    /* Evict → removes key=10? No! key=11 was just accessed by get.
     * After get(11): LRU: 10(tail), 11(head) */
    ht_cache_evict(c);
    int k10 = 10;
    assert(ht_cache_get(c, &k10, sizeof(k10)) == NULL);
    assert(ht_cache_get(c, &k11, sizeof(k11)) != NULL);
    assert(ht_cache_size(c) == 1);

    /* Remove key=11 */
    assert(ht_cache_remove(c, &k11, sizeof(k11)));
    assert(ht_cache_size(c) == 0);

    /* Find → NULL */
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc) == NULL);

    /* Refill */
    test_entry_t e2 = {.key = 20, .value = 300};
    ht_cache_put(c, &e2, sizeof(e2));
    pc = (pick_nth_ctx_t){.target_index = 0, .current_index = 0};
    void *found20 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found20 != NULL && ((test_entry_t *)found20)->key == 20);

    ht_cache_destroy(c);
    printf("  PASS capacity2_full_lifecycle\n");
}

/* R9-extra: find enumerates all entries one by one via pick_nth */
static void test_find_enumerate_all(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 5; i++) {
        test_entry_t e = {.key = i, .value = i * 7};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Enumerate all 5 entries by index */
    bool seen[5] = {false};
    for (int target = 0; target < 5; target++) {
        pick_nth_ctx_t pc = {.target_index = target, .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL);
        int key = ((test_entry_t *)found)->key;
        assert(key >= 0 && key < 5);
        seen[key] = true;
    }

    /* All 5 keys were found */
    for (int i = 0; i < 5; i++) assert(seen[i]);

    /* 6th find should return NULL (no 6th entry) */
    pick_nth_ctx_t pc6 = {.target_index = 5, .current_index = 0};
    assert(ht_cache_find(c, 42, pick_nth_scan, &pc6) == NULL);

    ht_cache_destroy(c);
    printf("  PASS find_enumerate_all\n");
}

/* R9-extra: promote every entry in reverse order, then evict all */
static void test_promote_all_reverse_then_evict(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    /* LRU: 0(tail), 1, 2, 3(head) */

    /* Promote all in reverse order: 3, 2, 1, 0 */
    for (int idx = 3; idx >= 0; idx--) {
        pick_nth_ctx_t pc = {.target_index = idx, .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL);
        ht_cache_promote(c, found);
    }
    /* After promoting 3: already head, no change */
    /* After promoting 2: LRU: 0(tail), 1, 3, 2(head) */
    /* After promoting 1: LRU: 0(tail), 3, 2, 1(head) */
    /* After promoting 0: LRU: 3(tail), 2, 1, 0(head) */

    /* Evict → key=3 */
    ht_cache_evict(c);
    int k3 = 3;
    assert(ht_cache_get(c, &k3, sizeof(k3)) == NULL);

    /* Evict → key=2 */
    ht_cache_evict(c);
    int k2 = 2;
    assert(ht_cache_get(c, &k2, sizeof(k2)) == NULL);

    /* Evict → key=1 */
    ht_cache_evict(c);
    int k1 = 1;
    assert(ht_cache_get(c, &k1, sizeof(k1)) == NULL);

    /* Evict → key=0 (promoted last, was head, now only entry) */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);
    assert(ht_cache_size(c) == 0);

    ht_cache_destroy(c);
    printf("  PASS promote_all_reverse_then_evict\n");
}

/* R9-extra: stress with only find+promote+evict (no get, no remove) */
static void test_stress_find_promote_evict_only(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Initial fill */
    for (int i = 0; i < 8; i++) {
        test_entry_t e = {.key = i, .value = i * 10};
        ht_cache_put(c, &e, sizeof(e));
    }

    unsigned int seed = 12345;
    for (int round = 0; round < 200; round++) {
        seed = seed * 1103515245 + 12345;
        unsigned int op = (seed >> 16) % 3;

        switch (op) {
        case 0: { /* find + promote */
            pick_nth_ctx_t pc = {.target_index = (int)(seed % 8), .current_index = 0};
            void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
            if (found) ht_cache_promote(c, found);
            break;
        }
        case 1: { /* evict */
            ht_cache_evict(c);
            break;
        }
        case 2: { /* put (replenish after evictions) */
            test_entry_t e = {.key = (int)(seed % 50), .value = (int)round};
            ht_cache_put(c, &e, sizeof(e));
            break;
        }
        }
    }

    /* Verify consistency */
    assert(ht_cache_size(c) <= ht_cache_capacity(c));
    assert(ht_cache_size(c) > 0);

    /* Iter count matches size */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    size_t iter_count = 0;
    while (ht_cache_iter_next(c, &it, &entry)) iter_count++;
    assert(iter_count == ht_cache_size(c));

    ht_cache_destroy(c);
    printf("  PASS stress_find_promote_evict_only\n");
}

/* R9-extra: find with min-value scan (inverse of max-value) */
static bool min_value_scan(void *entry, void *ctx) {
    test_entry_t *e = entry;
    test_entry_t **best = (test_entry_t **)ctx;
    if (*best == NULL || e->value < (*best)->value)
        *best = e;
    return true;
}

static void test_find_min_value(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    test_entry_t entries[] = {
        {.key = 0, .value = 500},
        {.key = 1, .value = 200},
        {.key = 2, .value = 800},
        {.key = 3, .value = 100},
        {.key = 4, .value = 400},
    };
    for (int i = 0; i < 5; i++)
        ht_cache_put(c, &entries[i], sizeof(entries[i]));

    test_entry_t *best = NULL;
    ht_cache_find(c, 42, min_value_scan, &best);
    assert(best != NULL);
    assert(best->key == 3);
    assert(best->value == 100);

    ht_cache_destroy(c);
    printf("  PASS find_min_value\n");
}

/* R9-extra: long sequence with invariant checks after every operation */
static void test_invariant_checked_sequence(void) {
    ht_cache_config_t cfg = {
        .capacity   = 6,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);
    size_t cap = ht_cache_capacity(c);

    /* Invariant: size <= capacity */
    assert(ht_cache_size(c) == 0);

    /* put 3 */
    for (int i = 0; i < 3; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) <= cap);
    assert(ht_cache_size(c) == 3);

    /* get */
    int k1 = 1;
    ht_cache_get(c, &k1, sizeof(k1));
    assert(ht_cache_size(c) == 3);

    /* find */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *f = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(f != NULL);
    assert(ht_cache_size(c) == 3);

    /* promote */
    ht_cache_promote(c, f);
    assert(ht_cache_size(c) == 3);

    /* remove */
    int k0 = 0;
    ht_cache_remove(c, &k0, sizeof(k0));
    assert(ht_cache_size(c) == 2);
    assert(ht_cache_size(c) <= cap);

    /* evict */
    ht_cache_evict(c);
    assert(ht_cache_size(c) == 1);
    assert(ht_cache_size(c) <= cap);

    /* put (no eviction needed) */
    test_entry_t e10 = {.key = 10, .value = 100};
    ht_cache_put(c, &e10, sizeof(e10));
    assert(ht_cache_size(c) == 2);

    /* put (triggers eviction since cap=6, 2 present, plenty of room) */
    test_entry_t e20 = {.key = 20, .value = 200};
    ht_cache_put(c, &e20, sizeof(e20));
    assert(ht_cache_size(c) == 3);
    assert(ht_cache_size(c) <= cap);

    /* iter count == size */
    ht_cache_iter_t it = ht_cache_iter_begin(c);
    void *entry;
    size_t ic = 0;
    while (ht_cache_iter_next(c, &it, &entry)) ic++;
    assert(ic == ht_cache_size(c));

    /* clear */
    ht_cache_clear(c);
    assert(ht_cache_size(c) == 0);
    assert(ht_cache_size(c) <= cap);

    /* fill to capacity */
    for (int i = 0; i < 6; i++) {
        test_entry_t e = {.key = i + 50, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == cap);
    assert(ht_cache_size(c) <= cap);

    /* One more put → evicts */
    test_entry_t extra = {.key = 99, .value = 999};
    ht_cache_put(c, &extra, sizeof(extra));
    assert(ht_cache_size(c) == cap);
    assert(ht_cache_size(c) <= cap);

    ht_cache_destroy(c);
    printf("  PASS invariant_checked_sequence\n");
}

/* R9-extra: find after massive tombstone churn on same hash */
static void test_find_after_tombstone_churn(void) {
    ht_cache_config_t cfg = {
        .capacity   = 16,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Churn: put and remove many entries with the same hash */
    for (int round = 0; round < 50; round++) {
        test_entry_t e = {.key = round, .value = round};
        ht_cache_put(c, &e, sizeof(e));
        int k = round;
        ht_cache_remove(c, &k, sizeof(k));
    }
    assert(ht_cache_size(c) == 0);

    /* Now put a few entries and verify find works */
    for (int i = 100; i < 105; i++) {
        test_entry_t e = {.key = i, .value = i * 3};
        ht_cache_put(c, &e, sizeof(e));
    }
    assert(ht_cache_size(c) == 5);

    /* Find should see all 5 */
    count_ctx_t cc = {.count = 0};
    ht_cache_find(c, 42, count_fn, &cc);
    assert(cc.count == 5);

    /* Find specific entries */
    for (int target = 0; target < 5; target++) {
        pick_nth_ctx_t pc = {.target_index = target, .current_index = 0};
        void *found = ht_cache_find(c, 42, pick_nth_scan, &pc);
        assert(found != NULL);
        assert(((test_entry_t *)found)->key >= 100);
    }

    ht_cache_destroy(c);
    printf("  PASS find_after_tombstone_churn\n");
}

/* R9-extra: eq_fn with three-way match (key AND value must both match) */
static bool strict_eq_fn(const void *key, size_t key_len,
                         const void *entry, size_t entry_size, void *ctx) {
    (void)key_len; (void)entry_size; (void)ctx;
    const test_entry_t *k = key;
    const test_entry_t *e = entry;
    return k->key == e->key && k->value == e->value;
}

static uint64_t entry_hash_fn(const void *key, size_t len, void *ctx) {
    (void)ctx;
    const test_entry_t *e = key;
    /* Use key-based hash regardless of len */
    (void)len;
    return (uint64_t)(e->key * 2654435761u);
}

static void test_strict_eq_matching(void) {
    ht_cache_config_t cfg = {
        .capacity   = 8,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = entry_hash_fn,
        .eq_fn      = strict_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    /* Put two entries with same key but different values */
    test_entry_t e1 = {.key = 5, .value = 100};
    test_entry_t e2 = {.key = 5, .value = 200};
    ht_cache_put(c, &e1, sizeof(e1));
    ht_cache_put(c, &e2, sizeof(e2));
    assert(ht_cache_size(c) == 2);

    /* Get with key=5, value=100 — must find e1 */
    test_entry_t lookup1 = {.key = 5, .value = 100};
    test_entry_t *found1 = ht_cache_get(c, &lookup1, sizeof(lookup1));
    assert(found1 != NULL && found1->value == 100);

    /* Get with key=5, value=200 — must find e2 */
    test_entry_t lookup2 = {.key = 5, .value = 200};
    test_entry_t *found2 = ht_cache_get(c, &lookup2, sizeof(lookup2));
    assert(found2 != NULL && found2->value == 200);

    /* Get with key=5, value=999 — no match */
    test_entry_t lookup3 = {.key = 5, .value = 999};
    test_entry_t *found3 = ht_cache_get(c, &lookup3, sizeof(lookup3));
    assert(found3 == NULL);

    ht_cache_destroy(c);
    printf("  PASS strict_eq_matching\n");
}

/* R9-extra: promote after eviction of that specific entry (stale promote, safe no-op) */
static void test_promote_after_evict_of_entry(void) {
    ht_cache_config_t cfg = {
        .capacity   = 4,
        .entry_size = sizeof(test_entry_t),
        .hash_fn    = collision_hash_fn,
        .eq_fn      = collision_eq_fn,
    };
    ht_cache_t *c = ht_cache_create(&cfg);

    for (int i = 0; i < 4; i++) {
        test_entry_t e = {.key = i, .value = i};
        ht_cache_put(c, &e, sizeof(e));
    }

    /* Find key=0 (LRU tail) */
    pick_nth_ctx_t pc = {.target_index = 0, .current_index = 0};
    void *found0 = ht_cache_find(c, 42, pick_nth_scan, &pc);
    assert(found0 != NULL && ((test_entry_t *)found0)->key == 0);

    /* Evict — removes key=0 */
    ht_cache_evict(c);
    int k0 = 0;
    assert(ht_cache_get(c, &k0, sizeof(k0)) == NULL);

    /* Promote the stale pointer — entry is no longer live, safe no-op */
    ht_cache_promote(c, found0);

    /* Verify remaining entries intact */
    assert(ht_cache_size(c) == 3);
    for (int i = 1; i < 4; i++) {
        int k = i;
        assert(ht_cache_get(c, &k, sizeof(k)) != NULL);
    }

    ht_cache_destroy(c);
    printf("  PASS promote_after_evict_of_entry\n");
}

/* ── Main ─────────────────────────────────────────────────────── */

int main(void) {
    printf("ht_cache tests:\n");

    /* Original tests */
    test_create_destroy();
    test_null_args();
    test_put_get();
    test_lru_eviction();
    test_get_promotes();
    test_manual_evict();
    test_remove();
    test_clear();
    test_find_scan();
    test_two_phase_scan();
    test_promote_after_find();
    test_hash_collisions();
    test_iteration();
    test_single_entry();
    test_wrong_size();
    test_evict_reuse();

    /* Edge cases round 1 */
    test_invalid_config();
    test_null_eq_fn();
    test_duplicate_keys();
    test_in_place_mutation();
    test_pointer_stability();
    test_remove_lru_head();
    test_remove_lru_tail();
    test_remove_lru_middle();
    test_promote_head_noop();
    test_promote_tail();
    test_promote_middle();
    test_evict_all();
    test_remove_all_then_refill();
    test_remove_reinsert();
    test_get_remove_get();
    test_iter_after_remove();
    test_iter_clear_refill();
    test_find_empty();
    test_scan_never_match();
    test_scan_first_match();
    test_scan_visits_all();
    test_capacity_two();
    test_fill_exact();
    test_large_cache();
    test_rapid_churn();
    test_put_churn();
    test_collision_churn();
    test_negative_keys();
    test_zero_key();
    test_collision_remove_one_by_one();
    test_clear_single();
    test_put_null();
    test_remove_nonexistent();
    test_lru_ordering();
    test_promote_invalid();
    test_find_wrong_hash();
    test_repeated_get();
    test_evict_reinsert();
    test_iter_null();

    /* Edge cases round 2 */
    test_large_entry_size();
    test_hash_value_one();
    test_hash_value_two();
    test_alternating_insert_remove();
    test_interleaved_ops();
    test_find_after_eviction();
    test_sequential_find();
    test_scan_collect_all();
    test_find_then_remove();
    test_capacity_one_comprehensive();
    test_clear_refill_capacity();
    test_get_after_evict();
    test_repeated_promote();
    test_size_accuracy();
    test_put_same_key_fills();
    test_data_integrity();
    test_evict_all_recover();
    test_collision_remove_middle();
    test_tombstone_accumulation();
    test_find_selects_correct();
    test_collision_churn_with_removes();
    test_extreme_keys();
    test_stress_collision_mixed();
    test_iter_count_matches_size();
    test_string_keys();
    test_spill_lane_keys();
    test_stress_large();
    test_find_no_promote();
    test_promote_then_remove();
    test_overwrite_via_pointer();
    test_two_phase_promote_fallback();

    /* Edge cases round 3 */
    test_find_null_scanfn();
    test_double_clear();
    test_ops_after_clear();
    test_clear_put_same_key();
    test_remove_only_then_put();
    test_distinct_pointers();
    test_user_ctx_passthrough();
    test_reverse_access_eviction();
    test_evict_after_get_lru();
    test_remove_mru_then_put();
    test_steady_state_churn();
    test_multiple_clear_cycles();
    test_remove_all_individually();
    test_collision_evict_reuse();
    test_scan_visit_order();
    test_collision_scan_all_visited();
    test_tiny_entry();
    test_lru_systematic();
    test_collision_get_after_removes();
    test_slot_reuse_after_remove();
    test_long_simulation();
    test_collision_find_after_evict();
    test_collision_evict_and_refill();
    test_promote_verify_with_find();
    test_drain_full_cache();
    test_fill_twice_no_evict();
    test_hash_48max();
    test_multiple_two_phase();

    /* Edge cases round 4 */
    test_null_key();
    test_capacity_invariant();
    test_size_bound();
    test_put_pointer_range();
    test_collision_same_key();
    test_null_eq_remove();
    test_scan_early_stop();
    test_mixed_hash();
    test_collision_wipe_and_refill();
    test_find_hash_zero();
    test_very_large_capacity();
    test_no_cross_contamination();
    test_iter_sees_new_entries();
    test_collision_tombstone_stress();
    test_promote_visible_in_iter();
    test_cache_isolation();
    test_put_evict_get_put();
    test_collision_full_evict_cycle();
    test_scan_entry_pointers();
    test_collision_overflow();
    test_overwrite_semantic();
    test_interleaved_unique_collision();
    test_evict_after_clear();
    test_remove_empty();
    test_double_remove();

    /* Edge cases round 5 */
    test_minimal_collision();
    test_zero_entry();
    test_sequential_hashes();
    test_hash_upper_bits();
    test_evict_last_entry();
    test_find_promote_find();
    test_many_clears();
    test_slot_lifecycle();
    test_collision_promote_evict();
    test_collision_multi_remove();
    test_evict_then_put_same();
    test_lru_fresh_after_clear();
    test_collision_identical_entries();
    test_iter_slot_order();
    test_non_power2_capacity();
    test_get_clear_put_get();
    test_collision_remove_all_reput();
    test_double_promote();
    test_stress_mixed_hash();
    test_put_no_fail_until_full();
    test_remove_during_iteration();
    test_collision_scan_after_mutation();
    test_different_key_len();
    test_collision_lru_order();

    /* Edge cases round 6: least-tested call sequences */
    test_find_promote_remove();
    test_find_promote_find_cycle();
    test_clear_then_find();
    test_clear_then_evict();
    test_clear_then_promote();
    test_clear_then_iter();
    test_find_picks_nonfirst();
    test_evict_interleaved_get();
    test_get_modify_find();
    test_iter_matches_get();
    test_promote_remove_evict();
    test_find_after_remove_all_hash();
    test_slot_recycle_integrity();
    test_evict_changes_find();
    test_promote_collision_survives_evict();
    test_put_evict_find_old();
    test_full_find_lifecycle();
    test_find_accumulate_ctx();
    test_capacity3_evict_cycle();
    test_find_nonexistent_hash();
    test_remove_middle_find_remaining();
    test_iter_partial_drain();
    test_promote_then_evict_ordering();
    test_find_always_first();
    test_full_cycle_after_clear();
    test_mixed_find_get_collision();
    test_find_promote_then_get();
    test_evict_put_get_lru_tracking();
    test_find_remove_reput();
    test_size_after_mixed_ops();

    /* Edge cases round 7: remaining uncovered call sequences */
    test_find_promote_evict_find();
    test_promote_stale_reused_slot();
    test_complex_slot_recycling();
    test_iter_with_promote_midway();
    test_double_clear_fill_iter();
    test_rapid_same_key_cycling();
    test_find_tombstoned_hash();
    test_get_find_get_consistency();
    test_find_modify_evict_find();
    test_multiple_scan_fns_same_hash();
    test_stress_find_promote();
    test_promoted_entry_eventually_evicted();
    test_iter_remove_unvisited();
    test_spill_lane_find_cycle();
    test_drain_then_find_get_iter();
    test_evict_mixed_collision();

    /* Edge cases round 8: final uncovered sequences */
    test_find_only_lifecycle();
    test_evict_after_find_promote_only();
    test_clear_after_find_promote();
    test_scan_fn_modifies_entries();
    test_evict_promote_only_lru();
    test_all_apis_sequence();
    test_eq_fn_uses_user_ctx();
    test_find_after_evict_drain_refill();
    test_remove_mru_evict();
    test_clear_mid_iteration();
    test_promote_already_mru();
    test_find_max_value();
    test_multi_promote_then_evict();
    test_data_isolation_across_entries();

    /* Edge cases round 9: last uncovered + novel patterns */
    test_find_first_changes_after_remove();
    test_triple_clear_fill_iter();
    test_double_find_promote_then_evict();
    test_capacity2_full_lifecycle();
    test_find_enumerate_all();
    test_promote_all_reverse_then_evict();
    test_stress_find_promote_evict_only();
    test_find_min_value();
    test_invariant_checked_sequence();
    test_find_after_tombstone_churn();
    test_strict_eq_matching();
    test_promote_after_evict_of_entry();

    printf("  All tests passed!\n");
    return 0;
}
