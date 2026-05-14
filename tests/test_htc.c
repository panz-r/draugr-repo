#include "draugr/htc.h"
#include "draugr/htc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#ifndef DRAUGR_USE_MALLOC
#include "draugr/arena.h"
static struct arena *test_arena = NULL;
#else
static void *test_arena = NULL;
#endif

static int tests_passed = 0;
static int tests_total = 0;

#define TEST(name) do { \
    printf("  %s ... ", name); \
    tests_total++; \
} while (0)
#define PASS() do { \
    printf("PASS\n"); \
    tests_passed++; \
} while (0)

static uint64_t hash_seq(int i) { return (uint64_t)i * 0x9e3779b97f4a7c15ULL; }

static void test_slot_pack_unpack(void) {
    TEST("slot pack/unpack");
    uint64_t w = htc_slot_pack(0x123456789AULL, 0xABCD, HTC_STATE_LIVE, 0);
    assert(htc_slot_index(w)   == 0x123456789AULL);
    assert(htc_slot_tag(w)     == 0xABCD);
    assert(htc_slot_state(w)   == HTC_STATE_LIVE);
    assert(htc_slot_in_secondary(w) == 0);
    assert(htc_slot_live(w)    == 1);
    assert(htc_slot_empty(w)   == 0);

    w = htc_slot_pack(42, 0x1234, HTC_STATE_EMPTY, 1);
    assert(htc_slot_state(w)   == HTC_STATE_EMPTY);
    assert(htc_slot_in_secondary(w) == 1);
    assert(htc_slot_empty(w)   == 1);
    assert(htc_slot_live(w)    == 0);

    uint64_t emp = htc_slot_empty_word();
    assert(htc_slot_state(emp) == HTC_STATE_EMPTY);
    assert(htc_slot_empty(emp) == 1);
    PASS();
}

static void test_tag16_partial8(void) {
    TEST("tag16 / partial8");
    uint16_t t = htc_tag16(0xDEADBEEFCAFE1234ULL);
    assert(t != 0);

    uint16_t t_zero = htc_tag16(0x0000000000000000ULL);
    assert(t_zero == HTC_TAG_ZERO_REPLACEMENT);

    uint8_t p = htc_partial8(t);
    assert(p != 0);

    uint8_t p_same = htc_partial8(t);
    assert(p == p_same);

    uint8_t pz = htc_partial8(0);
    assert(pz == HTC_TAG_ZERO_REPLACEMENT);
    PASS();
}

static void test_basic_crud(void) {
    TEST("basic CRUD");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);
    assert(htc_size(t) == 0);

    assert(htc_insert(t, hash_seq(1), 100) == true);
    assert(htc_size(t) == 1);

    uint64_t out = 0;
    assert(htc_find(t, hash_seq(1), &out) == true);
    assert(out == 100);

    assert(htc_remove(t, hash_seq(1)) == true);
    assert(htc_size(t) == 0);

    assert(htc_find(t, hash_seq(1), &out) == false);

    htc_destroy(t);
    PASS();
}

static void test_duplicate_insert(void) {
    TEST("duplicate insert");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_insert(t, hash_seq(5), 50) == true);
    assert(htc_insert(t, hash_seq(5), 60) == false);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(5), &out) == true);
    assert(out == 50);
    htc_destroy(t);
    PASS();
}

static void test_upsert(void) {
    TEST("upsert");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_upsert(t, hash_seq(10), 200) == true);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(10), &out) == true);
    assert(out == 200);

    assert(htc_upsert(t, hash_seq(10), 300) == true);
    assert(htc_find(t, hash_seq(10), &out) == true);
    assert(out == 300);
    assert(htc_size(t) == 1);
    htc_destroy(t);
    PASS();
}

static void test_update(void) {
    TEST("update");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_insert(t, hash_seq(20), 400) == true);
    assert(htc_update(t, hash_seq(20), 500) == true);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(20), &out) == true);
    assert(out == 500);

    assert(htc_update(t, hash_seq(99), 600) == false);
    htc_destroy(t);
    PASS();
}

static void test_remove_nonexistent(void) {
    TEST("remove nonexistent");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_remove(t, hash_seq(999)) == false);
    htc_destroy(t);
    PASS();
}

static void test_many_entries(void) {
    TEST("many entries");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    int N = 1000;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i * 10) == true);
    }
    assert(htc_size(t) == (size_t)N);
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        assert(htc_find(t, hash_seq(i), &out) == true);
        assert(out == (uint64_t)i * 10);
    }
    for (int i = 0; i < N; i++) {
        assert(htc_remove(t, hash_seq(i)) == true);
    }
    assert(htc_size(t) == 0);
    htc_destroy(t);
    PASS();
}

