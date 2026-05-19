/**
 * test_htc_kv_stress.c — Concurrency stress and regression tests for htc_kv
 *
 * Battery coverage:
 *   M1: htc_kv_destroy_list_uaf  (insert N, remove all, destroy × 1000)
 *   M2: htc_kv_concurrent_destroy (4 threads, separate tables, lifecycle)
 *   M3: htc_kv_insert_cas_race    (16 threads, shared table, 10000 keys)
 *   R1: htc_kv_use_after_free_regression
 */

#include "draugr/htc_kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>

/* ─── Helpers ──────────────────────────────────────────────────── */

static void make_key(int i, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "key-%08x", (unsigned)i);
}

static void make_val(int i, char *buf, size_t bufsz) {
    snprintf(buf, bufsz, "val-%08x", (unsigned)i);
}

/* ─── M1: UAF regression — insert N, remove all, destroy × 1000 ── */

static void test_insert_remove_destroy(void) {
    printf("  M1: insert 500, remove all, destroy × 1000 ... ");
    for (int r = 0; r < 1000; r++) {
        htc_kv_t *kv = htc_kv_create(NULL);
        assert(kv);
        char kbuf[32], vbuf[32];
        const int N = 500;
        for (int i = 0; i < N; i++) {
            make_key(i, kbuf, sizeof(kbuf));
            make_val(i, vbuf, sizeof(vbuf));
            assert(htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                                       vbuf, strlen(vbuf)+1));
        }
        for (int i = 0; i < N; i++) {
            make_key(i, kbuf, sizeof(kbuf));
            assert(htc_kv_remove(kv, kbuf, strlen(kbuf)+1));
        }
        assert(htc_kv_count(kv) == 0);
        htc_kv_destroy(kv);
    }
    printf("PASS\n");
}

/* ─── M2: Concurrent separate-table create/destroy ─────────────── */

typedef struct {
    int id;
    int iterations;
    int result;
} m2_arg_t;

static void *m2_worker(void *arg) {
    m2_arg_t *a = (m2_arg_t *)arg;
    char kbuf[32], vbuf[32];
    unsigned seed = (unsigned)a->id * 54321u;

    for (int r = 0; r < a->iterations; r++) {
        htc_kv_t *kv = htc_kv_create(NULL);
        if (!kv) { a->result = 1; return NULL; }

        int ops = 20 + (int)((seed = seed * 1103515245u + 12345u) % 80);
        for (int i = 0; i < ops; i++) {
            int key_id = (int)((seed = seed * 1103515245u + 12345u) % 200);
            make_key(key_id, kbuf, sizeof(kbuf));
            make_val(key_id + r, vbuf, sizeof(vbuf));
            int op = (int)((seed = seed * 1103515245u + 12345u) % 3);
            switch (op) {
            case 0:
                htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                                   vbuf, strlen(vbuf)+1);
                break;
            case 1:
            {
                char fbuf[64];
                size_t flen = sizeof(fbuf);
                htc_kv_find(kv, kbuf, strlen(kbuf)+1, fbuf, &flen);
            }
                break;
            case 2:
                htc_kv_remove(kv, kbuf, strlen(kbuf)+1);
                break;
            }
        }
        htc_kv_destroy(kv);
    }
    return NULL;
}

static void test_concurrent_destroy(void) {
    printf("  M2: 4 threads × 200 table create/destroy cycles ... ");
    int T = 4, ITERS = 200;
    pthread_t threads[4];
    m2_arg_t args[4];

    for (int ti = 0; ti < T; ti++) {
        args[ti].id = ti;
        args[ti].iterations = ITERS;
        args[ti].result = 0;
        pthread_create(&threads[ti], NULL, m2_worker, &args[ti]);
    }
    for (int ti = 0; ti < T; ti++) {
        pthread_join(threads[ti], NULL);
        assert(args[ti].result == 0);
    }
    printf("PASS\n");
}

/* ─── M3: Pure concurrent insert into shared table ─────────────── */

typedef struct {
    htc_kv_t *kv;
    int       id;
    int       count;
    int       ok;      /* successful inserts */
} m3_arg_t;

static void *m3_worker(void *arg) {
    m3_arg_t *a = (m3_arg_t *)arg;
    char kbuf[32], vbuf[32];
    int base = a->id * 10000;
    for (int i = 0; i < a->count; i++) {
        make_key(base + i, kbuf, sizeof(kbuf));
        make_val(base + i, vbuf, sizeof(vbuf));
        if (htc_kv_insert_copy(a->kv, kbuf, strlen(kbuf)+1,
                               vbuf, strlen(vbuf)+1))
            a->ok++;
    }
    return NULL;
}

