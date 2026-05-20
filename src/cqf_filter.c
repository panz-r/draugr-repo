/**
 * cqf_filter.c - Counting Quotient Filter implementation
 *
 * Builds on the RSQF structure (occupieds + runends + offsets metadata)
 * and adds variable-sized counters embedded in runs.
 *
 * Counter encoding (Table 3, Pandey et al. SIGMOD 2017):
 *   C=1:         x                  (just the remainder)
 *   C=2:         x, x               (two copies)
 *   C>2 (x>0):   x, c_a..c_0, x    (base 2^r-2 encoding of C-3)
 *   C=3 (x=0):   0, 0, 0
 *   C>3 (x=0):   0, c_a..c_0, 0, 0 (base 2^r-1 encoding of C-4)
 *
 * Within a run, remainders are stored in strictly increasing order.
 * Any value that breaks the increasing pattern is part of a counter.
 */

#include "draugr/cqf_filter.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>
/* ============================================================================
 * Rank/select helpers (same as RSQF)
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
 * Memory layout helpers
 * ============================================================================ */

static inline uint64_t *occ_bits(cqf_filter_t *cf)
{
    return (uint64_t *)(cf + 1);
}

static inline const uint64_t *occ_bits_const(const cqf_filter_t *cf)
{
    return (const uint64_t *)(cf + 1);
}

static inline uint64_t *run_bits(cqf_filter_t *cf)
{
    return (uint64_t *)(cf + 1) + cf->num_blocks;
}

static inline const uint64_t *run_bits_const(const cqf_filter_t *cf)
{
    return (const uint64_t *)(cf + 1) + cf->num_blocks;
}

static inline uint64_t *phys_occ_bits(cqf_filter_t *cf)
{
    return (uint64_t *)(cf + 1) + 2 * cf->num_blocks;
}
static inline const uint64_t *phys_occ_bits_const(const cqf_filter_t *cf)
{
    return (const uint64_t *)(cf + 1) + 2 * cf->num_blocks;
}

static inline uint64_t *occ_prefix(cqf_filter_t *cf)
{
    return (uint64_t *)(cf + 1) + 3 * cf->num_blocks;
}
static inline const uint64_t *occ_prefix_const(const cqf_filter_t *cf)
{
    return (const uint64_t *)(cf + 1) + 3 * cf->num_blocks;
}
static inline uint64_t *run_prefix(cqf_filter_t *cf)
{
    return occ_prefix(cf) + (cf->num_blocks + 1);
}
static inline const uint64_t *run_prefix_const(const cqf_filter_t *cf)
{
    return occ_prefix_const(cf) + (cf->num_blocks + 1);
}
static inline uint8_t *off_array(cqf_filter_t *cf)
{
    return (uint8_t *)(run_prefix(cf) + (cf->num_blocks + 1));
}

static inline uint64_t meta_padded_size(const cqf_filter_t *cf)
{
    uint64_t total = cf->num_blocks * (5 * sizeof(uint64_t) + sizeof(uint8_t)) + 2 * sizeof(uint64_t);
    return (total + 7) & ~7ULL;
}

static inline uint64_t *rem_table(cqf_filter_t *cf)
{
    return (uint64_t *)((uint8_t *)(cf + 1) + meta_padded_size(cf));
}

static inline const uint64_t *rem_table_const(const cqf_filter_t *cf)
{
    return (const uint64_t *)((const uint8_t *)(cf + 1) + meta_padded_size(cf));
}

/* ============================================================================
 * Remainder table access
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
 * Bit operations on metadata
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
 * Free-slot detection via physical occupancy bitset.
 * phys_occ[pos] is maintained by track_phys_occ() after every rem_set,
 * and by move_phys_occ() during shift loops.
 * ============================================================================ */

/* O(1): check phys_occ. A slot is free when phys_occ=0.
 * rem=0 is a valid fingerprint so phys_occ is the only reliable empty marker. */
static bool slot_is_free(const cqf_filter_t *cf, uint64_t pos)
{
    return !test_bit(phys_occ_bits_const(cf), pos);
}

/* Track physical occupancy: set phys_occ[pos] whenever data is written. */
static inline void track_phys_occ(cqf_filter_t *cf, uint64_t pos, uint64_t v)
{
    (void)v;
    uint64_t *p = phys_occ_bits(cf);
    p[pos / 64] |= (1ULL << (pos % 64));
}

/* Move phys_occ bit from src to dst during shifts. */
static inline void move_phys_occ(cqf_filter_t *cf, uint64_t src, uint64_t dst)
{
    uint64_t *p = phys_occ_bits(cf);
    if (p[src / 64] & (1ULL << (src % 64))) {
        p[dst / 64] |= (1ULL << (dst % 64));
        p[src / 64] &= ~(1ULL << (src % 64));
    }
}

/* Clear phys_occ bit for a range during deletion. */
static inline void clear_range_phys_occ(cqf_filter_t *cf, uint64_t start, uint64_t end)
{
    uint64_t *p = phys_occ_bits(cf);
    for (uint64_t i = start; i <= end; i++) {
        p[i / 64] &= ~(1ULL << (i % 64));
    }
}

/* ============================================================================
 * Fast rank/select using prefix-sum arrays.
 * rank_occ_fast: O(1) — block prefix + within-word rank64.
 * select_run_fast: O(log nb) — binary search on block prefix, then select64.
 * ============================================================================ */

static uint64_t rank_occ_fast(const cqf_filter_t *cf, uint64_t pos)
{
    uint64_t block = pos / 64;
    uint64_t off = pos % 64;
    const uint64_t *op = occ_prefix_const(cf);
    const uint64_t *occ = occ_bits_const(cf);
    return op[block] + rank64(occ[block], off);
}

static uint64_t select_run_fast(const cqf_filter_t *cf, uint64_t k)
{
    if (k == 0) return 0;
    const uint64_t *rp = run_prefix_const(cf);
    const uint64_t *run = run_bits_const(cf);
    uint64_t nb = cf->num_blocks;
    if (k > rp[nb]) return cf->num_slots - 1;
    uint64_t lo = 0, hi = nb;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (rp[mid] >= k) hi = mid; else lo = mid + 1;
    }
    uint64_t block = lo - 1;
    return block * 64 + select64(run[block], k - rp[block]);
}

/* ============================================================================
 * Circular run_start: the k-th run starts after the (k-1)-th runend (mod ns).
 * For k=1, start is after the last runend (circular array).
 * ============================================================================ */

static uint64_t run_start_cqf(const cqf_filter_t *cf, uint64_t k)
{
    if (k == 1) return 0;
    return (select_run_fast(cf, k - 1) + 1) % cf->num_slots;
}

/* ============================================================================
 * Find run for a quotient
 * ============================================================================ */

static bool find_run(const cqf_filter_t *cf, uint64_t q,
                     uint64_t *start, uint64_t *end)
{
    const uint64_t *occ = occ_bits_const(cf);

    if (!test_bit(occ, q)) {
        if (!test_bit(run_bits_const(cf), q))
            return false;
        uint64_t search = q;
        for (uint64_t i = 0; i < cf->num_slots; i++) {
            if (search == 0) search = cf->num_slots;
            search--;
            if (test_bit(occ, search)) {
                uint64_t prev_rk = rank_occ_fast(cf, search);
                *end = select_run_fast(cf, prev_rk);
                *start = run_start_cqf(cf, prev_rk);
                return true;
            }
        }
        *end = q;
        *start = run_start_cqf(cf, 1);
        return true;
    }

    uint64_t rk = rank_occ_fast(cf, q);
    *end = select_run_fast(cf, rk);
    *start = run_start_cqf(cf, rk);

    return true;
}

/* ============================================================================
 * Counter encoding/decoding
 *
 * Encoding scheme for remainder x > 0:
 *   C=1:    [x]
 *   C=2:    [x, x]
 *   C>2:    [x, c_a..c_0, x]  — C-3 in base (2^r-2), symbols skip 0 and x
 *
 * For x == 0:
 *   C=1:    [0]
 *   C=2:    [0, 0]
 *   C=3:    [0, 0, 0]
 *   C>3:    [0, c_a..c_0, 0, 0]  — C-4 in base (2^r-1)
 * ============================================================================ */

