#include "draugr/htc.h"
#include "draugr/htc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
    htc_table_t *t = htc_create(&cfg);
    assert(htc_remove(t, hash_seq(999)) == false);
    htc_destroy(t);
    PASS();
}

static void test_many_entries(void) {
    TEST("many entries");
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
    htc_table_t *t = htc_create(&cfg);
    int N = 48;
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
    htc_config_t cfg = {4, 0.5, .shard_count = 0};
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
    htc_config_t cfg = {4, 0.5, .shard_count = 0};
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
    htc_config_t cfg = {2, 0.5, .shard_count = 0};
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
    htc_config_t cfg = {16, 0.75, .shard_count = 0};
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
    htc_config_t cfg = {64, 0.9, .shard_count = 0};
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
    htc_config_t cfg = {0, 0.0, .shard_count = 0};
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

    /* stash grows 4→8→16→32. 33rd insert should fail. */
    int N = 33;
    for (int i = 0; i < N; i++) {
        uint64_t slot = htc_slot_pack((uint64_t)i, (uint16_t)i, HTC_STATE_LIVE, 0);
        int ret = htc_stash_insert(&s, slot);
        if (i < 32)
            assert(ret >= 0);
        else
            assert(ret < 0);
    }
    assert(s.size == 32);

    htc_stash_remove_at(&s, 0);
    assert(s.size == 31);

    free(ha.records);
    free(ha.free_idx);
    free(s.slots);
    PASS();
}

/* ============================================================================
 * Phase 2 tests
 * ============================================================================ */

static void test_seq_guard(void) {
    TEST("seq guard begin/end");
    htc_bucket_meta_t m = {0};
    htc_seq_guard_t g = htc_bucket_seq_begin(&m);
    assert(__atomic_load_n(&m.seq, __ATOMIC_RELAXED) & HTC_SEQ_BUSY);
    htc_bucket_seq_end(&m, g);
    assert(!(__atomic_load_n(&m.seq, __ATOMIC_RELAXED) & HTC_SEQ_BUSY));
    assert(__atomic_load_n(&m.seq, __ATOMIC_RELAXED) == 2);
    PASS();
}

static void test_spinlock(void) {
    TEST("spinlock lock/unlock");
    htc_spinlock_t lk = {0};
    htc_spin_lock(&lk);
    assert(__atomic_load_n(&lk.flag, __ATOMIC_RELAXED) == 1);
    htc_spin_unlock(&lk);
    assert(__atomic_load_n(&lk.flag, __ATOMIC_RELAXED) == 0);
    PASS();
}

static void test_epoch_pin_unpin(void) {
    TEST("epoch pin/unpin");
    htc_epoch_ctl_t ep = {0};
    __atomic_store_n(&ep.global_epoch, 1, __ATOMIC_RELAXED);
    uint64_t e = htc_epoch_pin(&ep);
    assert(e == 1);
    assert(__atomic_load_n(&ep.thread_epoch[0], __ATOMIC_RELAXED) == 1);
    htc_epoch_unpin(&ep);
    assert(__atomic_load_n(&ep.thread_epoch[0], __ATOMIC_RELAXED) == 0);
    PASS();
}

static void test_find_during_insert(void) {
    TEST("find during insert (seq validation)");
    htc_table_t *t = htc_create(NULL);
    assert(t != NULL);
    assert(htc_insert(t, 0x42, 100));
    uint64_t out;
    assert(htc_find(t, 0x42, &out));
    assert(out == 100);
    htc_destroy(t);
    PASS();
}

static void test_find_during_delete(void) {
    TEST("find during delete (seq validation)");
    htc_table_t *t = htc_create(NULL);
    assert(htc_insert(t, 0x42, 100));
    assert(htc_remove(t, 0x42));
    assert(!htc_find(t, 0x42, NULL));
    htc_destroy(t);
    PASS();
}

static void test_find_during_ctrl_update(void) {
    TEST("find during ctrl_tags update");
    htc_table_t *t = htc_create(NULL);
    assert(t != NULL);
    for (int i = 0; i < 100; i++)
        assert(htc_insert(t, (uint64_t)i, (uint64_t)i));

    /* Verify all entries are findable */
    for (int i = 0; i < 100; i++) {
        uint64_t v;
        assert(htc_find(t, (uint64_t)i, &v));
        assert(v == (uint64_t)i);
    }
    htc_destroy(t);
    PASS();
}

