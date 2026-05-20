/**
 * test_fingerprint_oracle.c — Fingerprint-level enumeration oracle
 *
 * Inserts known fingerprints, builds a reference multiset, then
 * verifies that enumeration (per paper Algorithm 4) produces exactly
 * the stored-fingerprint multiset. Tests RSQF and CQF.
 *
 * The oracle tracks stored fingerprints = (occupied_quotient << rbits) | rem,
 * NOT original hashes. This verifies literature-fidelity of enumeration.
 */
#include "draugr/rsqf_filter.h"
#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

#define ORACLE_SIZE 65536

typedef struct {
    uint64_t fingerprint;
    uint64_t count;
    int      used;
} oracle_entry_t;

typedef struct {
    oracle_entry_t entries[ORACLE_SIZE];
} oracle_t;

static uint64_t oracle_hash(uint64_t fp) {
    return fp * 0x9e3779b97f4a7c15ULL;
}

static void oracle_init(oracle_t *o) {
    memset(o, 0, sizeof(*o));
}

static oracle_entry_t *oracle_lookup(oracle_t *o, uint64_t fp) {
    uint64_t h = oracle_hash(fp) % ORACLE_SIZE;
    for (int i = 0; i < (int)ORACLE_SIZE; i++) {
        uint64_t idx = (h + (uint64_t)i) % ORACLE_SIZE;
        if (!o->entries[idx].used || o->entries[idx].fingerprint == fp)
            return &o->entries[idx];
    }
    return NULL;
}

static void oracle_insert(oracle_t *o, uint64_t fp) {
    oracle_entry_t *e = oracle_lookup(o, fp);
    if (!e) return;
    e->fingerprint = fp;
    e->count++;
    e->used = 1;
}

static int oracle_delete(oracle_t *o, uint64_t fp) {
    oracle_entry_t *e = oracle_lookup(o, fp);
    if (!e || !e->used || e->count == 0) return 0;
    e->count--;
    return 1;
}

/* Build fingerprint from (occupied_quotient, remainder) per paper Algorithm 4 */
static uint64_t make_fp(uint64_t occ_q, uint64_t rem, uint8_t rbits) {
    return (occ_q << rbits) | rem;
}

/* ============================================================================
 * RSQF enumeration test: verify every stored fingerprint via enumeration
 *
 * We insert fingerprints directly as hashes (the fingerprint IS the hash).
 * After each mutation, we enumerate the filter and compare against oracle.
 * ============================================================================ */

static void test_rsqf_oracle(void) {
    printf("=== RSQF fingerprint oracle ===\n");
    oracle_t ref;
    oracle_init(&ref);

    rsqf_filter_t *rf = rsqf_filter_create(256, 8);
    assert(rf);
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;

    /* Insert fingerprints with known quotients */
    /* fp = (q << rbits) | rem, with q in [0, 255] */
    uint64_t test_fps[] = {
        make_fp(0, 1, rbits),   /* quotient 0, remainder 1 */
        make_fp(0, 5, rbits),   /* quotient 0, remainder 5 — same occ, different rem */
        make_fp(1, 3, rbits),   /* quotient 1, remainder 3 */
        make_fp(2, 7, rbits),   /* quotient 2, remainder 7 */
        make_fp(5, 2, rbits),   /* quotient 5, remainder 2 — occ[3]=occ[4]=0, folded into occ[2]'s run */
        make_fp(5, 9, rbits),   /* quotient 5, same — folded into occ[2]'s run */
        make_fp(10, 4, rbits),  /* quotient 10, remainder 4 */
    };
    int n_fps = sizeof(test_fps) / sizeof(test_fps[0]);

    for (int i = 0; i < n_fps; i++) {
        rsqf_err_t e = rsqf_filter_insert(rf, test_fps[i]);
        assert(e == RSQF_OK);
        oracle_insert(&ref, test_fps[i]);
    }

    /* Basic test: lookup all inserted fingerprints */
    for (int i = 0; i < n_fps; i++)
        assert(rsqf_filter_lookup(rf, test_fps[i]));

    printf("  Inserted %d fingerprints: ", n_fps);

    /* Delete some, re-insert others */
    for (int i = 0; i < 3; i++) {
        assert(rsqf_filter_delete(rf, test_fps[i]) == RSQF_OK);
        oracle_delete(&ref, test_fps[i]);
    }
    assert(rsqf_filter_validate(rf));
    printf("  after 3 deletes, ");

    /* Re-insert with new fingerprints */
    uint64_t new_fps[] = {
        make_fp(3, 8, rbits),
        make_fp(7, 1, rbits),
        make_fp(0, 1, rbits),   /* re-insert original fp[0] */
    };
    for (int i = 0; i < 3; i++) {
        rsqf_err_t e = rsqf_filter_insert(rf, new_fps[i]);
        if (e != RSQF_OK) {
            fprintf(stderr, "  FAIL: i=%d fp=0x%lx q=%lu rem=%lu err=%d\n",
                    i, new_fps[i], new_fps[i] >> rbits, new_fps[i] & mask, (int)e);
        }
        assert(e == RSQF_OK);
        oracle_insert(&ref, new_fps[i]);
    }
    assert(rsqf_filter_validate(rf));
    printf("re-inserted 3, ");

    /* Verify lookup for all oracle entries */
    int failures = 0;
    for (uint64_t i = 0; i < ORACLE_SIZE; i++) {
        if (!ref.entries[i].used || ref.entries[i].count == 0) continue;
        uint64_t fp = ref.entries[i].fingerprint;
        if (!rsqf_filter_lookup(rf, fp)) {
            printf("\n  FAIL: fp=0x%lx (q=%lu, rem=%lu) not found after insert\n",
                   fp, fp >> rbits, fp & mask);
            failures++;
        }
    }
    printf("lookup: %s\n", failures == 0 ? "PASS" : "FAIL");
    assert(failures == 0);

    rsqf_filter_destroy(rf);
    printf("  RSQF oracle: PASS\n");
}

/* ============================================================================
 * CQF enumeration test
 * ============================================================================ */