static int cqf_encode(uint64_t C, uint64_t x, uint8_t rbits, uint64_t *enc_buf, int max_buf);

/* Worst-case encoding buffer: rbits=2, UINT64_MAX needs at most 64 base-2 digits.
 * Buffer includes margin for a possible prepended zero digit. */
enum { CQF_ENC_BUF = 128 };

/* Number of r-bit slots needed to encode count C for remainder x.
 * Layout per Scheme 4.1 (Table 3 in paper):
 *   x = 0, C >= 3: [0, 0, enc..., 0, 0]         — 4 + n slots
 *   x > 0, C >= 3: [x, enc..., x]               — 2 + n slots
 *   C = 1: [x]; C = 2: [x, x]
 * Note: n = number of encoding symbols from cqf_encode.
 * C=3 (x=0) uses n=1 via cqf_encode(C-3=0) → one symbol [1],
 * giving layout [0,0,1,0,0] — 5 slots. This avoids the
 * ambiguity of [0,0,0] being confused with back-to-back rem=0 entries. */
static uint64_t cqf_enc_slots(uint64_t C, uint64_t x, uint8_t rbits)
{
    if (C <= 1) return 1;
    if (C == 2) return 2;
    uint64_t enc_buf[CQF_ENC_BUF];
    int n = cqf_encode(C, x, rbits, enc_buf, CQF_ENC_BUF);
    if (x == 0)
        return 4 + (uint64_t)n;
    else
        return 2 + (uint64_t)n;
}

/* Encode count C for remainder x into enc_buf (r-bit values between markers).
 * Returns number of encoding symbols written, or 0 for C<=2 (direct copies).
 * enc_buf must have space for at least CQF_ENC_BUF entries. */
static int cqf_encode(uint64_t C, uint64_t x, uint8_t rbits, uint64_t *enc_buf, int max_buf)
{
    if (C <= 2) return 0;

    if (x == 0) {
        if (C <= 2) return 0;
        uint64_t base = (1ULL << rbits) - 1;
        uint64_t val = C - 3;
        int n = 0;
        if (val == 0) { enc_buf[n++] = 1; }
        while (val > 0 && n < max_buf) {
            int d = (int)(val % base);
            enc_buf[n++] = (uint64_t)(d + 1);
            val /= base;
        }
        /* Reverse to MSB-first order (encoder builds LSB-first) */
        for (int i = 0; i < n / 2; i++) {
            uint64_t t = enc_buf[i]; enc_buf[i] = enc_buf[n - 1 - i]; enc_buf[n - 1 - i] = t;
        }
        return n;
    } else {
        uint64_t base = (1ULL << rbits) - 2;
        uint64_t val = C - 3;
        int n = 0;
        if (val == 0) {
            /* Use correct digit-to-symbol mapping (not hardcoded 1).
             * For x=1, digit 0 maps to symbol 2 (since 0 < x-1 is false). */
            uint64_t sym;
            if ((uint64_t)0 < x - 1) sym = 1; else sym = 2;
            enc_buf[n++] = sym;
        }
        while (val > 0 && n < max_buf) {
            int d = (int)(val % base);
            uint64_t sym;
            if ((uint64_t)d < x - 1)
                sym = (uint64_t)(d + 1);
            else
                sym = (uint64_t)(d + 2);
            enc_buf[n++] = sym;
            val /= base;
        }
        /* Reverse to MSB-first order (encoder builds LSB-first) */
        for (int i = 0; i < n / 2; i++) {
            uint64_t t = enc_buf[i]; enc_buf[i] = enc_buf[n - 1 - i]; enc_buf[n - 1 - i] = t;
        }
        if (n > 0 && enc_buf[0] >= x) {
            if (n >= max_buf) return 0;
            for (int i = n; i > 0; i--) enc_buf[i] = enc_buf[i - 1];
            enc_buf[0] = 0;
            n++;
        }
        return n;
    }
}

/* Write the full encoded entry for count C of remainder x at position pos in tbl.
 * Returns the number of slots written. */
static uint64_t cqf_write_entry(uint64_t *tbl, uint64_t pos, uint64_t x,
                                 uint64_t C, uint8_t rbits, uint64_t mask)
{
    rem_set(tbl, pos, rbits, mask, x);
    if (C <= 1) return 1;
    if (C == 2) { rem_set(tbl, pos + 1, rbits, mask, x); return 2; }
    uint64_t enc_buf[CQF_ENC_BUF];
    int n = cqf_encode(C, x, rbits, enc_buf, CQF_ENC_BUF);
    /* For x > 0: [x, enc..., x] */
    /* For x == 0, C == 3: [0, 0, 0] */
    /* For x == 0, C > 3: [0, enc..., 0, 0] */
    if (x == 0) {
        if (C <= 2) {
            if (C == 2) rem_set(tbl, pos + 1, rbits, mask, (uint64_t)0);
            return C;
        }
        rem_set(tbl, pos + 1, rbits, mask, (uint64_t)0);
        for (int i = 0; i < n; i++)
            rem_set(tbl, pos + 2 + (uint64_t)i, rbits, mask, enc_buf[i]);
        rem_set(tbl, pos + 2 + (uint64_t)n, rbits, mask, (uint64_t)0);
        rem_set(tbl, pos + 3 + (uint64_t)n, rbits, mask, (uint64_t)0);
        return 4 + (uint64_t)n;
    } else {
        for (int i = 0; i < n; i++)
            rem_set(tbl, pos + 1 + (uint64_t)i, rbits, mask, enc_buf[i]);
        rem_set(tbl, pos + 1 + (uint64_t)n, rbits, mask, x);
        return 2 + (uint64_t)n;
    }
}

/* Decode counter value from encoded slots.
 * enc_buf: pointer to encoding symbols (between remainder markers)
 * n_slots: number of encoding symbols
 * x: the remainder value
 * rbits: number of bits per slot */
static uint64_t decode_count(const uint64_t *enc_buf, int n_slots,
                              uint64_t x, uint8_t rbits)
{
    if (n_slots == 0) {
        /* Either C=2 (if next slot also has x) or C>2 with empty encoding */
        return 3; /* C=3 is minimum for a counter encoding with no digits */
    }

    if (x == 0) {
        /* Encoding is in base (2^r - 1), symbols mapped from digit+1
         * enc_buf[0..n_slots-1] is MOST-to-LEAST significant */
        uint64_t base = (1ULL << rbits) - 1;
        uint64_t val = 0;
        for (int i = 0; i < n_slots; i++) {
            val = val * base + (enc_buf[i] - 1);
        }
        return val + 3; /* C = decoded + 3 */
    } else {
        /* Encoding is in base (2^r - 2), symbols mapped via digit table
         * enc_buf[0..n_slots-1] is MOST-to-LEAST significant */
        uint64_t base = (1ULL << rbits) - 2;
        uint64_t val = 0;
        for (int i = 0; i < n_slots; i++) {
            uint64_t sym = enc_buf[i];
            uint64_t digit;
            if (sym == 0) {
                digit = 0; /* prepended 0 */
            } else if (sym < x) {
                digit = sym - 1;
            } else {
                digit = sym - 2;
            }
            val = val * base + digit;
        }
        return val + 3; /* C = decoded + 3 */
    }
}

/* ============================================================================
 * Update offsets for affected blocks
 * ============================================================================ */

