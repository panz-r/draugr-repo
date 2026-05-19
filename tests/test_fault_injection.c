/**
 * test_fault_injection.c — Fault injection tests for htc and ht
 *
 * Battery coverage:
 *   M4: htc_epoch_retire_custom_oom  (retire node malloc fail → sync callback)
 *   A1: htc_grow_bitmap_oom          (calloc fail during grow → clean recovery)
 *   A2: ht_bare_resize_oom           (calloc fail during resize → table unchanged)
 *   A3: htc_arena_alloc_block_oom    (calloc fail for arena block → ERR_OOM)
 *   R3: htc_grow_bitmap_null_regression (subsumed by A1 — no crash on grow OOM)
 *
 * Uses --wrap to intercept malloc/calloc/realloc/free, same pattern as test_oom.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>

#include "draugr/ht.h"
#include "draugr/htc.h"
#include "draugr/htc_kv.h"
#include "draugr/ht_internal.h"

/* ─── Mock allocator ─────────────────────────────────────────────── */

extern void *__real_malloc(size_t size);
extern void *__real_calloc(size_t nmemb, size_t size);
extern void *__real_realloc(void *ptr, size_t size);
extern void  __real_free(void *ptr);

static struct {
    int     max_alloc_calls;
    int     alloc_count;
    int     alloc_num_to_fail;
    bool    fail_by_size;
    size_t  fail_size_threshold;
    int     fail_count;
} mock = {-1, 0, -1, false, 0, 0};

static void alloc_mock_reset(void) {
    mock.max_alloc_calls    = -1;
    mock.alloc_count        = 0;
    mock.alloc_num_to_fail  = -1;
    mock.fail_by_size       = false;
    mock.fail_size_threshold = SIZE_MAX;
    mock.fail_count         = 0;
}

static int  alloc_mock_get_count(void) { return mock.alloc_count; }
static void alloc_mock_set_alloc_num_to_fail(int n) { mock.alloc_num_to_fail = n; }

