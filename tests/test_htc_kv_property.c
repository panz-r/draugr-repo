/**
 * test_htc_kv_property.c — Property-based tests for htc_kv
 *
 * Generates random sequences of insert/remove/find operations and
 * verifies htc_kv produces consistent results with a simple model.
 */

#include "draugr/htc_kv.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>

#define N_STEPS    2000
#define N_KEYS     200

/* ─── Simple model ──────────────────────────────────────────── */
typedef struct { char key[32]; char val[64]; int used; } pair_t;

static int mod_find(pair_t *pairs, const char *key) {
    for (int i = 0; i < N_KEYS; i++)
        if (pairs[i].used && strcmp(pairs[i].key, key) == 0) return i;
    return -1;
}

static int mod_insert(pair_t *pairs, int *cnt, const char *k, const char *v) {
    if (mod_find(pairs, k) >= 0) return 0;
    for (int i = 0; i < N_KEYS; i++) {
        if (!pairs[i].used) {
            strncpy(pairs[i].key, k, 31); pairs[i].key[31] = 0;
            strncpy(pairs[i].val, v, 63); pairs[i].val[63] = 0;
            pairs[i].used = 1; (*cnt)++; return 1;
        }
    }
    return 0;
}

static int mod_remove(pair_t *pairs, int *cnt, const char *k) {
    int i = mod_find(pairs, k);
    if (i < 0) return 0;
    memset(&pairs[i], 0, sizeof(pairs[i])); (*cnt)--; return 1;
}

/* ─── PRNG ──────────────────────────────────────────────────── */
static uint64_t xrs(uint64_t *s) {
    uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; *s = x; return x;
}
static void rkey(char *b, int len, uint64_t *r) {
    for (int i = 0; i < len - 1; i++) b[i] = 'a' + (xrs(r) % 26);
    b[len - 1] = 0;
}

/* ─── Single seed test ──────────────────────────────────────── */
static int test_seed(uint64_t seed, FILE *log) {
    htc_kv_t *kv = htc_kv_create(NULL);
    pair_t pairs[N_KEYS];
    memset(pairs, 0, sizeof(pairs));
    int model_cnt = 0;
    uint64_t rng = seed;

    char keys[N_KEYS][16];
    for (int i = 0; i < N_KEYS; i++) rkey(keys[i], 8, &rng);

    for (int step = 0; step < N_STEPS; step++) {
        int ki = (int)(xrs(&rng) % N_KEYS);
        char *k = keys[ki];
        int op = (int)(xrs(&rng) % 100);

        if (op < 40) {
            char v[16]; rkey(v, 12, &rng);
            int me = mod_insert(pairs, &model_cnt, k, v);
            int ke = htc_kv_insert_copy(kv, k, strlen(k)+1, v, strlen(v)+1) ? 1 : 0;
            if (me != ke) {
                fprintf(log, "insert mismatch step %d: key=%s model=%d kv=%d\n",
                        step, k, me, ke);
                htc_kv_destroy(kv); return 1;
            }
        } else if (op < 70) {
            int me = mod_remove(pairs, &model_cnt, k);
            int ke = htc_kv_remove(kv, k, strlen(k)+1) ? 1 : 0;
            if (me != ke) {
                fprintf(log, "remove mismatch step %d: key=%s model=%d kv=%d\n",
                        step, k, me, ke);
                htc_kv_destroy(kv); return 1;
            }
        } else {
            int mi = mod_find(pairs, k);
            int found = htc_kv_find(kv, k, strlen(k)+1, NULL, NULL) ? 1 : 0;
            int me = (mi >= 0) ? 1 : 0;
            if (me != found) {
                fprintf(log, "find mismatch step %d: key=%s model=%d kv=%d\n",
                        step, k, me, found);
                htc_kv_destroy(kv); return 1;
            }
            if (mi >= 0) {
                char buf[64]; size_t blen = sizeof(buf);
                assert(htc_kv_find(kv, k, strlen(k)+1, buf, &blen));
                buf[blen < 63 ? blen : 63] = 0;
                if (strcmp(buf, pairs[mi].val) != 0) {
                    fprintf(log, "value mismatch step %d: key=%s exp=%s got=%s\n",
                            step, k, pairs[mi].val, buf);
                    htc_kv_destroy(kv); return 1;
                }
            }
        }
    }

    if ((int)htc_kv_count(kv) != model_cnt) {
        fprintf(log, "count mismatch: model=%d kv=%zu\n", model_cnt, htc_kv_count(kv));
        htc_kv_destroy(kv); return 1;
    }

    htc_kv_destroy(kv);
    return 0;
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int n_tests = 100;
    if (argc > 1) n_tests = atoi(argv[1]);
    uint64_t seed = (uint64_t)time(NULL);
    if (argc > 2) seed = strtoull(argv[2], NULL, 10);

    printf("htc_kv property tests: %d runs, seed=0x%lx\n", n_tests, seed);
    int fails = 0;
    for (int r = 0; r < n_tests; r++) {
        uint64_t s = seed + (uint64_t)r * 0x9e3779b97f4a7c15ULL;
        int f = test_seed(s, stderr);
        fails += f;
        printf("  run %3d/%d  %s\n", r+1, n_tests, f ? "FAIL" : "pass");
        if (f) break;
    }
    printf("%s: %d/%d passed\n", fails ? "FAILED" : "ALL PASS",
           n_tests - fails, n_tests);
    return fails ? 1 : 0;
}
