#include "draugr/htc.h"
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    /* Stage 3 baseline: synchronous grow, front cache disabled */
    htc_config_t cfg = {64, 0.75, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);

    int N = 500000;
    double t0 = now_us();
    for (int i = 0; i < N; i++)
        htc_insert(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, (uint64_t)i);
    double t1 = now_us();

    int found = 0;
    for (int i = 0; i < N; i++) {
        uint64_t out;
        if (htc_find(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, &out) && out == (uint64_t)i)
            found++;
    }
    double t2 = now_us();

    int removed = 0;
    for (int i = 0; i < N; i += 2)
        if (htc_remove(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL))
            removed++;
    double t3 = now_us();

    printf("insert: %d ops in %.0f us = %.0f ops/sec\n", N, t1-t0, N/((t1-t0)/1e6));
    printf("find:   %d/%d hit in %.0f us = %.0f ops/sec\n", found, N, t2-t1, N/((t2-t1)/1e6));
    printf("remove: %d ops in %.0f us = %.0f ops/sec\n", removed, t3-t2, removed/((t3-t2)/1e6));
    printf("total:  %d ops in %.0f us\n", N + N + removed, t3-t0);

    htc_destroy(t);
    return 0;
}