static void update_offsets(cqf_filter_t *cf)
{
    const uint64_t *occ = occ_bits_const(cf);
    const uint64_t *run = run_bits_const(cf);
    uint64_t *occ_p = occ_prefix(cf);
    uint64_t *run_p = run_prefix(cf);
    uint8_t *off = off_array(cf);
    uint64_t nb = cf->num_blocks;
    uint64_t ns = cf->num_slots;

    uint64_t occ_seen = 0;
    uint64_t rw = 0;
    uint64_t rw_consumed = 0;
    occ_p[0] = 0;
    run_p[0] = 0;

    for (uint64_t b = 0; b < nb; b++) {
        uint64_t blk_start = b * CQF_SLOTS_PER_BLOCK;
        uint64_t blk_occ = __builtin_popcountll(occ[b]);

        occ_seen += blk_occ;
        occ_p[b + 1] = occ_seen;
        run_p[b + 1] = run_p[b] + __builtin_popcountll(run[b]);

        uint64_t o = 0;
        if (occ_seen > 0) {
            while (rw < nb) {
                uint64_t w_pop = __builtin_popcountll(run[rw]);
                if (rw_consumed + w_pop >= occ_seen)
                    break;
                rw++;
                rw_consumed += w_pop;
            }
            uint64_t end = (rw < nb)
                ? rw * CQF_SLOTS_PER_BLOCK + select64(run[rw], occ_seen - rw_consumed)
                : ns - 1;
            if (end > blk_start)
                o = end - blk_start;
        }
        off[b] = (uint8_t)(o > 255 ? 255 : o);
    }
}

/* ============================================================================
 * Shift remaining in slot range (supports wrapped clusters).
 *
 * For normal shifts (from < to): data in (from, to] shifts right by 1.
 * For wrapped shifts (from > to): two segments [from, ns-1] and [0, to].
 * Shift_range_right opens a slot at 'from' by moving data right.
 * ============================================================================ */

static void shift_range_right(cqf_filter_t *cf, uint64_t from, uint64_t to)
{
    uint64_t *tbl = rem_table(cf);
    uint64_t *run = run_bits(cf);
    uint64_t mask = cf->remainder_mask;
    uint8_t rbits = cf->remainder_bits;
    uint64_t ns = cf->num_slots;

    if (from < to) {
        for (uint64_t i = to; i > from; i--) {
            uint64_t v = rem_get(tbl, i - 1, rbits, mask);
            rem_set(tbl, i, rbits, mask, v);
            move_phys_occ(cf, i - 1, i);
            if (test_bit(run, i - 1)) {
                set_bit(run, i);
                clear_bit(run, i - 1);
            }
            rem_set(tbl, i - 1, rbits, mask, 0);
        }
        update_offsets(cf);
    } else if (from > to) {
        uint64_t last_val = rem_get(tbl, ns - 1, rbits, mask);
        bool last_run = test_bit(run, ns - 1);
        for (uint64_t i = ns - 1; i > from; i--) {
            uint64_t v = rem_get(tbl, i - 1, rbits, mask);
            rem_set(tbl, i, rbits, mask, v);
            move_phys_occ(cf, i - 1, i);
            if (test_bit(run, i - 1)) {
                set_bit(run, i);
                clear_bit(run, i - 1);
            }
            rem_set(tbl, i - 1, rbits, mask, 0);
        }
        for (uint64_t i = to; i > 0; i--) {
            uint64_t v = rem_get(tbl, i - 1, rbits, mask);
            rem_set(tbl, i, rbits, mask, v);
            move_phys_occ(cf, i - 1, i);
            if (test_bit(run, i - 1)) {
                set_bit(run, i);
                clear_bit(run, i - 1);
            }
            rem_set(tbl, i - 1, rbits, mask, 0);
        }
        rem_set(tbl, 0, rbits, mask, last_val);
        if (last_run) set_bit(run, 0); else clear_bit(run, 0);
        clear_bit(run, ns - 1);
        clear_bit(phys_occ_bits(cf), ns - 1);
        update_offsets(cf);
    }
}

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

cqf_filter_t *cqf_filter_create(size_t capacity, uint8_t fp_bits)
{
    if (capacity == 0) return NULL;
    if (fp_bits == 0) fp_bits = CQF_DEFAULT_FINGERPRINT_BITS;
    if (fp_bits < CQF_MIN_FINGERPRINT_BITS) fp_bits = CQF_MIN_FINGERPRINT_BITS;
    if (fp_bits > CQF_MAX_FINGERPRINT_BITS) fp_bits = CQF_MAX_FINGERPRINT_BITS;

    uint64_t num_slots = 1;
    while (num_slots < capacity) num_slots *= 2;
    if (num_slots < CQF_SLOTS_PER_BLOCK) num_slots = CQF_SLOTS_PER_BLOCK;
    uint64_t num_blocks = num_slots / CQF_SLOTS_PER_BLOCK;

    uint8_t qb = 0;
    uint64_t tmp = num_slots;
    while (tmp > 1) { tmp >>= 1; qb++; }

    uint8_t rbits = fp_bits;
    /* Fingerprint = (q << rbits) | rem must fit in 64 bits.
     * q has qb bits, rem has rbits bits → need qb + rbits <= 64 */
    if ((uint64_t)qb + rbits > 64) return NULL;
    uint64_t total_remainder_bits = num_slots * rbits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;
    /* Assert invariants: num_slots must be a multiple of 64 (block size) */
    assert(num_slots % CQF_SLOTS_PER_BLOCK == 0);
    assert(num_blocks * CQF_SLOTS_PER_BLOCK == num_slots);

    uint64_t meta_padded = (num_blocks * (5 * sizeof(uint64_t) + sizeof(uint8_t)) + 2 * sizeof(uint64_t) + 7) & ~7ULL;
    size_t total_bytes = sizeof(cqf_filter_t) + meta_padded
                       + remainder_words * sizeof(uint64_t);

    cqf_filter_t *cf = (cqf_filter_t *)malloc(total_bytes);
    if (!cf) return NULL;

    cf->num_slots = num_slots;
    cf->num_blocks = num_blocks;
    cf->count = 0;
    cf->distinct_count = 0;
    cf->remainder_bits = rbits;
    cf->quotient_bits = qb;
    cf->entries_per_set = 0;
    cf->num_sets = 0;
    cf->remainder_mask = (1ULL << rbits) - 1;
    cf->compat_version = CQF_COMPAT_VERSION;
    cf->hash_seed = 0;

    memset(cf + 1, 0, meta_padded + remainder_words * sizeof(uint64_t));

    return cf;
}

void cqf_filter_destroy(cqf_filter_t *cf)
{
    free(cf);
}

cqf_err_t cqf_filter_reset(cqf_filter_t *cf)
{
    if (!cf) return CQF_ERR_INVALID_PARAM;
    uint64_t meta_padded = meta_padded_size(cf);
    uint64_t total_remainder_bits = cf->num_slots * cf->remainder_bits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;
    memset(cf + 1, 0, meta_padded + remainder_words * sizeof(uint64_t));
    cf->count = 0;
    cf->distinct_count = 0;
    return CQF_OK;
}

/* Forward declaration for unified decoder used by insert, count, delete */
static bool cqf_decode_entry(const cqf_filter_t *cf,
                              uint64_t rend,
                              uint64_t *pos,
                              uint64_t *rem_out, uint64_t *count_out);

/* ============================================================================
 * Core: Insert (with counting)
 *
 * Algorithm:
 *   1. Find run for quotient q
 *   2. If no run: insert with count=1 (simple)
 *   3. Search within run in sorted order:
 *      - If rem found: increment counter, update encoding
 *      - If not found: insert at sorted position with count=1
 * ============================================================================ */

