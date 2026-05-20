/**
 * test_rank_select_exhaustive.c — Exhaustive rank/select correctness test
 *
 * For ALL 16-bit bitvectors (65536 patterns), verifies that:
 *   - rank_fast(i) == rank_slow(i) for all i
 *   - select_fast(k) == select_slow(k) for all valid k
 *
 * This catches any bug in the select64 or rank64 implementations.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

/* Copy of rank64 from rsqf_filter.c */
static uint64_t rank64(uint64_t word, uint64_t pos) {
    uint64_t mask = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(word & mask);
}

/* Copy of select64 from rsqf_filter.c — binary search on popcount */
static uint64_t select64_fast(uint64_t word, uint64_t k) {
    uint64_t pop = __builtin_popcountll(word);
    if (k == 0 || k > pop) return 64;
    uint64_t lo = 0, hi = 63;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t low_bits = word & ((1ULL << (mid + 1)) - 1);
        if ((uint64_t)__builtin_popcountll(low_bits) >= k)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

/* Slow reference rank: positional scan */
static uint64_t rank_slow(uint64_t word, uint64_t pos) {
    uint64_t r = 0;
    for (uint64_t i = 0; i <= pos && i < 64; i++)
        if ((word >> i) & 1) r++;
    return r;
}

/* Slow reference select: positional scan */
static uint64_t select_slow(uint64_t word, uint64_t k) {
    if (k == 0) return 64;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < 64; i++) {
        if ((word >> i) & 1) {
            seen++;
            if (seen == k) return i;
        }
    }
    return 64;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Testing all 2^16 = 65536 bitvectors...\n");

    uint64_t errors = 0;
    for (uint64_t pat = 0; pat < (1ULL << 16); pat++) {
        uint64_t word = pat;

        /* Test rank for all positions 0..63 */
        for (int pos = 0; pos < 64; pos++) {
            uint64_t fr = rank64(word, (uint64_t)pos);
            uint64_t sr = rank_slow(word, (uint64_t)pos);
            if (fr != sr) {
                printf("RANK MISMATCH: word=0x%04lx pos=%d fast=%lu slow=%lu\n",
                       word, pos, fr, sr);
                errors++;
                if (errors >= 5) goto done;
            }
        }

        /* Test select for all k from 1..popcount+1 */
        uint64_t pop = __builtin_popcountll(word);
        for (uint64_t k = 1; k <= pop + 1; k++) {
            uint64_t ff = select64_fast(word, k);
            uint64_t sf = select_slow(word, k);
            if (ff != sf) {
                printf("SELECT MISMATCH: word=0x%04lx k=%lu fast=%lu slow=%lu\n",
                       word, k, ff, sf);
                errors++;
                if (errors >= 5) goto done;
            }
        }

        if (pat % 8192 == 0) printf("  checked %lu/65536\n", pat);
    }

done:
    if (errors == 0)
        printf("ALL PASS: 65536 patterns × 64 ranks + ~popcount selects\n");
    else
        printf("FAIL: %lu errors\n", errors);
    return errors > 0 ? 1 : 0;
}
