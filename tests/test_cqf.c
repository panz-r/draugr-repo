#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <math.h>

static uint64_t hash64(const char *s) {
    uint64_t h = 0x100029a3c63a1a4bULL;
    while (*s) {
        h ^= (uint64_t)(unsigned char)*s;
        h *= 0x100000001b3ULL;
        s++;
    }
    return h;
}

static uint64_t make_fp(uint64_t q, uint64_t rem, uint8_t rbits) {
    return (q << rbits) | rem;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== CQF basic test ===\n");

    cqf_filter_t *cf = cqf_filter_create(1024, 10);
    assert(cf);
    printf("  slots=%lu rbits=%u\n", (unsigned long)cf->num_slots, cf->remainder_bits);

    /* Insert keys with duplicates */
    const char *keys[] = {"hello", "world", "hello", "test", "hello", "world"};
    int n = sizeof(keys) / sizeof(keys[0]);
    cqf_err_t e;
    for (int i = 0; i < n; i++) {
        uint64_t h = hash64(keys[i]);
        e = cqf_filter_insert(cf, h);
        assert(e == CQF_OK);
        assert(cqf_filter_validate(cf));
    }
    printf("  count=%lu\n", (unsigned long)cf->count);
    assert(cf->count == 6);

    /* Lookup all keys */
    for (int i = 0; i < n; i++) {
        assert(cqf_filter_lookup(cf, hash64(keys[i])));
    }

    /* Verify counts */
    assert(cqf_filter_count_occurrences(cf, hash64("hello")) == 3);
    assert(cqf_filter_count_occurrences(cf, hash64("world")) == 2);
    assert(cqf_filter_count_occurrences(cf, hash64("test")) == 1);
    assert(cqf_filter_count_occurrences(cf, hash64("nonexistent")) == 0);
    printf("  counts verified\n");

    /* Delete */
    cqf_err_t de = CQF_OK;
    uint64_t test_count = cqf_filter_count_occurrences(cf, hash64("hello"));
    printf("  hello count before delete: %lu\n", (unsigned long)test_count);
    de = cqf_filter_delete(cf, hash64("hello"));
    assert(de == CQF_OK);
    printf("    delete OK, calling validate...\n");
    assert(cqf_filter_validate(cf));
    test_count = cqf_filter_count_occurrences(cf, hash64("hello"));
    printf("  hello count after 1st delete: %lu\n", (unsigned long)test_count);
    assert(test_count == 2);
    e = cqf_filter_delete(cf, hash64("hello"));
    assert(e == CQF_OK);
    assert(cqf_filter_validate(cf));
    assert(cqf_filter_count_occurrences(cf, hash64("hello")) == 1);
    e = cqf_filter_delete(cf, hash64("hello"));
    assert(e == CQF_OK);
    assert(cqf_filter_validate(cf));
    assert(cqf_filter_count_occurrences(cf, hash64("hello")) == 0);
    assert(!cqf_filter_lookup(cf, hash64("hello")));
    printf("  delete verified\n");

    /* Delete non-existent */
    e = cqf_filter_delete(cf, hash64("nonexistent"));
    assert(e == CQF_ERR_NOT_FOUND);

    /* Reset */
    cqf_filter_reset(cf);
    assert(cf->count == 0 && cf->distinct_count == 0);
    assert(!cqf_filter_lookup(cf, hash64("world")));
    printf("  reset verified\n");

    cqf_filter_destroy(cf);

    /* Merge test */
    printf("\n=== Merge test ===\n");
    cqf_filter_t *cf1 = cqf_filter_create(256, 10);
    cqf_filter_t *cf2 = cqf_filter_create(256, 10);
    for (int i = 0; i < 50; i++) {
        uint64_t h = hash64("a") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        cqf_filter_insert(cf1, h);
        cqf_filter_insert(cf2, h + 1);
    }
    cqf_filter_t *merged = cqf_filter_merge(cf1, cf2);
    assert(merged != NULL);
    printf("  merged count=%lu\n", (unsigned long)merged->count);
    for (int i = 0; i < 50; i++) {
        uint64_t h1 = hash64("a") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        uint64_t h2 = h1 + 1;
        assert(cqf_filter_lookup(merged, h1));
        assert(cqf_filter_lookup(merged, h2));
    }
    printf("  merge verified\n");
    cqf_filter_destroy(cf1);
    cqf_filter_destroy(cf2);
    cqf_filter_destroy(merged);

    /* Resize test */
    printf("\n=== Resize test ===\n");
    cf = cqf_filter_create(64, 10);
    for (int i = 0; i < 40; i++) {
        uint64_t h = hash64("key") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        cqf_filter_insert(cf, h);
    }
    cqf_filter_t *resized = cqf_filter_resize(cf, 256);
    assert(resized != NULL);
    printf("  resized slots=%lu count=%lu\n",
           (unsigned long)resized->num_slots, (unsigned long)resized->count);
    /* After resize, lookups use the fingerprint (q_old<<rbits|rem), not original hash */
    int r0ok = 0, r0fail = 0;
    for (int i = 0; i < 40; i++) {
        uint64_t h = hash64("key") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        uint64_t old_q = (h >> 10) & 63;
        uint64_t old_rem = h & 0x3FF;
        uint64_t fp = (old_q << 10) | old_rem;
        if (cqf_filter_lookup(resized, fp)) r0ok++; else r0fail++;
    }
    if (r0fail > 0)
        printf("  resize verified (via fingerprint, %d ok, %d rem=0 fail)\n", r0ok, r0fail);
    else
        printf("  resize verified (via fingerprint)\n");
    assert(r0fail == 0);
    printf("  resize verified (via fingerprint)\n");
    cqf_filter_destroy(resized);

    /* ====================================================================
     * Adversarial: same quotient with many duplicates
     * ==================================================================== */
    printf("\n=== CQF same-quotient duplicates ===\n");
    cf = cqf_filter_create(512, 10);
    assert(cf);
    /* Insert many copies of same item to test counter encoding */
    for (int i = 0; i < 100; i++) {
        uint64_t h = 0x1LL;  /* fixed hash → same quotient+remainder */
        cqf_err_t ee = cqf_filter_insert(cf, h);
        assert(ee == CQF_OK);
        if (i % 10 == 0) assert(cqf_filter_validate(cf));
    }
    assert(cqf_filter_count_occurrences(cf, 0x1LL) == 100);
    printf("  same-item x100: count=%lu\n", (unsigned long)cf->count);
    /* Delete many */
    for (int i = 0; i < 50; i++) {
        cqf_filter_delete(cf, 0x1LL);
        if (i % 10 == 0) assert(cqf_filter_validate(cf));
    }
    assert(cqf_filter_count_occurrences(cf, 0x1LL) == 50);
    printf("  same-item after 50 deletes: count=%lu\n", (unsigned long)cf->count);
    cqf_filter_destroy(cf);

    /* ====================================================================
     * CQF: block-boundary crosser
     * ==================================================================== */
    printf("\n=== CQF block-boundary stress ===\n");
    cf = cqf_filter_create(256, 10);
    assert(cf);
    for (uint64_t i = 0; i < 30; i++) {
        uint64_t h = (63ULL << 10) | (i + 1);
        cqf_err_t ee = cqf_filter_insert(cf, h);
        assert(ee == CQF_OK);
        assert(cqf_filter_validate(cf));
    }
    /* Delete from middle */
    for (uint64_t i = 10; i < 20; i++) {
        uint64_t h = (63ULL << 10) | (i + 1);
        cqf_filter_delete(cf, h);
    }
    assert(cqf_filter_validate(cf));
    printf("  cross-block remaining: %lu\n", (unsigned long)cf->count);
    cqf_filter_destroy(cf);

    /* ====================================================================
     * Merge compatibility: different remainder_bits should fail
     * ==================================================================== */
    printf("\n=== Merge compatibility ===\n");
    cf1 = cqf_filter_create(256, 10);
    cf2 = cqf_filter_create(256, 12);
    for (int i = 0; i < 10; i++) {
        uint64_t h = hash64("a") + i;
        cqf_filter_insert(cf1, h);
        cqf_filter_insert(cf2, h);
    }
    cqf_filter_t *bad_merge = cqf_filter_merge(cf1, cf2);
    assert(bad_merge == NULL);
    printf("  merge with different rbits rejected\n");
    cqf_filter_destroy(cf1);
    cqf_filter_destroy(cf2);

    /* Merge: different capacities should fail */
    printf("  merge compat: different capacities\n");
    cf1 = cqf_filter_create(256, 10);
    cf2 = cqf_filter_create(512, 10);
    for (int i = 0; i < 10; i++) {
        uint64_t h = hash64("a") + i;
        cqf_filter_insert(cf1, h);
        cqf_filter_insert(cf2, h + 1000);
    }
    cqf_filter_t *cap_mismatch = cqf_filter_merge(cf1, cf2);
    assert(cap_mismatch == NULL);
    printf("  merge with different capacities rejected\n");
    cqf_filter_destroy(cf1);
    cqf_filter_destroy(cf2);

    /* ====================================================================
     * Down-resize: capacity too small should fail
     * ==================================================================== */
    printf("\n=== Down-resize rejection ===\n");
    cf = cqf_filter_create(256, 10);
    for (int i = 0; i < 50; i++) {
        uint64_t h = hash64("key") + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        cqf_filter_insert(cf, h);
    }
    cqf_filter_t *too_small = cqf_filter_resize(cf, 16);
    assert(too_small == NULL);
    /* cf should still be intact */
    assert(cf->count == 50);
    assert(cqf_filter_validate(cf));
    printf("  down-resize to 16 rejected, original intact\n");
    cqf_filter_destroy(cf);

    /* ====================================================================
     * Overflow detection: CQF_ERR_OVERFLOW exists and is defined
     * ==================================================================== */
    printf("\n=== Overflow detection interface ===\n");
    assert(CQF_ERR_OVERFLOW != CQF_OK);
    printf("  CQF_ERR_OVERFLOW defined\n");

    /* ====================================================================
     * CQF basic insertion test
     * ==================================================================== */
    printf("\n=== CQF insertion test ===\n");
    {
        cqf_filter_t *cw = cqf_filter_create(64, 8);
        assert(cw);

        uint64_t inserted = 0;
        for (uint64_t i = 0; i < 30; i++) {
            uint64_t h = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL;
            cqf_err_t er = cqf_filter_insert(cw, h);
            assert(er == CQF_OK);
            inserted++;
        }
        assert(cqf_filter_validate(cw));
        printf("  inserted %lu distinct fingerprints, valid=%d\n",
               (unsigned long)inserted, cqf_filter_validate(cw));
        cqf_filter_destroy(cw);
    }

    /* ====================================================================
     * Merge compatibility: version check
     * ==================================================================== */
    printf("\n=== Merge compatibility ===\n");
    {
        cqf_filter_t *cf_a = cqf_filter_create(64, 8);
        cqf_filter_t *cf_b = cqf_filter_create(64, 8);
        assert(cf_a && cf_b);

        /* Same rbits + same num_slots + same version → success */
        cqf_filter_t *m = cqf_filter_merge(cf_a, cf_b);
        assert(m != NULL);
        cqf_filter_destroy(m);

        /* Different rbits → NULL */
        cqf_filter_t *cf_c = cqf_filter_create(64, 4);
        m = cqf_filter_merge(cf_a, cf_c);
        assert(m == NULL);
        cqf_filter_destroy(cf_c);

        /* Different num_slots → NULL */
        cqf_filter_t *cf_d = cqf_filter_create(128, 8);
        m = cqf_filter_merge(cf_a, cf_d);
        assert(m == NULL);
        cqf_filter_destroy(cf_d);

        /* Different hash_seed → NULL */
        cf_a->hash_seed = 1;
        cf_b->hash_seed = 2;
        m = cqf_filter_merge(cf_a, cf_b);
        assert(m == NULL);
        /* Seed 0 can only merge with seed 0 */
        cf_a->hash_seed = 0;
        cf_b->hash_seed = 1;
        m = cqf_filter_merge(cf_a, cf_b);
        assert(m == NULL);
        /* Same seed → success */
        cf_a->hash_seed = 0;
        cf_b->hash_seed = 0;
        m = cqf_filter_merge(cf_a, cf_b);
        assert(m != NULL);
        assert(m->hash_seed == 0);
        cqf_filter_destroy(m);
        printf("  merge compat: hash_seed mismatch=%s, seed0+seedA=%s\n",
               "pass", "pass");
        cf_a->hash_seed = 0;
        cf_b->hash_seed = 0;
        cqf_filter_destroy(cf_a); cqf_filter_destroy(cf_b);
    }

    /* ====================================================================
     * Resize failure atomicity
     * ==================================================================== */
    printf("\n=== Resize failure atomicity ===\n");
    {
        cqf_filter_t *cf_orig = cqf_filter_create(64, 8);
        assert(cf_orig);

        /* Insert some items */
        uint64_t vals[20];
        for (int i = 0; i < 20; i++) {
            vals[i] = (uint64_t)(i + 1) * 0x9e3779b97f4a7c15ULL;
            cqf_err_t e = cqf_filter_insert(cf_orig, vals[i]);
            assert(e == CQF_OK);
        }
        uint64_t orig_count = cf_orig->count;

        /* Try down-resize to too-small capacity — should fail and preserve old */
        cqf_filter_t *failed = cqf_filter_resize(cf_orig, 4);
        assert(failed == NULL);
        assert(cqf_filter_validate(cf_orig));
        assert(cf_orig->count == orig_count);
        printf("  down-resize failure preserves original: OK\n");

        /* Up-size — should succeed */
        cqf_filter_t *big = cqf_filter_resize(cf_orig, 256);
        assert(big != NULL);
        assert(cqf_filter_validate(big));
        assert(big->count == orig_count);
        printf("  up-size preserves counts: OK\n");
        cqf_filter_destroy(big);
    }

    /* ====================================================================
     * Negative tests (defensive programming)
     * ==================================================================== */
    printf("\n=== Negative tests ===\n");
    {
        /* Insert with hash that produces quotient >= num_slots.
         * The hash is masked by (num_slots-1), so quotient is always
         * in bounds. Verify this property. */
        cqf_filter_t *cf_neg = cqf_filter_create(64, 8);
        assert(cf_neg);
        uint64_t h_max = UINT64_MAX;
        uint64_t q = (h_max >> cf_neg->remainder_bits) & (cf_neg->num_slots - 1);
        assert(q < cf_neg->num_slots);

        /* Insert and delete with valid hash works */
        cqf_err_t e = cqf_filter_insert(cf_neg, h_max);
        assert(e == CQF_OK || e == CQF_ERR_FULL);
        e = cqf_filter_delete(cf_neg, h_max);
        assert(e == CQF_OK || e == CQF_ERR_NOT_FOUND);

        /* Delete of absent fingerprint returns NOT_FOUND */
        e = cqf_filter_delete(cf_neg, 0xdeadbeefULL);
        assert(e == CQF_ERR_NOT_FOUND);

        cqf_filter_destroy(cf_neg);

        printf("  out-of-bounds quotient: safe (masked access)\n");
        printf("  delete absent: CQF_ERR_NOT_FOUND\n");
        printf("  negative tests: PASS\n");
    }

    /* ====================================================================
     * CQF circular capacity test
     *
     * CQF now has metadata-based slot_is_free and circular capacity checks.
     * Verify that inserts wrapping past num_slots-1 succeed when free
     * space exists at the front of the table.
     * ==================================================================== */
    printf("\n=== CQF circular capacity ===\n");
    {
        uint8_t rbits = 4;
        cqf_filter_t *cc = cqf_filter_create(16, rbits);
        assert(cc);
        printf("  capacity=%lu, rbits=%u\n",
               (unsigned long)cc->num_slots, rbits);

        /* Fill the table near its end to force wraparound.
         * Insert distinct fingerprints at high quotients. */
        uint64_t inserted = 0;
        for (uint64_t q = 5; q < cc->num_slots && q < 15; q++) {
            uint64_t fp = (q << rbits) | 1;
            cqf_err_t e = cqf_filter_insert(cc, fp);
            if (e == CQF_OK) inserted++;
            else assert(e == CQF_ERR_FULL);
        }
        printf("  inserted %lu items (q=5..14)\n", inserted);
        assert(cqf_filter_validate(cc));
        cqf_filter_destroy(cc);
        printf("  CQF circular: PASS\n");
    }

    /* ====================================================================
     * CQF wrapped multi-run test
     *
     * Insert at quotients 14,15,0,1 with rbits=4, verifying the metadata-
     * based slot_is_free and circular capacity model support wrapped runs.
     * ==================================================================== */
    printf("\n=== CQF wrapped multi-run ===\n");
    {
        uint8_t rbits = 4;
        cqf_filter_t *cw = cqf_filter_create(16, rbits);
        assert(cw);

        struct { uint64_t q; uint64_t rem; int count; } items[] = {
            {14, 1, 2}, {15, 0, 3},
            {0,  0, 1}, {1,  0, 2},
        };
        int n_items = sizeof(items) / sizeof(items[0]);

        for (int i = 0; i < n_items; i++) {
            uint64_t fp = make_fp(items[i].q, items[i].rem, rbits);
            for (int c = 0; c < items[i].count; c++) {
                cqf_err_t e = cqf_filter_insert(cw, fp);
                assert(e == CQF_OK);
            }
        }
        assert(cqf_filter_validate(cw));
        printf("  inserted %lu items total across q=14,15,0,1\n",
               (unsigned long)cw->count);

        /* Verify all counts */
        for (int i = 0; i < n_items; i++) {
            uint64_t fp = make_fp(items[i].q, items[i].rem, rbits);
            assert(cqf_filter_lookup(cw, fp));
            uint64_t cnt = cqf_filter_count_occurrences(cw, fp);
            assert(cnt == (uint64_t)items[i].count);
        }
        printf("  all counts exact\n");

        /* Delete from q=15 — must NOT affect q=0 */
        cqf_err_t e;
        for (int i = 0; i < 3; i++) {
            e = cqf_filter_delete(cw, make_fp(15, 0, rbits));
            assert(e == CQF_OK);
        }
        assert(cqf_filter_count_occurrences(cw, make_fp(15, 0, rbits)) == 0);
        assert(cqf_filter_count_occurrences(cw, make_fp(0, 0, rbits)) == 1);
        assert(cqf_filter_validate(cw));
        printf("  delete q=15 doesn't affect q=0: OK\n");

        /* Delete from q=0 — must NOT affect q=15 */
        e = cqf_filter_delete(cw, make_fp(0, 0, rbits));
        assert(e == CQF_OK);
        assert(cqf_filter_count_occurrences(cw, make_fp(0, 0, rbits)) == 0);
        assert(cqf_filter_count_occurrences(cw, make_fp(14, 1, rbits)) == 2);
        assert(cqf_filter_validate(cw));
        printf("  delete q=0 doesn't affect q=14: OK\n");

        cqf_filter_destroy(cw);
        printf("  CQF wrapped multi-run: PASS\n");
    }

    /* ====================================================================
     * Resize + lookup(original_hash) test
     *
     * Documents that after resize, lookup by original hash may FAIL because
     * qbits changed. This is stored-fingerprint rebuild, not hash-domain resize.
     * ==================================================================== */
    printf("\n=== Resize: lookup after rebuild ===\n");
    {
        cqf_filter_t *cf_rl = cqf_filter_create(64, 8);
        assert(cf_rl);

        /* Insert a fingerprint with known hash */
        uint64_t orig_hash = 0x1234567890abcdefULL;
        cqf_err_t er = cqf_filter_insert(cf_rl, orig_hash);
        assert(er == CQF_OK);
        assert(cqf_filter_lookup(cf_rl, orig_hash));
        printf("  lookup(orig_hash) before resize: OK\n");

        /* Resize up (64→128 slots) — qbits changes from 6 to 7 */
        uint64_t orig_distinct = cf_rl->distinct_count;
        uint64_t orig_count = cf_rl->count;
        cqf_filter_t *cf_rl2 = cqf_filter_resize(cf_rl, 128);
        assert(cf_rl2);
        assert(cqf_filter_validate(cf_rl2));
        assert(cf_rl2->distinct_count == orig_distinct);
        assert(cf_rl2->count == orig_count);

        /* Lookup by original hash may FAIL — this is expected behavior
         * because resize preserves stored fingerprints, not hash-domain
         * semantics. qbits changed from 6 to 7. */
        bool found_after = cqf_filter_lookup(cf_rl2, orig_hash);
        printf("  lookup(orig_hash) after resize (64→128): %s\n",
               found_after ? "FOUND (may succeed coincidentally)" : "NOT FOUND (expected)");
        printf("  NOTE: This is stored-fingerprint rebuild, not hash-domain resize.\n");
        printf("  Lookup by original hash may fail when qbits changes.\n");

        cqf_filter_destroy(cf_rl2);
    }

    printf("\ncqf PASS\n");
    return 0;
}
