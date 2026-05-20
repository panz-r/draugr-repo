/**
 * rsqf_filter.c - Rank-and-Select Quotient Filter implementation
 *
 * Implements the RSQF from:
 *   Pandey, Bender, Johnson, and Patro.
 *   "A General-Purpose Counting Filter: Making Every Bit Count." SIGMOD 2017.
 *
 * Additional implementation details from:
 *   Bungeroth. "Implementation of a Rank and Select Based Quotient Filter." 2018.
 *
 *   - 64-slot blocks with occupieds + runends + phys_occ + prefix sums + offsets
 *   - ~5.125 bits/slot metadata overhead with prefix arrays
 *   - Circular slot array (wraparound clusters supported)
 *   - Rank: O(1) via occ prefix sums
 *   - Select: O(log num_blocks) via binary search on run prefix sums
 */

#include "draugr/rsqf_filter.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>
#include <assert.h>


/* ============================================================================
 * Word-level rank and select
 * rank64(word, pos) — number of 1-bits in word[0..pos] inclusive
 * select64(word, k) — position of k-th 1-bit (1-indexed)
 * ============================================================================ */

static inline uint64_t rank64(uint64_t word, uint64_t pos)
{
    uint64_t mask = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(word & mask);
}

static inline uint64_t select64(uint64_t word, uint64_t k)
{
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

/* ============================================================================
 * Memory layout helpers (single allocation):
 *   rsqf_filter_t
 *   occupieds[nb]       — uint64_t[nb]
 *   runends[nb]        — uint64_t[nb]
 *   phys_occ[nb]       — uint64_t[nb]  (physical occupancy for rem=0 support)
 *   occ_prefix[nb+1]   — uint64_t[nb+1]  (block-level prefix sums of occ)
 *   run_prefix[nb+1]   — uint64_t[nb+1]  (block-level prefix sums of run)
 *   offsets[nb]        — uint8_t[nb]   (padded to 8B for rem_table alignment)
 *   rem_table[]        — uint64_t[(rbits*slots+63)/64]
 * Metadata: ~5.125 bits/slot = (64+64+64+64+64+8)/64 + 2*64/(nb*64)
 * ============================================================================ */

static inline uint64_t *occ_bits(rsqf_filter_t *rf)
{
    return (uint64_t *)(rf + 1);
}
static inline const uint64_t *occ_bits_const(const rsqf_filter_t *rf)
{
    return (const uint64_t *)(rf + 1);
}
static inline uint64_t *run_bits(rsqf_filter_t *rf)
{
    return (uint64_t *)(rf + 1) + rf->num_blocks;
}
static inline const uint64_t *run_bits_const(const rsqf_filter_t *rf)
{
    return (const uint64_t *)(rf + 1) + rf->num_blocks;
}
static inline uint64_t *phys_occ_bits(rsqf_filter_t *rf)
{
    return (uint64_t *)(rf + 1) + 2 * rf->num_blocks;
}
static inline const uint64_t *phys_occ_bits_const(const rsqf_filter_t *rf)
{
    return (const uint64_t *)(rf + 1) + 2 * rf->num_blocks;
}
static inline uint64_t *occ_prefix(rsqf_filter_t *rf)
{
    return (uint64_t *)(rf + 1) + 3 * rf->num_blocks;
}
static inline const uint64_t *occ_prefix_const(const rsqf_filter_t *rf)
{
    return (const uint64_t *)(rf + 1) + 3 * rf->num_blocks;
}
static inline uint64_t *run_prefix(rsqf_filter_t *rf)
{
    return occ_prefix(rf) + (rf->num_blocks + 1);
}
static inline const uint64_t *run_prefix_const(const rsqf_filter_t *rf)
{
    return occ_prefix_const(rf) + (rf->num_blocks + 1);
}
static inline uint8_t *off_array(rsqf_filter_t *rf)
{
    return (uint8_t *)(run_prefix(rf) + (rf->num_blocks + 1));
}
static inline const uint8_t *off_array_const(const rsqf_filter_t *rf)
{
    return (const uint8_t *)(run_prefix_const(rf) + (rf->num_blocks + 1));
}
static inline uint64_t meta_size(const rsqf_filter_t *rf)
{
    uint64_t total = rf->num_blocks * (5 * sizeof(uint64_t) + sizeof(uint8_t)) + 2 * sizeof(uint64_t);
    return (total + 7) & ~7ULL;
}
static inline uint64_t *rem_table(rsqf_filter_t *rf)
{
    return (uint64_t *)((uint8_t *)(rf + 1) + meta_size(rf));
}
static inline const uint64_t *rem_table_const(const rsqf_filter_t *rf)
{
    return (const uint64_t *)((const uint8_t *)(rf + 1) + meta_size(rf));
}

/* ============================================================================
 * Bit operations
 * ============================================================================ */

static inline bool test_bit(const uint64_t *bv, uint64_t pos)
{
    return (bv[pos / 64] >> (pos % 64)) & 1ULL;
}
static inline void set_bit(uint64_t *bv, uint64_t pos)
{
    bv[pos / 64] |= (1ULL << (pos % 64));
}
static inline void clear_bit(uint64_t *bv, uint64_t pos)
{
    bv[pos / 64] &= ~(1ULL << (pos % 64));
}

/* ============================================================================
 * Remainder table access (bit-packed)
 * ============================================================================ */

static inline uint64_t rem_get(const uint64_t *tbl, uint64_t idx,
                                uint8_t rbits, uint64_t mask)
{
    uint64_t bp = idx * rbits;
    uint64_t wi = bp / 64;
    uint64_t bo = bp % 64;
    uint64_t v = tbl[wi] >> bo;
    if (bo + rbits > 64)
        v |= tbl[wi + 1] << (64 - bo);
    return v & mask;
}

static inline void rem_set(uint64_t *tbl, uint64_t idx, uint8_t rbits,
                            uint64_t mask, uint64_t v)
{
    uint64_t bp = idx * rbits;
    uint64_t wi = bp / 64;
    uint64_t bo = bp % 64;
    uint64_t m = mask << bo;
    tbl[wi] = (tbl[wi] & ~m) | ((v & mask) << bo);
    if (bo + rbits > 64) {
        uint64_t total_bits = idx * rbits + rbits;
        assert(wi + 1 < (total_bits + 63) / 64 && "rem_set: out-of-bounds word access");
        uint64_t m2 = mask >> (64 - bo);
        tbl[wi + 1] = (tbl[wi + 1] & ~m2) | ((v & mask) >> (64 - bo));
    }
}

/* ============================================================================
 * Rank/select using prefix-sum arrays (canonical RSQF approach).
 *
 * rank_occ(rf, pos):   O(1) — block prefix + within-word rank64.
 * select_run(rf, k):   O(log nblocks) — binary search on run prefix,
 *                       then select64 within the block.
 * ============================================================================ */

static uint64_t rank_occ(const rsqf_filter_t *rf, uint64_t pos)
{
    uint64_t block = pos / 64;
    uint64_t off = pos % 64;
    const uint64_t *op = occ_prefix_const(rf);
    const uint64_t *occ = occ_bits_const(rf);
    return op[block] + rank64(occ[block], off);
}

static uint64_t select_run(const rsqf_filter_t *rf, uint64_t k)
{
    if (k == 0) return 0;
    const uint64_t *rp = run_prefix_const(rf);
    const uint64_t *run = run_bits_const(rf);
    uint64_t nb = rf->num_blocks;
    if (k > rp[nb]) return rf->num_slots - 1;
    uint64_t lo = 0, hi = nb;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (rp[mid] >= k) hi = mid; else lo = mid + 1;
    }
    uint64_t block = lo - 1;
    return block * 64 + select64(run[block], k - rp[block]);
}