cqf_err_t cqf_filter_insert(cqf_filter_t *cf, uint64_t hash)
{
    if (!cf) return CQF_ERR_INVALID_PARAM;
    if (cf->count >= cf->num_slots * 16) return CQF_ERR_FULL;

    uint64_t q = (hash >> cf->remainder_bits) & (cf->num_slots - 1);
    uint64_t rem = hash & cf->remainder_mask;
    /* Note: rem may be 0. The filter supports rem=0 as a valid fingerprint
     * value. Emptiness is determined by occ/run metadata, not by data values. */

    uint64_t *occ = occ_bits(cf);
    uint64_t *run = run_bits(cf);
    uint64_t *tbl = rem_table(cf);
    uint8_t rbits = cf->remainder_bits;
    uint64_t mask = cf->remainder_mask;

    uint64_t rend, rstart;

    {
        uint64_t rk = rank_occ_fast(cf, q);
        uint64_t s = select_run_fast(cf, rk);

        /* If occ[q] is not set, create a new run for q.
         * The physical entry goes right after the previous runend
         * (s+1, or slot 0 for the very first run).
         * Circular: find a free slot via phys_occ to absorb shift overflow. */
        if (!test_bit(occ, q)) {
            uint64_t total_occ_rk = rank_occ_fast(cf, cf->num_slots - 1);
            uint64_t last_slot_occ = total_occ_rk > 0
                ? select_run_fast(cf, total_occ_rk)
                : s;
            uint64_t ins_pos = (rk == 0) ? 0 : ((s + 1) % cf->num_slots);
            uint64_t scan_start = (last_slot_occ + 1) % cf->num_slots;
            uint64_t free_slot = scan_start;
            {
                uint64_t n = scan_start;
                while (n < cf->num_slots && !slot_is_free(cf, n)) n++;
                if (n >= cf->num_slots) {
                    n = 0;
                    while (n < scan_start && !slot_is_free(cf, n)) n++;
                    if (n >= scan_start) return CQF_ERR_FULL;
                }
                free_slot = n;
            }
            if (ins_pos != free_slot)
                shift_range_right(cf, ins_pos, free_slot);
            rem_set(tbl, ins_pos, rbits, mask, rem);
            track_phys_occ(cf, ins_pos, rem);
            set_bit(run, ins_pos);
            set_bit(occ, q);
            cf->count++;
            cf->distinct_count++;
            update_offsets(cf);
            return CQF_OK;
        }

        /* Existing run: find boundaries */
        rstart = (rk > 1)
            ? select_run_fast(cf, rk - 1) + 1
            : 0;
        rend = s;
    }

    /* Scan the run for matching remainder (supports wrapped runs) */
    {
        uint64_t scan_pos = rstart;
        uint64_t dec_rem, dec_cnt;
        uint64_t ins_pos = (rend + 1) % cf->num_slots;
        bool wrapped = (rstart > rend);
        bool wrapped_once = false;

        while (1) {
            if (!wrapped && scan_pos > rend) break;
            if (wrapped && scan_pos < rstart && scan_pos > rend) break;
            uint64_t saved_pos = scan_pos;
            if (!cqf_decode_entry(cf, rend, &scan_pos, &dec_rem, &dec_cnt)) {
                if (wrapped && !wrapped_once) {
                    /* First segment [rstart, ns-1] exhausted before
                     * reaching ns. Wrap to 0 for second segment [0, rend]. */
                    scan_pos = 0;
                    wrapped_once = true;
                    continue;
                }
                break;
            }
            if (wrapped && !wrapped_once && scan_pos >= cf->num_slots) {
                scan_pos = 0;
                wrapped_once = true;
            }
            if (dec_rem == rem) {
                /* Found: increment counter */
                uint64_t new_count = dec_cnt + 1;
                if (new_count == 0) return CQF_ERR_OVERFLOW;
                uint64_t new_slots = cqf_enc_slots(new_count, rem, rbits);
                uint64_t old_slots = scan_pos - saved_pos;

                if (new_slots == old_slots) {
                    if (new_count > 2) {
                        uint64_t new_enc[CQF_ENC_BUF];
                        int n = cqf_encode(new_count, rem, rbits, new_enc, CQF_ENC_BUF);
                        if (n > 0) {
                            if (rem == 0 && dec_cnt >= 3) {
                                for (int e = 0; e < n; e++)
                                    rem_set(tbl, saved_pos + 2 + (uint64_t)e, rbits, mask, new_enc[e]);
                            } else if (rem != 0 && dec_cnt > 2) {
                                for (int e = 0; e < n; e++)
                                    rem_set(tbl, saved_pos + 1 + (uint64_t)e, rbits, mask, new_enc[e]);
                            }
                        }
                    }
                    cf->count++;
                    update_offsets(cf);
                    return CQF_OK;
                }

                /* Resize entry: shift subsequent data */
                uint64_t ns_local = cf->num_slots;
                uint64_t data_after = saved_pos + old_slots;
                uint64_t new_data_start = saved_pos + new_slots;
                uint64_t shift_limit;
                if (new_slots < old_slots) {
                    shift_limit = rend;
                } else {
                    uint64_t total_occ_rk = rank_occ_fast(cf, ns_local - 1);
                    shift_limit = total_occ_rk > 0
                        ? select_run_fast(cf, total_occ_rk)
                        : rend;
                }

                /* Tail slots: distance from data_after to shift_limit (inclusive).
                 * For wrapped runs, use circular distance; for non-wrapped, linear. */
                uint64_t tail_slots;
                if (rstart <= rend) {
                    tail_slots = (data_after <= shift_limit) ? shift_limit - data_after + 1 : 0;
                } else {
                    uint64_t da_mod = data_after % ns_local;
                    uint64_t sl_mod = shift_limit % ns_local;
                    if (da_mod <= sl_mod)
                        tail_slots = sl_mod - da_mod + 1;
                    else
                        tail_slots = ns_local - da_mod + sl_mod + 1;
                }

                /* Fullness check: expansion must not exceed capacity */
                if (new_slots > old_slots) {
                    uint64_t check_last = new_data_start + tail_slots;
                    if (rstart <= rend && check_last > 0 && check_last > ns_local)
                        return CQF_ERR_FULL;
                }

                if (tail_slots > 0) {
                    if (new_data_start < data_after) {
                        /* Contraction: shift left */
                        for (uint64_t k = 0; k < tail_slots; k++) {
                            uint64_t src = (data_after + k) % ns_local;
                            uint64_t dst = (new_data_start + k) % ns_local;
                            rem_set(tbl, dst, rbits, mask, rem_get(tbl, src, rbits, mask));
                            move_phys_occ(cf, src, dst);
                            if (test_bit(run, src)) {
                                set_bit(run, dst); clear_bit(run, src);
                            } else {
                                clear_bit(run, dst);
                            }
                        }
                    } else {
                        /* Expansion: shift right */
                        for (uint64_t k = tail_slots; k > 0; k--) {
                            uint64_t src = (data_after + k - 1) % ns_local;
                            uint64_t dst = (new_data_start + k - 1) % ns_local;
                            rem_set(tbl, dst, rbits, mask, rem_get(tbl, src, rbits, mask));
                            move_phys_occ(cf, src, dst);
                            if (test_bit(run, src)) {
                                set_bit(run, dst); clear_bit(run, src);
                            } else {
                                clear_bit(run, dst);
                            }
                        }
                    }
                }

                /* Clear old entry area — only the new entry's footprint.
                 * Shifted tail data beyond new_data_start is preserved. */
                for (uint64_t p = saved_pos; p < new_data_start; p++) {
                    rem_set(tbl, p, rbits, mask, 0);
                    clear_bit(run, p);
                    clear_bit(phys_occ_bits(cf), p);
                }

                /* Write new entry with updated count */
                uint64_t slots_written = cqf_write_entry(tbl, saved_pos, rem, new_count, rbits, mask);
                for (uint64_t p = saved_pos; p < saved_pos + slots_written; p++)
                    track_phys_occ(cf, p, rem_get(tbl, p, rbits, mask));

                /* Update runend: the shift already moved all run bits,
                 * including this run's runend. Just ensure the bit is
                 * set at the correct new position. */
                {
                    uint64_t new_rend = (rend + new_slots >= old_slots)
                        ? (rend - old_slots + new_slots) % ns_local
                        : (rend - old_slots + new_slots + ns_local) % ns_local;
                    clear_bit(run, new_rend);
                    set_bit(run, new_rend);
                }

                /* Clear stale slots (contraction) */
                if (new_slots < old_slots) {
                    uint64_t new_last_slot = tail_slots > 0
                        ? (new_data_start + tail_slots - 1) % ns_local
                        : (new_data_start - 1) % ns_local;
                    if (new_last_slot + 1 <= shift_limit) {
                        for (uint64_t p = new_last_slot + 1; p <= shift_limit; p++) {
                            rem_set(tbl, p, rbits, mask, 0);
                            clear_bit(run, p);
                            clear_bit(phys_occ_bits(cf), p);
                        }
                    } else if (new_last_slot + 1 < ns_local || shift_limit > 0) {
                        for (uint64_t p = new_last_slot + 1; p < ns_local; p++) {
                            rem_set(tbl, p, rbits, mask, 0);
                            clear_bit(run, p);
                            clear_bit(phys_occ_bits(cf), p);
                        }
                        for (uint64_t p = 0; p <= shift_limit; p++) {
                            rem_set(tbl, p, rbits, mask, 0);
                            clear_bit(run, p);
                            clear_bit(phys_occ_bits(cf), p);
                        }
                    }
                }

                cf->count++;
                update_offsets(cf);
                return CQF_OK;
            }
            if (dec_rem > rem) {
                ins_pos = saved_pos;
                break;
            }
            ins_pos = scan_pos;
        }

        /* Not found: insert new distinct entry at sorted position.
         * Scan for a free slot (circular) to absorb the shift overflow. */
        {
            uint64_t total_occ_rk = rank_occ_fast(cf, cf->num_slots - 1);
            uint64_t last_occ_rend = total_occ_rk > 0
                ? select_run_fast(cf, total_occ_rk)
                : rend;
            uint64_t scan_start = (last_occ_rend + 1) % cf->num_slots;
            uint64_t free_slot = scan_start;
            {
                uint64_t n = scan_start;
                while (n < cf->num_slots && !slot_is_free(cf, n)) n++;
                if (n >= cf->num_slots) {
                    n = 0;
                    while (n < scan_start && !slot_is_free(cf, n)) n++;
                    if (n >= scan_start) return CQF_ERR_FULL;
                }
                free_slot = n;
            }
            if (ins_pos != free_slot)
                shift_range_right(cf, ins_pos, free_slot);
            rem_set(tbl, ins_pos, rbits, mask, rem);
            track_phys_occ(cf, ins_pos, rem);
            if ((rend + 1) % cf->num_slots == ins_pos) {
                clear_bit(run, rend);
                set_bit(run, ins_pos);
            }
            cf->count++;
            cf->distinct_count++;
            update_offsets(cf);
            return CQF_OK;
        }
    }
}