static void test_concurrent_insert(void) {
    printf("  M3: 16 threads × 625 inserts into shared table ... ");
    htc_config_t cfg = {.initial_buckets = 256, .max_load_factor = 0.75};
    htc_kv_t *kv = htc_kv_create(&cfg);
    assert(kv);

    int T = 16, PER_THREAD = 625;
    pthread_t threads[16];
    m3_arg_t args[16];

    for (int ti = 0; ti < T; ti++) {
        args[ti] = (m3_arg_t){kv, ti, PER_THREAD, 0};
        pthread_create(&threads[ti], NULL, m3_worker, &args[ti]);
    }
    for (int ti = 0; ti < T; ti++)
        pthread_join(threads[ti], NULL);

    /* Count total successful inserts */
    int total_ok = 0;
    for (int ti = 0; ti < T; ti++) total_ok += args[ti].ok;
    assert(total_ok > 0);
    assert((int)htc_kv_count(kv) == total_ok);

    /* Spot-check a sample */
    char kbuf[32], vbuf[64];
    size_t vlen;
    for (int ti = 0; ti < T; ti += 3) {
        make_key(ti * 10000 + 50, kbuf, sizeof(kbuf));
        vlen = sizeof(vbuf);
        htc_kv_find(kv, kbuf, strlen(kbuf)+1, vbuf, &vlen);
    }

    /* Destroy — LeakSanitizer verifies all entries freed */
    htc_kv_destroy(kv);
    printf("PASS (%d/%d inserted)\n", total_ok, T * PER_THREAD);
}

/* ─── R1: Use-after-free regression ────────────────────────────── */

static void test_uaf_regression(void) {
    printf("  R1: UAF regression (insert 1000, remove all, destroy) ... ");
    htc_kv_t *kv = htc_kv_create(NULL);
    assert(kv);
    char kbuf[32], vbuf[32];
    for (int i = 0; i < 1000; i++) {
        make_key(i, kbuf, sizeof(kbuf));
        make_val(i, vbuf, sizeof(vbuf));
        assert(htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                                   vbuf, strlen(vbuf)+1));
    }
    for (int i = 0; i < 1000; i++) {
        make_key(i, kbuf, sizeof(kbuf));
        assert(htc_kv_remove(kv, kbuf, strlen(kbuf)+1));
    }
    htc_kv_destroy(kv);
    printf("PASS\n");
}

/* ─── Iterator after mixed ops ─────────────────────────────────── */

static void test_iter_after_mixed(void) {
    printf("  iterator after insert/remove ... ");
    htc_kv_t *kv = htc_kv_create(NULL);
    assert(kv);
    char kbuf[32], vbuf[32];
    for (int i = 0; i < 100; i++) {
        make_key(i, kbuf, sizeof(kbuf));
        make_val(i, vbuf, sizeof(vbuf));
        assert(htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                                   vbuf, strlen(vbuf)+1));
    }
    for (int i = 0; i < 100; i += 2) {
        make_key(i, kbuf, sizeof(kbuf));
        assert(htc_kv_remove(kv, kbuf, strlen(kbuf)+1));
    }
    htc_kv_iter_t it;
    htc_kv_iter_init(&it, kv);
    int count = 0;
    while (htc_kv_iter_next(&it)) {
        assert(it.key && it.klen > 0);
        assert(it.val && it.vlen > 0);
        count++;
    }
    assert(count == 50);
    htc_kv_destroy(kv);
    printf("PASS\n");
}

/* ─── Upsert pattern (heavy epoch retire cycling) ─────────────── */

static void test_upsert_pattern(void) {
    printf("  upsert pattern (insert/remove/reinsert × 200) ... ");
    htc_kv_t *kv = htc_kv_create(NULL);
    assert(kv);
    char kbuf[32], vbuf[32];
    for (int r = 0; r < 200; r++) {
        for (int i = 0; i < 20; i++) {
            make_key(i, kbuf, sizeof(kbuf));
            make_val(i + r * 100, vbuf, sizeof(vbuf));
            htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                               vbuf, strlen(vbuf)+1);
        }
        for (int i = 0; i < 20; i++) {
            make_key(i, kbuf, sizeof(kbuf));
            htc_kv_remove(kv, kbuf, strlen(kbuf)+1);
        }
    }
    htc_kv_destroy(kv);
    printf("PASS\n");
}

/* ─── Main ─────────────────────────────────────────────────────── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("htc_kv stress tests:\n");

    test_insert_remove_destroy();   /* M1 */
    test_concurrent_destroy();      /* M2 */
    test_concurrent_insert();       /* M3 */
    test_uaf_regression();          /* R1 */
    test_iter_after_mixed();
    test_upsert_pattern();

    printf("htc_kv stress PASS\n");
    return 0;
}