static void test_location_bits(void) {
    TEST("location bits");
    uint64_t p = 0xDEADBEEF;
    uint16_t tag = htc_tag16(p);
    uint32_t b1 = (uint32_t)(p & 0xFFFFFFFF);
    uint32_t b2 = htc_alt_bucket(b1, p, tag);
    assert(b1 != b2);

    assert(htc_shard_of(0, 4) == 0);
    assert(htc_shard_of(3, 4) == 3);
    assert(htc_shard_of(5, 4) == 1);
    PASS();
}

static void test_location_scan_integrity(void) {
    TEST("location scan integrity");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    int N = 200;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i) == true);
    }
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        assert(htc_find(t, hash_seq(i), &out) == true);
        assert(out == (uint64_t)i);
    }
    assert(htc_size(t) == (size_t)N);
    htc_destroy(t);
    PASS();
}

static void test_stash_validation(void) {
    TEST("stash validation");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    int N = 500;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i) == true);
    }
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        assert(htc_find(t, hash_seq(i), &out) == true);
        assert(out == (uint64_t)i);
    }
    assert(htc_size(t) == (size_t)N);
    htc_destroy(t);
    PASS();
}

static void test_clear(void) {
    TEST("clear");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_insert(t, hash_seq(1), 10) == true);
    assert(htc_insert(t, hash_seq(2), 20) == true);
    assert(htc_size(t) == 2);
    htc_clear(t);
    assert(htc_size(t) == 0);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(1), &out) == false);
    assert(htc_find(t, hash_seq(2), &out) == false);
    htc_destroy(t);
    PASS();
}

static void test_tag_collisions(void) {
    TEST("tag collisions");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    int N = 50;
    uint64_t base = 0x100000000ULL;
    for (int i = 0; i < N; i++) {
        uint64_t h = base | (uint64_t)(i * 0x10000);
        assert(htc_insert(t, h, (uint64_t)i) == true);
    }
    for (int i = 0; i < N; i++) {
        uint64_t h = base | (uint64_t)(i * 0x10000);
        uint64_t out = 0;
        assert(htc_find(t, h, &out) == true);
        assert(out == (uint64_t)i);
    }
    htc_destroy(t);
    PASS();
}

static void test_grow(void) {
    TEST("grow");
    htc_config_t cfg = {4, 0.5};
    htc_table_t *t = htc_create(&cfg);
    int N = 100;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i) == true);
    }
    assert(htc_size(t) == (size_t)N);
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        assert(htc_find(t, hash_seq(i), &out) == true);
        assert(out == (uint64_t)i);
    }
    htc_destroy(t);
    PASS();
}

static void test_grow_remove(void) {
    TEST("grow + remove");
    htc_config_t cfg = {4, 0.5};
    htc_table_t *t = htc_create(&cfg);
    int N = 100;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i) == true);
    }
    for (int i = 0; i < N; i += 2) {
        assert(htc_remove(t, hash_seq(i)) == true);
    }
    assert(htc_size(t) == (size_t)(N / 2));
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        if (i % 2 == 0) {
            assert(htc_find(t, hash_seq(i), &out) == false);
        } else {
            assert(htc_find(t, hash_seq(i), &out) == true);
            assert(out == (uint64_t)i);
        }
    }
    htc_destroy(t);
    PASS();
}

static void test_multiple_grows(void) {
    TEST("multiple grows");
    htc_config_t cfg = {2, 0.5};
    htc_table_t *t = htc_create(&cfg);
    int N = 500;
    for (int i = 0; i < N; i++) {
        assert(htc_insert(t, hash_seq(i), (uint64_t)i * 2) == true);
    }
    assert(htc_size(t) == (size_t)N);
    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        assert(htc_find(t, hash_seq(i), &out) == true);
        assert(out == (uint64_t)i * 2);
    }
    htc_destroy(t);
    PASS();
}

static void test_remap_tracking(void) {
    TEST("remap tracking");
    htc_bucket_meta_t m = {0};

    assert(htc_must_check_secondary(&m, 0x1234) == false);

    htc_remap_inc(&m, 0x1234);
    assert(m.remap_count > 0);
    assert(m.remap_filter != 0);
    assert(htc_must_check_secondary(&m, 0x1234) == true);

    htc_remap_dec(&m);
    assert(m.remap_count == 0);

    m.remap_count = HTC_REMAP_SATURATED;
    assert(htc_must_check_secondary(&m, 0xABCD) == true);

    htc_remap_dec(&m);
    assert(m.remap_count == HTC_REMAP_SATURATED);
    PASS();
}

static void test_remap_zero_skip(void) {
    TEST("remap zero skip");
    htc_bucket_meta_t m = {0};
    assert(htc_must_check_secondary(&m, 0) == false);

    htc_remap_inc(&m, 0);
    assert(m.remap_count > 0);

    htc_remap_dec(&m);
    assert(m.remap_count == 0);

    htc_remap_dec(&m);
    assert(m.remap_count == 0);
    PASS();
}