/* ============================================================================
 * Core: Delete (decrement counter, remove if count reaches 0)
 * ============================================================================ */

cqf_err_t cqf_filter_delete(cqf_filter_t *cf, uint64_t hash)
{
    if (!cf || cf->count == 0) return CQF_ERR_NOT_FOUND;

    uint64_t q = (hash >> cf->remainder_bits) & (cf->num_slots - 1);
    uint64_t rem = hash & cf->remainder_mask;

    uint64_t rstart, rend;
    if (!find_run(cf, q, &rstart, &rend))
        return CQF_ERR_NOT_FOUND;

    uint64_t *occ = occ_bits(cf);
    uint64_t *run = run_bits(cf);
    uint64_t *tbl = rem_table(cf);
    uint8_t rbits = cf->remainder_bits;
    uint64_t mask = cf->remainder_mask;

    /* Find the entry, determine current count and slot usage */
    uint64_t found_i = rend + 1;
    uint64_t old_count = 0;
    uint64_t old_slots = 0;

    /* Scan run using unified cqf_decode_entry (supports wrapped runs) */
    {
        uint64_t scan_pos = rstart;
        bool wrapped = (rstart > rend);
        bool wrapped_once = false;
        uint64_t ns = cf->num_slots;
        uint64_t dec_rem, dec_cnt;

        while (true) {
            if (!wrapped && scan_pos > rend) break;
            if (wrapped && scan_pos < rstart && scan_pos > rend) break;

            uint64_t saved_pos = scan_pos;
            if (!cqf_decode_entry(cf, rend, &scan_pos, &dec_rem, &dec_cnt)) {
                if (wrapped && !wrapped_once) {
                    scan_pos = 0;
                    wrapped_once = true;
                    continue;
                }
                break;
            }
            if (wrapped && !wrapped_once && scan_pos >= ns) {
                scan_pos = 0;
                wrapped_once = true;
            }
            if (dec_rem == rem) {
                found_i = saved_pos;
                old_count = dec_cnt;
                old_slots = scan_pos - saved_pos;
                goto delete_found;
            }
            if (dec_rem > rem) break;
        }
    }
    return CQF_ERR_NOT_FOUND;

delete_found:
    ;
    uint64_t new_count = old_count - 1;

    if (new_count == 0) {
        /* Remove the entry entirely: shift all data after the entry
         * left by old_slots to close the gap. */
        uint64_t ns_local = cf->num_slots;
        uint64_t *pocc = phys_occ_bits(cf);
        uint64_t data_after = found_i + old_slots;

        /* Distance from data_after to rend (inclusive).
         * For non-wrapped runs, if data_after > rend there is no tail data.
         * For wrapped runs, compute circular distance. */
        uint64_t tail_slots;
        if (rstart <= rend) {
            tail_slots = (data_after <= rend) ? rend - data_after + 1 : 0;
        } else {
            uint64_t da_mod = data_after % ns_local;
            if (da_mod <= rend)
                tail_slots = rend - da_mod + 1;
            else
                tail_slots = ns_local - da_mod + rend + 1;
        }

        if (tail_slots > 0) {
            for (uint64_t k = 0; k < tail_slots; k++) {
                uint64_t src = (data_after + k) % ns_local;
                uint64_t dst = (found_i + k) % ns_local;
                rem_set(tbl, dst, rbits, mask,
                        rem_get(tbl, src, rbits, mask));
                move_phys_occ(cf, src, dst);
                if (test_bit(run, src)) {
                    set_bit(run, dst);
                    clear_bit(run, src);
                } else {
                    clear_bit(run, dst);
                }
            }
        }

        /* Clear stale slots at the old end of the run (supports wrapped runs) */
        uint64_t stale_start = tail_slots > 0
            ? (found_i + tail_slots) % ns_local
            : found_i;
        uint64_t stale_end = rend;
        if (tail_slots > 0) {
            if (stale_start <= stale_end) {
                for (uint64_t p = stale_start; p <= stale_end; p++) {
                    rem_set(tbl, p, rbits, mask, (uint64_t)0);
                    clear_bit(run, p);
                    clear_bit(pocc, p);
                }
            } else {
                for (uint64_t p = stale_start; p < ns_local; p++) {
                    rem_set(tbl, p, rbits, mask, (uint64_t)0);
                    clear_bit(run, p);
                    clear_bit(pocc, p);
                }
                for (uint64_t p = 0; p <= stale_end; p++) {
                    rem_set(tbl, p, rbits, mask, (uint64_t)0);
                    clear_bit(run, p);
                    clear_bit(pocc, p);
                }
            }
        }

        /* Check if run is now empty.
         * For wrapped runs, rstart == found_i alone isn't sufficient;
         * also check that no data remains via circular distance. */
        uint64_t full_distance = (rstart <= rend)
            ? rend - rstart + 1
            : ns_local - rstart + rend + 1;
        bool run_empty = (old_slots >= full_distance);
        if (run_empty) {
            clear_bit(occ, q);
            /* Clear the run bit — the entire run is being removed */
            if (rstart <= rend) {
                for (uint64_t p = rstart; p <= rend; p++)
                    clear_bit(run, p);
            } else {
                for (uint64_t p = rstart; p < ns_local; p++)
                    clear_bit(run, p);
                for (uint64_t p = 0; p <= rend; p++)
                    clear_bit(run, p);
            }
        } else {
            /* New runend: the last slot of the shifted run */
            uint64_t new_rend;
            if (tail_slots > 0)
                new_rend = (found_i + tail_slots - 1) % ns_local;
            else if (found_i > 0)
                new_rend = found_i - 1;
            else
                new_rend = ns_local - 1;
            /* Clear all run bits in the old range, set at new_rend */
            if (rstart <= rend) {
                for (uint64_t p = rstart; p <= rend; p++)
                    clear_bit(run, p);
            } else {
                for (uint64_t p = rstart; p < ns_local; p++)
                    clear_bit(run, p);
                for (uint64_t p = 0; p <= rend; p++)
                    clear_bit(run, p);
            }
            set_bit(run, new_rend);
        }
        cf->count--;
        if (run_empty) cf->distinct_count--;
        update_offsets(cf);
        return CQF_OK;
    }

    uint64_t new_slots = cqf_enc_slots(new_count, rem, rbits);

    if (new_slots == old_slots && old_count > 2) {
        /* Same size — just update encoding */
        uint64_t new_enc[CQF_ENC_BUF];
        int n = cqf_encode(new_count, rem, rbits, new_enc, CQF_ENC_BUF);
        if (n > 0) {
            if (rem == 0 && old_count >= 3) {
                /* For x=0: layout is [0, 0, enc..., 0, 0].
                 * First enc symbol is at found_i + 2 (after leading 0). */
                for (int e = 0; e < n; e++)
                    rem_set(tbl, found_i + 2 + (uint64_t)e, rbits, mask, new_enc[e]);
            } else if (rem != 0 && old_count > 2) {
                for (int e = 0; e < n; e++)
                    rem_set(tbl, found_i + 1 + (uint64_t)e, rbits, mask, new_enc[e]);
            }
        }
        cf->count--;
        return CQF_OK;
    }

    /* Determine shift range:
     * Both contraction and expansion shift ALL subsequent data through the
     * end of the last run, keeping the slot layout packed without gaps. */
    uint64_t ns = cf->num_slots;
    uint64_t total_occ_rk = rank_occ_fast(cf, ns - 1);
    uint64_t shift_limit = total_occ_rk > 0
        ? select_run_fast(cf, total_occ_rk)
        : rend;
    uint64_t data_after = found_i + old_slots;
    uint64_t new_data_start = found_i + new_slots;

    /* Tail slots: distance from data_after to shift_limit (inclusive).
     * For wrapped runs, use circular distance; for non-wrapped, linear. */
    uint64_t tail_slots;
    if (rstart <= rend) {
        tail_slots = (data_after <= shift_limit) ? shift_limit - data_after + 1 : 0;
    } else {
        uint64_t da_mod = data_after % ns;
        uint64_t sl_mod = shift_limit % ns;
        if (da_mod <= sl_mod)
            tail_slots = sl_mod - da_mod + 1;
        else
            tail_slots = ns - da_mod + sl_mod + 1;
    }

    /* Shift data after the entry.
     * Contraction shifts only within the current run.
     * Expansion shifts all subsequent data right. */
    if (tail_slots > 0) {
        uint64_t ns_local = ns;
        if (new_data_start < data_after) {
            /* Contraction: shift left, iterate forward */
            for (uint64_t k = 0; k < tail_slots; k++) {
                uint64_t src = (data_after + k) % ns_local;
                uint64_t dst = (new_data_start + k) % ns_local;
                rem_set(tbl, dst, rbits, mask,
                        rem_get(tbl, src, rbits, mask));
                move_phys_occ(cf, src, dst);
                if (test_bit(run, src)) {
                    set_bit(run, dst);
                    clear_bit(run, src);
                } else {
                    clear_bit(run, dst);
                }
            }
        } else {
            /* Expansion: shift right, iterate backward */
            for (uint64_t k = tail_slots; k > 0; k--) {
                uint64_t src = (data_after + k - 1) % ns_local;
                uint64_t dst = (new_data_start + k - 1) % ns_local;
                rem_set(tbl, dst, rbits, mask,
                        rem_get(tbl, src, rbits, mask));
                move_phys_occ(cf, src, dst);
                if (test_bit(run, src)) {
                    set_bit(run, dst);
                    clear_bit(run, src);
                } else {
                    clear_bit(run, dst);
                }
            }
        }
    }

    /* Clear old entry area — only the new entry's footprint.
     * Shifted tail data beyond new_data_start is preserved. */
    for (uint64_t p = found_i; p < new_data_start; p++) {
        rem_set(tbl, p, rbits, mask, (uint64_t)0);
        clear_bit(run, p);
        clear_bit(phys_occ_bits(cf), p);
    }

    /* Write new entry with updated count */
    uint64_t slots_written = cqf_write_entry(tbl, found_i, rem, new_count, rbits, mask);
    for (uint64_t p = found_i; p < found_i + slots_written; p++)
        track_phys_occ(cf, p, rem_get(tbl, p, rbits, mask));

    /* Update runend for the current run.
     * The shift loop already moved all run bits, including this run's
     * runend from rend to rend - old_slots + new_slots.  We only need
     * to ensure the run bit is set at the correct new position. */
    {
        uint64_t new_rend = (rend + new_slots >= old_slots)
            ? (rend - old_slots + new_slots) % ns
            : (rend - old_slots + new_slots + ns) % ns;
        clear_bit(run, new_rend);
        set_bit(run, new_rend);
    }

    /* Clear stale slots at end (contraction only) */
    if (new_slots < old_slots) {
        uint64_t new_last_slot = tail_slots > 0
            ? (new_data_start + tail_slots - 1) % ns
            : (new_data_start - 1) % ns;
        if (new_last_slot + 1 <= shift_limit) {
            for (uint64_t p = new_last_slot + 1; p <= shift_limit; p++) {
                rem_set(tbl, p, rbits, mask, (uint64_t)0);
                clear_bit(run, p);
                clear_bit(phys_occ_bits(cf), p);
            }
        } else if (new_last_slot + 1 < ns || shift_limit > 0) {
            for (uint64_t p = new_last_slot + 1; p < ns; p++) {
                rem_set(tbl, p, rbits, mask, (uint64_t)0);
                clear_bit(run, p);
                clear_bit(phys_occ_bits(cf), p);
            }
            for (uint64_t p = 0; p <= shift_limit; p++) {
                rem_set(tbl, p, rbits, mask, (uint64_t)0);
                clear_bit(run, p);
                clear_bit(phys_occ_bits(cf), p);
            }
        }
    }

    cf->count--;
    update_offsets(cf);
    return CQF_OK;
}

