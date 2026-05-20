#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

static uint64_t hash64(const char *s) {
    uint64_t h = 0x100029a3c63a1a4bULL;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 0x100000001b3ULL;
        s++;
    }
    return h;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== Merge hang reproduction ===\n");

    cqf_filter_t *cf1 = cqf_filter_create(256, 10);
    cqf_filter_t *cf2 = cqf_filter_create(256, 10);
    assert(cf1 && cf2);
    printf("  created filters\n");

    for (int i = 0; i < 50; i++) {
        uint64_t h = hash64("a") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        cqf_err_t e = cqf_filter_insert(cf1, h);
        assert(e == CQF_OK);
        assert(cqf_filter_validate(cf1));
        e = cqf_filter_insert(cf2, h + 1);
        assert(e == CQF_OK);
        assert(cqf_filter_validate(cf2));
    }
    printf("  inserted 50 items each\n");
    printf("  cf1: count=%lu, distinct=%lu\n", (unsigned long)cf1->count, (unsigned long)cf1->distinct_count);
    printf("  cf2: count=%lu, distinct=%lu\n", (unsigned long)cf2->count, (unsigned long)cf2->distinct_count);

    printf("  merging...\n");
    cqf_filter_t *merged = cqf_filter_merge(cf1, cf2);
    printf("  merge returned: %p\n", (void*)merged);
    assert(merged != NULL);
    printf("  merged count=%lu\n", (unsigned long)merged->count);

    for (int i = 0; i < 50; i++) {
        uint64_t h1 = hash64("a") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        uint64_t h2 = h1 + 1;
        if (!cqf_filter_lookup(merged, h1)) {
            printf("  MISSING h1 at i=%d\n", i);
            assert(0);
        }
        if (!cqf_filter_lookup(merged, h2)) {
            printf("  MISSING h2 at i=%d\n", i);
            assert(0);
        }
    }
    printf("  merge verified\n");

    cqf_filter_destroy(cf1);
    cqf_filter_destroy(cf2);
    cqf_filter_destroy(merged);

    printf("\n=== ALL PASSED ===\n");
    return 0;
}