/* ============================================================================
 * Offset-guided rank-select — O(1) runend lookup for an occupied quotient.
 *
 * Canonical RSQF per Pandey et al. §3.2 and Bungeroth §3.2 (Figure 3):
 *   rank_select(q) = SELECT(runends, RANK(occupieds, q))
 *
 * Uses the block offset to jump directly to the first runend in the
 * block, then scans forward within the block for any additional
 * occupied quotients between the block start and q.  The scan is
 * bounded by the number of occupied quotients in a single block
 * (≤ 64), so this is O(1) per operation.
 *
 * Precondition: test_bit(occ, q) == true (q is an occupied quotient).
 * ============================================================================ */

static uint64_t rank_select(const rsqf_filter_t *rf, uint64_t q)
{
    uint64_t b = q / RSQF_SLOTS_PER_BLOCK;
    uint64_t base = b * RSQF_SLOTS_PER_BLOCK;
    uint64_t off_val = off_array_const(rf)[b];

    /* Overflow sentinel: offset was clamped because the run extends
     * beyond 254 slots from the block start.  Fall back to the
     * O(log nblocks) binary-search select. */
    if (off_val == 255)
        return select_run(rf, rank_occ(rf, q));

    if (q == base)
        return q + off_val;

    uint64_t rk_base = (base > 0) ? rank_occ(rf, base - 1) : 0;
    uint64_t rk_qm1 = rank_occ(rf, q - 1);
    uint64_t d = rk_qm1 - rk_base;

    if (d == 0)
        return base + off_val;

    uint64_t pos = base + off_val;
    const uint64_t *run = run_bits_const(rf);
    uint64_t ns = rf->num_slots;
    uint64_t need = d;

    while (need > 0) {
        pos++;
        if (pos >= ns) pos = 0;

        uint64_t wi = pos / 64;
        if (wi >= rf->num_blocks) break;
        uint64_t bi = pos % 64;
        uint64_t word = run[wi] >> bi;

        if (word) {
            uint64_t skip = __builtin_ctzll(word);
            pos += skip;
            need--;
        } else {
            pos = (wi + 1) * 64 - 1;
        }
    }

    return pos;
}