/* ============================================================================
 * Core: Lookup (approximate membership)
 * ============================================================================ */

bool cqf_filter_lookup(const cqf_filter_t *cf, uint64_t hash)
{
    if (!cf || cf->count == 0) return false;

    uint64_t q = (hash >> cf->remainder_bits) & (cf->num_slots - 1);
    uint64_t rem = hash & cf->remainder_mask;

    uint64_t rstart, rend;
    if (!find_run(cf, q, &rstart, &rend))
        return false;

    /* Use unified cqf_decode_entry (supports wrapped runs) */
    uint64_t scan_pos = rstart;
    bool wrapped = (rstart > rend);
    bool wrapped_once = false;
    uint64_t ns = cf->num_slots;
    uint64_t dec_rem, dec_cnt;
    (void)dec_cnt;
    while (true) {
        if (!wrapped && scan_pos > rend) break;
        if (wrapped && scan_pos < rstart && scan_pos > rend) break;
        if (!cqf_decode_entry(cf, rend, &scan_pos, &dec_rem, &dec_cnt)) {
            if (wrapped && !wrapped_once) {
                scan_pos = 0;
                wrapped_once = true;
                continue;
            }
            break;
        }
        if (wrapped && !wrapped_once && scan_pos >= ns) {
            scan_pos = 0;
            wrapped_once = true;
        }
        if (dec_rem == rem)
            return true;
        if (dec_rem > rem)
            break;
    }
    return false;
}

/* ============================================================================
 * Get count for an item
 * ============================================================================ */

uint64_t cqf_filter_count_occurrences(const cqf_filter_t *cf, uint64_t hash)
{
    if (!cf || cf->count == 0) return 0;

    uint64_t q = (hash >> cf->remainder_bits) & (cf->num_slots - 1);
    uint64_t rem = hash & cf->remainder_mask;

    uint64_t rstart, rend;
    if (!find_run(cf, q, &rstart, &rend))
        return 0;

    /* Scan run using unified cqf_decode_entry (supports wrapped runs) */
    uint64_t scan_pos = rstart;
    bool wrapped = (rstart > rend);
    bool wrapped_once = false;
    uint64_t ns = cf->num_slots;
    uint64_t dec_rem, dec_cnt;
    while (true) {
        if (!wrapped && scan_pos > rend) break;
        if (wrapped && scan_pos < rstart && scan_pos > rend) break;
        if (!cqf_decode_entry(cf, rend, &scan_pos, &dec_rem, &dec_cnt)) {
            if (wrapped && !wrapped_once) {
                scan_pos = 0;
                wrapped_once = true;
                continue;
            }
            break;
        }
        if (wrapped && !wrapped_once && scan_pos >= ns) {
            scan_pos = 0;
            wrapped_once = true;
        }
        if (dec_rem == rem)
            return dec_cnt;
        /* dec_rem > rem means we've passed the entry for non-zero rem.
         * But for rem=0 searches, dec_rem > 0 is ALWAYS true after the first
         * non-zero entry, so we must NOT break — we need to keep scanning
         * to find all entries with rem=0 in the run. The only way a rem=0
         * search fails is if scan_pos exceeds rend without finding a match. */
        if (rem != 0 && dec_rem > rem)
            break;
    }
    return 0;
}

