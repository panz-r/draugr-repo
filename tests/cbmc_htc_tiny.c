/**
 * cbmc_htc_tiny.c — Minimal CBMC harness for bounded verification.
 *
 * Verifies only the most basic properties on a tiny config.
 * Configuration: 2 buckets, 8 slots, 1 hash, bounded operations.
 *
 * CBMC command:
 *   cbmc -D__CBMC__ -DDRAUGR_USE_MALLOC --unwind 5 --bounds-check \
 *         -I ../include cbmc_htc_tiny.c ../src/htc.c --function main
 */

#include "draugr/htc.h"
#include "draugr/htc_internal.h"
#include <assert.h>

int main(void) {
    htc_config_t cfg = {2, 0.99, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);
    assert(t != NULL);

    /* Property 1: empty table */
    assert(htc_size(t) == 0);
    assert(htc_find(t, 0, NULL) == HTC_ERR_NOT_FOUND);

    /* Property 2: insert succeeds */
    assert(htc_insert(t, 42, 100) == HTC_OK);
    assert(htc_size(t) == 1);

    /* Property 3: find returns inserted value */
    {
        uint64_t out;
        assert(htc_find(t, 42, &out) == HTC_OK);
        assert(out == 100);
    }

    /* Property 4: duplicate returns error */
    assert(htc_insert(t, 42, 200) == HTC_ERR_DUPLICATE);
    assert(htc_size(t) == 1);

    /* Property 5: find still returns original */
    {
        uint64_t out;
        assert(htc_find(t, 42, &out) == HTC_OK);
        assert(out == 100);
    }

    /* Property 6: remove succeeds */
    assert(htc_remove(t, 42) == HTC_OK);
    assert(htc_size(t) == 0);

    /* Property 7: find returns not found after remove */
    assert(htc_find(t, 42, NULL) == HTC_ERR_NOT_FOUND);

    /* Property 8: remove missing returns error */
    assert(htc_remove(t, 42) == HTC_ERR_NOT_FOUND);

    /* Property 9: update missing returns error */
    assert(htc_update(t, 42, 300) == HTC_ERR_NOT_FOUND);

    /* Property 10: reinsert and grow */
    assert(htc_insert(t, 42, 400) == HTC_OK);
    {
        uint64_t out;
        assert(htc_find(t, 42, &out) == HTC_OK);
        assert(out == 400);
    }

    /* Property 11: grow preserves data */
    assert(htc_grow(t, false) == HTC_OK);
    {
        uint64_t out;
        assert(htc_find(t, 42, &out) == HTC_OK);
        assert(out == 400);
    }

    htc_destroy(t);
    return 0;
}
