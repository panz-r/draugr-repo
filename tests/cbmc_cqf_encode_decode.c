/**
 * cbmc_cqf_encode_decode.c — CBMC harness for CQF encode/decode roundtrip
 *
 * Verifies:
 *   1. decode(encode(C, x)) == C for x=0, x=1, x=2, x=max_remainder
 *   2. cqf_enc_slots(C, x, rbits) == actual slots written by cqf_write_entry
 *
 * Run: cbmc -D__CBMC__ --unwind 6 --bounds-check --pointer-check \
 *      tests/cbmc_cqf_encode_decode.c --function main
 */
#ifdef __CBMC__
#include <assert.h>
#include <stdint.h>
#include <string.h>

/* ── Remainder table access ── */

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
        uint64_t m2 = mask >> (64 - bo);
        tbl[wi + 1] = (tbl[wi + 1] & ~m2) | ((v & mask) >> (64 - bo));
    }
}

/* ── CQF encode ── */

static int cqf_encode(uint64_t C, uint64_t x, uint8_t rbits,
                       uint64_t *enc_buf, int max_buf)
{
    if (C <= 2) return 0;
    if (x == 0) {
        if (C == 3) return 0;
        uint64_t base = (1ULL << rbits) - 1;
        uint64_t val = C - 4;
        int n = 0;
        if (val == 0) { enc_buf[n++] = 1; }
        while (val > 0 && n < max_buf) {
            int d = (int)(val % base);
            enc_buf[n++] = (uint64_t)(d + 1);
            val /= base;
        }
        for (int i = 0; i < n / 2; i++) {
            uint64_t t = enc_buf[i];
            enc_buf[i] = enc_buf[n - 1 - i];
            enc_buf[n - 1 - i] = t;
        }
        return n;
    } else {
        uint64_t base = (1ULL << rbits) - 2;
        uint64_t val = C - 3;
        int n = 0;
        if (val == 0) {
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
        for (int i = 0; i < n / 2; i++) {
            uint64_t t = enc_buf[i];
            enc_buf[i] = enc_buf[n - 1 - i];
            enc_buf[n - 1 - i] = t;
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

/* ── CQF decode_count ── */

static uint64_t decode_count(const uint64_t *enc_buf, int n_slots,
                              uint64_t x, uint8_t rbits)
{
    if (n_slots == 0) return 3;
    if (x == 0) {
        uint64_t base = (1ULL << rbits) - 1;
        uint64_t val = 0;
        for (int i = 0; i < n_slots; i++)
            val = val * base + (enc_buf[i] - 1);
        return val + 4;
    } else {
        uint64_t base = (1ULL << rbits) - 2;
        uint64_t val = 0;
        for (int i = 0; i < n_slots; i++) {
            uint64_t sym = enc_buf[i];
            uint64_t digit;
            if (sym == 0)
                digit = 0;
            else if (sym < x)
                digit = sym - 1;
            else
                digit = sym - 2;
            val = val * base + digit;
        }
        return val + 3;
    }
}

/* ── cqf_enc_slots ── */

static uint64_t cqf_enc_slots(uint64_t C, uint64_t x, uint8_t rbits)
{
    if (C <= 1) return 1;
    if (C == 2) return 2;
    if (x == 0 && C == 3) return 3;
    uint64_t enc_buf[16];
    int n = cqf_encode(C, x, rbits, enc_buf, 16);
    if (x == 0)
        return 4 + (uint64_t)n;
    else
        return 2 + (uint64_t)n;
}

/* ── cqf_write_entry ── */

static uint64_t cqf_write_entry(uint64_t *tbl, uint64_t pos, uint64_t x,
                                 uint64_t C, uint8_t rbits, uint64_t mask)
{
    rem_set(tbl, pos, rbits, mask, x);
    if (C <= 1) return 1;
    if (C == 2) { rem_set(tbl, pos + 1, rbits, mask, x); return 2; }
    uint64_t enc_buf[16];
    int n = cqf_encode(C, x, rbits, enc_buf, 16);
    if (x == 0) {
        if (C == 3) {
            rem_set(tbl, pos + 1, rbits, mask, (uint64_t)0);
            rem_set(tbl, pos + 2, rbits, mask, (uint64_t)0);
            return 3;
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

/* ── CBMC Harness ── */

void main(void)
{
    uint8_t rbits = nondet_uint8();
    uint64_t C = nondet_uint64();
    uint64_t x = nondet_uint64();

    __CPROVER_assume(rbits == 2 || rbits == 3);
    __CPROVER_assume(C >= 1 && C <= 6);
    uint64_t mask = (1ULL << rbits) - 1;
    __CPROVER_assume(x == 0 || x == 1 || x == 2 || x == mask);

    uint64_t enc_buf[16];

    /* Property 1: encode returns n >= 0 symbols */
    int n = cqf_encode(C, x, rbits, enc_buf, 16);
    __CPROVER_assert(n >= 0, "encode returns non-negative symbol count");

    /* Property 2: decode(encode(C,x)) == C for all valid C,x */
    if (C <= 2) {
        /* Direct representation (no encoding symbols) */
        __CPROVER_assert(n == 0, "C<=2: no encoding symbols needed");
    } else if (x == 0 && C == 3) {
        __CPROVER_assert(n == 0, "x=0, C=3: special case, 3 zeros");
    } else {
        /* Has encoding symbols. Decode should recover C. */
        uint64_t C2 = decode_count(enc_buf, n, x, rbits);
        __CPROVER_assert(C2 == C,
            "decode(encode(C,x)) == C for C>=3 with encoding");
    }

    /* Property 3: cqf_enc_slots predicts actual write_entry slot count */
    {
        uint64_t tbl[6];
        memset(tbl, 0, sizeof(tbl));
        uint64_t pred = cqf_enc_slots(C, x, rbits);
        uint64_t actual = cqf_write_entry(tbl, 0, x, C, rbits, mask);
        __CPROVER_assert(pred == actual,
            "cqf_enc_slots matches cqf_write_entry actual slot count");
    }
}
#endif