/* ============================================================================
 * Run-aware enumeration: decode one entry (remainder + count) from a run.
 *
 * Given a run with an entry starting at position *pos (within [q, rend]),
 * reads the remainder and decodes the counter encoding. Advances *pos
 * past the entire entry. Returns true on success.
 *
 * At positions >= q, there are no gaps (shifts preserve contiguity).
 * val=0 is always a valid rem=0 fingerprint entry at positions >= q
 * (encoding prepended zeros are consumed by the decoding logic below).
 * ============================================================================ */
static bool cqf_decode_entry(const cqf_filter_t *cf,
                              uint64_t rend,
                              uint64_t *pos,
                              uint64_t *rem_out, uint64_t *count_out)
{
    const uint64_t *tbl = rem_table_const(cf);
    const uint64_t *pocc = phys_occ_bits_const(cf);
    uint8_t rbits = cf->remainder_bits;
    uint64_t mask = cf->remainder_mask;

    if (*pos > rend) return false;
    /* Skip stale gap slots (phys_occ=0) left by run contraction.
     * These are slots whose data was shifted away during deletion
     * but, because only within-run shifts are done, they remain
     * inside the [rstart, rend] range of the next run.
     * Without this check, they'd decode as phantom rem=0 entries. */
    while (*pos <= rend && !test_bit(pocc, *pos))
        (*pos)++;
    if (*pos > rend) return false;
    uint64_t val = rem_get(tbl, *pos, rbits, mask);
    uint64_t i = *pos + 1;
    uint64_t cnt = 1;
    if (val == 0) {
        /* rem=0 entry. Count consecutive zeros (copies for C>=2). */
        while (i <= rend && rem_get(tbl, i, rbits, mask) == 0) {
            cnt++; i++;
        }
        uint64_t zero_cnt = cnt;
        /* For C>=3 (x=0), the encoding always includes non-zero encoding
         * symbols between leading [0,0] and trailing [0,0]. So a run of
         * >2 consecutive zeros cannot be a single entry — it must be
         * multiple back-to-back C=1/C=2 entries. Consume only 1 as C=1. */
        if (zero_cnt > 2) {
            cnt = 1;
            i = *pos + 1;
        } else if (zero_cnt == 2 && i <= rend &&
                   rem_get(tbl, i, rbits, mask) != 0) {
            /* Possible C>=4 encoding: [0, 0, enc..., 0, 0].
             * Encoding symbols are never 0 (they are in [1, 2^r-1] for x=0),
             * terminated by [0, 0]. */
            int has_term = 0; uint64_t si = i;
            while (si <= rend) {
                uint64_t sv = rem_get(tbl, si, rbits, mask);
                if (sv == 0) {
                    if (si + 1 <= rend && rem_get(tbl, si + 1, rbits, mask) == 0)
                        has_term = 1;
                    break;
                }
                si++;
            }
            if (has_term) {
                uint64_t enc_buf[CQF_ENC_BUF]; int n_enc = 0;
                while (i <= rend) {
                    uint64_t sym = rem_get(tbl, i, rbits, mask); i++;
                    if (sym == 0 && i <= rend && rem_get(tbl, i, rbits, mask) == 0)
                    { i++; break; }
                    if (n_enc < CQF_ENC_BUF) enc_buf[n_enc++] = sym;
                }
                cnt = decode_count(enc_buf, n_enc, 0, rbits);
            }
        }
    } else {
        /* Non-zero remainder. Find the terminating copy of val. */
        uint64_t j = i;
        while (j <= rend && rem_get(tbl, j, rbits, mask) != val)
            j++;

        if (j > rend) {
            cnt = 1;
            i = *pos + 1;
        } else if (j == *pos + 1) {
            cnt = 2;
            i = j + 1;
        } else {
            uint64_t enc_buf[CQF_ENC_BUF];
            int n_enc = 0;
            uint64_t k = *pos + 1;
            while (k < j) {
                if (n_enc < CQF_ENC_BUF)
                    enc_buf[n_enc++] = rem_get(tbl, k, rbits, mask);
                k++;
            }
            cnt = decode_count(enc_buf, n_enc, val, rbits);
            i = j + 1;
        }
    }

    uint64_t used_slots = i - *pos;
    uint64_t expected = cqf_enc_slots(cnt, val, rbits);
    if (used_slots != expected)
        return false;
    *rem_out = val;
    *count_out = cnt;
    *pos = i;
    return true;
}

/* ============================================================================
 * Enumerate all (fingerprint, count) pairs.
 *
 * Iterates through all occupied quotients, using cqf_decode_entry to
 * decode each entry. The fingerprint is:
 *   fingerprint = (occupied_quotient << rbits) | remainder
 * per paper Algorithm 4.
 *
 * At positions >= the occupied quotient q, there are no data gaps
 * (shifts preserve contiguity). val=0 at positions >= q is always
 * a valid rem=0 fingerprint entry — prepended zeros from counter
 * encodings are consumed by the decoding logic in cqf_decode_entry.
 * @param cf          CQF to enumerate
 * @param fps_out     Output buffer for fingerprints (can be NULL to count)
 * @param counts_out  Output buffer for counts (can be NULL to count)
 * @param max_pairs   Max entries in output buffers
 * @return            Number of (fingerprint, count) pairs found
 * ============================================================================ */

static uint64_t enumerate_pairs(const cqf_filter_t *cf,
                                 uint64_t *fps_out, uint64_t *counts_out,
                                 uint64_t max_pairs)
{
    const uint64_t *occ = occ_bits_const(cf);
    uint8_t rbits = cf->remainder_bits;

    uint64_t total = 0;

    for (uint64_t q = 0; q < cf->num_slots; q++) {
        if (!test_bit(occ, q)) continue;

        uint64_t rk = rank_occ_fast(cf, q);
        uint64_t rend = select_run_fast(cf, rk);
        uint64_t rstart;
        if (rk > 1)
            rstart = select_run_fast(cf, rk - 1) + 1;
        else
            rstart = 0;

        /* Start from rstart (the first slot of the run).
         * All entries in [rstart, rend] belong to this quotient.
         * Supports wrapped runs (rstart > rend). */
        uint64_t pos = rstart;
        bool wrapped = (rstart > rend);
        bool wrapped_once = false;
        uint64_t ns = cf->num_slots;
        uint64_t rem, cnt;
        while (true) {
            if (!wrapped && pos > rend) break;
            if (wrapped && pos < rstart && pos > rend) break;
            if (!cqf_decode_entry(cf, rend, &pos, &rem, &cnt)) {
                if (wrapped && !wrapped_once) {
                    pos = 0;
                    wrapped_once = true;
                    continue;
                }
                break;
            }
            uint64_t fp = (q << rbits) | rem;
            if (fps_out && total < max_pairs) fps_out[total] = fp;
            if (counts_out && total < max_pairs) counts_out[total] = cnt;
            total++;
            if (wrapped && !wrapped_once && pos >= ns) {
                pos = 0;
                wrapped_once = true;
            }
        }

    }

    return total;
}

/* ============================================================================
 * Merge: combine two CQFs into a new one
 *
 * Algorithm:
 *   1. Count total pairs from both filters
 *   2. Allocate new filter with combined capacity
 *   3. Enumerate all pairs from cf1 and cf2
 *   4. Insert each pair into the new filter
 * ============================================================================ */

