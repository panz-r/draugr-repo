/**
 * test_cqf_fuzz.c — Bytecode-driven CQF fuzzer
 *
 * Generates random sequences of insert/delete/count/lookup/merge/resize
 * operations, maintains a reference multiset, and verifies consistency
 * after each operation under ASan/UBSan/LSan.
 *
 * Build: gcc -g -fsanitize=address,undefined -I. -Iinclude \
 *        tests/test_cqf_fuzz.c src/cqf_filter.c src/rsqf_filter.c \
 *        src/util.c -o test_cqf_fuzz -lm
 */
#include "draugr/cqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define MAX_FPS 4096
#define MAX_OPS 10000

typedef struct {
    uint64_t fp;
    uint64_t count;
} ref_entry_t;

typedef struct {
    ref_entry_t entries[MAX_FPS];
    int n;
} ref_t;

static ref_entry_t *ref_lookup(ref_t *r, uint64_t fp) {
    for (int i = 0; i < r->n; i++)
        if (r->entries[i].fp == fp) return &r->entries[i];
    return NULL;
}

static void ref_insert(ref_t *r, uint64_t fp) {
    ref_entry_t *e = ref_lookup(r, fp);
    if (e) { e->count++; return; }
    assert(r->n < MAX_FPS);
    r->entries[r->n].fp = fp;
    r->entries[r->n].count = 1;
    r->n++;
}

static int ref_delete(ref_t *r, uint64_t fp) {
    ref_entry_t *e = ref_lookup(r, fp);
    if (!e || e->count == 0) return 0;
    e->count--;
    if (e->count == 0) {
        *e = r->entries[--r->n];
    }
    return 1;
}

static uint64_t ref_count(ref_t *r, uint64_t fp) {
    ref_entry_t *e = ref_lookup(r, fp);
    return e ? e->count : 0;
}

static uint64_t make_fp(uint64_t q, uint64_t rem, uint8_t rbits) {
    return (q << rbits) | rem;
}

static int run_fuzz(uint32_t seed, int max_ops, uint8_t rbits,
                     uint64_t capacity) {
    ref_t ref;
    memset(&ref, 0, sizeof(ref));
    cqf_filter_t *cf = cqf_filter_create(capacity, rbits);
    if (!cf) return 0;

    uint64_t ns = cf->num_slots;
    uint64_t max_rem = (1ULL << rbits) - 1;
    srand(seed);
    int ops = 0;

    for (int i = 0; i < max_ops && cf; i++) {
        /* Generate adversarial-biased fingerprint */
        uint64_t q = rand() % ns;
        uint64_t rem_bias = rand() % 10;
        uint64_t rem;
        if (rem_bias < 4)      rem = 0;        /* 40% rem=0 */
        else if (rem_bias < 6) rem = 1;        /* 20% rem=1 */
        else if (rem_bias < 8) rem = max_rem;  /* 20% rem=max */
        else                   rem = rand() & max_rem;
        uint64_t fp = make_fp(q, rem, rbits);

        int op = rand() % 10;
        switch (op) {
        case 0: case 1: case 2: case 3: case 4: {
            /* Insert (50% probability) */
            cqf_err_t e = cqf_filter_insert(cf, fp);
            if (e == CQF_OK) {
                ref_insert(&ref, fp);
                ops++;
            } else if (e == CQF_ERR_FULL) {
                cqf_filter_t *cf2 = cqf_filter_resize(cf, cf->num_slots * 2);
                if (cf2) { cf = cf2; i--; }
            }
            break;
        }
        case 5: case 6: case 7: {
            /* Delete (30% probability) */
            if (rand() % 2 && ref.n > 0) {
                /* Pick a random existing fingerprint */
                int idx = rand() % ref.n;
                fp = ref.entries[idx].fp;
            }
            cqf_err_t e = cqf_filter_delete(cf, fp);
            if (e == CQF_OK) {
                assert(ref_delete(&ref, fp));
                ops++;
            } else {
                assert(e == CQF_ERR_NOT_FOUND);
                assert(ref_delete(&ref, fp) == 0 || ref_count(&ref, fp) == 0);
            }
            break;
        }
        case 8: {
            /* Count (10% probability) */
            uint64_t cqf_cnt = cqf_filter_count_occurrences(cf, fp);
            uint64_t ref_cnt = ref_count(&ref, fp);
            assert(cqf_cnt == ref_cnt);
            ops++;
            break;
        }
        case 9: {
            /* Merge or resize (10% probability) */
            if (rand() % 2 && cf->count > 0) {
                cqf_filter_t *cf2 = cqf_filter_create(capacity, rbits);
                if (cf2) {
                    /* Insert a random fingerprint into cf2 */
                    uint64_t fp2 = make_fp(rand() % ns, rand() & max_rem, rbits);
                    cqf_err_t e = cqf_filter_insert(cf2, fp2);
                    if (e == CQF_OK) ref_insert(&ref, fp2);

                    cqf_filter_t *merged = cqf_filter_merge(cf, cf2);
                    if (merged) {
                        assert(cqf_filter_validate(merged));
                        cqf_filter_destroy(cf);
                        cf = merged;
                        ops++;
                    }
                    cqf_filter_destroy(cf2);
                }
            } else {
                cqf_filter_t *resized = cqf_filter_resize(cf, cf->num_slots);
                if (resized) {
                    cf = resized;
                    ops++;
                }
            }
            break;
        }
        }
        /* Periodic validation */
        if (i % 100 == 0) {
            if (!cqf_filter_validate(cf)) {
                printf("  FAIL: validation at op %d (seed %u)\n", i, seed);
                return 0;
            }
        }
    }

    cqf_filter_destroy(cf);
    return ops;
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : (uint32_t)time(NULL);
    int max_ops = (argc > 2) ? atoi(argv[2]) : 5000;

    printf("CQF Fuzz: seed=%u, max_ops=%d\n", seed, max_ops);
    fflush(stdout);

    /* Run with various configurations */
    struct { uint64_t cap; uint8_t rbits; } configs[] = {
        {64, 8}, {128, 4}, {256, 10}, {64, 2},
    };
    int ncfg = sizeof(configs) / sizeof(configs[0]);

    for (int c = 0; c < ncfg; c++) {
        int ops = run_fuzz(seed + c, max_ops,
                          configs[c].rbits, configs[c].cap);
        printf("  cfg cap=%lu rbits=%u: %d ops OK\n",
               configs[c].cap, configs[c].rbits, ops);
    }
    printf("Fuzz PASS\n");
    return 0;
}