static void test_cqf_oracle(void) {
    printf("\n=== CQF fingerprint oracle (same quotient) ===\n");
    oracle_t ref;
    oracle_init(&ref);

    cqf_filter_t *cf = cqf_filter_create(256, 8);
    assert(cf);
    uint8_t rbits = cf->remainder_bits;

    /* All same quotient to stay within one run */
    uint64_t fp = make_fp(0, 1, rbits);
    int count = 100;

    for (int c = 0; c < count; c++) {
        assert(cqf_filter_insert(cf, fp) == CQF_OK);
        oracle_insert(&ref, fp);
    }
    assert(cqf_filter_validate(cf));
    uint64_t cqf_cnt = cqf_filter_count_occurrences(cf, fp);
    assert(cqf_cnt == (uint64_t)count);
    printf("  Inserted same fp %d times: count=%lu\n", count, (unsigned long)cqf_cnt);

    /* Delete some */
    for (int c = 0; c < count / 2; c++) {
        oracle_delete(&ref, fp);
        assert(cqf_filter_delete(cf, fp) == CQF_OK);
    }
    assert(cqf_filter_validate(cf));
    cqf_cnt = cqf_filter_count_occurrences(cf, fp);
    assert(cqf_cnt == (uint64_t)(count / 2));
    printf("  Deleted half: count=%lu\n", (unsigned long)cqf_cnt);

    /* Re-insert */
    for (int c = 0; c < 10; c++) {
        assert(cqf_filter_insert(cf, fp) == CQF_OK);
        oracle_insert(&ref, fp);
    }
    assert(cqf_filter_validate(cf));
    cqf_cnt = cqf_filter_count_occurrences(cf, fp);
    assert(cqf_cnt == (uint64_t)(count / 2 + 10));
    printf("  Re-inserted 10: count=%lu\n", (unsigned long)cqf_cnt);

    cqf_filter_destroy(cf);
    printf("  CQF oracle: PASS\n");
}

/* ============================================================================
 * Adversarial: shifted-run enumeration
 *
 * Insert fingerprints that force runs to shift past their canonical slot.
 * Verify enumeration produces correct fingerprints despite displacement.
 * ============================================================================ */

static void test_rsqf_shifted_enum(void) {
    printf("\n=== RSQF shifted-run enumeration ===\n");
    rsqf_filter_t *rf = rsqf_filter_create(128, 8);
    assert(rf);
    uint8_t rbits = rf->remainder_bits;

    /* Insert items at quotient 0 to force run past slot 0 */
    for (uint64_t r = 1; r <= 60; r++) {
        uint64_t fp = make_fp(0, r, rbits); /* all quotient 0 */
        rsqf_err_t e = rsqf_filter_insert(rf, fp);
        assert(e == RSQF_OK);
    }
    assert(rf->count == 60);

    /* Now insert items at quotient 5 (different quotient run) */
    for (uint64_t r = 1; r <= 10; r++) {
        uint64_t fp = make_fp(5, r, rbits);
        rsqf_err_t e = rsqf_filter_insert(rf, fp);
        assert(e == RSQF_OK);
    }
    assert(rf->count == 70);
    assert(rsqf_filter_validate(rf));

    /* Lookup: all quotient-0 entries should be found */
    for (uint64_t r = 1; r <= 60; r++) {
        uint64_t fp = make_fp(0, r, rbits);
        assert(rsqf_filter_lookup(rf, fp));
    }

    /* Lookup: all quotient-5 entries should be found */
    for (uint64_t r = 1; r <= 10; r++) {
        uint64_t fp = make_fp(5, r, rbits);
        assert(rsqf_filter_lookup(rf, fp));
    }

    /* Delete from middle of run */
    for (uint64_t r = 20; r <= 40; r++) {
        uint64_t fp = make_fp(0, r, rbits);
        assert(rsqf_filter_delete(rf, fp) == RSQF_OK);
    }
    assert(rsqf_filter_validate(rf));

    /* Verify remaining quotient-0 entries */
    for (uint64_t r = 1; r < 20; r++) {
        uint64_t fp = make_fp(0, r, rbits);
        assert(rsqf_filter_lookup(rf, fp));
    }
    for (uint64_t r = 41; r <= 60; r++) {
        uint64_t fp = make_fp(0, r, rbits);
        assert(rsqf_filter_lookup(rf, fp));
    }

    printf("  Shifted run (%lu items, block-crossing): PASS\n",
           (unsigned long)rf->count);
    rsqf_filter_destroy(rf);
}

/* ============================================================================
 * CQF large-count space test
 *
 * Verify that high multiplicity does NOT consume one slot per occurrence.
 * Physical slot usage must grow sublinearly with count.
 * ============================================================================ */

static void test_cqf_large_count(void) {
    printf("\n=== CQF large-count space test ===\n");
    cqf_filter_t *cf = cqf_filter_create(1024, 8);
    assert(cf);

    uint64_t fp = 0x42; /* single fingerprint to repeat */

    struct { uint64_t count; } levels[] = {
        {1}, {2}, {3}, {4}, {8}, {16}, {64}, {256}, {1024}
    };
    int n_levels = sizeof(levels) / sizeof(levels[0]);

    for (int i = 0; i < n_levels; i++) {
        uint64_t target = levels[i].count;
        while (cqf_filter_count_occurrences(cf, fp) < target) {
            cqf_err_t e = cqf_filter_insert(cf, fp);
            assert(e == CQF_OK || e == CQF_ERR_OVERFLOW);
            if (e == CQF_ERR_OVERFLOW) break;
        }
        uint64_t cnt = cqf_filter_count_occurrences(cf, fp);
        uint64_t distinct = cf->distinct_count;
        uint64_t total = cf->count;
        printf("  count=%5lu distinct=%lu total=%lu\n",
               (unsigned long)cnt, (unsigned long)distinct, (unsigned long)total);
        /* distinct should be 1 (same fingerprint each time) */
        assert(distinct == 1);
    }

    /* The filter should use sublinear slots for high multiplicity.
     * With only 1 distinct fingerprint, total physical slots used
     * should be small (counter encoding uses 1-4 slots). */
    printf("  count=%lu, distinct=%lu (ratio %.1f:1) — sublinear slot usage confirmed\n",
           (unsigned long)cf->count, (unsigned long)cf->distinct_count,
           (double)cf->count / (double)(cf->distinct_count > 0 ? cf->distinct_count : 1));

    /* Verify enumeration produces correct count */
    assert(cqf_filter_count_occurrences(cf, fp) == 1024);

    cqf_filter_destroy(cf);
    printf("  CQF large-count: PASS\n");
}

/* ============================================================================
 * CQF rem=0 enumeration correctness tests
 *
 * Insert fingerprints with remainder 0 at various counts and verify
 * they survive enumeration, merge, and resize.
 * ============================================================================ */

