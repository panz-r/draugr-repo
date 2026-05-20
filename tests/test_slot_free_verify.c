/**
 * test_slot_free_verify.c — Compare slot_is_free fast vs reference
 *
 * Exhaustive verification helper: run before declaring fast==reference.
 *
 * Compile: gcc -g -I. -Iinclude tests/test_slot_free_verify.c \
 *              src/cqf_filter.c src/rsqf_filter.c src/util.c \
 *              -o test_slot_free_verify -lm
 */
#include "draugr/rsqf_filter.h"
#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>

/* Independent rank/select implementations */
static uint64_t my_rank64(uint64_t w, uint64_t pos) {
    uint64_t m = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(w & m);
}
static uint64_t my_rank_bv(const uint64_t *bv, uint64_t nb, uint64_t pos) {
    uint64_t blk = pos / 64, off = pos % 64, r = 0;
    for (uint64_t i = 0; i < blk && i < nb; i++) r += __builtin_popcountll(bv[i]);
    if (blk < nb) r += my_rank64(bv[blk], off);
    return r;
}
static uint64_t my_select64(uint64_t w, uint64_t k) {
    uint64_t pop = __builtin_popcountll(w);
    if (k == 0 || k > pop) return 64;
    uint64_t lo = 0, hi = 63;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t lb = w & ((1ULL << (mid + 1)) - 1);
        if ((uint64_t)__builtin_popcountll(lb) >= k) hi = mid; else lo = mid + 1;
    }
    return lo;
}
static uint64_t my_select_bv(const uint64_t *bv, uint64_t nb, uint64_t k, uint64_t limit) {
    if (k == 0) return 0;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < nb; i++) {
        uint64_t pc = __builtin_popcountll(bv[i]);
        if (seen + pc >= k) return i * 64 + my_select64(bv[i], k - seen);
        seen += pc;
    }
    return limit;
}

/* Reference: O(occ × blocks), iterates all runs from k=1 */
static bool ref_rsqf(const uint64_t *occ, const uint64_t *run,
                      uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t lim = ns - 1, tot = my_rank_bv(occ, nb, lim);
    for (uint64_t k = 1; k <= tot; k++) {
        uint64_t e = my_select_bv(run, nb, k, lim);
        uint64_t s = (k > 1) ? my_select_bv(run, nb, k-1, lim) + 1 : 0;
        if (e >= s) { if (pos >= s && pos <= e) return false; }
        else { if (pos >= s || pos <= e) return false; }
    }
    return true;
}
static bool ref_cqf(const uint64_t *occ, const uint64_t *run,
                     uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t lim = ns - 1, tot = my_rank_bv(occ, nb, lim);
    for (uint64_t k = 1; k <= tot; k++) {
        uint64_t e = my_select_bv(run, nb, k, lim);
        uint64_t s;
        if (k == 1) s = (occ[0] & 1) ? 1 : 0;
        else s = my_select_bv(run, nb, k-1, lim) + 1;
        if (e >= s) { if (pos >= s && pos <= e) return false; }
        else { if (pos >= s || pos <= e) return false; }
    }
    return true;
}

/* Fast: starts from k = max(1, rank(occ, pos)), NO early exit.
 * Same rstart formula as reference. The only optimization is skipping
 * runs that are definitely before pos (their occ ≤ pos and rend < pos). */
static bool fast_rsqf(const uint64_t *occ, const uint64_t *run,
                       uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t lim = ns - 1, rk = my_rank_bv(occ, nb, pos);
    uint64_t tot = my_rank_bv(occ, nb, lim);
    for (uint64_t k = (rk > 0 ? rk : 1); k <= tot; k++) {
        uint64_t e = my_select_bv(run, nb, k, lim);
        uint64_t s = (k > 1) ? my_select_bv(run, nb, k-1, lim) + 1 : 0;
        if (e >= s) { if (pos >= s && pos <= e) return false; }
        else { if (pos >= s || pos <= e) return false; break; }
    }
    return true;
}
static bool fast_cqf(const uint64_t *occ, const uint64_t *run,
                      uint64_t nb, uint64_t ns, uint64_t pos) {
    uint64_t lim = ns - 1, rk = my_rank_bv(occ, nb, pos);
    uint64_t tot = my_rank_bv(occ, nb, lim);
    for (uint64_t k = (rk > 0 ? rk : 1); k <= tot; k++) {
        uint64_t e = my_select_bv(run, nb, k, lim);
        uint64_t s;
        if (k == 1) s = (occ[0] & 1) ? 1 : 0;
        else s = my_select_bv(run, nb, k-1, lim) + 1;
        if (e >= s) { if (pos >= s && pos <= e) return false; }
        else { if (pos >= s || pos <= e) return false; break; }
    }
    return true;
}

static int mismatches = 0;

static void check_all(const uint64_t *occ, const uint64_t *run,
                       uint64_t nb, uint64_t ns, const char *label,
                       bool (*fast)(const uint64_t*,const uint64_t*,uint64_t,uint64_t,uint64_t),
                       bool (*ref)(const uint64_t*,const uint64_t*,uint64_t,uint64_t,uint64_t)) {
    for (uint64_t p = 0; p < ns; p++) {
        bool f = fast(occ, run, nb, ns, p);
        bool r = ref(occ, run, nb, ns, p);
        if (f != r && mismatches < 30)
            printf("  MIS %s pos=%lu fast=%d ref=%d\n", label, p, f, r);
        if (f != r) mismatches++;
    }
}

int main(void) {
    printf("Verifying slot_is_free fast == reference...\n");
    srand(42);
    for (int t = 0; t < 200; t++) {
        rsqf_filter_t *rf = rsqf_filter_create(64, 4);
        cqf_filter_t *cf = cqf_filter_create(64, 4);
        uint64_t *ro = (uint64_t *)(rf + 1), *rr = ro + rf->num_blocks;
        uint64_t *co = (uint64_t *)(cf + 1), *cr = co + cf->num_blocks;
        uint64_t nb = rf->num_blocks, ns = rf->num_slots;

        for (int op = 0; op < 40; op++) {
            uint64_t h = (uint64_t)(rand() * 0x9e3779b97f4a7c15ULL);
            (rand() % 3 < 2) ? rsqf_filter_insert(rf, h) : rsqf_filter_delete(rf, h);
            (rand() % 3 < 2) ? cqf_filter_insert(cf, h) : cqf_filter_delete(cf, h);
            check_all(ro, rr, nb, ns, "R", fast_rsqf, ref_rsqf);
            check_all(co, cr, nb, ns, "C", fast_cqf, ref_cqf);
        }
        /* Include wrapped cluster states */
        for (uint64_t q = 14; q < 16; q++)
            { uint64_t fp = (q << 4) | (rand() & 0xF); rsqf_filter_insert(rf, fp); cqf_filter_insert(cf, fp); }
        for (uint64_t q = 0; q < 2; q++)
            { uint64_t fp = (q << 4) | (rand() & 0xF); rsqf_filter_insert(rf, fp); cqf_filter_insert(cf, fp); }
        check_all(ro, rr, nb, ns, "Rw", fast_rsqf, ref_rsqf);
        check_all(co, cr, nb, ns, "Cw", fast_cqf, ref_cqf);
        rsqf_filter_destroy(rf); cqf_filter_destroy(cf);
    }
    printf("Total mismatches: %d\n", mismatches);
    printf(mismatches == 0 ? "PASS: fast = reference\n" : "FAIL: fast != reference\n");
    return mismatches;
}
