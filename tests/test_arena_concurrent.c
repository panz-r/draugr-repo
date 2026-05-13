/**
 * test_arena_concurrent.c — Concurrent arena tests
 *
 * Tests:
 *   1. wbuf concurrent flush + add race (seal-before-snapshot fix)
 *   2. slab_set_alloc under multi-threaded contention
 */

#include "draugr/arena.h"
#include "draugr/arena_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <stdatomic.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while(0)

#define TEST_ASSERT_EQ(a, b, msg) TEST_ASSERT((a) == (b), msg)

/* ══════════════════════════════════════════════════════════════════
   Test 1: Concurrent wbuf flush + add
   Exercises the seal-before-snapshot fix in wbuf_flush.
   Verifies no crash or state corruption under concurrent access.
   ══════════════════════════════════════════════════════════════════ */

#define WBUF_NTHREADS 4
#define WBUF_ROUNDS 50

typedef struct {
    struct arena_write_buffer *wbuf;
    struct arena *arena;
    int freq;
    int sc;
    atomic_bool stop;
} wbuf_thread_ctx_t;

static void *wbuf_adder(void *arg) {
    wbuf_thread_ctx_t *ctx = arg;
    uint8_t key[8] = {0};
    uint8_t val[8] = {0};

    while (!atomic_load(&ctx->stop)) {
        wbuf_add(ctx->wbuf, key, sizeof(key), val, sizeof(val));
    }
    return NULL;
}

static void test_concurrent_wbuf_flush_add(void) {
    printf("Running: %s\n", __func__);

    for (int round = 0; round < WBUF_ROUNDS; round++) {
        struct arena *a = arena_create(0);
        TEST_ASSERT(a != NULL, "arena_create returned NULL");

        struct arena_write_buffer *wbuf = a->arenas[ARENA_FREQ_HOT][0].u.hot.wbuf;
        TEST_ASSERT(wbuf != NULL, "wbuf is NULL");

        wbuf_thread_ctx_t ctx = {
            .wbuf = wbuf,
            .arena = a,
            .freq = ARENA_FREQ_HOT,
            .sc = 0,
            .stop = ATOMIC_VAR_INIT(false),
        };

        pthread_t threads[WBUF_NTHREADS];
        for (int i = 0; i < WBUF_NTHREADS; i++) {
            pthread_create(&threads[i], NULL, wbuf_adder, &ctx);
        }

        /* Flush from main thread. Each round is a fresh arena/ring. */
        for (int i = 0; i < 50; i++) {
            wbuf_flush(a, ARENA_FREQ_HOT, 0);
        }

        atomic_store(&ctx.stop, true);
        for (int i = 0; i < WBUF_NTHREADS; i++) {
            pthread_join(threads[i], NULL);
        }

        /* Ring may be full (wbuf permanently sealed) — that's OK.
         * Verify: no crash, no corruption. */
        arena_destroy(a);
    }
}

/* ══════════════════════════════════════════════════════════════════
   Test 2: Concurrent slab_set_alloc under contention
   Multiple threads allocate from the same arena simultaneously.
   ══════════════════════════════════════════════════════════════════ */

#define SLAB_NTHREADS 8
#define SLAB_ALLOCS_PER_THREAD 200

typedef struct {
    struct arena *arena;
    atomic_int alloc_ok;
    void *ptrs[SLAB_ALLOCS_PER_THREAD];
} slab_thread_ctx_t;

static void *slab_allocator(void *arg) {
    slab_thread_ctx_t *ctx = arg;

    for (int i = 0; i < SLAB_ALLOCS_PER_THREAD; i++) {
        void *p = arena_alloc(ctx->arena, 16);
        if (p) {
            memset(p, (uint8_t)(i & 0xFF), 16);
            ctx->ptrs[i] = p;
            atomic_fetch_add(&ctx->alloc_ok, 1);
        } else {
            ctx->ptrs[i] = NULL;
        }
    }
    return NULL;
}

static void test_concurrent_slab_alloc(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(0);
    TEST_ASSERT(a != NULL, "arena_create returned NULL");

    slab_thread_ctx_t tctx[SLAB_NTHREADS];
    pthread_t threads[SLAB_NTHREADS];

    for (int t = 0; t < SLAB_NTHREADS; t++) {
        tctx[t].arena = a;
        atomic_store(&tctx[t].alloc_ok, 0);
        memset(tctx[t].ptrs, 0, sizeof(tctx[t].ptrs));
    }

    for (int t = 0; t < SLAB_NTHREADS; t++) {
        pthread_create(&threads[t], NULL, slab_allocator, &tctx[t]);
    }

    for (int t = 0; t < SLAB_NTHREADS; t++) {
        pthread_join(threads[t], NULL);
    }

    int total_ok = 0;
    for (int t = 0; t < SLAB_NTHREADS; t++) {
        total_ok += atomic_load(&tctx[t].alloc_ok);
    }
    TEST_ASSERT(total_ok > 0, "expected some successful allocations");

    /* Verify all allocated pointers are distinct */
    int n_ptrs = 0;
    void **all_ptrs = calloc((size_t)total_ok, sizeof(void *));
    TEST_ASSERT(all_ptrs != NULL, "calloc failed");

    for (int t = 0; t < SLAB_NTHREADS; t++) {
        for (int i = 0; i < SLAB_ALLOCS_PER_THREAD; i++) {
            if (tctx[t].ptrs[i]) {
                all_ptrs[n_ptrs++] = tctx[t].ptrs[i];
            }
        }
    }
    TEST_ASSERT_EQ(n_ptrs, total_ok, "pointer count mismatch");

    for (int i = 1; i < n_ptrs; i++) {
        void *key = all_ptrs[i];
        int j = i - 1;
        while (j >= 0 && all_ptrs[j] > key) {
            all_ptrs[j + 1] = all_ptrs[j];
            j--;
        }
        all_ptrs[j + 1] = key;
    }

    int dupes = 0;
    for (int i = 1; i < n_ptrs; i++) {
        if (all_ptrs[i] == all_ptrs[i - 1]) dupes++;
    }
    TEST_ASSERT_EQ(dupes, 0, "duplicate pointers found — slab corruption");
    free(all_ptrs);

    arena_destroy(a);
}

/* ══════════════════════════════════════════════════════════════════
   Main
   ══════════════════════════════════════════════════════════════════ */

int main(void) {
    printf("=== Arena Concurrent Tests ===\n\n");

    test_concurrent_wbuf_flush_add();
    test_concurrent_slab_alloc();

    printf("\n=== Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
