#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Test 1: C=1→C=2, rem != 0, simple case */
    printf("=== C=1→C=2, rem != 0 ===\n");
    cqf_filter_t *cf = cqf_filter_create(64, 10);
    assert(cf);
    printf("  slots=%lu rbits=%u\n", (unsigned long)cf->num_slots, cf->remainder_bits);
    assert(cqf_filter_validate(cf));

    /* Pick hash that maps to quotient 0 with rem=1 */
    uint64_t h = (0ULL << 10) | 1ULL;  /* q=0, rem=1 */
    cqf_err_t e = cqf_filter_insert(cf, h);
    assert(e == CQF_OK);
    printf("  first insert OK\n");
    assert(cqf_filter_validate(cf));

    /* Insert same hash again → C=1→C=2 expansion */
    e = cqf_filter_insert(cf, h);
    printf("  second insert result: %d\n", e);
    assert(e == CQF_OK);
    printf("  second insert OK\n");
    assert(cqf_filter_validate(cf));

    assert(cqf_filter_count_occurrences(cf, h) == 2);
    printf("  count=2 OK\n");
    cqf_filter_destroy(cf);

    /* Test 2: C=1→C=2, rem == 0, simple case */
    printf("\n=== C=1→C=2, rem == 0 ===\n");
    cf = cqf_filter_create(64, 10);
    assert(cf);
    h = (0ULL << 10) | 0ULL;  /* q=0, rem=0 */
    e = cqf_filter_insert(cf, h);
    assert(e == CQF_OK);
    printf("  first insert OK\n");
    assert(cqf_filter_validate(cf));

    e = cqf_filter_insert(cf, h);
    printf("  second insert result: %d\n", e);
    assert(e == CQF_OK);
    printf("  second insert OK\n");
    assert(cqf_filter_validate(cf));

    assert(cqf_filter_count_occurrences(cf, h) == 2);
    printf("  count=2 OK\n");
    cqf_filter_destroy(cf);

    /* Test 3: C=1→C=2 with non-first quotient, entries after */
    printf("\n=== C=1→C=2, entries after ===\n");
    cf = cqf_filter_create(64, 10);
    assert(cf);

    /* Insert at q=1, to set up a non-trivial run */
    h = (1ULL << 10) | 5ULL;  /* q=1, rem=5 */
    e = cqf_filter_insert(cf, h);
    assert(e == CQF_OK);

    h = (1ULL << 10) | 7ULL;  /* q=1, rem=7 — different rem, same quotient, creates run */
    e = cqf_filter_insert(cf, h);
    assert(e == CQF_OK);
    assert(cqf_filter_validate(cf));

    /* Now insert duplicate of rem=5 → C=1→C=2 with data after */
    h = (1ULL << 10) | 5ULL;
    e = cqf_filter_insert(cf, h);
    printf("  duplicate insert result: %d\n", e);
    assert(e == CQF_OK);
    printf("  duplicate insert OK\n");
    assert(cqf_filter_validate(cf));

    assert(cqf_filter_count_occurrences(cf, h) == 2);
    printf("  count=2 OK\n");

    /* Verify both remainders are present */
    assert(cqf_filter_count_occurrences(cf, (1ULL << 10) | 7ULL) == 1);
    printf("  other entry still present OK\n");
    cqf_filter_destroy(cf);

    /* Test 4: Many duplicates to exercise C=2→C=3+ expansion */
    printf("\n=== C=2→C=3, many duplicates ===\n");
    cf = cqf_filter_create(64, 10);
    assert(cf);

    h = (2ULL << 10) | 3ULL;  /* q=2, rem=3 */
    for (int i = 0; i < 10; i++) {
        e = cqf_filter_insert(cf, h);
        if (e != CQF_OK) {
            printf("  FAIL at i=%d, error=%d\n", i, e);
            break;
        }
        if (i % 3 == 0) assert(cqf_filter_validate(cf));
    }
    assert(e == CQF_OK);
    assert(cqf_filter_count_occurrences(cf, h) == 10);
    printf("  10 duplicates OK\n");
    assert(cqf_filter_validate(cf));
    cqf_filter_destroy(cf);

    /* Test 5: rem=0 many duplicates */
    printf("\n=== C=1→C=2→..., rem=0, many duplicates ===\n");
    cf = cqf_filter_create(64, 10);
    assert(cf);

    h = (3ULL << 10) | 0ULL;  /* q=3, rem=0 */
    for (int i = 0; i < 10; i++) {
        e = cqf_filter_insert(cf, h);
        if (e != CQF_OK) {
            printf("  FAIL at i=%d, error=%d\n", i, e);
            break;
        }
        if (i % 3 == 0) assert(cqf_filter_validate(cf));
    }
    assert(e == CQF_OK);
    assert(cqf_filter_count_occurrences(cf, h) == 10);
    printf("  10 duplicates (rem=0) OK\n");
    assert(cqf_filter_validate(cf));
    cqf_filter_destroy(cf);

    printf("\n=== ALL TESTS PASSED ===\n");
    return 0;
}