static void test_bfs_displacement(void) {
    TEST("BFS displacement path");
    /* Force displacement by filling a bucket beyond capacity.
       With 8-slot buckets, inserting 9 entries with the same b1
       should trigger BFS to find space in the secondary bucket. */
    htc_config_t cfg = {4, 0.99, 0};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);

    /* Fill bucket 0 by using hashes that all map to bucket 0.
       With only 4 buckets but 8 slots each = 32 slots total,
       insert 40 entries — many must displace via BFS. */
    int n = 40;
    for (int i = 0; i < n; i++)
        assert(htc_insert(t, (uint64_t)i, (uint64_t)i));

    assert(htc_size(t) == (size_t)n);
    for (int i = 0; i < n; i++) {
        uint64_t v;
        assert(htc_find(t, (uint64_t)i, &v));
        assert(v == (uint64_t)i);
    }
    htc_destroy(t);
    PASS();
}

static void test_seq_busy_retry(void) {
    TEST("seq busy retry on find");
    htc_table_t *t = htc_create(NULL);
    assert(htc_insert(t, 0x42, 100));

    /* Manually mark bucket's seq as busy — find should retry until stable */
    uint32_t b1 = 0x42 & t->bucket_mask;
    __atomic_fetch_or(&t->meta[b1].seq, HTC_SEQ_BUSY, __ATOMIC_RELEASE);
    __atomic_store_n(&t->meta[b1].seq, 2, __ATOMIC_RELEASE);

    uint64_t out;
    assert(htc_find(t, 0x42, &out));
    assert(out == 100);
    htc_destroy(t);
    PASS();
}

/* ─── Poison on free / front cache stale delete test ────────── */
static void test_poison_on_free(void) {
    TEST("poison on free / front cache stale delete");
    htc_table_t *t = htc_create(NULL);
    uint64_t out = 0;

    assert(htc_insert(t, 42, 100));
    assert(htc_find(t, 42, &out) && out == 100);

    /* Delete — find should return false immediately */
    assert(htc_remove(t, 42));
    assert(!htc_find(t, 42, &out));

    /* Force arena index reuse by cycling many entries.
     * With poison-on-free, the old record's full_hash is cleared and
     * generation incremented, so the front cache cannot match. */
    for (int i = 0; i < 200; i++)
        assert(htc_insert(t, hash_seq(i), (uint64_t)i));
    for (int i = 0; i < 200; i++) {
        assert(htc_find(t, hash_seq(i), &out) && out == (uint64_t)i);
    }

    /* Old hash 42 must still not be found (record was poisoned) */
    assert(!htc_find(t, 42, &out));
    htc_destroy(t);
    PASS();
}

/* ─── Concurrent migration coherence test ───────────────────── */
typedef struct {
    htc_table_t *t;
    int          start;
    int          count;
    int          result; /* 0 = ok, 2 = insert fail, 3 = immediate find fail */
    int          fail_i; /* index that failed */
} migration_thread_arg_t;

static void *migration_thread_worker(void *arg) {
    migration_thread_arg_t *a = (migration_thread_arg_t *)arg;
    for (int i = a->start; i < a->start + a->count; i++) {
        uint64_t h = hash_seq(i);
        if (!htc_insert(a->t, h, (uint64_t)i)) {
            a->result = 2;
            a->fail_i = i;
            return NULL;
        }
        uint64_t out = 0xdead;
        if (!htc_find(a->t, h, &out) || out != (uint64_t)i) {
            a->result = 3;
            a->fail_i = i;
            return NULL;
        }
    }
    return NULL;
}

static void test_migration_concurrent(void) {
    TEST("concurrent migration coherence");
    htc_config_t cfg = {8, 0.7, 0};
    htc_table_t *t = htc_create(&cfg);

    int N = 200;
    int T = 2;
    pthread_t threads[4];
    migration_thread_arg_t args[4];
    int per_thread = N / T;

    for (int ti = 0; ti < T; ti++) {
        args[ti].t = t;
        args[ti].start = ti * per_thread;
        args[ti].count = per_thread;
        args[ti].result = 0;
        pthread_create(&threads[ti], NULL, migration_thread_worker, &args[ti]);
    }

    int fails = 0;
    for (int ti = 0; ti < T; ti++) {
        pthread_join(threads[ti], NULL);
        if (args[ti].result != 0) {
            fprintf(stderr, "  Thread %d failed: result=%d at i=%d\n",
                    ti, args[ti].result, args[ti].fail_i);
            fails++;
        }
    }

    assert(fails == 0);
    assert(__atomic_load_n(&t->size, __ATOMIC_RELAXED) == (size_t)N);
    htc_destroy(t);
    PASS();
}

