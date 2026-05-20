/**
 * bench_filters.c — Performance benchmarks for all filter types
 *
 * Compares RSQF, CQF, Cuckoo, Vacuum, and Bloom filters on:
 *   - Insert throughput at various load factors
 *   - Positive lookup throughput
 *   - Negative lookup throughput
 *   - Delete throughput
 *   - Adversarial (all-same-quotient) insert throughput
 *
 * Build: gcc -I../include -O3 -o bench_filters bench_filters.c \
 *            ../src/rsqf_filter.c ../src/cqf_filter.c \
 *            ../src/cuckoo_filter.c ../src/vacuum_filter.c ../src/bloom_filter.c -lm
 *
 * Usage: ./bench_filters [capacity=100000] [fp_bits=10]
 */

#include "draugr/rsqf_filter.h"
#include "draugr/cqf_filter.h"
#include "draugr/cuckoo_filter.h"
#include "draugr/vacuum_filter.h"
#include "draugr/bloom_filter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <inttypes.h>

/* Timer */
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* Hash for keys */
static uint64_t hash_key(uint64_t k) {
    return k * 0x9e3779b97f4a7c15ULL;
}

/* Report throughput in million ops/sec */
static void report(const char *name, const char *op, uint64_t n, double us) {
    double ops = (double)n / (us / 1e6);
    printf("%-20s %-12s %8lu ops  %8.0f us  %8.2f M/s\n", name, op, (unsigned long)n, us, ops / 1e6);
}

/* ============================================================================
 * RSQF benchmark
 * ============================================================================ */
static void bench_rsqf(uint64_t capacity, uint8_t fp_bits, uint64_t nops) {
    rsqf_filter_t *rf = rsqf_filter_create(capacity, fp_bits);
    if (!rf) { printf("RSQF       create      FAILED\n"); return; }

    uint64_t i;
    double t0, t1;

    /* Insert */
    t0 = now_us();
    for (i = 0; i < nops && i < rsqf_filter_capacity(rf); i++) {
        rsqf_filter_insert(rf, hash_key(i));
    }
    t1 = now_us();
    uint64_t n = (uint64_t)rsqf_filter_count(rf);
    report("RSQF", "insert", n, t1 - t0);

    /* Positive lookup */
    t0 = now_us();
    for (i = 0; i < nops && i < n; i++) {
        rsqf_filter_lookup(rf, hash_key(i));
    }
    t1 = now_us();
    report("RSQF", "lookup-pos", n, t1 - t0);

    /* Negative lookup */
    uint64_t base = capacity * 2;
    t0 = now_us();
    for (i = 0; i < nops / 2; i++) {
        rsqf_filter_lookup(rf, hash_key(base + i));
    }
    t1 = now_us();
    report("RSQF", "lookup-neg", nops / 2, t1 - t0);

    /* Delete */
    t0 = now_us();
    for (i = 0; i < n; i++) {
        rsqf_filter_delete(rf, hash_key(i));
    }
    t1 = now_us();
    report("RSQF", "delete", n, t1 - t0);

    /* Load factor info */
    printf("  (load factor before delete: %.1f%%)\n",
           rsqf_filter_load_factor(rf) * 100.0);

    rsqf_filter_destroy(rf);
}

/* ============================================================================
 * CQF benchmark
 * ============================================================================ */
static void bench_cqf(uint64_t capacity, uint8_t fp_bits, uint64_t nops) {
    cqf_filter_t *cf = cqf_filter_create(capacity, fp_bits);
    if (!cf) { printf("CQF        create      FAILED\n"); return; }

    uint64_t i;
    double t0, t1;

    /* Insert */
    t0 = now_us();
    for (i = 0; i < nops && i < cqf_filter_capacity(cf); i++) {
        cqf_filter_insert(cf, hash_key(i));
    }
    t1 = now_us();
    uint64_t n = (uint64_t)cqf_filter_count(cf);
    report("CQF", "insert", n, t1 - t0);

    /* Positive lookup */
    t0 = now_us();
    for (i = 0; i < nops && i < n; i++) {
        cqf_filter_lookup(cf, hash_key(i));
    }
    t1 = now_us();
    report("CQF", "lookup-pos", n, t1 - t0);

    /* Negative lookup */
    uint64_t base = capacity * 2;
    t0 = now_us();
    for (i = 0; i < nops / 2; i++) {
        cqf_filter_lookup(cf, hash_key(base + i));
    }
    t1 = now_us();
    report("CQF", "lookup-neg", nops / 2, t1 - t0);

    /* Delete */
    t0 = now_us();
    for (i = 0; i < n; i++) {
        cqf_filter_delete(cf, hash_key(i));
    }
    t1 = now_us();
    report("CQF", "delete", n, t1 - t0);

    printf("  (load factor before delete: %.1f%%)\n",
           cqf_filter_load_factor(cf) * 100.0);

    cqf_filter_destroy(cf);
}

