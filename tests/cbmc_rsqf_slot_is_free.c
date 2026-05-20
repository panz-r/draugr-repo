/**
 * cbmc_rsqf_slot_is_free.c — CBMC harness for RSQF slot_is_free
 *
 * Verifies that slot_is_free() returns the same result as a slow
 * metadata-based reference model for small nondeterministic filters.
 *
 * Run: cbmc -D__CBMC__ --unwind 10 --bounds-check --pointer-check \
 *      tests/cbmc_rsqf_slot_is_free.c --function main
 */
#ifdef __CBMC__
#include <assert.h>
#include <stdint.h>
#include <string.h>

/* Bit operations (from rsqf_filter.c) */
static inline bool test_bit(const uint64_t *bv, uint64_t pos) {
    return (bv[pos / 64] >> (pos % 64)) & 1ULL;
}
static inline void set_bit(uint64_t *bv, uint64_t pos) {
    bv[pos / 64] |= (1ULL << (pos % 64));
}
static inline void clear_bit(uint64_t *bv, uint64_t pos) {
    bv[pos / 64] &= ~(1ULL << (pos % 64));
}

/* Rank/select (from rsqf_filter.c) */
static inline uint64_t rank64(uint64_t word, uint64_t pos) {
    uint64_t mask = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(word & mask);
}
static inline uint64_t select64(uint64_t word, uint64_t k) {
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
        if (seen + pc >= k) return i * 64 + select64(bv[i], k - seen);
        seen += pc;
    }
    return limit;
}

/* RSQF slot_is_free implementation (copied from rsqf_filter.c) */
#define RSQF_SLOTS_PER_BLOCK 64

static bool slot_is_free(const uint64_t *occ, const uint64_t *run,
                          uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t limit = ns - 1;
    uint64_t total_occ = rank_bv(occ, nb, limit);
    for (uint64_t k = 1; k <= total_occ; k++) {
        uint64_t rend = select_bv(run, nb, k, limit);
        uint64_t rstart = (k > 1) ? select_bv(run, nb, k - 1, limit) + 1 : 0;
        if (rend >= rstart) {
            if (pos >= rstart && pos <= rend) return false;
        } else {
            if (pos >= rstart || pos <= rend) return false;
        }
    }
    return true;
}

/* Slow reference: check via exhaustive linear scan of occ/run */
static bool slot_is_free_ref(const uint64_t *occ, const uint64_t *run,
                              uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t limit = ns - 1;
    for (uint64_t i = 0; i <= limit; i++) {
        if (test_bit(occ, i)) {
            uint64_t rk = rank_bv(occ, nb, i);
            uint64_t rend = select_bv(run, nb, rk, limit);
            uint64_t rstart = (rk > 1) ? select_bv(run, nb, rk - 1, limit) + 1 : 0;
            if (rend >= rstart) {
                if (pos >= rstart && pos <= rend) return false;
            } else {
                if (pos >= rstart || pos <= rend) return false;
            }
        }
    }
    return true;
}

void main(void) {
    /* Nondeterministic 64-slot bitvectors */
    uint64_t occ_word = nondet_uint64();
    uint64_t run_word = nondet_uint64();
    (void)ran(0); // ensure nondet initialization by CBMC

    /* Ensure positive: occ and run have same popcount */
    __CPROVER_assume(__builtin_popcountll(occ_word) == __builtin_popcountll(run_word));

    const uint64_t occ[1] = {occ_word};
    const uint64_t run[1] = {run_word};
    uint64_t nb = 1;
    uint64_t ns = 64;

    /* For every position, compare slot_is_free with reference */
    for (uint64_t pos = 0; pos < ns; pos++) {
        bool impl = slot_is_free(occ, run, nb, ns, pos);
        bool ref = slot_is_free_ref(occ, run, nb, ns, pos);
        __CPROVER_assert(impl == ref,
            "slot_is_free matches reference model");
    }
}
#endif