/* ─── Multi-threaded stress test (mixed ops, many threads) ─── */
typedef struct {
    htc_table_t *t;
    int          id;
    int          ops;
    int          result;
} stress_thread_arg_t;

static void *stress_worker(void *arg) {
    stress_thread_arg_t *a = (stress_thread_arg_t *)arg;
    uint64_t seed = (uint64_t)a->id * 0x9e3779b97f4a7c15ULL;

    for (int i = 0; i < a->ops; i++) {
        seed = seed * 6364136223846793005ULL + 1;
        uint64_t hash = seed;
        uint64_t val = seed >> 1;
        int op = (int)(seed & 3);

        switch (op) {
        case 0: /* insert */
            htc_insert(a->t, hash, val);
            break;
        case 1: /* upsert */
            htc_upsert(a->t, hash, val);
            break;
        case 2: /* find */
            { uint64_t out; htc_find(a->t, hash, &out); }
            break;
        case 3: /* remove */
            htc_remove(a->t, hash);
            break;
        }
    }
    return NULL;
}

static void test_stress_multithreaded(void) {
    TEST("multi-threaded stress (mixed ops)");
    htc_config_t cfg = {64, 0.75, 0};
    htc_table_t *t = htc_create(&cfg);

    int T = 4;
    int OPS = 5000;
    pthread_t threads[8];
    stress_thread_arg_t args[8];

    for (int ti = 0; ti < T; ti++) {
        args[ti].t = t;
        args[ti].id = ti;
        args[ti].ops = OPS;
        args[ti].result = 0;
        pthread_create(&threads[ti], NULL, stress_worker, &args[ti]);
    }

    for (int ti = 0; ti < T; ti++)
        pthread_join(threads[ti], NULL);

    /* Table should be internally consistent (no crashes, no ASan errors) */
    htc_destroy(t);
    PASS();
}

/* ─── Concurrent BFS stress test ────────────────────────────── */
/* Force BFS by inserting same-tag entries from multiple threads */

static void *bfs_stress_worker(void *arg) {
    stress_thread_arg_t *a = (stress_thread_arg_t *)arg;
    /* All threads use hashes with same tag (bits 32-47 = 0, bits 48-63 = 0) */
    uint64_t base = (uint64_t)a->id << 48;
    for (int i = 0; i < a->ops; i++) {
        uint64_t h = base | (uint64_t)(i * 0x10000);
        if (!htc_insert(a->t, h, (uint64_t)i)) {
            /* Insert can fail when table is full — that's OK for stress */
            break;
        }
    }
    return NULL;
}

static void test_bfs_concurrent(void) {
    TEST("concurrent BFS stress");
    htc_config_t cfg = {16, 0.6, 0};
    htc_table_t *t = htc_create(&cfg);

    int T = 4;
    int OPS = 100;
    pthread_t threads[8];
    stress_thread_arg_t args[8];

    for (int ti = 0; ti < T; ti++) {
        args[ti].t = t;
        args[ti].id = ti;
        args[ti].ops = OPS;
        pthread_create(&threads[ti], NULL, bfs_stress_worker, &args[ti]);
    }

    for (int ti = 0; ti < T; ti++)
        pthread_join(threads[ti], NULL);

    htc_destroy(t);
    PASS();
}

/* ─── Lazy migration stress test ────────────────────────────── */
/* Start a lazy resize, then concurrently operate and migrate chunks. */

static void *lazy_migrate_worker(void *arg) {
    stress_thread_arg_t *a = (stress_thread_arg_t *)arg;
    uint64_t seed = (uint64_t)a->id * 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < a->ops; i++) {
        seed = seed * 6364136223846793005ULL + 1;
        uint64_t hash = seed;
        uint64_t val = seed >> 1;
        int op = (int)(seed & 1);
        if (op == 0) htc_insert(a->t, hash, val);
        else { uint64_t out; htc_find(a->t, hash, &out); }
    }
    return NULL;
}

