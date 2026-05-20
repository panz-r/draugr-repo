#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Test: insert items with controlled quotient collisions */
    printf("=== Controlled quotient collision test ===\n");

    cqf_filter_t *cf = cqf_filter_create(64, 10);
    assert(cf);
    printf("  slots=%lu rbits=%u\n", (unsigned long)cf->num_slots, cf->remainder_bits);

    /* Insert items with quotient 10, different remainders */
    uint64_t base_q = 10;
    int n = 5;
    for (int i = 0; i < n; i++) {
        uint64_t h = (base_q << 10) | (uint64_t)(i + 1);
        cqf_err_t e = cqf_filter_insert(cf, h);
        assert(e == CQF_OK);
        printf("  inserted q=%lu rem=%d -> count=%lu valid=%d\n",
               base_q, i + 1, (unsigned long)cf->count, cqf_filter_validate(cf));
    }
    printf("  all %d same-quotient items inserted\n", n);

    /* Insert items with quotient 20, different remainders */
    base_q = 20;
    for (int i = 0; i < n; i++) {
        uint64_t h = (base_q << 10) | (uint64_t)(i + 1);
        cqf_err_t e = cqf_filter_insert(cf, h);
        assert(e == CQF_OK);
        printf("  inserted q=%lu rem=%d -> count=%lu valid=%d\n",
               base_q, i + 1, (unsigned long)cf->count, cqf_filter_validate(cf));
    }
    printf("  all %d second-group items inserted\n", n);

    /* Now insert a duplicate into q=10's group (triggers C=1->C=2 expansion) */
    uint64_t h = (10ULL << 10) | 1ULL;
    cqf_err_t e = cqf_filter_insert(cf, h);
    printf("  duplicate insert result=%d count=%lu valid=%d\n",
           e, (unsigned long)cf->count, cqf_filter_validate(cf));
    assert(e == CQF_OK);

    /* Verify all items are present and the filter is valid */
    assert(cqf_filter_validate(cf));
    assert(cqf_filter_count_occurrences(cf, h) == 2);
    printf("  duplicate count verified\n");

    /* Verify remaining items in q=10 */
    for (int i = 2; i <= n; i++) {
        uint64_t h2 = (10ULL << 10) | (uint64_t)i;
        assert(cqf_filter_lookup(cf, h2));
        assert(cqf_filter_count_occurrences(cf, h2) == 1);
    }
    printf("  q=10 remaining items verified\n");

    /* Verify q=20 items */
    for (int i = 1; i <= n; i++) {
        uint64_t h2 = (20ULL << 10) | (uint64_t)i;
        assert(cqf_filter_lookup(cf, h2));
        assert(cqf_filter_count_occurrences(cf, h2) == 1);
    }
    printf("  q=20 items verified\n");

    cqf_filter_destroy(cf);
    printf("\n=== ALL PASSED ===\n");
    return 0;
}