/* ============================================================================
 * Cuckoo filter benchmark
 * ============================================================================ */
static void bench_cuckoo(uint64_t capacity, uint8_t fp_bits, uint64_t nops) {
    cuckoo_filter_t *cf = cuckoo_filter_create(capacity, 0, fp_bits, 0);
    if (!cf) { printf("Cuckoo     create      FAILED\n"); return; }

    uint64_t i;
    double t0, t1;

    /* Insert */
    t0 = now_us();
    for (i = 0; i < nops && i < cuckoo_filter_capacity(cf); i++) {
        cuckoo_filter_insert(cf, hash_key(i));
    }
    t1 = now_us();
    uint64_t n = (uint64_t)cuckoo_filter_count(cf);
    report("Cuckoo", "insert", n, t1 - t0);

    /* Positive lookup */
    t0 = now_us();
    for (i = 0; i < nops && i < n; i++) {
        cuckoo_filter_lookup(cf, hash_key(i));
    }
    t1 = now_us();
    report("Cuckoo", "lookup-pos", n, t1 - t0);

    /* Negative lookup */
    uint64_t base = capacity * 2;
    t0 = now_us();
    for (i = 0; i < nops / 2; i++) {
        cuckoo_filter_lookup(cf, hash_key(base + i));
    }
    t1 = now_us();
    report("Cuckoo", "lookup-neg", nops / 2, t1 - t0);

    /* Delete */
    t0 = now_us();
    for (i = 0; i < n; i++) {
        cuckoo_filter_delete(cf, hash_key(i));
    }
    t1 = now_us();
    report("Cuckoo", "delete", n, t1 - t0);

    printf("  (load factor before delete: %.1f%%)\n",
           cuckoo_filter_load_factor(cf) * 100.0);

    cuckoo_filter_destroy(cf);
}

/* ============================================================================
 * Vacuum filter benchmark
 * ============================================================================ */
static void bench_vacuum(uint64_t capacity, uint8_t fp_bits, uint64_t nops) {
    vacuum_filter_t *vf = vacuum_filter_create(capacity, 0, fp_bits, 0);
    if (!vf) { printf("Vacuum     create      FAILED\n"); return; }

    uint64_t i;
    double t0, t1;

    /* Insert */
    t0 = now_us();
    for (i = 0; i < nops && i < vacuum_filter_capacity(vf); i++) {
        vacuum_filter_insert(vf, hash_key(i));
    }
    t1 = now_us();
    uint64_t n = (uint64_t)vacuum_filter_count(vf);
    report("Vacuum", "insert", n, t1 - t0);

    /* Positive lookup */
    t0 = now_us();
    for (i = 0; i < nops && i < n; i++) {
        vacuum_filter_lookup(vf, hash_key(i));
    }
    t1 = now_us();
    report("Vacuum", "lookup-pos", n, t1 - t0);

    /* Negative lookup */
    uint64_t base = capacity * 2;
    t0 = now_us();
    for (i = 0; i < nops / 2; i++) {
        vacuum_filter_lookup(vf, hash_key(base + i));
    }
    t1 = now_us();
    report("Vacuum", "lookup-neg", nops / 2, t1 - t0);

    /* Delete */
    t0 = now_us();
    for (i = 0; i < n; i++) {
        vacuum_filter_delete(vf, hash_key(i));
    }
    t1 = now_us();
    report("Vacuum", "delete", n, t1 - t0);

    printf("  (load factor before delete: %.1f%%)\n",
           vacuum_filter_load_factor(vf) * 100.0);

    vacuum_filter_destroy(vf);
}

/* ============================================================================
 * Bloom filter benchmark (insert + lookup only)
 * ============================================================================ */