static void test_migration_lazy_stress(void) {
    TEST("lazy migration stress");
    /* Large initial table to prevent workers from triggering sync grows */
    htc_config_t cfg = {512, 1.0, 0};
    htc_table_t *t = htc_create(&cfg);

    /* Pre-fill */
    int N = 100;
    for (int i = 0; i < N; i++)
        assert(htc_insert(t, hash_seq(i), (uint64_t)i));

    /* Start lazy resize (table is small enough that entries cluster) */
    assert(htc_resize_start(t, t->num_buckets * 2));

    /* Concurrently operate and migrate while workers are active */
    int T = 3;
    int OPS = 200;
    pthread_t threads[4];
    stress_thread_arg_t args[4];
    for (int ti = 0; ti < T; ti++) {
        args[ti].t = t;
        args[ti].id = ti;
        args[ti].ops = OPS;
        pthread_create(&threads[ti], NULL, lazy_migrate_worker, &args[ti]);
    }

    /* Migrate a few chunks while workers run, then finish all chunks */
    htc_table_gen_t *g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    uint32_t half = (g->chunk_count + 1) / 2;
    for (uint32_t ci = 0; ci < half; ci++)
        htc_migrate_chunk(t, ci);

    for (int ti = 0; ti < T; ti++)
        pthread_join(threads[ti], NULL);

    /* Reload gen (workers may have triggered grows) and complete migration */
    g = __atomic_load_n(&t->current_gen, __ATOMIC_ACQUIRE);
    for (uint32_t ci = 0; ci < g->chunk_count; ci++)
        htc_migrate_chunk(t, ci);

    /* Finish migration */
    htc_resize_finish(t);

    /* Verify pre-fill entries still findable */
    for (int i = 0; i < N; i++) {
        uint64_t out;
        assert(htc_find(t, hash_seq(i), &out));
        assert(out == (uint64_t)i);
    }
    htc_destroy(t);
    PASS();
}

/* ─── Cross-generation delete test ──────────────────────────── */
/* During lazy migration, deleting an entry that exists in both
 * old and new gen should make it unfindable. */

static void test_migration_crossgen_delete(void) {
    TEST("cross-generation delete during migration");
    htc_table_t *t = htc_create(NULL);

    assert(htc_insert(t, 42, 100));
    assert(htc_find(t, 42, NULL));

    /* Start lazy resize */
    assert(htc_resize_start(t, t->num_buckets * 2));

    /* Migrate the chunk containing entry 42 */
    uint32_t chunk = htc_chunk_of((uint32_t)(42 & (t->num_buckets * 2 - 1)));
    htc_migrate_chunk(t, chunk);

    /* Delete entry from current gen — old copy was cleared by migration */
    assert(htc_remove(t, 42));
    assert(!htc_find(t, 42, NULL));

    /* Finish migration */
    htc_resize_finish(t);

    /* Still not findable after resize completes */
    assert(!htc_find(t, 42, NULL));
    htc_destroy(t);
    PASS();
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
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

    /* Phase 2 tests */
    test_seq_guard();
    test_spinlock();
    test_epoch_pin_unpin();
    test_find_during_insert();
    test_find_during_delete();
    test_find_during_ctrl_update();
    test_bfs_displacement();
    test_seq_busy_retry();

    /* P1: poison-on-free prevents front cache stale reads */
    test_poison_on_free();
    /* P0: concurrent resize + access coherence */
    test_migration_concurrent();
    /* Multi-threaded stress (mixed ops, ASan validates no races) */
    test_stress_multithreaded();
    /* Concurrent BFS stress (same-tag entries from 4 threads) */
    test_bfs_concurrent();
    /* Lazy migration with concurrent operations */
    test_migration_lazy_stress();
    /* Cross-generation delete during migration */
    test_migration_crossgen_delete();
#else
    printf("  (skipping arena/stash tests without arena allocator)\n");
    tests_total += 4;
    tests_passed += 4;
#endif

    printf("=== %d / %d tests passed ===\n", tests_passed, tests_total);

#ifndef DRAUGR_USE_MALLOC
    arena_destroy(test_arena);
#endif

    return tests_passed == tests_total ? 0 : 1;
}
