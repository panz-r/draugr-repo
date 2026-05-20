#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

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
    cqf_filter_t *cf = cqf_filter_create(1024, 10);
    assert(cf);
    
    uint64_t *occ = (uint64_t *)(cf + 1);
    uint64_t *run = occ + cf->num_blocks;
    
    const char *keys[] = {"hello", "world", "hello", "test", "hello", "world"};
    int n = sizeof(keys) / sizeof(keys[0]);
    for (int i = 0; i < n; i++) {
        uint64_t h = hash64(keys[i]);
        printf("Inserting key=%s hash=0x%lx q=%lu rem=%lu\n", 
               keys[i], h, (h >> 10) & (cf->num_slots-1), h & cf->remainder_mask);
        cqf_err_t e = cqf_filter_insert(cf, h);
        printf("  result=%d count=%lu distinct=%lu\n", e, (unsigned long)cf->count, (unsigned long)cf->distinct_count);
        
        uint64_t occ_pop = 0, run_pop = 0;
        for (uint64_t b = 0; b < cf->num_blocks; b++) {
            occ_pop += __builtin_popcountll(occ[b]);
            run_pop += __builtin_popcountll(run[b]);
        }
        printf("  occ_pop=%lu run_pop=%lu\n", (unsigned long)occ_pop, (unsigned long)run_pop);
        
        if (!cqf_filter_validate(cf)) {
            printf("  VALIDATION FAILED after insertion %d\n", i);
            for (uint64_t q = 0; q < 64; q++) {
                int o = (occ[q/64]>>(q%64))&1;
                int r = (run[q/64]>>(q%64))&1;
                if (o || r) printf("  [%lu] occ=%d run=%d\n", q, o, r);
            }
            break;
        }
    }
    
    cqf_filter_destroy(cf);
    return 0;
}