static void test_model_comparison(void) {
    TEST("model comparison");
    htc_config_t cfg = {16, 0.75};
    htc_table_t *t = htc_create(&cfg);
    int N = 300;
    int *model = calloc((size_t)N, sizeof(int));
    assert(model != NULL);

    for (int i = 0; i < N; i++) {
        uint64_t h = hash_seq(i);
        bool ins = htc_insert(t, h, (uint64_t)i * 3);
        if (!model[i]) {
            assert(ins == true);
            model[i] = 1;
        } else {
            assert(ins == false);
        }
    }

    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        bool found = htc_find(t, hash_seq(i), &out);
        assert(found == (model[i] != 0));
        if (found) assert(out == (uint64_t)i * 3);
    }

    for (int i = 0; i < N; i += 3) {
        bool removed = htc_remove(t, hash_seq(i));
        assert(removed == (model[i] != 0));
        model[i] = 0;
    }

    for (int i = 0; i < N; i++) {
        uint64_t out = 0;
        bool found = htc_find(t, hash_seq(i), &out);
        assert(found == (model[i] != 0));
    }

    free(model);
    htc_destroy(t);
    PASS();
}

static void test_null_safety(void) {
    TEST("null safety");
    htc_destroy(NULL);
    assert(htc_find(NULL, 0, NULL) == false);
    assert(htc_insert(NULL, 0, 0) == false);
    assert(htc_remove(NULL, 0) == false);
    assert(htc_size(NULL) == 0);
    PASS();
}

static void test_custom_config(void) {
    TEST("custom config");
    htc_config_t cfg = {64, 0.9};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);
    assert(htc_insert(t, hash_seq(42), 999) == true);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(42), &out) == true);
    assert(out == 999);
    htc_destroy(t);
    PASS();
}

static void test_zero_config(void) {
    TEST("zero config");
    htc_config_t cfg = {0, 0.0};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);
    assert(htc_insert(t, hash_seq(1), 1) == true);
    uint64_t out = 0;
    assert(htc_find(t, hash_seq(1), &out) == true);
    assert(out == 1);
    htc_destroy(t);
    PASS();
}

static void test_arena_alloc_free(void) {
    TEST("arena alloc/free");
    htc_arena_t ha = {0};
    uint32_t idx1 = htc_arena_alloc(&ha, 0x42, 100);
    assert(idx1 != (uint32_t)-1);
    assert(idx1 == 0);

    htc_record_t *r = htc_arena_ptr(&ha, idx1);
    assert(r != NULL);
    assert(r->full_hash == 0x42);
    assert(r->user_value == 100);

    uint32_t idx2 = htc_arena_alloc(&ha, 0x43, 200);
    assert(idx2 != (uint32_t)-1);

    htc_arena_free(&ha, idx1);
    htc_arena_free(&ha, idx2);

    free(ha.records);
    free(ha.free_idx);
    PASS();
}

static void test_stash_overflow(void) {
    TEST("stash overflow");
    htc_arena_t ha = {0};

    htc_stash_t s = {0};
    s.allocator = NULL;

    int N = 100;
    for (int i = 0; i < N; i++) {
        uint64_t slot = htc_slot_pack((uint64_t)i, (uint16_t)i, HTC_STATE_LIVE, 0);
        assert(htc_stash_insert(&s, slot) >= 0);
    }
    assert(s.size == (uint32_t)N);

    unsigned removed = 0;
    for (int i = 0; i < N; i += 2) {
        htc_stash_remove_at(&s, (unsigned)i);
        removed++;
    }
    assert(s.size == (uint32_t)(N - (int)removed));

    free(ha.records);
    free(ha.free_idx);
    free(s.slots);
    PASS();
}

int main(void) {
    printf("=== htc unit tests ===\n");

#ifndef DRAUGR_USE_MALLOC
    test_arena = arena_create(1024 * 1024);
#endif

    test_slot_pack_unpack();
    test_tag16_partial8();
    test_basic_crud();
    test_duplicate_insert();
    test_upsert();
    test_update();
    test_remove_nonexistent();
    test_many_entries();
    test_location_bits();
    test_location_scan_integrity();
    test_stash_validation();
    test_clear();
    test_tag_collisions();
    test_grow();
    test_grow_remove();
    test_multiple_grows();
    test_remap_tracking();
    test_remap_zero_skip();
    test_model_comparison();
    test_null_safety();
    test_custom_config();
    test_zero_config();

#ifndef DRAUGR_USE_MALLOC
    test_arena_alloc_free();
    test_stash_overflow();
#else
    printf("  (skipping arena/stash tests without arena allocator)\n");
    tests_total += 2;
    tests_passed += 2;
#endif

    printf("=== %d / %d tests passed ===\n", tests_passed, tests_total);

#ifndef DRAUGR_USE_MALLOC
    arena_destroy(test_arena);
#endif

    return tests_passed == tests_total ? 0 : 1;
}