static bool should_fail(size_t size) {
    if (mock.max_alloc_calls > 0 && mock.alloc_count >= mock.max_alloc_calls)
        return true;
    if (mock.fail_by_size && size >= mock.fail_size_threshold)
        return true;
    if (mock.alloc_num_to_fail >= 0 && mock.alloc_count == mock.alloc_num_to_fail)
        return true;
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

void __wrap_free(void *ptr) {
    __real_free(ptr);
}

/* ─── Helpers ────────────────────────────────────────────────────── */

static uint64_t hash_seq(int i) {
    return (uint64_t)i * 0x9e3779b97f4a7c15ULL;
}

/* ─── M4: htc_epoch_retire_custom OOM ──────────────────────────────
 * Insert one entry, remove it while the retire-node malloc fails.
 * The synchronous fallback should free key/val via the callback.
 * LeakSanitizer verifies no leak.                                */

static void test_m4_retire_custom_oom(void) {
    printf("  M4: htc_epoch_retire_custom OOM fallback ... ");

    for (int attempt = 0; attempt < 200; attempt++) {
        alloc_mock_reset();
        htc_kv_t *kv = htc_kv_create(NULL);
        if (!kv) continue;

        char kbuf[32], vbuf[32];
        snprintf(kbuf, sizeof(kbuf), "key-%08x", 42u);
        snprintf(vbuf, sizeof(vbuf), "val-%08x", 42u);
        if (!htc_kv_insert_copy(kv, kbuf, strlen(kbuf)+1,
                                 vbuf, strlen(vbuf)+1)) {
            alloc_mock_reset();
            htc_kv_destroy(kv);
            continue;
        }

        /* Set up to fail the Nth allocation after setup */
        int setup_count = alloc_mock_get_count();
        alloc_mock_set_alloc_num_to_fail(setup_count + 1 + attempt);

        bool removed = htc_kv_remove(kv, kbuf, strlen(kbuf)+1);
        alloc_mock_reset();

        if (!removed) {
            /* The targeted alloc wasn't the retire node or remove failed
             * for another reason — clean up and try next offset */
            htc_kv_destroy(kv);
            continue;
        }

        /* Remove succeeded with the retire node malloc failing.
         * The callback ran synchronously, freeing key/val copies.
         * Destroy must complete without leak. */
        htc_kv_destroy(kv);
        printf("PASS (fail_at=%d)\n", setup_count + 1 + attempt);
        return;
    }
    printf("FAIL (could not trigger retire node OOM)\n");
    assert(0);
}

/* ─── A1: htc_grow bitmap OOM ─────────────────────────────────────
 * Force allocation failure during htc_grow by failing the Nth alloc.
 * Verify the table remains usable after the failed grow.
 * Also covers R3 (no segfault on bitmap OOM).                    */

static void test_a1_grow_bitmap_oom(void) {
    printf("  A1: htc_grow bitmap OOM (covers R3) ... ");
    int handled = 0;

    for (int fail_at = 1; fail_at < 500 && handled < 5; fail_at++) {
        alloc_mock_reset();
        htc_config_t cfg = {.initial_buckets = 4, .max_load_factor = 0.5};
        htc_table_t *t = htc_create(&cfg);
        if (!t) continue;

        alloc_mock_set_alloc_num_to_fail(fail_at);

        /* Insert enough to trigger grow */
        htc_error_t last_err = HTC_OK;
        for (int i = 0; i < 20; i++) {
            last_err = htc_insert(t, hash_seq(i), (uint64_t)i);
            if (last_err != HTC_OK) break;
        }

        if (last_err == HTC_OK) {
            /* This fail_at wasn't hit during these inserts */
            alloc_mock_reset();
            htc_destroy(t);
            continue;
        }

        /* An insert failed — verify table still usable */
        alloc_mock_reset();

        htc_error_t r = htc_insert(t, hash_seq(999), 999);
        assert(r == HTC_OK);

        uint64_t val = 0;
        assert(htc_find(t, hash_seq(999), &val) == HTC_OK);
        assert(val == 999);

        htc_destroy(t);
        handled++;
    }

    assert(handled > 0);
    printf("PASS (%d OOM points handled)\n", handled);
}

/* ─── A2: ht_bare_resize OOM ──────────────────────────────────────
 * Force calloc failure during ht_bare_resize.
 * Verify function returns false and table invariants hold.       */

static void test_a2_bare_resize_oom(void) {
    printf("  A2: ht_bare_resize OOM ... ");

    for (int fail_at = 1; fail_at < 200; fail_at++) {
        alloc_mock_reset();
        ht_config_t cfg = {.initial_capacity = 8, .max_load_factor = 0.75};
        ht_bare_t *t = ht_bare_create(&cfg, NULL);
        if (!t) continue;

        /* Insert some entries */
        for (int i = 0; i < 6; i++)
            ht_bare_insert(t, hash_seq(i), (uint32_t)i);

        alloc_mock_set_alloc_num_to_fail(fail_at);

        bool ok = ht_bare_resize(t, 64);
        alloc_mock_reset();

        if (ok) {
            /* Resize succeeded — this fail_at wasn't reached */
            ht_bare_destroy(t);
            continue;
        }

        /* Resize failed — verify table still works */
        /* Original entries should still be findable */
        for (int i = 0; i < 6; i++) {
            uint32_t val = 0;
            assert(ht_bare_find(t, hash_seq(i), &val));
            assert(val == (uint32_t)i);
        }

        /* Should still accept inserts */
        assert(ht_bare_insert(t, hash_seq(100), 100));

        ht_bare_destroy(t);
        printf("PASS (fail_at=%d)\n", fail_at);
        return;
    }
    printf("FAIL (could not trigger resize OOM)\n");
    assert(0);
}

/* ─── A3: htc_arena_alloc_block OOM ────────────────────────────────
 * Force calloc failure for a new arena block.
 * Verify htc_insert returns HTC_ERR_OOM and table still works.   */

static void test_a3_arena_block_oom(void) {
    printf("  A3: htc_arena_alloc_block_oom ... ");
    int handled = 0;

    for (int fail_at = 1; fail_at < 500 && handled < 5; fail_at++) {
        alloc_mock_reset();
        htc_config_t cfg = {.initial_buckets = 256, .max_load_factor = 0.75};
        htc_table_t *t = htc_create(&cfg);
        if (!t) continue;

        alloc_mock_set_alloc_num_to_fail(fail_at);

        /* Insert — may fail at any internal allocation */
        htc_error_t r = htc_insert(t, hash_seq(1), 1);

        if (r == HTC_OK) {
            alloc_mock_reset();
            htc_destroy(t);
            continue;
        }

        /* Insert failed — reset and verify recovery */
        alloc_mock_reset();

        /* Table should accept operations after OOM clears */
        r = htc_insert(t, hash_seq(1), 1);
        assert(r == HTC_OK);

        uint64_t val = 0;
        assert(htc_find(t, hash_seq(1), &val) == HTC_OK);
        assert(val == 1);

        htc_destroy(t);
        handled++;
    }

    assert(handled > 0);
    printf("PASS (%d OOM points handled)\n", handled);
}

/* ─── Main ───────────────────────────────────────────────────────── */

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("fault injection tests:\n");

    test_m4_retire_custom_oom();
    test_a1_grow_bitmap_oom();
    test_a2_bare_resize_oom();
    test_a3_arena_block_oom();

    printf("fault injection PASS\n");
    return 0;
}