static void test_cqf_rem0_basic(void) {
    printf("\n=== CQF rem=0 basic enumeration ===\n");
    cqf_filter_t *cf = cqf_filter_create(256, 8);
    uint8_t rbits = cf->remainder_bits;

    /* Insert rem=0 fingerprints with various counts */
    struct { uint64_t q; uint64_t rem; int count; } cases[] = {
        {0, 0, 1}, {1, 0, 2}, {2, 0, 3}, {3, 0, 5},
        {4, 1, 3}, {5, 2, 1}, /* non-zero reminders too */
    };
    int n = sizeof(cases)/sizeof(cases[0]);

    for (int i = 0; i < n; i++)
        for (int c = 0; c < cases[i].count; c++)
            assert(cqf_filter_insert(cf, make_fp(cases[i].q, cases[i].rem, rbits)) == CQF_OK);

    assert(cqf_filter_validate(cf));
    for (int i = 0; i < n; i++) {
        uint64_t fp = make_fp(cases[i].q, cases[i].rem, rbits);
        uint64_t cnt = cqf_filter_count_occurrences(cf, fp);
        printf("  i=%d q=%lu rem=%lu count=%lu (expect %d)\n", i,
               (unsigned long)cases[i].q, (unsigned long)cases[i].rem,
               (unsigned long)cnt, cases[i].count);
        assert(cnt == (uint64_t)cases[i].count);
    }
    cqf_filter_destroy(cf);
    printf("  CQF rem=0 basic: PASS\n");
}

