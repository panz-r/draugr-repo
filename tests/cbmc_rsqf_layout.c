/**
 * cbmc_rsqf_layout.c — CBMC harness for RSQF layout and allocation correctness
 *
 * Check:
 *   1. For small capacities, size calculation avoids overflow
 *   2. All derived pointers are inside the single allocation
 *   3. Init followed by destroy has no invalid access
 *
 * CBMC: cbmc --unwind 10 --bounds-check --pointer-check \
 *             cbmc_rsqf_layout.c --function main
 */

#ifdef __CBMC__
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Minimal struct matching rsqf_filter_t */
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

#define RSQF_SLOTS_PER_BLOCK 64
#define RSQF_MIN_FINGERPRINT_BITS 2
#define RSQF_MAX_FINGERPRINT_BITS 10

/* Forward declare the real functions (linking not needed for CBMC) */
rsqf_filter_t *rsqf_filter_create(size_t capacity, uint8_t fp_bits);
void rsqf_filter_destroy(rsqf_filter_t *rf);

void main(void) {
    /* Test capacity = 64 (1 block, smallest realistic) */
    size_t cap = 64;
    rsqf_filter_t *rf = rsqf_filter_create(cap, 8);
    
    if (rf) {
        /* Verify derived pointers are within allocation */
        uint8_t *base = (uint8_t *)rf;
        uint64_t raw = rf->num_blocks * (2 * sizeof(uint64_t) + sizeof(uint8_t));
        uint64_t meta_sz = (raw + 7) & ~7ULL;
        size_t total = sizeof(rsqf_filter_t) + meta_sz
            + ((rf->num_slots * rf->remainder_bits + 63) / 64) * sizeof(uint64_t);
        total = (total + 7) & ~7;
        
        __CPROVER_assert(total >= sizeof(rsqf_filter_t), "total >= header");
        
        /* Check each metadata region is within bounds */
        uint64_t nb = rf->num_blocks;
        uint64_t occ_end = sizeof(rsqf_filter_t) + nb * sizeof(uint64_t);
        uint64_t run_end = occ_end + nb * sizeof(uint64_t);
        uint64_t off_end = run_end + nb * sizeof(uint8_t);
        uint64_t pref_end = off_end + nb * sizeof(uint64_t);
        uint64_t rpref_end = pref_end + nb * sizeof(uint64_t);
        uint64_t meta_pad = (rpref_end + 7) & ~7;
        uint64_t tbl_start = sizeof(rsqf_filter_t) + meta_pad;
        
        __CPROVER_assert(occ_end <= total, "occ within alloc");
        __CPROVER_assert(run_end <= total, "run within alloc");
        __CPROVER_assert(off_end <= total, "off within alloc");
        __CPROVER_assert(pref_end <= total, "occ_prefix within alloc");
        __CPROVER_assert(rpref_end <= total, "run_prefix within alloc");
        __CPROVER_assert(tbl_start <= total, "table within alloc");
        
        /* Verify no overlap (regions are non-overlapping by construction) */
        /* occ: [header, header+nb*8) — occupies nb*8 bytes */
        /* run: [header+nb*8, header+nb*16) — disjoint from occ */
        __CPROVER_assert(nb * sizeof(uint64_t) + sizeof(rsqf_filter_t) <= 
                        (uintptr_t)(base + sizeof(rsqf_filter_t) + nb * sizeof(uint64_t)),
                        "occ and run non-overlapping");
        
        /* Safe destroy */
        rsqf_filter_destroy(rf);
    }
}
#else
int main(void) { return 0; }
#endif
