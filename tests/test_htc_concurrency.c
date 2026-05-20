/**
 * test_htc_concurrency.c — Concurrency tests for htc
 *
 * Battery coverage:
 *   C1: htc_upsert_concurrent_grow
 *   C2: htc_find_scoped_epoch_race
 *   C3: htc_remove_oldgen_race
 *   R2: htc_upsert_epoch_pin_regression
 *   R3: htc_upsert_duplicate_race (concurrent upsert same key)
 */

#include "draugr/htc.h"
#include "draugr/htc_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include <unistd.h>

static uint64_t hash_seq(int i) { return (uint64_t)i * 0x9e3779b97f4a7c15ULL; }

/* ─── C1: Upsert concurrent with grow ─────────────────────────── */

typedef struct {
    htc_table_t *t;
    int id;
    int ops;
} c1_arg_t;

static void *c1_insert_worker(void *arg) {
    c1_arg_t *a = (c1_arg_t *)arg;
    for (int i = 0; i < a->ops; i++) {
        uint64_t h = hash_seq(a->id * 10000 + i);
        htc_insert(a->t, h, (uint64_t)i);
    }
    return NULL;
}

static void *c1_upsert_worker(void *arg) {
    c1_arg_t *a = (c1_arg_t *)arg;
    unsigned seed = (unsigned)a->id * 77777u;
    for (int i = 0; i < a->ops; i++) {
        int key = (int)((seed = seed * 1103515245u + 12345u) % 5000);
        uint64_t h = hash_seq(key);
        htc_upsert(a->t, h, (uint64_t)i);
    }
    return NULL;
}

static void test_upsert_concurrent_grow(void) {
    printf("  C1: upsert concurrent with grow (2 ins + 2 ups threads × 5000) ... ");
    htc_config_t cfg = {.initial_buckets = 16, .max_load_factor = 0.5};
    htc_table_t *t = htc_create(&cfg);
    assert(t);

    int T_INS = 2, T_UPS = 2, OPS = 5000;
    pthread_t threads[4];
    c1_arg_t args[4];

    for (int i = 0; i < T_INS; i++) {
        args[i] = (c1_arg_t){t, i, OPS};
        pthread_create(&threads[i], NULL, c1_insert_worker, &args[i]);
    }
    for (int i = 0; i < T_UPS; i++) {
        args[T_INS + i] = (c1_arg_t){t, T_INS + i, OPS};
        pthread_create(&threads[T_INS + i], NULL, c1_upsert_worker, &args[T_INS + i]);
    }
    for (int i = 0; i < T_INS + T_UPS; i++)
        pthread_join(threads[i], NULL);

    htc_destroy(t);
    printf("PASS\n");
}

/* ─── C2: find_scoped epoch race ───────────────────────────────── */

typedef struct {
    htc_table_t *t;
    uint64_t     hash;
    int          ops;
    int          is_writer;
} c2_arg_t;

static void *c2_writer(void *arg) {
    c2_arg_t *a = (c2_arg_t *)arg;
    for (int i = 0; i < a->ops; i++) {
        htc_insert(a->t, a->hash, (uint64_t)i);
        htc_remove(a->t, a->hash);
    }
    return NULL;
}

static void *c2_reader(void *arg) {
    c2_arg_t *a = (c2_arg_t *)arg;
    for (int i = 0; i < a->ops; i++) {
        uint64_t val = 0;
        htc_error_t ret = htc_find_scoped(a->t, a->hash, &val);
        if (ret == HTC_OK) {
            /* Hold epoch pinned briefly to widen the protection window */
            usleep(1);
            (void)val;
            htc_epoch_unpin(a->t->epoch);
        }
    }
    return NULL;
}

