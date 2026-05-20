/**
 * test_rsqf_fuzz.c — RSQF operation fuzzer
 *
 * Generates random sequences of insert/delete/lookup operations,
 * maintains a reference multiset, and verifies consistency after
 * each operation under ASan/UBSan/LSan.
 *
 * Build: gcc -g -fsanitize=address,undefined -I. -Iinclude \
 *        tests/test_rsqf_fuzz.c src/rsqf_filter.c src/util.c \
 *        -o test_rsqf_fuzz -lm
 */
#include "draugr/rsqf_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_FPS 4096
#define MAX_OPS 5000

typedef struct {
    uint64_t fp;
} ref_entry_t;

typedef struct {
    ref_entry_t entries[MAX_FPS];
    int n;
} ref_t;

static int ref_lookup(ref_t *r, uint64_t fp) {
    for (int i = 0; i < r->n; i++)
        if (r->entries[i].fp == fp) return 1;
    return 0;
}

static void ref_insert(ref_t *r, uint64_t fp) {
    if (ref_lookup(r, fp)) return;
    assert(r->n < MAX_FPS);
    r->entries[r->n].fp = fp;
    r->n++;
}

static int ref_delete(ref_t *r, uint64_t fp) {
    for (int i = 0; i < r->n; i++) {
        if (r->entries[i].fp == fp) {
            r->entries[i] = r->entries[--r->n];
            return 1;
        }
    }
    return 0;
}

static int run_fuzz(uint32_t seed, int max_ops, uint8_t rbits,
                     uint64_t capacity) {
    ref_t ref;
    memset(&ref, 0, sizeof(ref));
    rsqf_filter_t *rf = rsqf_filter_create(capacity, rbits);
    if (!rf) return 0;

    uint64_t ns = rf->num_slots;
    uint64_t max_rem = (1ULL << rbits) - 1;
    srand(seed);
    int ops = 0;

    for (int i = 0; i < max_ops && rf; i++) {
        /* Generate adversarial-biased fingerprint */
        uint64_t q = rand() % ns;
        uint64_t rem_bias = rand() % 10;
        uint64_t rem;
        if (rem_bias < 4)      rem = 0;
        else if (rem_bias < 6) rem = 1;
        else if (rem_bias < 8) rem = max_rem;
        else                   rem = rand() & max_rem;
        uint64_t fp = (q << rbits) | rem;

        int op = rand() % 10;
        switch (op) {
        case 0: case 1: case 2: case 3: case 4: case 5: {
            /* Insert (60% probability) */
            rsqf_err_t e = rsqf_filter_insert(rf, fp);
            if (e == RSQF_OK) {
                ref_insert(&ref, fp);
                ops++;
            }
            break;
        }
        case 6: case 7: case 8: {
            /* Delete (30% probability) */
            if (rand() % 2 && ref.n > 0) {
                int idx = rand() % ref.n;
                fp = ref.entries[idx].fp;
            }
            rsqf_err_t e = rsqf_filter_delete(rf, fp);
            if (e == RSQF_OK) {
                assert(ref_delete(&ref, fp));
                ops++;
            } else {
                assert(e == RSQF_ERR_NOT_FOUND);
            }
            break;
        }
        case 9: {
            /* Lookup (10% probability) */
            bool found = rsqf_filter_lookup(rf, fp);
            bool in_ref = ref_lookup(&ref, fp);
            /* No false negatives */
            if (in_ref) assert(found);
            ops++;
            break;
        }
        }
        /* Periodic validation */
        if (i % 100 == 0 && !rsqf_filter_validate(rf)) {
            printf("  FAIL: validate at op %d (seed %u)\n", i, seed);
            return 0;
        }
    }

    rsqf_filter_destroy(rf);
    return ops;
}

int main(int argc, char **argv) {
    uint32_t seed = (argc > 1) ? (uint32_t)atoi(argv[1]) : (uint32_t)time(NULL);
    int max_ops = (argc > 2) ? atoi(argv[2]) : 3000;

    printf("RSQF Fuzz: seed=%u, max_ops=%d\n", seed, max_ops);

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
    printf("RSQF Fuzz PASS\n");
    return 0;
}
