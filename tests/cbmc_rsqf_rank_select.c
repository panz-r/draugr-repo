/**
 * cbmc_rsqf_rank_select.c — CBMC harness for rank/select correctness
 *
 * Verifies that rank_bv and select_bv match a slow reference for
 * multi-block bitvectors with bounded capacity.
 *
 * CBMC: cbmc --unwind 12 --bounds-check --pointer-check \
 *             cbmc_rsqf_rank_select.c --function main
 */
#ifdef __CBMC__
#include <assert.h>
#include <stdint.h>
#include <string.h>

#define MAX_BLOCKS 2
#define BITS_PER_BLOCK 64

/* Reference slow rank */
static uint64_t rank_slow(const uint64_t *bv, uint64_t nblocks, uint64_t pos) {
    uint64_t r = 0;
    for (uint64_t i = 0; i <= pos && i < nblocks * BITS_PER_BLOCK; i++)
        if ((bv[i / 64] >> (i % 64)) & 1) r++;
    return r;
}

/* Reference slow select */
static uint64_t select_slow(const uint64_t *bv, uint64_t nblocks, uint64_t k) {
    if (k == 0) return 0;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < nblocks * BITS_PER_BLOCK; i++) {
        if ((bv[i / 64] >> (i % 64)) & 1) {
            seen++;
            if (seen == k) return i;
        }
    }
    return nblocks * BITS_PER_BLOCK; /* not found */
}

/* Functions under test */
static uint64_t rank64(uint64_t word, uint64_t pos) {
    uint64_t mask = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(word & mask);
}
static uint64_t select64(uint64_t word, uint64_t k) {
    uint64_t pop = __builtin_popcountll(word);
    if (k == 0 || k > pop) return 64;
    uint64_t lo = 0, hi = 63;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t low_bits = word & ((1ULL << (mid + 1)) - 1);
        if ((uint64_t)__builtin_popcountll(low_bits) >= k) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}
static uint64_t rank_bv(const uint64_t *bv, uint64_t nblocks, uint64_t pos) {
    uint64_t block = pos / 64;
    uint64_t off = pos % 64;
    uint64_t r = 0;
    for (uint64_t i = 0; i < block && i < nblocks; i++)
        r += __builtin_popcountll(bv[i]);
    if (block < nblocks) r += rank64(bv[block], off);
    return r;
}
static uint64_t select_bv(const uint64_t *bv, uint64_t nblocks, uint64_t k, uint64_t limit) {
    if (k == 0) return 0;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < nblocks; i++) {
        uint64_t pc = __builtin_popcountll(bv[i]);
        if (seen + pc >= k)
            return i * 64 + select64(bv[i], k - seen);
        seen += pc;
    }
    return limit;
}

void main(void) {
    uint64_t bv[MAX_BLOCKS];
    
    /* Nondeterministic bitvector values */
    bv[0] = nondet_uint64();
    bv[1] = nondet_uint64();
    
    __CPROVER_assume(MAX_BLOCKS == 2);
    
    /* Test rank at every position */
    for (uint64_t pos = 0; pos < MAX_BLOCKS * BITS_PER_BLOCK; pos++) {
        uint64_t fast = rank_bv(bv, MAX_BLOCKS, pos);
        uint64_t slow = rank_slow(bv, MAX_BLOCKS, pos);
        assert(fast == slow);
    }
    
    /* Test select for every valid k */
    uint64_t total_pop = __builtin_popcountll(bv[0]) + __builtin_popcountll(bv[1]);
    for (uint64_t k = 1; k <= total_pop; k++) {
        uint64_t fast = select_bv(bv, MAX_BLOCKS, k, MAX_BLOCKS * BITS_PER_BLOCK);
        uint64_t slow = select_slow(bv, MAX_BLOCKS, k);
        assert(fast == slow);
    }
    
    /* Test select k=0 returns 0 */
    assert(select_bv(bv, MAX_BLOCKS, 0, MAX_BLOCKS * BITS_PER_BLOCK) == 0);
    
    /* Test select k > popcount returns limit */
    assert(select_bv(bv, MAX_BLOCKS, total_pop + 1, MAX_BLOCKS * BITS_PER_BLOCK - 1)
           == MAX_BLOCKS * BITS_PER_BLOCK - 1);
}
#endif
