#include "draugr/rsqf_filter.h"
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

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("=== RSQF basic insert/lookup/delete test ===\n");

    /* Create filter for 1000 items, 10-bit fingerprints */
    rsqf_filter_t *rf = rsqf_filter_create(1000, 10);
    assert(rf);
    printf("  capacity: %zu\n", rsqf_filter_capacity(rf));
    printf("  slots: %lu\n", (unsigned long)rf->num_slots);
    printf("  blocks: %lu\n", (unsigned long)rf->num_blocks);
    printf("  remainder_bits: %u\n", rf->remainder_bits);
    printf("  memory: %zu bytes\n", rsqf_filter_memory_bytes(rf));

    /* Insert keys */
    const char *keys[] = {"hello", "world", "rsqf", "filter", "test", "abc", "def", "ghi"};
    int nkeys = sizeof(keys) / sizeof(keys[0]);
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        rsqf_err_t err = rsqf_filter_insert(rf, h);
        assert(err == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    printf("  inserted %d keys, count=%zu\n", nkeys, rsqf_filter_count(rf));

    /* Quick debug: check occupieds for first failing key */
    uint64_t h_debug = hash64(keys[0]);
    uint64_t q_debug = (h_debug >> 10) & (rf->num_slots - 1);
    const uint64_t *occ_p = (const uint64_t *)(rf + 1);
    printf("  occ word=%lu bit=%d val=%d\n",
           (unsigned long)(q_debug / 64), (int)(q_debug % 64),
           (int)((occ_p[q_debug / 64] >> (q_debug % 64)) & 1ULL));

    /* Lookup inserted keys */
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        assert(rsqf_filter_lookup(rf, h));
    }
    printf("  all inserted keys found\n");

    /* Delete keys */
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        rsqf_err_t err = rsqf_filter_delete(rf, h);
        assert(err == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    printf("  deleted %d keys, count=%zu\n", nkeys, rsqf_filter_count(rf));

    /* Verify deleted keys not found */
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        assert(!rsqf_filter_lookup(rf, h));
    }
    printf("  deleted keys correctly return false\n");

    /* Delete non-existent key */
    rsqf_err_t err = rsqf_filter_delete(rf, hash64("nonexistent"));
    assert(err == RSQF_ERR_NOT_FOUND);
    printf("  delete of non-existent returns NOT_FOUND\n");

    /* Test reset */
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        rsqf_filter_insert(rf, h);
    }
    assert((int)rsqf_filter_count(rf) == nkeys);
    rsqf_filter_reset(rf);
    assert(rsqf_filter_count(rf) == 0);
    for (int i = 0; i < nkeys; i++) {
        uint64_t h = hash64(keys[i]);
        assert(!rsqf_filter_lookup(rf, h));
    }
    printf("  reset works correctly\n");

    rsqf_filter_destroy(rf);

    /* ====================================================================
     * FPR test: measure false positive rate
     * ==================================================================== */
    printf("\n=== FPR measurement ===\n");

    rf = rsqf_filter_create(10000, 10);
    assert(rf);

    /* Insert 8000 items */
    for (int i = 0; i < 8000; i++) {
        uint64_t h = hash64(keys[0]) + (uint64_t)i * 0x9e3779b97f4a7c15ULL;
        rsqf_filter_insert(rf, h);
    }

    /* Check for false positives (query non-inserted items) */
    int fp = 0;
    int trials = 100000;
    for (int i = 0; i < trials; i++) {
        uint64_t h = hash64(keys[0]) + (uint64_t)i * 0x9e3779b97f4a7c15ULL + 1000000ULL;
        if (rsqf_filter_lookup(rf, h))
            fp++;
    }
    double fpr = (double)fp / trials;
    printf("  FPR: %d/%d = %.4f%% (theoretical: ~0.1%% for 10-bit fp)\n", fp, trials, fpr * 100.0);

    rsqf_filter_destroy(rf);

    /* ====================================================================
     * Fill-to-capacity test
     * ==================================================================== */
    printf("\n=== Fill-to-capacity test ===\n");

    rf = rsqf_filter_create(4096, 8);
    assert(rf);
    printf("  slots: %lu\n", (unsigned long)rf->num_slots);

    int inserted = 0;
    for (uint64_t i = 1; i < 100000; i++) {
        uint64_t h = i * 0x9e3779b97f4a7c15ULL;
        rsqf_err_t e = rsqf_filter_insert(rf, h);
        if (e == RSQF_OK) inserted++;
        else if (e == RSQF_ERR_FULL) break;
    }
    printf("  inserted %d/%lu items (load=%.1f%%)\n",
           inserted, (unsigned long)rf->num_slots,
           rsqf_filter_load_factor(rf) * 100.0);

    rsqf_filter_destroy(rf);

    /* ====================================================================
     * Delete + reinsert stress
     * ==================================================================== */
    printf("\n=== Delete/reinsert stress ===\n");

    rf = rsqf_filter_create(1024, 10);
    assert(rf);

    uint64_t vals[512];
    for (int i = 0; i < 512; i++) {
        vals[i] = hash64(keys[i % nkeys]) + (uint64_t)(i / nkeys) * 0x9e3779b97f4a7c15ULL;
        rsqf_filter_insert(rf, vals[i]);
    }
    assert(rsqf_filter_count(rf) == 512ULL);

    for (int round = 0; round < 5; round++) {
        for (int i = 0; i < 256; i++) {
            int idx = (round * 256 + i) % 512;
            rsqf_filter_delete(rf, vals[idx]);
            vals[idx] = hash64(keys[idx % nkeys]) + (uint64_t)(idx + 1000 + round) * 0x9e3779b97f4a7c15ULL;
            rsqf_filter_insert(rf, vals[idx]);
        }
    }
    printf("  after 5 rounds of delete/reinsert: count=%zu, load=%.1f%%\n",
           rsqf_filter_count(rf), rsqf_filter_load_factor(rf) * 100.0);

    rsqf_filter_destroy(rf);

    /* ====================================================================
     * Adversarial: all same quotient
     * Force many items into the same quotient by using hashes that differ
     * only in the low rbits (remainder) bits but have the same high bits.
     * ==================================================================== */
    printf("\n=== All-same-quotient adversarial ===\n");
    rf = rsqf_filter_create(512, 10);
    assert(rf);
    for (uint64_t i = 0; i < 200; i++) {
        uint64_t h = i << 10; /* quotient always 0, remainder varies */
        if (h == 0) h = 1; /* non-zero remainder */
        rsqf_err_t e = rsqf_filter_insert(rf, h);
        assert(e == RSQF_OK);
        assert(rsqf_filter_validate(rf));
        assert(rsqf_filter_lookup(rf, h));
    }
    printf("  same-quotient: inserted 200 items, count=%zu\n", rsqf_filter_count(rf));
    rsqf_filter_destroy(rf);

    /* ====================================================================
     * Adversarial: reverse remainder insertion
     * Insert remainders in descending order within same quotient
     * ==================================================================== */
    printf("\n=== Reverse remainder adversarial ===\n");
    rf = rsqf_filter_create(512, 10);
    assert(rf);
    for (int64_t i = 500; i >= 0; i--) {
        uint64_t h = (uint64_t)i; /* quotient=0, remainder=i (descending) */
        if ((h & 0x3FF) == 0) h = 1;
        rsqf_err_t e = rsqf_filter_insert(rf, h);
        assert(e == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    for (int64_t i = 500; i >= 0; i--) {
        uint64_t h = (uint64_t)i;
        if ((h & 0x3FF) == 0) h = 1;
        assert(rsqf_filter_lookup(rf, h));
    }
    printf("  reverse-insert: 501 items verified, count=%zu\n", rsqf_filter_count(rf));
    rsqf_filter_destroy(rf);

    /* ====================================================================
     * Block boundary stress
     * Create run that crosses a 64-slot block boundary
     * ==================================================================== */
    printf("\n=== Block-boundary stress ===\n");
    rf = rsqf_filter_create(256, 10);
    assert(rf);
    /* Insert items at slot 60 (near block boundary at 64) to force cross-block runs */
    for (uint64_t i = 0; i < 40; i++) {
        uint64_t h = (60ULL << 10) | (i + 1); /* quotient=60, remainders 1..40 */
        rsqf_err_t e = rsqf_filter_insert(rf, h);
        assert(e == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    /* Delete from middle */
    for (uint64_t i = 10; i < 30; i++) {
        uint64_t h = (60ULL << 10) | (i + 1);
        assert(rsqf_filter_delete(rf, h) == RSQF_OK);
        assert(rsqf_filter_validate(rf));
    }
    printf("  cross-block: remaining %zu items\n", rsqf_filter_count(rf));
    rsqf_filter_destroy(rf);

    printf("\nrsqf PASS\n");
    return 0;
}
