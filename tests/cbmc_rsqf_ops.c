/**
 * cbmc_rsqf_ops.c — CBMC harness for RSQF insert/lookup correctness
 *
 * For a tiny filter (capacity=64, 1 block), nondeterministically
 * insert hashes and verify lookup succeeds.
 *
 * CBMC: cbmc --unwind 8 --bounds-check --pointer-check \
 *             cbmc_rsqf_ops.c --function main
 */

#ifdef __CBMC__
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal struct matching rsqf_filter_t (same as real header) */
typedef struct {
    uint64_t num_slots;
    uint64_t num_blocks;
    uint64_t count;
    uint8_t  remainder_bits;
    uint8_t  quotient_bits;
    uint16_t entries_per_set;
    uint16_t num_sets;
    uint64_t remainder_mask;
} rsqf_filter_t;

/* Forward declarations */
rsqf_filter_t *rsqf_filter_create(size_t capacity, uint8_t fp_bits);
void rsqf_filter_destroy(rsqf_filter_t *rf);
int rsqf_filter_insert(rsqf_filter_t *rf, uint64_t hash);
int rsqf_filter_lookup(const rsqf_filter_t *rf, uint64_t hash);
int rsqf_filter_delete(rsqf_filter_t *rf, uint64_t hash);
int rsqf_filter_validate(const rsqf_filter_t *rf);

void main(void) {
    rsqf_filter_t *rf = rsqf_filter_create(64, 8);
    
    if (!rf) return;
    
    /* Nondeterministic sequence of up to 5 insert/lookup/delete */
    uint64_t h1, h2, h3;
    
    /* Insert 3 distinct hashes */
    h1 = nondet_uint64();
    __CPROVER_assume(h1 != 0);
    __CPROVER_assume((h1 & 0xFF) != 0); /* non-zero remainder */
    int ok1 = rsqf_filter_insert(rf, h1);
    if (ok1 == 0) { /* RSQF_OK */
        __CPROVER_assert(rsqf_filter_lookup(rf, h1) == 1, "insert1 → lookup true");
        __CPROVER_assert(rsqf_filter_validate(rf), "validate after insert1");
    }
    
    h2 = nondet_uint64();
    __CPROVER_assume(h2 != h1);
    __CPROVER_assume((h2 & 0xFF) != 0);
    int ok2 = rsqf_filter_insert(rf, h2);
    if (ok2 == 0) {
        __CPROVER_assert(rsqf_filter_lookup(rf, h2) == 1, "insert2 → lookup true");
        if (ok1 == 0) {
            __CPROVER_assert(rsqf_filter_lookup(rf, h1) == 1, "h1 still present");
        }
        __CPROVER_assert(rsqf_filter_validate(rf), "validate after insert2");
    }
    
    h3 = nondet_uint64();
    __CPROVER_assume(h3 != h1 && h3 != h2);
    __CPROVER_assume((h3 & 0xFF) != 0);
    int ok3 = rsqf_filter_insert(rf, h3);
    if (ok3 == 0) {
        __CPROVER_assert(rsqf_filter_lookup(rf, h3) == 1, "insert3 → lookup true");
        if (ok1 == 0) {
            __CPROVER_assert(rsqf_filter_lookup(rf, h1) == 1, "h1 still present");
        }
        __CPROVER_assert(rsqf_filter_validate(rf), "validate after insert3");
    }
    
    /* Delete h1 (if inserted) */
    if (ok1 == 0) {
        int del_ok = rsqf_filter_delete(rf, h1);
        if (del_ok == 0) {
            /* If h3 inserted, it must still be findable */
            if (ok3 == 0) {
                __CPROVER_assert(rsqf_filter_lookup(rf, h3) == 1, "h3 still present after delete");
            }
            __CPROVER_assert(rsqf_filter_validate(rf), "validate after delete");
        }
    }
    
    rsqf_filter_destroy(rf);
}
#else
int main(void) { return 0; }
#endif