static void bench_bloom(uint64_t capacity, uint8_t fp_bits, uint64_t nops) {
    (void)fp_bits;
    bloom_filter_t *bf = bloom_filter_create(capacity, 0.01);
    if (!bf) { printf("Bloom      create      FAILED\n"); return; }

    /* Keys must be passed as data, not hashes. Use 8-byte keys. */
    uint64_t i;
    double t0, t1;

    /* Insert */
    t0 = now_us();
    for (i = 0; i < nops; i++) {
        uint64_t k = hash_key(i);
        bloom_filter_insert(bf, &k, sizeof(k));
    }
    t1 = now_us();
    report("Bloom", "insert", nops, t1 - t0);

    /* Positive lookup */
    t0 = now_us();
    for (i = 0; i < nops; i++) {
        uint64_t k = hash_key(i);
        bloom_filter_lookup(bf, &k, sizeof(k));
    }
    t1 = now_us();
    report("Bloom", "lookup-pos", nops, t1 - t0);

    /* Negative lookup */
    t0 = now_us();
    for (i = 0; i < nops / 2; i++) {
        uint64_t k = hash_key(capacity * 2 + i);
        bloom_filter_lookup(bf, &k, sizeof(k));
    }
    t1 = now_us();
    report("Bloom", "lookup-neg", nops / 2, t1 - t0);

    printf("  (bits per item after insert: %.1f)\n",
           bloom_filter_bits_per_item(bf));

    bloom_filter_destroy(bf);
}

/* ============================================================================
 * Adversarial: all same quotient benchmark (RSQF only)
 * ============================================================================ */
static void bench_rsqf_adversarial(uint64_t nops) {
    uint64_t capacity = nops * 2;
    rsqf_filter_t *rf = rsqf_filter_create(capacity, 10);
    if (!rf) { printf("RSQF-adv   create      FAILED\n"); return; }

    /* Force all items to same quotient by controlling hash bits */
    double t0 = now_us();
    for (uint64_t i = 0; i < nops; i++) {
        uint64_t h = (0ULL << 10) | (i + 1); /* quotient=0, remainder=i+1 */
        if (rsqf_filter_insert(rf, h) != RSQF_OK) break;
    }
    uint64_t n = (uint64_t)rsqf_filter_count(rf);
    double t1 = now_us();
    report("RSQF-adv", "same-quot-insert", n, t1 - t0);

    /* Lookup all */
    t0 = now_us();
    for (uint64_t i = 0; i < n; i++) {
        uint64_t h = (0ULL << 10) | (i + 1);
        rsqf_filter_lookup(rf, h);
    }
    t1 = now_us();
    report("RSQF-adv", "lookup-pos", n, t1 - t0);

    printf("  (same-quotient load: %.1f%% across %lu slots)\n",
           rsqf_filter_load_factor(rf) * 100.0, (unsigned long)rf->num_slots);

    rsqf_filter_destroy(rf);
}

/* ============================================================================
 * Main
 * ============================================================================ */
int main(int argc, char **argv) {
    uint64_t capacity = (argc > 1) ? (uint64_t)atoll(argv[1]) : 100000;
    uint8_t  fp_bits  = (argc > 2) ? (uint8_t)atoi(argv[2]) : 10;
    uint64_t nops     = (uint64_t)(capacity * 0.85); /* 85% load */

    printf("=== Filter Benchmarks ===\n");
    printf("capacity=%lu  fp_bits=%u  nops=%lu\n\n",
           (unsigned long)capacity, fp_bits, (unsigned long)nops);

    printf("--- Cuckoo Filter ---\n");
    bench_cuckoo(capacity, fp_bits, nops);
    printf("\n");

    printf("--- Vacuum Filter ---\n");
    bench_vacuum(capacity, fp_bits, nops);
    printf("\n");

    printf("--- Bloom Filter (1%% FPR) ---\n");
    bench_bloom(capacity, fp_bits, nops);
    printf("\n");

    printf("--- RSQF ---\n");
    bench_rsqf(capacity, fp_bits, nops);
    printf("\n");

    printf("--- CQF ---\n");
    bench_cqf(capacity, fp_bits, nops);
    printf("\n");

    printf("--- Adversarial RSQF (all-same-quotient) ---\n");
    bench_rsqf_adversarial(capacity);
    printf("\n");

    printf("Done.\n");
    return 0;
}