/* ============================================================================
 * Free-slot detection: O(1) via physical occupancy bitset.
 * phys_occ[pos] is maintained by track_phys_occ() after every rem_set,
 * and by move_phys_occ() during shift loops.
 * ============================================================================ */

/* O(1): check physical occupancy bit.
 * A slot is free when phys_occ=0 (no data written there).
 * rem=0 is a valid fingerprint so phys_occ is the only reliable empty marker. */
static bool slot_is_free(const rsqf_filter_t *rf, uint64_t pos)
{
    return !test_bit(phys_occ_bits_const(rf), pos);
}

/* Track physical occupancy: set phys_occ[pos] whenever data is written.
 * rem=0 is a valid fingerprint, so phys_occ is always set for any write.
 * phys_occ is cleared explicitly during delete/shift (via clear_bit). */
static inline void track_phys_occ(rsqf_filter_t *rf, uint64_t pos, uint64_t v)
{
    (void)v;
    uint64_t *p = phys_occ_bits(rf);
    p[pos / 64] |= (1ULL << (pos % 64));
}

/* Move phys_occ bit from src to dst during shifts (like run bit movement). */
static inline void move_phys_occ(rsqf_filter_t *rf, uint64_t src, uint64_t dst)
{
    uint64_t *p = phys_occ_bits(rf);
    if (p[src / 64] & (1ULL << (src % 64))) {
        p[dst / 64] |= (1ULL << (dst % 64));
        p[src / 64] &= ~(1ULL << (src % 64));
    }
}
/* ============================================================================
 * Atomic slot move: copy remainder AND runend bit from src to dst,
 * then clear src remainder and clear src runend bit.
 * This is the fundamental operation for all shift logic.
 * ============================================================================ */

static void move_slot_right(rsqf_filter_t *rf, uint64_t dst, uint64_t src)
{
    uint64_t *tbl = rem_table(rf);
    uint64_t *run = run_bits(rf);
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;
    uint64_t v = rem_get(tbl, src, rbits, mask);
    bool rb = test_bit(run, src);
    rem_set(tbl, dst, rbits, mask, v);
    move_phys_occ(rf, src, dst);
    if (rb) set_bit(run, dst); else clear_bit(run, dst);
    clear_bit(run, src);
    clear_bit(phys_occ_bits(rf), src);
    rem_set(tbl, src, rbits, mask, 0);
}

/* ============================================================================
 * Update offsets and prefix sums.
 *
 * Pass 1: recompute occ_prefix[] and run_prefix[] from first_block onward.
 * Pass 2: recompute block offsets from first_block onward.
 *
 * For a full recompute (e.g. after wraparound shifts, reset, or repair
 * after external corruption), pass first_block = 0.  For incremental
 * updates, pass the minimum block index where any occ or run bit
 * changed — the first_block / nb fraction of blocks will be skipped.
 *
 * Offsets follow the canonical RSQF definition (Pandey et al. §3.2):
 * offset[b] = distance from block start to the runend of the first
 * occupied quotient in block b.
 * ============================================================================ */

static void update_offsets_from(rsqf_filter_t *rf, uint64_t first_block)
{
    const uint64_t *occ = occ_bits_const(rf);
    const uint64_t *run = run_bits_const(rf);
    uint8_t *off = off_array(rf);
    uint64_t *occ_p = occ_prefix(rf);
    uint64_t *run_p = run_prefix(rf);
    uint64_t nb = rf->num_blocks;

    /* Pass 1: prefix sums from first_block onward.
     * Preceding entries are assumed correct (maintained by previous calls). */
    if (first_block == 0) {
        occ_p[0] = 0;
        run_p[0] = 0;
    }
    for (uint64_t b = first_block; b < nb; b++) {
        occ_p[b + 1] = occ_p[b] + __builtin_popcountll(occ[b]);
        run_p[b + 1] = run_p[b] + __builtin_popcountll(run[b]);
    }

    /* Pass 2: block offsets from first_block onward */
    for (uint64_t b = first_block; b < nb; b++) {
        uint64_t blk_start = b * RSQF_SLOTS_PER_BLOCK;
        uint64_t o = 0;
        if (occ_p[b + 1] > occ_p[b]) {
            uint64_t end = select_run(rf, occ_p[b] + 1);
            if (end > blk_start)
                o = end - blk_start;
        }
        /* 254 = max guaranteed-correct offset; 255 = overflow sentinel
         * (rank_select falls back to select_run when it sees 255). */
        off[b] = (uint8_t)(o > 254 ? 255 : o);
    }
}