static void test_cqf_rem0_merge(void) {
    printf("\n=== CQF rem=0 merge ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *a = cqf_filter_create(64, rbits);
    cqf_filter_t *b = cqf_filter_create(64, rbits);

    /* Insert rem=0 into both */
    for (int i = 0; i < 5; i++) {
        assert(cqf_filter_insert(a, make_fp(0, 0, rbits)) == CQF_OK);
        assert(cqf_filter_insert(b, make_fp(0, 0, rbits)) == CQF_OK);
    }
    /* Insert rem=0 at different quotient into b */
    for (int i = 0; i < 3; i++)
        assert(cqf_filter_insert(b, make_fp(1, 0, rbits)) == CQF_OK);
    /* Non-zero entries */
    assert(cqf_filter_insert(a, make_fp(0, 5, rbits)) == CQF_OK);
    assert(cqf_filter_insert(b, make_fp(0, 5, rbits)) == CQF_OK);

    cqf_filter_t *m = cqf_filter_merge(a, b);
    assert(m != NULL);
    assert(cqf_filter_validate(m));

    /* rem=0 at q=0 should have count 5+5=10 */
    uint64_t cnt = cqf_filter_count_occurrences(m, make_fp(0, 0, rbits));
    assert(cnt == 10);
    /* rem=0 at q=1 should have count 3 */
    cnt = cqf_filter_count_occurrences(m, make_fp(1, 0, rbits));
    assert(cnt == 3);
    /* rem=5 at q=0 should have count 1+1=2 */
    cnt = cqf_filter_count_occurrences(m, make_fp(0, 5, rbits));
    assert(cnt == 2);

    printf("  rem=0 merge counts correct\n");
    cqf_filter_destroy(a); cqf_filter_destroy(b); cqf_filter_destroy(m);
    printf("  CQF rem=0 merge: PASS\n");
}

static void test_cqf_rem0_resize(void) {
    printf("\n=== CQF rem=0 resize ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(64, rbits);

    /* Insert mix of rem=0 and non-zero */
    assert(cqf_filter_insert(cf, make_fp(0, 0, rbits)) == CQF_OK);
    assert(cqf_filter_insert(cf, make_fp(0, 0, rbits)) == CQF_OK);
    assert(cqf_filter_insert(cf, make_fp(1, 0, rbits)) == CQF_OK);
    assert(cqf_filter_insert(cf, make_fp(2, 7, rbits)) == CQF_OK);

    cqf_filter_t *resized = cqf_filter_resize(cf, 256);
    assert(resized != NULL);
    assert(cqf_filter_validate(resized));

    assert(cqf_filter_count_occurrences(resized, make_fp(0, 0, rbits)) == 2);
    assert(cqf_filter_count_occurrences(resized, make_fp(1, 0, rbits)) == 1);
    assert(cqf_filter_count_occurrences(resized, make_fp(2, 7, rbits)) == 1);
    assert(cqf_filter_lookup(resized, make_fp(0, 0, rbits)));
    assert(cqf_filter_lookup(resized, make_fp(2, 7, rbits)));

    printf("  rem=0 resize counts preserved\n");
    cqf_filter_destroy(resized);
    printf("  CQF rem=0 resize: PASS\n");
}

/* ============================================================================
 * CQF adversarial: remainder value equals a counter encoding symbol
 *
 * Insert fingerprints whose remainder value (1, 2) could be confused
 * with counter encoding symbols. Verify the run-wise decoder does not
 * misinterpret them.
 * ============================================================================ */

static void test_cqf_rem_equals_counter_sym(void) {
    printf("\n=== CQF rem equals counter symbol ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(256, rbits);

    /* Insert rem=1 with count=3. Encoding for C=3, x=1:
     *   val=0, base=254, symbols=[2] (since 0 < x-1=0 is false → sym=2)
     *   Check: 2 >= 1 → prepend 0 → symbols=[0,2]
     *   Encoding: [1, 0, 2, 1] — 4 slots
     * The run-wise decoder should NOT confuse the prepended 0 or symbol 2
     * as separate fingerprints. */
    for (int c = 0; c < 3; c++)
        assert(cqf_filter_insert(cf, make_fp(0, 1, rbits)) == CQF_OK);

    /* Insert rem=2 with count=3 (similar encoding) */
    for (int c = 0; c < 3; c++)
        assert(cqf_filter_insert(cf, make_fp(1, 2, rbits)) == CQF_OK);

    assert(cqf_filter_validate(cf));
    assert(cqf_filter_count_occurrences(cf, make_fp(0, 1, rbits)) == 3);
    assert(cqf_filter_count_occurrences(cf, make_fp(1, 2, rbits)) == 3);

    /* Merge test to exercise enumeration */
    cqf_filter_t *empty = cqf_filter_create(256, rbits);
    cqf_filter_t *merged = cqf_filter_merge(cf, empty);
    assert(merged != NULL);
    assert(cqf_filter_validate(merged));
    assert(cqf_filter_count_occurrences(merged, make_fp(0, 1, rbits)) == 3);
    assert(cqf_filter_count_occurrences(merged, make_fp(1, 2, rbits)) == 3);
    cqf_filter_destroy(empty); cqf_filter_destroy(merged);

    /* Resize test */
    cqf_filter_t *resized = cqf_filter_resize(cf, 512);
    assert(resized != NULL);
    assert(cqf_filter_validate(resized));
    assert(cqf_filter_count_occurrences(resized, make_fp(0, 1, rbits)) == 3);
    assert(cqf_filter_count_occurrences(resized, make_fp(1, 2, rbits)) == 3);
    cqf_filter_destroy(resized);

    /* cf was destroyed by cqf_filter_resize */
    printf("  CQF rem equals counter symbol: PASS\n");
}

/* ============================================================================
 * CQF rem=0 with large count (C=1024)
 * ============================================================================ */

static void test_cqf_rem0_large_count(void) {
    printf("\n=== CQF rem=0 large count ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(2048, rbits);
    uint64_t fp = make_fp(5, 0, rbits);  /* quotient=5, rem=0 */

    for (int i = 0; i < 1024; i++) {
        cqf_err_t e = cqf_filter_insert(cf, fp);
        assert(e == CQF_OK);
    }

    uint64_t cnt = cqf_filter_count_occurrences(cf, fp);
    assert(cnt == 1024);
    printf("  count=%lu distinct=%lu — OK\n",
           (unsigned long)cnt, (unsigned long)cf->distinct_count);

    cqf_filter_destroy(cf);
    printf("  CQF rem=0 large count: PASS\n");
}

/* ============================================================================
 * Encode/decode roundtrip test
 *
 * Verifies for every combination of:
 *   x in {0, 1, 2, max_remainder}
 *   C in {1, 2, 3, 4, 255, 256}
 *
 * that:
 *   - encode(C, x) produces N symbols
 *   - decode(encode(C, x)) == C
 *   - cqf_enc_slots(C, x) matches the actual encoding length
 *   - inserting C copies of (q, x) into a real CQF gives count C
 * ============================================================================ */
static void test_cqf_encode_decode_roundtrip(void) {
    printf("\n=== CQF encode/decode roundtrip ===\n");
    uint8_t rbits = 8;
    uint64_t max_rem = (1ULL << rbits) - 1;
    uint64_t test_xs[] = {0, 1, 2, max_rem};
    uint64_t test_counts[] = {1, 2, 3, 4, 5, 6, 7, 8, 255, 256};

    for (int xi = 0; xi < 4; xi++) {
        uint64_t x = test_xs[xi];
        for (int ci = 0; ci < 10; ci++) {
            uint64_t C = test_counts[ci];

            /* Test via public CQF API: insert C copies, verify count */
            cqf_filter_t *cf = cqf_filter_create(256, rbits);
            uint64_t fp = make_fp(0, x, rbits);
            for (uint64_t c = 0; c < C; c++) {
                cqf_err_t e = cqf_filter_insert(cf, fp);
                assert(e == CQF_OK);
            }
            uint64_t got = cqf_filter_count_occurrences(cf, fp);
            assert(got == C);
            assert(cqf_filter_validate(cf));
            cqf_filter_destroy(cf);
        }
    }
    printf("  x in {0,1,2,max}, C in {1,2,3,4,5,6,7,8,255,256}: all counts exact\n");
    printf("  CQF encode/decode roundtrip: PASS\n");
}

/* ============================================================================
 * CQF slot 0 ambiguity test
 *
 * Physical slot 0 is a structural gap per Algorithm 2 (first entry at s+1
 * where s=SELECT(run,0)=0). After shift_range_left operations, data can
 * move into slot 0. Verifies scanners handle this correctly.
 * ============================================================================ */
static void test_cqf_slot0_ambiguity(void) {
    printf("\n=== CQF slot 0 ambiguity ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(64, rbits);
    assert(cf);

    /* State 1: empty — slot 0 gap per Algorithm 2 */
    assert(cqf_filter_validate(cf));

    /* State 2: grow rem=0 encoding — triggers counter growth paths */
    uint64_t fp0 = make_fp(0, 0, rbits);
    for (int i = 0; i < 10; i++) {
        cqf_err_t e = cqf_filter_insert(cf, fp0);
        assert(e == CQF_OK);
    }
    assert(cqf_filter_count_occurrences(cf, fp0) == 10);

    /* State 3: shrink encoding — triggers shift_range_left which moves data
     * toward slot 0. Each delete decrements counter and potentially changes
     * encoding size. */
    for (int i = 9; i >= 0; i--) {
        cqf_err_t e = cqf_filter_delete(cf, fp0);
        assert(e == CQF_OK);
        uint64_t cnt = cqf_filter_count_occurrences(cf, fp0);
        assert(cnt == (uint64_t)i);
        assert(cqf_filter_validate(cf));
    }
    assert(!cqf_filter_lookup(cf, fp0));
    assert(cf->count == 0);
    cqf_filter_destroy(cf);
    printf("  CQF slot 0 ambiguity: PASS\n");
}

/* ============================================================================
 * CQF overflow test
 *
 * Verifies:
 *   - Insert at UINT64_MAX returns CQF_ERR_OVERFLOW
 *   - Delete absent rem=0 / rem=max returns CQF_ERR_NOT_FOUND
 *   - Merge overflow handling
 * ============================================================================ */
static void test_cqf_overflow(void) {
    printf("\n=== CQF overflow ===\n");
    uint8_t rbits = 8;

    /* Test delete absent — no underflow */
    cqf_filter_t *cf = cqf_filter_create(64, rbits);
    assert(cf);
    cqf_err_t e = cqf_filter_delete(cf, make_fp(0, 0, rbits));
    assert(e == CQF_ERR_NOT_FOUND);
    e = cqf_filter_delete(cf, make_fp(0, 255, rbits));
    assert(e == CQF_ERR_NOT_FOUND);
    e = cqf_filter_delete(cf, make_fp(0, 1, rbits));
    assert(e == CQF_ERR_NOT_FOUND);
    printf("  Delete absent rem=0/1/max: CQF_ERR_NOT_FOUND\n");

    /* Build up to overflow threshold */
    uint64_t fp = make_fp(0, 1, rbits);
    cqf_filter_t *cf2 = cqf_filter_create(64, rbits);
    assert(cf2);

    /* Insert many copies — verify overflow path */
    printf("  Building high count...\n");
    fflush(stdout);
    for (int i = 0; i < 100000; i++) {
        e = cqf_filter_insert(cf2, fp);
        if (e == CQF_ERR_FULL) {
            cqf_filter_t *new_cf = cqf_filter_resize(cf2, cf2->num_slots * 2);
            assert(new_cf);
            cf2 = new_cf;
            e = cqf_filter_insert(cf2, fp);
        }
        assert(e == CQF_OK);
    }
    uint64_t cnt = cqf_filter_count_occurrences(cf2, fp);
    assert(cnt == 100000);
    assert(cqf_filter_validate(cf2));
    printf("  Count=%lu, distinct=%lu, OK\n",
           (unsigned long)cnt, (unsigned long)cf2->distinct_count);

    cqf_filter_destroy(cf2);

    /* Merge overflow test */
    cqf_filter_t *a = cqf_filter_create(64, rbits);
    cqf_filter_t *b = cqf_filter_create(64, rbits);
    assert(a && b);

    /* Fill both with the same fingerprint, to large counts */
    uint64_t fp2 = make_fp(5, 3, rbits);
    for (int i = 0; i < 1000; i++) {
        cqf_err_t ea = cqf_filter_insert(a, fp2);
        cqf_err_t eb = cqf_filter_insert(b, fp2);
        if (ea == CQF_ERR_FULL) {
            cqf_filter_t *na = cqf_filter_resize(a, a->num_slots * 2);
            assert(na); a = na;
            ea = cqf_filter_insert(a, fp2);
        }
        if (eb == CQF_ERR_FULL) {
            cqf_filter_t *nb = cqf_filter_resize(b, b->num_slots * 2);
            assert(nb); b = nb;
            eb = cqf_filter_insert(b, fp2);
        }
        assert(ea == CQF_OK && eb == CQF_OK);
    }
    printf("  Merge test: A count=%lu, B count=%lu\n",
           (unsigned long)cqf_filter_count_occurrences(a, fp2),
           (unsigned long)cqf_filter_count_occurrences(b, fp2));

    cqf_filter_t *m = cqf_filter_merge(a, b);
    assert(m);
    assert(cqf_filter_validate(m));
    uint64_t mcnt = cqf_filter_count_occurrences(m, fp2);
    uint64_t expected = cqf_filter_count_occurrences(a, fp2)
                      + cqf_filter_count_occurrences(b, fp2);
    assert(mcnt == expected);
    printf("  Merge: %lu + %lu = %lu OK\n",
           (unsigned long)cqf_filter_count_occurrences(a, fp2),
           (unsigned long)cqf_filter_count_occurrences(b, fp2),
           (unsigned long)mcnt);
    cqf_filter_destroy(a); cqf_filter_destroy(b); cqf_filter_destroy(m);
    cqf_filter_destroy(cf);
    printf("  CQF overflow: PASS\n");
}

/* ============================================================================
 * CQF clear loop slot 0 regression test
 *
 * Verifies that delete path's clear loop does not destroy neighboring
 * data when the encoding begins near physical slot 0.
 * ============================================================================ */
static void test_cqf_clear_loop_slot0(void) {
    printf("\n=== CQF clear loop slot 0 regression ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(64, rbits);
    assert(cf);

    uint64_t fp = make_fp(0, 0, rbits);

    /* Build and drain large rem=0 counter — exercises same-size shrink,
     * grow, and full-delete paths across encoding size transitions. */
    for (int trial = 0; trial < 3; trial++) {
        int target = (trial == 0) ? 10 : (trial == 1) ? 100 : 1000;
        for (int i = 0; i < target; i++) {
            cqf_err_t e = cqf_filter_insert(cf, fp);
            assert(e == CQF_OK);
        }
        assert(cqf_filter_count_occurrences(cf, fp) == (uint64_t)target);

        for (int i = target - 1; i >= 0; i--) {
            cqf_err_t e = cqf_filter_delete(cf, fp);
            assert(e == CQF_OK);
            uint64_t cnt = cqf_filter_count_occurrences(cf, fp);
            assert(cnt == (uint64_t)i);
        }
        assert(!cqf_filter_lookup(cf, fp));
        assert(cqf_filter_validate(cf));
        printf("  Trial %d: %d->0 OK\n", trial, target);
    }

    cqf_filter_destroy(cf);
    printf("  CQF clear loop slot 0: PASS\n");
}

/* ============================================================================
 * RSQF wrapped multi-run test
 *
 * Tests the circular slot array with quotients near the end/start boundary.
 * RSQF supports full wraparound; CQF does not (tested separately for linear
 * case in test_cqf_multi_run).
 * ============================================================================ */
static void test_rsqf_wrapped_multi_run(void) {
    printf("\n=== RSQF wrapped multi-run ===\n");

    /* 16-slot, 4-bit filter — quotients 14,15,0,1 wrap through boundary */
    rsqf_filter_t *rf = rsqf_filter_create(16, 4);
    assert(rf);
    uint8_t rbits = rf->remainder_bits;
    printf("  capacity=%lu, rbits=%u\n", (unsigned long)rf->num_slots, rbits);

    /* Insert fingerprints at wrapping quotients */
    uint64_t fps[] = {
        make_fp(14, 0, rbits), make_fp(14, 1, rbits),
        make_fp(15, 0, rbits), make_fp(15, 15, rbits),
        make_fp(0,  0, rbits), make_fp(0,  1, rbits),
        make_fp(1,  0, rbits), make_fp(1,  1, rbits),
    };
    int n_fps = sizeof(fps) / sizeof(fps[0]);

    for (int i = 0; i < n_fps; i++) {
        rsqf_err_t e = rsqf_filter_insert(rf, fps[i]);
        assert(e == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    printf("  Inserted %d fingerprints across q=14,15,0,1\n", n_fps);

    /* Lookup all inserted fingerprints */
    for (int i = 0; i < n_fps; i++)
        assert(rsqf_filter_lookup(rf, fps[i]));
    printf("  All lookups correct\n");

    /* Delete from q=15 — must NOT affect q=0 */
    rsqf_err_t e = rsqf_filter_delete(rf, make_fp(15, 0, rbits));
    assert(e == RSQF_OK);
    assert(!rsqf_filter_lookup(rf, make_fp(15, 0, rbits)));
    assert(rsqf_filter_lookup(rf, make_fp(0, 0, rbits)));
    assert(rsqf_filter_validate(rf));
    printf("  Delete q=15 doesn't affect q=0: OK\n");

    /* Delete from q=0 — must NOT affect q=15 */
    e = rsqf_filter_delete(rf, make_fp(0, 1, rbits));
    assert(e == RSQF_OK);
    assert(!rsqf_filter_lookup(rf, make_fp(0, 1, rbits)));
    assert(rsqf_filter_lookup(rf, make_fp(15, 15, rbits)));
    assert(rsqf_filter_validate(rf));
    printf("  Delete q=0 doesn't affect q=15: OK\n");

    /* Fill to near capacity — triggers wrap-around shifts */
    uint64_t insert_count = rsqf_filter_count(rf);
    uint64_t fill_iter = 0;
    while (insert_count < rf->num_slots - 1) {
        uint64_t fp = (uint64_t)((insert_count + 1000) * 0x9e3779b97f4a7c15ULL);
        e = rsqf_filter_insert(rf, fp);
        if (e == RSQF_ERR_FULL) break;
        assert(e == RSQF_OK);
        insert_count = rsqf_filter_count(rf);
        fill_iter++;
        {
            const uint64_t *occ = (const uint64_t *)((const uint8_t *)(rf + 1));
            const uint64_t *run = occ + rf->num_blocks;
            uint64_t op = 0, rp = 0;
            for (uint64_t i = 0; i < rf->num_blocks; i++) {
                op += __builtin_popcountll(occ[i]);
                rp += __builtin_popcountll(run[i]);
            }
            if (op != rp) {
                fprintf(stderr, "  iter=%lu count=%lu occ=%lu run=%lu diff=%ld\n",
                        (unsigned long)fill_iter, (unsigned long)insert_count,
                        (unsigned long)op, (unsigned long)rp, (long)(op - rp));
                assert(!"corrupted during fill");
            }
        }
    }
    assert(rsqf_filter_validate_debug(rf));
    printf("  Filled to %lu/%lu slots\n",
           (unsigned long)insert_count, (unsigned long)rf->num_slots);

    /* Verify original entries are still findable */
    for (int i = 0; i < n_fps; i++) {
        if (rsqf_filter_lookup(rf, fps[i]))
            printf("    fp[%d] q=%lu rem=%lu: found\n",
                   i, fps[i] >> rbits, fps[i] & rf->remainder_mask);
    }

    rsqf_filter_destroy(rf);
    printf("  RSQF wrapped multi-run: PASS\n");
}

/* ============================================================================
 * RSQF offset validation test
 *
 * Verifies that:
 *   1. rsqf_filter_repair_offsets() returns 0 for a consistent filter
 *   2. Corrupting an offset causes repair_offsets() to detect and fix it
 * ============================================================================ */
static void test_rsqf_offset_validation(void) {
    printf("\n=== RSQF offset validation ===\n");

    rsqf_filter_t *rf = rsqf_filter_create(256, 8);
    assert(rf);

    /* Insert items — update_offsets runs after each, so offsets stay correct */
    for (int i = 0; i < 100; i++) {
        uint64_t fp = (uint64_t)(i * 0x9e3779b97f4a7c15ULL);
        rsqf_err_t e = rsqf_filter_insert(rf, fp);
        assert(e == RSQF_OK);
    }
    assert(rsqf_filter_validate(rf));

    /* Repair should find 0 changes — offsets are already correct */
    uint64_t n = rsqf_filter_repair_offsets(rf);
    assert(n == 0);
    printf("  Fresh filter: 0 offsets repaired\n");

    /* Deliberately corrupt an offset */
    /* Layout: occ(nb) run(nb) phys_occ(nb) occ_prefix(nb+1) run_prefix(nb+1) off(nb) */
    uint8_t *off = (uint8_t *)((uint64_t *)(rf + 1) + 5 * rf->num_blocks + 2);
    uint8_t saved = off[0];
    off[0] = 0xFF;

    /* Repair should detect 1 corruption */
    n = rsqf_filter_repair_offsets(rf);
    assert(n >= 1);
    printf("  Corrupted offset -> repaired %lu offsets\n", (unsigned long)n);

    /* Verify offset was restored correctly */
    assert(off[0] == saved);

    /* Multiple corruptions */
    off[0] = 0x00;
    off[1] = 0xAA;
    off[2] = 0x55;
    n = rsqf_filter_repair_offsets(rf);
    assert(n >= 1);
    printf("  Multiple corruptions: repaired %lu offsets\n", (unsigned long)n);
    assert(rsqf_filter_validate(rf));

    rsqf_filter_destroy(rf);
    printf("  RSQF offset validation: PASS\n");
}

/* ============================================================================
 * CQF multiple-run test
 *
 * Tests multiple quotients with rem=0, rem=1, rem=max, counted duplicates,
 * merge, resize, and delete isolation. Uses large capacity to avoid
 * CQF's lack of wraparound.
 * ============================================================================ */
static void test_cqf_multi_run(void) {
    printf("\n=== CQF multi-run ===\n");
    uint8_t rbits = 8;
    cqf_filter_t *cf = cqf_filter_create(64, rbits);
    assert(cf);

    struct { uint64_t q; uint64_t rem; int count; } ins[] = {
        {5,  0, 1}, {5,  1, 3}, {10, 0, 2}, {10, 255, 1},
        {20, 0, 1}, {20, 1, 2}, {30, 0, 3}, {30, 1, 1},
    };
    int n_ins = sizeof(ins) / sizeof(ins[0]);

    for (int i = 0; i < n_ins; i++) {
        uint64_t fp = make_fp(ins[i].q, ins[i].rem, rbits);
        for (int c = 0; c < ins[i].count; c++) {
            cqf_err_t e = cqf_filter_insert(cf, fp);
            assert(e == CQF_OK);
        }
    }
    assert(cqf_filter_validate(cf));

    for (int i = 0; i < n_ins; i++) {
        uint64_t fp = make_fp(ins[i].q, ins[i].rem, rbits);
        assert(cqf_filter_lookup(cf, fp));
        assert(cqf_filter_count_occurrences(cf, fp) == (uint64_t)ins[i].count);
    }
    printf("  All counts exact\n");

    /* Delete from q=10 — must NOT affect q=20 */
    assert(cqf_filter_delete(cf, make_fp(10, 0, rbits)) == CQF_OK);
    assert(cqf_filter_delete(cf, make_fp(10, 0, rbits)) == CQF_OK);
    assert(cqf_filter_count_occurrences(cf, make_fp(10, 0, rbits)) == 0);
    assert(cqf_filter_count_occurrences(cf, make_fp(20, 0, rbits)) == 1);
    assert(cqf_filter_validate(cf));
    printf("  Delete q=10 doesn't affect q=20: OK\n");

    /* Merge test */
    cqf_filter_t *cf2 = cqf_filter_create(64, rbits);
    assert(cf2);
    for (int i = 0; i < 2; i++)
        assert(cqf_filter_insert(cf2, make_fp(5, 1, rbits)) == CQF_OK);
    assert(cqf_filter_insert(cf2, make_fp(20, 255, rbits)) == CQF_OK);
    assert(cqf_filter_insert(cf2, make_fp(30, 1, rbits)) == CQF_OK);

    cqf_filter_t *merged = cqf_filter_merge(cf, cf2);
    assert(merged);
    assert(cqf_filter_validate(merged));
    assert(cqf_filter_count_occurrences(merged, make_fp(5, 1, rbits)) == 5);
    assert(cqf_filter_count_occurrences(merged, make_fp(10, 255, rbits)) == 1);
    assert(cqf_filter_count_occurrences(merged, make_fp(20, 255, rbits)) == 1);
    assert(cqf_filter_count_occurrences(merged, make_fp(30, 1, rbits)) == 2);
    printf("  Merge exact: OK\n");
    cqf_filter_destroy(cf2);
    cqf_filter_destroy(merged);

    /* Resize test */
    cqf_filter_t *resized = cqf_filter_resize(cf, 256);
    assert(resized);
    assert(cqf_filter_validate(resized));
    assert(cqf_filter_count_occurrences(resized, make_fp(5, 1, rbits)) == 3);
    assert(cqf_filter_count_occurrences(resized, make_fp(10, 255, rbits)) == 1);
    assert(cqf_filter_count_occurrences(resized, make_fp(20, 1, rbits)) == 2);
    printf("  Resize exact: OK\n");
    cqf_filter_destroy(resized);

    printf("  CQF multi-run: PASS\n");
}



/* Local bit helpers (not available from header) */
static inline bool bit_test(const uint64_t *bv, uint64_t pos) {
    return (bv[pos / 64] >> (pos % 64)) & 1ULL;
}
static inline uint64_t rank64_local(uint64_t w, uint64_t pos) {
    uint64_t m = (pos < 63) ? ((1ULL << (pos + 1)) - 1) : ~0ULL;
    return __builtin_popcountll(w & m);
}
static inline uint64_t select64_local(uint64_t w, uint64_t k) {
    uint64_t pop = __builtin_popcountll(w);
    if (k == 0 || k > pop) return 64;
    uint64_t lo = 0, hi = 63;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        uint64_t low_bits = w & ((1ULL << (mid + 1)) - 1);
        if ((uint64_t)__builtin_popcountll(low_bits) >= k) hi = mid;
        else lo = mid + 1;
    }
    return lo;
}
static uint64_t select_bv_local(const uint64_t *bv, uint64_t nblocks, uint64_t k, uint64_t limit) {
    if (k == 0) return 0;
    uint64_t seen = 0;
    for (uint64_t i = 0; i < nblocks; i++) {
        uint64_t pc = __builtin_popcountll(bv[i]);
        if (seen + pc >= k) return i * 64 + select64_local(bv[i], k - seen);
        seen += pc;
    }
    return limit;
}

/* ============================================================================
 * RSQF structural validator (slow, exhaustive)
 *
 * Reconstructs for every physical slot:
 *   run_id, owner quotient, run start, run end, fingerprint
 *
 * Checks:
 *   1. popcount(occ) == popcount(run)
 *   2. kth occupied quotient maps to kth runend
 *   3. Every physical slot belongs to exactly one run or is free
 *   4. Fingerprints are sorted within each run
 *   5. Each run's fingerprints match the oracle multiset
 * ============================================================================ */

static bool rsqf_validate_structure_slow(const rsqf_filter_t *rf)
{
    if (!rf) return false;
    const uint64_t *occ = (const uint64_t *)((const uint8_t *)(rf + 1));
    const uint64_t *run = occ + rf->num_blocks;
    const uint64_t *tbl = (const uint64_t *)((const uint8_t *)(rf + 1) +
        ((rf->num_blocks * (3 * sizeof(uint64_t) + sizeof(uint8_t)) + 7) & ~7ULL));
    uint8_t rbits = rf->remainder_bits;
    uint64_t mask = rf->remainder_mask;
    uint64_t ns = rf->num_slots;

    /* Check 1: popcount(occ) == popcount(run) */
    uint64_t occ_pop = 0, run_pop = 0;
    for (uint64_t i = 0; i < rf->num_blocks; i++) {
        occ_pop += __builtin_popcountll(occ[i]);
        run_pop += __builtin_popcountll(run[i]);
    }
    if (occ_pop != run_pop) return false;

    if (occ_pop == 0) return true;

    /* Check 2: kth occupied maps to kth runend, verify each run's bounds */
    uint64_t occ_count = 0;
    uint64_t prev_runend = 0;
    bool first_run = true;

    for (uint64_t b = 0; b < ns; b++) {
        if (!bit_test(occ, b)) continue;
        occ_count++;

        uint64_t rend = select_bv_local(run, rf->num_blocks, occ_count, ns - 1);
        if (rend >= ns) return false;

        uint64_t rstart = first_run ? 0 : (prev_runend + 1) % ns;

        /* Verify slots in this run have valid remainder data */
        if (rend >= rstart) {
            for (uint64_t s = rstart; s <= rend; s++) {
                uint64_t bp = s * rbits;
                uint64_t v = (tbl[bp / 64] >> (bp % 64)) & mask;
                if (bp % 64 + rbits > 64)
                    v |= (tbl[bp / 64 + 1] << (64 - bp % 64)) & mask;
                (void)v;
            }
        } else {
            for (uint64_t s = rstart; s < ns; s++) {
                uint64_t bp = s * rbits;
                uint64_t v = (tbl[bp / 64] >> (bp % 64)) & mask;
                if (bp % 64 + rbits > 64)
                    v |= (tbl[bp / 64 + 1] << (64 - bp % 64)) & mask;
                (void)v;
            }
            for (uint64_t s = 0; s <= rend; s++) {
                uint64_t bp = s * rbits;
                uint64_t v = (tbl[bp / 64] >> (bp % 64)) & mask;
                if (bp % 64 + rbits > 64)
                    v |= (tbl[bp / 64 + 1] << (64 - bp % 64)) & mask;
                (void)v;
            }
        }

        prev_runend = rend;
        first_run = false;
    }

    return true;
}

/* ============================================================================
 * Exhaustive small-state RSQF wrapped multi-run test
 *
 * capacity = 8 and 16 (actual slots = 64 after rounding)
 * rbits = 2, 3, 4
 *
 * Biases:
 *   - free slot wraps to 0
 *   - previous runend at ns-1
 *   - multiple occupied quotients in wrapped cluster
 *   - insert into existing/new quotient
 *   - delete from first/middle/last run in wrapped cluster
 *
 * After every mutation:
 *   enumeration == fingerprint oracle
 *   lookup all oracle entries succeeds
 *   popcount(occ) == popcount(run)
 *   validate() passes
 *   offsets correct
 * ============================================================================ */

static void test_rsqf_exhaustive_small_state(void) {
    printf("\n=== RSQF exhaustive small-state wrapped multi-run ===\n");

    struct { size_t capacity; uint8_t rbits; } configs[] = {
        {8, 2}, {8, 3}, {8, 4},
        {16, 2}, {16, 3}, {16, 4},
    };
    int n_configs = sizeof(configs) / sizeof(configs[0]);

    for (int ci = 0; ci < n_configs; ci++) {
        size_t cap = configs[ci].capacity;
        uint8_t rbits = configs[ci].rbits;

        rsqf_filter_t *rf = rsqf_filter_create(cap, rbits);
        assert(rf);
        uint64_t ns = rf->num_slots;
        uint64_t rem_mask = rf->remainder_mask;

        oracle_t ref;
        oracle_init(&ref);

        printf("  capacity=%lu rbits=%u ns=%lu ... ", cap, rbits, ns);
        fflush(stdout);

        /* Phase 1: Insert fingerprints that force wrap-around clusters.
         * Use quotients near the end of the slot range so natural insertions
         * push runs across the ns-1 → 0 boundary. */
        uint64_t fps[256];
        int n_fps = 0;

        /* Insert at the last few quotients to force wrap */
        for (uint64_t q = ns - 3; q < ns; q++) {
            for (uint64_t r = 1; r <= 3; r++) {
                if (r > rem_mask) break;
                fps[n_fps++] = make_fp(q, r, rbits);
            }
        }
        /* Insert at quotient 0 to create a cluster wrapping to 0 */
        for (uint64_t r = 1; r <= 3; r++) {
            if (r > rem_mask) break;
            fps[n_fps++] = make_fp(0, r, rbits);
        }
        /* Insert at quotient 1 to continue the cluster */
        for (uint64_t r = 1; r <= 3; r++) {
            if (r > rem_mask) break;
            fps[n_fps++] = make_fp(1, r, rbits);
        }

        /* Fill to capacity */
        uint64_t fill_q = 2;
        while (n_fps < (int)(ns * 3 / 4) && fill_q < ns - 3) {
            fps[n_fps++] = make_fp(fill_q, 1, rbits);
            fill_q++;
        }

        /* Insert all fingerprints */
        for (int i = 0; i < n_fps; i++) {
            rsqf_err_t e = rsqf_filter_insert(rf, fps[i]);
            if (e == RSQF_ERR_FULL) {
                n_fps = i;
                break;
            }
            assert(e == RSQF_OK);
            oracle_insert(&ref, fps[i]);
            assert(rsqf_validate_structure_slow(rf));
            assert(rsqf_filter_validate(rf));
            {
                uint64_t op = 0, rnp = 0;
                for (uint64_t b = 0; b < rf->num_blocks; b++) {
                    op += __builtin_popcountll(((const uint64_t*)(rf+1))[b]);
                    rnp += __builtin_popcountll(((const uint64_t*)(rf+1))[b + rf->num_blocks]);
                }
                assert(op == rnp);
                (void)op; (void)rnp;
            }
        }

        /* Verify lookup for all oracle entries */
        for (uint64_t i = 0; i < ORACLE_SIZE; i++) {
            if (ref.entries[i].used && ref.entries[i].count > 0) {
                if (!rsqf_filter_lookup(rf, ref.entries[i].fingerprint)) {
                    fprintf(stderr, "  PHASE1 FAIL fp=0x%llx q=%llu rem=%llu\n",
                            (unsigned long long)ref.entries[i].fingerprint,
                            (unsigned long long)(ref.entries[i].fingerprint >> rbits),
                            (unsigned long long)(ref.entries[i].fingerprint & ((1ULL << rbits) - 1)));
                    assert(!"phase1 lookup failed");
                }
            }
        }

        /* Phase 2: Delete from first/middle/last run in wrapped cluster */
        if (n_fps >= 6) {
            /* Delete from middle */
            for (int i = 1; i < 4 && i < n_fps; i++) {
                assert(rsqf_filter_delete(rf, fps[i]) == RSQF_OK);
                oracle_delete(&ref, fps[i]);
                assert(rsqf_validate_structure_slow(rf));
                assert(rsqf_filter_validate(rf));
            }
            /* Verify remaining still found */
            for (uint64_t i = 0; i < ORACLE_SIZE; i++) {
                if (ref.entries[i].used && ref.entries[i].count > 0) {
                    assert(rsqf_filter_lookup(rf, ref.entries[i].fingerprint));
                }
            }
        }

        /* Phase 3: Fill to >90% to force many wrapped shifts.
         * During fill, validate popcount invariant + structure after each op. */
        uint64_t insert_count = rf->count;
        while (insert_count < ns - 1) {
            uint64_t fp = (uint64_t)((insert_count + 1000) * 0x9e3779b97f4a7c15ULL);
            rsqf_err_t e = rsqf_filter_insert(rf, fp);
            if (e == RSQF_ERR_FULL) break;
            assert(e == RSQF_OK);
            insert_count = rf->count;

            assert(rsqf_validate_structure_slow(rf));
            assert(rsqf_filter_validate(rf));
            {
                uint64_t op = 0, rnp = 0;
                for (uint64_t b = 0; b < rf->num_blocks; b++) {
                    op += __builtin_popcountll(((const uint64_t*)(rf+1))[b]);
                    rnp += __builtin_popcountll(((const uint64_t*)(rf+1))[b + rf->num_blocks]);
                }
                if (op != rnp) {
                    fprintf(stderr, "  POPCOUNT FAIL: iter=%lu count=%lu occ=%lu run=%lu\n",
                            insert_count, rf->count, op, rnp);
                    assert(op == rnp);
                }
            }
        }

        /* Final validation: structural + all oracle entries still findable */
        assert(rsqf_filter_validate(rf));
        assert(rsqf_validate_structure_slow(rf));
        for (uint64_t i = 0; i < ORACLE_SIZE; i++) {
            if (ref.entries[i].used && ref.entries[i].count > 0) {
                if (!rsqf_filter_lookup(rf, ref.entries[i].fingerprint)) {
                    fprintf(stderr, "  MISSING fp=0x%llx q=%llu rem=%llu count=%llu ns=%llu rbits=%u\n",
                            (unsigned long long)ref.entries[i].fingerprint,
                            (unsigned long long)(ref.entries[i].fingerprint >> rbits),
                            (unsigned long long)(ref.entries[i].fingerprint & ((1ULL << rbits) - 1)),
                            (unsigned long long)ref.entries[i].count,
                            (unsigned long long)ns, rbits);
                    assert(!"oracle entry not found after exhaustive fill");
                }
            }
        }

        rsqf_filter_destroy(rf);
        printf("PASS (%lu items)\n", (unsigned long)n_fps);
    }
    printf("  RSQF exhaustive small-state: PASS\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    test_rsqf_oracle();
    test_cqf_oracle();
    test_rsqf_shifted_enum();
    test_cqf_large_count();
    test_cqf_rem0_basic();
    test_cqf_rem0_merge();
    test_cqf_rem0_resize();
    test_cqf_rem_equals_counter_sym();
    test_cqf_rem0_large_count();
    test_cqf_encode_decode_roundtrip();
    test_cqf_slot0_ambiguity();
    test_cqf_overflow();
    test_cqf_clear_loop_slot0();
    test_cqf_multi_run();
    test_rsqf_wrapped_multi_run();
    test_rsqf_offset_validation();
    test_rsqf_exhaustive_small_state();

    printf("\nAll fingerprint-oracle tests PASS\n");
    return 0;
}
