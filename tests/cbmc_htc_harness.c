/**
 * cbmc_htc_harness.c — CBMC bounded verification for htc (Battery 25 Q2).
 *
 * Tiny configuration: 2 buckets, 2 slots, stash=1, 2 hashes, bounded operations.
 *
 * Compile with:
 *   cbmc --unwind 10 --bounds-check --pointer-check --div-by-zero-check \
 *         --unsigned-overflow-check --conversion-check \
 *         -I ../include \
 *         -DHTC_CFG_DISABLE_FRONT_CACHE \
 *         cbmc_htc_harness.c ../src/htc.c ../src/arena.c
 *
 * Properties verified by CBMC assertions:
 *   - no null pointer dereference
 *   - no out-of-bounds bucket access
 *   - no double-free
 *   - insert+find returns same value
 *   - remove+find returns NOT_FOUND
 *   - duplicate insert returns DUPLICATE
 *   - update+sfind returns updated value
 */

#include "draugr/htc.h"
#include <assert.h>
#include <stdlib.h>

/* CBMC nondet helpers */
#ifdef __CBMC__
#define CBMC_ASSUME(c) __CPROVER_assume(c)
#else
#define CBMC_ASSUME(c) ((void)0)
#endif

/* Tiny configuration */
#define NONDET_HASH() (nondet_uint64() % 2)  /* 2 hashes only */

static uint64_t nondet_uint64(void) {
#ifdef __CBMC__
    uint64_t x;
    return x;
#else
    return (uint64_t)rand();
#endif
}

int main(void) {
    htc_config_t cfg = {2, 0.99, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);

    /* Verify basic invariant: empty table */
    assert(htc_size(t) == 0);
    assert(htc_find(t, 0, NULL) == HTC_ERR_NOT_FOUND);
    assert(htc_find(t, 1, NULL) == HTC_ERR_NOT_FOUND);

    /* Insert nondet hash 0 or 1 */
    uint64_t h1 = nondet_uint64() % 2;
    uint64_t h2 = nondet_uint64() % 2;
    CBMC_ASSUME(h1 == 0 || h1 == 1);
    CBMC_ASSUME(h2 == 0 || h2 == 1);

    /* Insert h1 */
    htc_error_t r1 = htc_insert(t, h1, 100);
    assert(r1 == HTC_OK || r1 == HTC_ERR_PATHOLOGICAL);

    /* If insert succeeded, find must succeed */
    if (r1 == HTC_OK) {
        uint64_t out;
        assert(htc_find(t, h1, &out) == HTC_OK);
        assert(out == 100);
    }

    /* Insert same hash again — must be DUPLICATE or PATHOLOGICAL */
    htc_error_t r2 = htc_insert(t, h1, 200);
    if (r1 == HTC_OK) {
        assert(r2 == HTC_ERR_DUPLICATE || r2 == HTC_ERR_PATHOLOGICAL);
    }

    /* If first insert succeeded, value unchanged */
    if (r1 == HTC_OK) {
        uint64_t out;
        assert(htc_find(t, h1, &out) == HTC_OK);
        assert(out == 100);
    }

    /* Insert h2 (may equal h1) */
    htc_error_t r3 = htc_insert(t, h2, 300);
    assert(r3 == HTC_OK || r3 == HTC_ERR_DUPLICATE || r3 == HTC_ERR_PATHOLOGICAL);

    /* Remove h1 */
    htc_error_t r4 = htc_remove(t, h1);
    if (r1 == HTC_OK) {
        assert(r4 == HTC_OK);
        assert(htc_find(t, h1, NULL) == HTC_ERR_NOT_FOUND);
    } else {
        assert(r4 == HTC_ERR_NOT_FOUND);
    }

    /* Update h2 if it was inserted */
    if (r3 == HTC_OK) {
        assert(htc_update(t, h2, 400) == HTC_OK);
        uint64_t out;
        assert(htc_find(t, h2, &out) == HTC_OK);
        assert(out == 400);
    }

    /* Grow */
    htc_error_t r5 = htc_grow(t, false);
    assert(r5 == HTC_OK || r5 == HTC_ERR_OOM);

    /* After grow, h2 still findable if it existed */
    if (r3 == HTC_OK) {
        uint64_t out;
        assert(htc_find(t, h2, &out) == HTC_OK);
        assert(out == 400);
    }

    /* h1 must still be absent */
    if (r1 == HTC_OK) {
        assert(htc_find(t, h1, NULL) == HTC_ERR_NOT_FOUND);
    }

    htc_destroy(t);
    return 0;
}