static uint64_t run_start(const rsqf_filter_t *rf, uint64_t k)
{
    uint64_t ns = rf->num_slots;
    if (k == 1)
        return 0;  /* first run always starts at slot 0 (Pandey et al. §3.1) */
    return (select_run(rf, k - 1) + 1) % ns;
}

/* ============================================================================
 * Find the run for a given quotient q.
 *
 * Uses rank/select for O(log nblocks) lookup instead of scanning.
 *
 * When occ[q] is set: the k-th occupied quotient (k = rank_occ(q))
 * maps directly to the k-th runend via select_run(k).
 *
 * When occ[q] is clear but run[q] is set: q is a runend whose
 * occupied quotient lies elsewhere.  rank_occ(q) gives the count of
 * occupied quotients before q — the rk-th runend is the one that
 * ends at q.  If rk == 0 the run wraps around and q belongs to the
 * last run (rank_occ(ns-1)).  The old O(num_slots) backward scan
 * for the "no preceding quotient" fallback is unreachable under the
 * invariant popcount(occ) == popcount(run).
 *
 * Returns true if a run exists for q, false if q has no entries.
 * ============================================================================ */

static bool find_run(const rsqf_filter_t *rf, uint64_t q,
                     uint64_t *start, uint64_t *end)
{
    const uint64_t *occ = occ_bits_const(rf);
    const uint64_t *run = run_bits_const(rf);
    uint64_t ns = rf->num_slots;

    if (!test_bit(occ, q)) {
        if (!test_bit(run, q))
            return false;
        /* occ[q]=0 but run[q]=1: q is a runend whose occupied
         * quotient is elsewhere.  Use rank/select to find it. */
        uint64_t rk = rank_occ(rf, q);
        if (rk == 0)
            rk = rank_occ(rf, ns - 1); /* wraparound — last run */
        *end = select_run(rf, rk);
        *start = run_start(rf, rk);
        return true;
    }

    /* Common path: occ[q] is set — use O(1) offset-guided lookup */
    *end = rank_select(rf, q);
    *start = run_start(rf, rank_occ(rf, q));

    return true;
}

/* ============================================================================
 * Utility
 * ============================================================================ */