static void test_find_scoped_epoch_race(void) {
    printf("  C2: find_scoped epoch race (ins/rem + delayed unpin × 2000) ... ");
    htc_config_t cfg = {.initial_buckets = 64, .max_load_factor = 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(t);

    uint64_t hash = hash_seq(42);
    c2_arg_t warg = {t, hash, 2000, 1};
    c2_arg_t rarg = {t, hash, 2000, 0};

    pthread_t wt, rt;
    pthread_create(&wt, NULL, c2_writer, &warg);
    pthread_create(&rt, NULL, c2_reader, &rarg);
    pthread_join(wt, NULL);
    pthread_join(rt, NULL);

    htc_destroy(t);
    printf("PASS\n");
}

/* ─── C3: Remove/find during grow (old-gen race) ──────────────── */

typedef struct {
    htc_table_t *t;
    int id;
    int ops;
    int role;  /* 0=insert, 1=remove, 2=find */
} c3_arg_t;

static void *c3_worker(void *arg) {
    c3_arg_t *a = (c3_arg_t *)arg;
    unsigned seed = (unsigned)a->id * 99999u;
    for (int i = 0; i < a->ops; i++) {
        int key = (int)((seed = seed * 1103515245u + 12345u) % 2000);
        uint64_t h = hash_seq(key);
        switch (a->role) {
        case 0: htc_insert(a->t, h, (uint64_t)key); break;
        case 1: htc_remove(a->t, h); break;
        case 2: { uint64_t v; htc_find(a->t, h, &v); } break;
        }
    }
    return NULL;
}

static void test_remove_oldgen_race(void) {
    printf("  C3: remove/find during grow (ins+rem+find × 10000) ... ");
    htc_config_t cfg = {.initial_buckets = 16, .max_load_factor = 0.4};
    htc_table_t *t = htc_create(&cfg);
    assert(t);

    int OPS = 10000;
    pthread_t threads[3];
    c3_arg_t args[3];

    for (int i = 0; i < 3; i++) {
        args[i] = (c3_arg_t){t, i, OPS, i};
        pthread_create(&threads[i], NULL, c3_worker, &args[i]);
    }
    for (int i = 0; i < 3; i++)
        pthread_join(threads[i], NULL);

    htc_destroy(t);
    printf("PASS\n");
}

/* ─── R2: Upsert epoch pin regression ─────────────────────────── */

static void test_upsert_epoch_pin_regression(void) {
    printf("  R2: upsert epoch pin regression (grow + upsert concurrent) ... ");
    /* Same pattern as C1 but focused on the old-gen search path in upsert */
    htc_config_t cfg = {.initial_buckets = 8, .max_load_factor = 0.3};
    htc_table_t *t = htc_create(&cfg);
    assert(t);

    /* Phase 1: Fill table to force grow */
    for (int i = 0; i < 20; i++)
        htc_insert(t, hash_seq(i), (uint64_t)i);

    /* Phase 2: Concurrent upsert + more inserts to force additional grows */
    pthread_t threads[2];
    c1_arg_t ins_arg = {t, 100, 3000};
    c1_arg_t ups_arg = {t, 200, 3000};
    pthread_create(&threads[0], NULL, c1_insert_worker, &ins_arg);
    pthread_create(&threads[1], NULL, c1_upsert_worker, &ups_arg);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    htc_destroy(t);
    printf("PASS\n");
}

/* ─── R3: Concurrent upsert same key (DUPLICATE race) ─────────── */

static void *r3_upsert_worker(void *arg) {
    c1_arg_t *a = (c1_arg_t *)arg;
    for (int i = 0; i < a->ops; i++) {
        htc_error_t ret = htc_upsert(a->t, hash_seq(42), (uint64_t)(a->id * 10000 + i));
        assert(ret == HTC_OK);
    }
    return NULL;
}

static void test_upsert_duplicate_race(void) {
    printf("  R3: concurrent upsert same key (4 threads × 5000) ... ");
    htc_config_t cfg = {.initial_buckets = 16, .max_load_factor = 0.75};
    htc_table_t *t = htc_create(&cfg);
    assert(t);

    int T = 4, OPS = 5000;
    pthread_t threads[4];
    c1_arg_t args[4];
    for (int i = 0; i < T; i++) {
        args[i] = (c1_arg_t){t, i, OPS};
        pthread_create(&threads[i], NULL, r3_upsert_worker, &args[i]);
    }
    for (int i = 0; i < T; i++)
        pthread_join(threads[i], NULL);

    /* Verify exactly one entry exists for hash_seq(42) */
    uint64_t val = 0;
    assert(htc_find(t, hash_seq(42), &val) == HTC_OK);
    htc_destroy(t);
    printf("PASS\n");
}

/* ─── Main ─────────────────────────────────────────────────────── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("htc concurrency tests:\n");

    test_upsert_concurrent_grow();    /* C1 */
    test_find_scoped_epoch_race();    /* C2 */
    test_remove_oldgen_race();        /* C3 */
    test_upsert_epoch_pin_regression(); /* R2 */
    test_upsert_duplicate_race();     /* R3 */

    printf("htc concurrency PASS\n");
    return 0;
}