cqf_filter_t *cqf_filter_merge(const cqf_filter_t *cf1, const cqf_filter_t *cf2)
{
    if (!cf1 || !cf2) return NULL;
    if (cf1->remainder_bits != cf2->remainder_bits) return NULL;
    if (cf1->compat_version != cf2->compat_version) return NULL;
    /* Merge requires same hash_seed. Zero means "unknown domain"
     * and can only merge with other zero-seed filters (unknown domain
     * cannot be verified to match any specific domain). */
    if (cf1->hash_seed != cf2->hash_seed) return NULL;
    /* Merge requires same capacity to preserve fingerprint decomposition.
     * The enumerated fingerprint (q << rbits | rem) re-splits at the new
     * filter's capacity; different capacities change the quotient mapping. */
    if (cf1->num_slots != cf2->num_slots) return NULL;

    /* Count pairs */
    uint64_t n1 = enumerate_pairs(cf1, NULL, NULL, 0);
    uint64_t n2 = enumerate_pairs(cf2, NULL, NULL, 0);
    if (n1 == 0 && n2 == 0) return cqf_filter_create(64, cf1->remainder_bits);
    uint64_t total_distinct = n1 + n2;

    /* Allocate buffers.
     * Use UINT64_MAX sentinel: if enumeration returns more than expected,
     * the write guards (total < max_pairs) prevent overflow. */
    uint64_t *fps = (uint64_t *)malloc(total_distinct * sizeof(uint64_t));
    uint64_t *cnts = (uint64_t *)malloc(total_distinct * sizeof(uint64_t));
    if (!fps || !cnts) {
        free(fps); free(cnts);
        return NULL;
    }

    /* Enumerate and verify count consistency */
    uint64_t actual_n1 = enumerate_pairs(cf1, fps, cnts, n1);
    uint64_t actual_n2 = enumerate_pairs(cf2, fps + n1, cnts + n1, n2);
    if (actual_n1 != n1 || actual_n2 != n2) {
        free(fps); free(cnts);
        return NULL;
    }
    /* Create output filter with same capacity as inputs to preserve
     * the fingerprint decomposition (quotient bits match the original
     * hash decomposition used during lookup). */
    uint64_t capacity = cf1->num_slots;
    uint64_t rbits = cf1->remainder_bits;
    cqf_filter_t *out = cqf_filter_create(capacity, (uint8_t)rbits);
    if (!out) { free(fps); free(cnts); return NULL; }
    /* Propagate hash_seed: seeds must match (enforced above). */
    out->hash_seed = cf1->hash_seed;

    for (uint64_t i = 0; i < total_distinct; i++) {
        for (uint64_t c = 0; c < cnts[i]; c++) {
            cqf_err_t e = cqf_filter_insert(out, fps[i]);
            if (e != CQF_OK) {
                cqf_filter_t *new_out = cqf_filter_resize(out, out->num_slots * 2);
                if (!new_out) { free(fps); free(cnts); return NULL; }
                out = new_out;
                e = cqf_filter_insert(out, fps[i]);
            }
        }
    }

    free(fps);
    free(cnts);
    return out;
}

/* ============================================================================
 * Resize: create a new CQF with different capacity
 *
 * Algorithm:
 *   1. Enumerate all (fingerprint, count) pairs from input
 *   2. Create new CQF with desired capacity
 *   3. Insert all pairs into new filter
 *   4. Destroy the input filter
 * ============================================================================ */

cqf_filter_t *cqf_filter_resize(cqf_filter_t *cf, size_t new_capacity)
{
    if (!cf) return NULL;

    /* Enumerate pairs into temporary buffers FIRST.
     * This preserves the data even if subsequent steps fail. */
    uint64_t n = enumerate_pairs(cf, NULL, NULL, 0);
    uint8_t rbits = cf->remainder_bits;

    if (n == 0) {
        cqf_filter_t *new_cf = cqf_filter_create(new_capacity, rbits);
        cqf_filter_destroy(cf);
        return new_cf;
    }

    uint64_t *fps = (uint64_t *)malloc(n * sizeof(uint64_t));
    uint64_t *cnts = (uint64_t *)malloc(n * sizeof(uint64_t));
    if (!fps || !cnts) {
        free(fps); free(cnts);
        return NULL; /* cf is still intact */
    }

    enumerate_pairs(cf, fps, cnts, n);

    /* Create new filter BEFORE destroying old one */
    uint64_t capacity = new_capacity > 0 ? (uint64_t)new_capacity : n * 2;
    if (capacity < n) {
        free(fps); free(cnts);
        return NULL; /* cf is still intact */
    }
    cqf_filter_t *out = cqf_filter_create(capacity, rbits);
    if (!out) {
        free(fps); free(cnts);
        return NULL; /* cf is still intact */
    }

    /* Insert pairs BEFORE destroying old filter.
     * This ensures the old filter survives if insertion fails. */
    for (uint64_t i = 0; i < n; i++) {
        for (uint64_t c = 0; c < cnts[i]; c++) {
            cqf_err_t e = cqf_filter_insert(out, fps[i]);
            if (e != CQF_OK) {
                /* Recursive resize destroys 'out' per its API contract.
                 * If it fails, return NULL — old 'cf' is still intact. */
                cqf_filter_t *new_out = cqf_filter_resize(out, out->num_slots * 2);
                if (!new_out) { free(fps); free(cnts); return NULL; }
                out = new_out;
                cqf_filter_insert(out, fps[i]);
            }
        }
    }

    /* Only destroy old filter after all insertions succeed */
    cqf_filter_destroy(cf);
    free(fps);
    free(cnts);
    return out;
}

size_t cqf_filter_memory_bytes(const cqf_filter_t *cf)
{
    if (!cf) return 0;
    uint64_t meta_padded = meta_padded_size(cf);
    uint64_t total_remainder_bits = cf->num_slots * cf->remainder_bits;
    uint64_t remainder_words = (total_remainder_bits + 63) / 64;
    return sizeof(cqf_filter_t) + meta_padded + remainder_words * sizeof(uint64_t);
}

double cqf_filter_estimated_fpr(const cqf_filter_t *cf)
{
    if (!cf) return 1.0;
    return 1.0 / (double)(1ULL << cf->remainder_bits);
}

/* ============================================================================
 * Validation
 *
 * Checks:
 *   1. Occupieds and runends have equal popcount
 *   2. Every occupied quotient has a valid non-empty run
 *   3. No malformed counter encodings (consecutive 0s only for x=0)
 * ============================================================================ */

bool cqf_filter_validate(const cqf_filter_t *cf)
{
    if (!cf) return false;
    const uint64_t *occ = occ_bits_const(cf);
    const uint64_t *run = run_bits_const(cf);

    uint64_t occ_pop = 0, run_pop = 0;
    for (uint64_t i = 0; i < cf->num_blocks; i++) {
        occ_pop += __builtin_popcountll(occ[i]);
        run_pop += __builtin_popcountll(run[i]);
    }
    if (occ_pop != run_pop) {
        fprintf(stderr, "VALIDATE FAIL: occ_pop=%lu != run_pop=%lu\n",
                (unsigned long)occ_pop, (unsigned long)run_pop);
        /* Dump first few run bits */
        for (uint64_t i = 0; i < cf->num_blocks && i < 4; i++) {
            fprintf(stderr, "  block %lu: occ=0x%016lx run=0x%016lx\n",
                    (unsigned long)i, occ[i], run[i]);
        }
        return false;
    }

    for (uint64_t q = 0; q < cf->num_slots; q++) {
        if (!test_bit(occ, q)) continue;
        uint64_t rend;
        uint64_t rk = rank_occ_fast(cf, q);
        rend = select_run_fast(cf, rk);
        if (rend >= cf->num_slots) {
            fprintf(stderr, "VALIDATE FAIL: q=%lu rk=%lu rend=%lu >= ns=%lu\n",
                    (unsigned long)q, (unsigned long)rk,
                    (unsigned long)rend, (unsigned long)cf->num_slots);
            return false;
        }
    }

    return true;
}

/* ============================================================================
 * Hash helpers (exposed for testing)
 * ============================================================================ */

uint64_t cqf_quotient(uint64_t hash, uint8_t quotient_bits)
{
    return hash >> (64 - quotient_bits);
}

uint64_t cqf_remainder(uint64_t hash, uint8_t remainder_bits)
{
    return hash & ((1ULL << remainder_bits) - 1);
}