static uint64_t next_power_of_2(uint64_t n)
{
    if (n == 0) return 1;
    n--;
    n |= n >> 1; n |= n >> 2; n |= n >> 4;
    n |= n >> 8; n |= n >> 16; n |= n >> 32;
    return n + 1;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

rsqf_filter_t *rsqf_filter_create(size_t capacity, uint8_t fp_bits)
{
    if (capacity == 0) return NULL;
    if (fp_bits == 0) fp_bits = RSQF_DEFAULT_FINGERPRINT_BITS;
    if (fp_bits < RSQF_MIN_FINGERPRINT_BITS) fp_bits = RSQF_MIN_FINGERPRINT_BITS;
    if (fp_bits > RSQF_MAX_FINGERPRINT_BITS) fp_bits = RSQF_MAX_FINGERPRINT_BITS;

    uint64_t num_slots = next_power_of_2(capacity);
    uint64_t num_blocks = (num_slots + RSQF_SLOTS_PER_BLOCK - 1) / RSQF_SLOTS_PER_BLOCK;
    num_slots = num_blocks * RSQF_SLOTS_PER_BLOCK;

    uint8_t qb = 0;
    uint64_t tmp = num_slots;
    while (tmp > 1) { tmp >>= 1; qb++; }

    uint64_t total_remainder_bits = num_slots * fp_bits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;

    /* Metadata: occupieds + runends + phys_occ + occ_prefix + run_prefix + offsets,
     * padded to 8B.  Must match meta_size(). */
    uint64_t raw_meta = num_blocks * (5 * sizeof(uint64_t) + sizeof(uint8_t)) + 2 * sizeof(uint64_t);
    uint64_t meta_sz = (raw_meta + 7) & ~7ULL;
    uint64_t tbl_bytes = remainder_words * sizeof(uint64_t);
    size_t total_bytes = sizeof(rsqf_filter_t) + meta_sz + tbl_bytes;

    rsqf_filter_t *rf = (rsqf_filter_t *)malloc(total_bytes);
    if (!rf) return NULL;

    rf->num_slots = num_slots;
    rf->num_blocks = num_blocks;
    rf->count = 0;
    rf->remainder_bits = fp_bits;
    rf->quotient_bits = qb;
    rf->entries_per_set = (uint16_t)((RSQF_SET_BYTES * 8) / fp_bits);
    rf->num_sets = (uint16_t)((num_slots + rf->entries_per_set - 1) / rf->entries_per_set);
    rf->remainder_mask = (1ULL << fp_bits) - 1;

    memset(rf + 1, 0, meta_sz + tbl_bytes);
    return rf;
}

void rsqf_filter_destroy(rsqf_filter_t *rf) { free(rf); }

rsqf_err_t rsqf_filter_reset(rsqf_filter_t *rf)
{
    if (!rf) return RSQF_ERR_INVALID_PARAM;
    uint64_t meta_sz = meta_size(rf);
    uint64_t total_remainder_bits = rf->num_slots * rf->remainder_bits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;
    memset(rf + 1, 0, meta_sz + remainder_words * sizeof(uint64_t));
    rf->count = 0;
    return RSQF_OK;
}

/* ============================================================================
 * Core: Lookup (Algorithm 1 in paper)
 * ============================================================================ */

bool rsqf_filter_lookup(const rsqf_filter_t *rf, uint64_t hash)
{
    if (!rf || rf->count == 0) return false;

    uint64_t q = (hash >> rf->remainder_bits) & (rf->num_slots - 1);
    uint64_t rem = hash & rf->remainder_mask;

    if (q >= rf->num_slots) return false;

    uint64_t rstart, rend;
    if (!find_run(rf, q, &rstart, &rend))
        return false;

    /* Debug: verify run boundaries are valid.
     * For a wrapped run (rstart > rend), verify that both segments
     * [rstart, ns-1] and [0, rend] are within bounds. */
    assert(rend < rf->num_slots && "runend exceeds slot array");
    assert(rstart < rf->num_slots && "runstart exceeds slot array");
    if (rstart > rend) {
        uint64_t seg1_len = rf->num_slots - rstart;
        uint64_t seg2_len = rend + 1;
        assert(seg1_len + seg2_len <= rf->num_slots && "wrapped run exceeds total slots");
    }

    const uint64_t *tbl = rem_table_const(rf);
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;

    if (rstart <= rend) {
        for (uint64_t i = rstart; i <= rend; i++) {
            uint64_t v = rem_get(tbl, i, rbits, mask);
            if (v == rem)
                return true;
        }
    } else {
        for (uint64_t i = rstart; i < rf->num_slots; i++) {
            uint64_t v = rem_get(tbl, i, rbits, mask);
            if (v == rem)
                return true;
        }
        for (uint64_t i = 0; i <= rend; i++) {
            uint64_t v = rem_get(tbl, i, rbits, mask);
            if (v == rem)
                return true;
        }
    }
    return false;
}

/* ============================================================================
 * Core: Insert (Algorithm 2 in paper)
 * ============================================================================ */

rsqf_err_t rsqf_filter_insert(rsqf_filter_t *rf, uint64_t hash)
{
    if (!rf) return RSQF_ERR_INVALID_PARAM;
    if (rf->count >= rf->num_slots) return RSQF_ERR_FULL;

    uint64_t q = (hash >> rf->remainder_bits) & (rf->num_slots - 1);
    uint64_t rem = hash & rf->remainder_mask;

    uint64_t *occ = occ_bits(rf);
    uint64_t *run = run_bits(rf);
    uint64_t *tbl = rem_table(rf);
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;

    uint64_t rend, rstart;
    bool has_run = find_run(rf, q, &rstart, &rend);

    if (!has_run) {
        if (slot_is_free(rf, q)) {
            /* Slot q is free — place the new run directly at q */
            rem_set(tbl, q, rbits, mask, rem);
            track_phys_occ(rf, q, rem);
            set_bit(occ, q);
            set_bit(run, q);
            rf->count++;
            update_offsets_from(rf, q / RSQF_SLOTS_PER_BLOCK);
            return RSQF_OK;
        }

        /* Slot q is occupied by data from another run.
         * To preserve the runend-SELECT = occ-RANK invariant, we must
         * NOT create a new run at position q (which would reorder runends
         * in SELECT).  Instead, insert after the preceding run's runend,
         * the same approach as the CQF. */

        uint64_t ns = rf->num_slots;
        /* Count occs up to q (q itself is not occ, so this is the count
         * of preceding occs). */
        uint64_t rk = rank_occ(rf, q);
        uint64_t s = (rk > 0) ? select_run(rf, rk) : 0;
        uint64_t ins_pos = (rk > 0) ? (s + 1) % ns : 0;

        if (rk > 0 && ins_pos == 0)
            return RSQF_ERR_FULL;

        /* Find the actual end of the cluster (contiguous data from q). */
        uint64_t rend = q;
        {
            uint64_t steps = 0;
            while (steps < ns) {
                uint64_t next = (rend + 1) % ns;
                if (slot_is_free(rf, next))
                    break;
                rend = next;
                steps++;
            }
            if (steps >= ns)
                return RSQF_ERR_FULL;
        }

        /* Find the first free slot at or after rend+1 */
        uint64_t scan_start = (rend + 1) % ns;
        uint64_t n = scan_start;
        {
            uint64_t pos = scan_start;
            while (pos < ns && !slot_is_free(rf, pos)) pos++;
            if (pos >= ns) {
                pos = 0;
                while (pos < scan_start && !slot_is_free(rf, pos)) pos++;
                if (pos >= scan_start) return RSQF_ERR_FULL;
            }
            n = pos;
        }

        /* Shift [ins_pos, n) right by 1 to open ins_pos.
         * inv: n >= ins_pos because n >= rend+1 >= s+1 = ins_pos. */
        if (n > ins_pos) {
            for (uint64_t i = n; i > ins_pos; i--)
                move_slot_right(rf, i, i - 1);
        } else if (ins_pos > n) {
            if (test_bit(run, ns - 1))
                return RSQF_ERR_FULL;
            uint64_t last_rem = rem_get(tbl, ns - 1, rbits, mask);
            bool last_run_bit = test_bit(run, ns - 1);
            for (uint64_t i = ns - 1; i > ins_pos; i--)
                move_slot_right(rf, i, i - 1);
            for (uint64_t i = n; i > 0; i--)
                move_slot_right(rf, i, i - 1);
            rem_set(tbl, 0, rbits, mask, last_rem);
            track_phys_occ(rf, 0, last_rem);
            if (last_run_bit) set_bit(run, 0); else clear_bit(run, 0);
        }

        /* Create the new run at ins_pos (preserving runend SELECT order). */
        rem_set(tbl, ins_pos, rbits, mask, rem);
        track_phys_occ(rf, ins_pos, rem);
        set_bit(occ, q);
        set_bit(run, ins_pos);
        rf->count++;
        /* Wraparound shifts touch slot 0 → full recompute;
         * otherwise only prefixes/offsets from the min affected block. */
        {
            uint64_t fb = (ins_pos > n) ? 0
                : ((q < ins_pos ? q : ins_pos) / RSQF_SLOTS_PER_BLOCK);
            update_offsets_from(rf, fb);
        }
        return RSQF_OK;
    }

    /* has_run is true — extend an existing run for quotient q */
    uint64_t s = rend + 1;
    if (s >= rf->num_slots) s = 0; /* wraparound */

    /* Find the first free slot starting from s (may wrap).
     * Uses metadata-based emptiness instead of data sentinel,
     * allowing remainder value 0 as a valid stored fingerprint.
     *
     * Edge case: if the second loop reaches scan_start, we return FULL.
     * In a single-threaded filter this cannot falsely fire because
     * no concurrent modification can free scan_start during the scan. */
    uint64_t n = s;
    if (n >= rf->num_slots) n = 0; /* wraparound */
    {
        uint64_t scan_start = n;
        while (n < rf->num_slots && !slot_is_free(rf, n))
            n++;
        if (n >= rf->num_slots) {
            n = 0;
            while (n < scan_start && !slot_is_free(rf, n))
                n++;
            if (n >= scan_start) return RSQF_ERR_FULL;
        }
    }

    /* When s wraps to 0 (rend == ns-1), inserting at position 0
     * can break SELECT ordering. Reject when:
     *   - occ_was_set: the same run's runend moves from ns-1 (highest
     *     SELECT position) to 0 (lowest), reordering all ranks.
     *   - !occ_was_set and q is not the lowest occ: the new runend
     *     at 0 gets SELECT rank 1 but belongs to a non-lowest q.
     * This applies to ALL branches (n==s, n>s, s>n) because shifting
     * does not fix the ordering — the new run bit at 0 still preempts
     * existing run bits at higher positions in SELECT order. */
    if (s == 0 && rend == rf->num_slots - 1) {
        bool occ_was_set = test_bit(occ, q);
        if (occ_was_set) return RSQF_ERR_FULL;
        uint64_t rk = rank_occ(rf, q);
        if (rk > 0) return RSQF_ERR_FULL;
    }

    if (n == s) {
        bool occ_was_set = test_bit(occ, q);
        rem_set(tbl, s, rbits, mask, rem);
        track_phys_occ(rf, s, rem);
        if (occ_was_set) {
            uint64_t prev_slot = (s == 0) ? rf->num_slots - 1 : s - 1;
            clear_bit(run, prev_slot);
        }
        set_bit(run, s);
        set_bit(occ, q);
        rf->count++;
        update_offsets_from(rf, (q < s ? q : s) / RSQF_SLOTS_PER_BLOCK);
        return RSQF_OK;
    }

    /* Handle shift: move [s, free_slot) circularly right by 1.
     * move_slot_right atomically transfers remainder + runend bit. */
    if (n > s) {
        /* Simple shift: [s, n) -> [s+1, n), all in one contiguous range */
        for (uint64_t i = n; i > s; i--)
            move_slot_right(rf, i, i - 1);
    } else if (s > n) {
        /* Wraparound shift: [s, ns) -> [s+1, ns), then [0, n) -> [0, n+1),
         * with slot ns-1 wrapping to slot 0.  If ns-1 has the runend bit
         * set, the shift would place it at position 0, reordering runends
         * and breaking the linear rank → select mapping — reject. */
        if (test_bit(run, rf->num_slots - 1))
            return RSQF_ERR_FULL;
        uint64_t last_data = rem_get(tbl, rf->num_slots - 1, rbits, mask);
        bool last_run_bit = test_bit(run, rf->num_slots - 1);
        /* First shift [s, ns-1] right by 1 — source is i-1, dest is i.
         * Slot ns-1's original content is saved. */
        for (uint64_t i = rf->num_slots - 1; i > s; i--)
            move_slot_right(rf, i, i - 1);
        /* Shift [0, n-1] right by 1. Then write saved ns-1 data into slot 0
         * using move_slot_right semantics (clears source slot). */
        for (uint64_t i = n; i > 0; i--)
            move_slot_right(rf, i, i - 1);
        rem_set(tbl, 0, rbits, mask, last_data);
        track_phys_occ(rf, 0, last_data);
        if (last_run_bit) set_bit(run, 0); else clear_bit(run, 0);
    }

    /* Write the new fingerprint at s, which is now free */
    rem_set(tbl, s, rbits, mask, rem);
    track_phys_occ(rf, s, rem);
    /*
     * Runend bookkeeping:
     *
     * For an existing occupied quotient (occ[q] already set): the run
     * extends from rend+1 to s. Move the runend from s-1 to s by
     * clearing the old and setting the new.
     *
     * For a new occupied quotient (occ[q] was not set): the entry is
     * appended to the preceding run. The old runend belongs to that
     * preceding run and must NOT be cleared. A new runend at s is
     * added so that SELECT(run, rank(q)) returns the correct slot.
     * This preserves popcount(occ) == popcount(run).
     */
    {
        uint64_t prev_slot = (s == 0) ? rf->num_slots - 1 : s - 1;
        bool occ_was_set = test_bit(occ, q);
        if (occ_was_set) {
            clear_bit(run, prev_slot);
        }
    }
    set_bit(run, s);
    set_bit(occ, q);
    rf->count++;
    {
        uint64_t fb = (s > n) ? 0
            : ((q < s ? q : s) / RSQF_SLOTS_PER_BLOCK);
        update_offsets_from(rf, fb);
    }
    return RSQF_OK;
}

/* ============================================================================
 * Core: Delete (reverse of insert)
 * ============================================================================ */

rsqf_err_t rsqf_filter_delete(rsqf_filter_t *rf, uint64_t hash)
{
    if (!rf || rf->count == 0) return RSQF_ERR_NOT_FOUND;

    uint64_t q = (hash >> rf->remainder_bits) & (rf->num_slots - 1);
    uint64_t rem = hash & rf->remainder_mask;

    uint64_t rstart, rend;
    if (!find_run(rf, q, &rstart, &rend))
        return RSQF_ERR_NOT_FOUND;

    uint64_t *run = run_bits(rf);
    uint64_t *tbl = rem_table(rf);
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;

    bool found = false;
    uint64_t del_pos = 0;
    uint64_t scan_count = (rend >= rstart) ? rend - rstart + 1
                                          : rf->num_slots - rstart + rend + 1;
    for (uint64_t k = 0; k < scan_count; k++) {
        uint64_t idx = (rstart + k) % rf->num_slots;
        if (rem_get(tbl, idx, rbits, mask) == rem) {
            del_pos = idx;
            found = true;
            break;
        }
    }
    if (!found) return RSQF_ERR_NOT_FOUND;

    /* Shift left to fill gap */
    uint64_t shift_count = (rend >= del_pos) ? rend - del_pos
                         : rf->num_slots - del_pos + rend;
    for (uint64_t k = 0; k < shift_count; k++) {
        uint64_t src = (del_pos + k + 1) % rf->num_slots;
        uint64_t dst = (del_pos + k) % rf->num_slots;
        uint64_t v = rem_get(tbl, src, rbits, mask);
        rem_set(tbl, dst, rbits, mask, v);
        move_phys_occ(rf, src, dst);
        if (test_bit(run, src)) {
            set_bit(run, dst);
            clear_bit(run, src);
        }
    }
    rem_set(tbl, rend, rbits, mask, 0);
    clear_bit(phys_occ_bits(rf), rend);
    clear_bit(run, rend);

    uint64_t *occ = occ_bits(rf);
    /* Determine if the run is now empty, and its new runend.
     * We rely on phys_occ, not data values, so rem=0 is treated correctly. */
    bool run_nonempty = false;
    uint64_t last_occupied = rend;
    uint64_t full_len = (rend >= rstart) ? rend - rstart + 1
                     : rf->num_slots - rstart + rend + 1;
    for (uint64_t k = 0; k < full_len; k++) {
        uint64_t idx = (rstart + k) % rf->num_slots;
        if (test_bit(phys_occ_bits_const(rf), idx)) {
            run_nonempty = true;
            last_occupied = idx;
        }
    }

    if (!run_nonempty) {
        clear_bit(occ, q);
        clear_bit(run, rend);
    } else {
        for (uint64_t p = rstart; ; p = (p + 1) % rf->num_slots) {
            clear_bit(run, p);
            if (p == rend) break;
        }
        set_bit(run, last_occupied);
    }
    rf->count--;
    {
        uint64_t fb = (rstart > rend) ? 0
            : ((q < rstart ? q : rstart) / RSQF_SLOTS_PER_BLOCK);
        update_offsets_from(rf, fb);
    }
    return RSQF_OK;
}

/* ============================================================================
 * Statistics
 * ============================================================================ */

size_t rsqf_filter_memory_bytes(const rsqf_filter_t *rf)
{
    if (!rf) return 0;
    uint64_t meta_sz = meta_size(rf);
    uint64_t total_remainder_bits = rf->num_slots * rf->remainder_bits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;
    return sizeof(rsqf_filter_t) + meta_sz + remainder_words * sizeof(uint64_t);
}

double rsqf_filter_estimated_fpr(const rsqf_filter_t *rf)
{
    if (!rf) return 1.0;
    return 1.0 / (double)(1ULL << rf->remainder_bits);
}

/* ============================================================================
 * Validation
 *
 * Checks:
 *   1. popcount(occupieds) == popcount(runends)
 *   2. Every occupied quotient has a non-empty run
 *   3. Prefix sums match slow recomputation
 *   4. No orphaned runends without occupied quotient
 * ============================================================================ */

bool rsqf_filter_validate(const rsqf_filter_t *rf)
{
    if (!rf) return false;
    const uint64_t *occ = occ_bits_const(rf);
    const uint64_t *run = run_bits_const(rf);

    uint64_t occ_pop = 0, run_pop = 0;
    for (uint64_t i = 0; i < rf->num_blocks; i++) {
        occ_pop += __builtin_popcountll(occ[i]);
        run_pop += __builtin_popcountll(run[i]);
    }
    if (occ_pop != run_pop) return false;

    for (uint64_t q = 0; q < rf->num_slots; q++) {
        if (!test_bit(occ, q)) continue;
        uint64_t rstart, rend;
        if (!find_run(rf, q, &rstart, &rend)) return false;
        if (rend >= rf->num_slots) return false;
    }

    return true;
}

/* Debug validate — prints details on failure. Only available in debug builds. */
bool rsqf_filter_validate_debug(const rsqf_filter_t *rf)
{
    if (!rf) { fprintf(stderr, "validate: null\n"); return false; }
    const uint64_t *occ = occ_bits_const(rf);
    const uint64_t *run = run_bits_const(rf);

    uint64_t occ_pop = 0, run_pop = 0;
    for (uint64_t i = 0; i < rf->num_blocks; i++) {
        occ_pop += __builtin_popcountll(occ[i]);
        run_pop += __builtin_popcountll(run[i]);
    }
    if (occ_pop != run_pop) {
        return false;
    }

    for (uint64_t q = 0; q < rf->num_slots; q++) {
        if (!test_bit(occ, q)) continue;
        uint64_t rstart, rend;
        if (!find_run(rf, q, &rstart, &rend)) {
            return false;
        }
        if (rend >= rf->num_slots) {
            return false;
        }
    }
    return true;
}

/* ============================================================================
 * Full offset recomputation (global — not incremental).
 * Returns the number of offsets that changed, or UINT64_MAX on error.
 * Always leaves the filter with correct offsets.
 * ============================================================================ */
uint64_t rsqf_filter_repair_offsets(rsqf_filter_t *rf)
{
    if (!rf) return UINT64_MAX;
    const uint64_t *occ_p = occ_prefix_const(rf);
    uint8_t *off = off_array(rf);
    uint64_t fixed = 0;

    for (uint64_t b = 0; b < rf->num_blocks; b++) {
        uint64_t s = b * RSQF_SLOTS_PER_BLOCK;
        uint64_t o = 0;
        if (occ_p[b + 1] > occ_p[b]) {
            /* First occupied quotient in this block */
            uint64_t end = select_run(rf, occ_p[b] + 1);
            if (end > s) o = end - s;
        }
        uint8_t clamped = (uint8_t)(o > 254 ? 255 : o);
        if (off[b] != clamped) { off[b] = clamped; fixed++; }
    }
    return fixed;
}

/* ============================================================================
 * Hash helpers
 * ============================================================================ */

uint64_t rsqf_quotient(uint64_t hash, uint8_t quotient_bits)
{
    return hash >> (64 - quotient_bits);
}

uint64_t rsqf_remainder(uint64_t hash, uint8_t remainder_bits)
{
    return hash & ((1ULL << remainder_bits) - 1);
}
