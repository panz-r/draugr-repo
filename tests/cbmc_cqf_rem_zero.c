/**
 * cbmc_cqf_rem_zero.c — CBMC harness for CQF rem=0 correctness
 *
 * Verifies for a small (4-slot) filter:
 *   1. rem=0 insert does not corrupt metadata
 *   2. rem=0 count returns the inserted count
 *   3. rem=0 delete preserves remaining entries
 *   4. rem=0 entries survive alongside rem!=0 entries
 *
 * Run: cbmc -D__CBMC__ --unwind 8 --bounds-check --pointer-check \
 *      tests/cbmc_cqf_rem_zero.c --function main
 */
#ifdef __CBMC__
#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Include the full source to access static functions and struct layout */
#include "../src/cqf_filter.c"

void main(void)
{
    /* Bounded small filter: 4 slots, 2 remainder bits */
    cqf_filter_t *cf = cqf_filter_create(4, 2);
    __CPROVER_assume(cf != NULL);

    /* Property 1: insert rem=0, verify it's present */
    uint64_t fp0 = make_fp(0, 0, cf->remainder_bits);
    cqf_err_t e = cqf_filter_insert(cf, fp0);
    __CPROVER_assume(e == CQF_OK);
    __CPROVER_assert(cqf_filter_validate(cf), "rem=0 insert: filter valid");
    __CPROVER_assert(cqf_filter_lookup(cf, fp0),
        "rem=0 insert: lookup returns true");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp0) == 1,
        "rem=0 insert: count == 1");
    __CPROVER_assert(cf->distinct_count == 1,
        "rem=0 insert: distinct_count == 1");

    /* Property 2: duplicate rem=0 insert increments counter */
    e = cqf_filter_insert(cf, fp0);
    __CPROVER_assume(e == CQF_OK);
    __CPROVER_assert(cqf_filter_validate(cf), "rem=0 dup: filter valid");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp0) == 2,
        "rem=0 dup: count == 2");

    /* Property 3: insert a non-zero remainder alongside rem=0 */
    uint64_t fp1 = make_fp(0, 1, cf->remainder_bits);
    e = cqf_filter_insert(cf, fp1);
    __CPROVER_assume(e == CQF_OK);
    __CPROVER_assert(cqf_filter_validate(cf), "rem=0 + rem=1: filter valid");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp0) == 2,
        "rem=0 + rem=1: rem=0 count unchanged");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp1) == 1,
        "rem=0 + rem=1: rem=1 count == 1");

    /* Property 4: delete rem=0, verify rem=1 survives */
    e = cqf_filter_delete(cf, fp0);
    __CPROVER_assume(e == CQF_OK);
    __CPROVER_assert(cqf_filter_validate(cf),
        "rem=0 delete: filter valid");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp0) == 1,
        "rem=0 delete: rem=0 count == 1");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp1) == 1,
        "rem=0 delete: rem=1 preserved");

    /* Property 5: delete rem=0 again, verify rem=0 gone */
    e = cqf_filter_delete(cf, fp0);
    __CPROVER_assume(e == CQF_OK);
    __CPROVER_assert(cqf_filter_validate(cf),
        "rem=0 delete all: filter valid");
    __CPROVER_assert(!cqf_filter_lookup(cf, fp0),
        "rem=0 delete all: lookup returns false");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp0) == 0,
        "rem=0 delete all: count == 0");
    __CPROVER_assert(cqf_filter_count_occurrences(cf, fp1) == 1,
        "rem=0 delete all: rem=1 preserved");

    cqf_filter_destroy(cf);
}
#endif
