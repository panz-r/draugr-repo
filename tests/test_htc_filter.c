#include "draugr/htc.h"
#include "draugr/htc_filter.h"
#include <stdio.h>
#include <assert.h>
#include <pthread.h>

typedef struct {
    htc_table_t *t;
    int id;
    int ops;
    int ok;
} worker_t;

static void *worker(void *arg) {
    worker_t *w = (worker_t *)arg;
    uint64_t seed = (uint64_t)w->id * 0x9e3779b97f4a7c15ULL;
    for (int i = 0; i < w->ops; i++) {
        seed = seed * 6364136223846793005ULL + 1;
        uint64_t h = seed;
        int op = (int)(seed & 3);
        if (op == 0) htc_insert(w->t, h, h >> 1);
        else if (op == 1) { uint64_t out; htc_find(w->t, h, &out); }
        else if (op == 2) htc_upsert(w->t, h, h >> 1);
        else htc_remove(w->t, h);
    }
    w->ok = 1;
    return NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    htc_config_t cfg = {64, 0.75, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);

    cuckoo_filter_t *cf = cuckoo_filter_create(50000, 4, 10, 200);
    htc_filter_t *hf = htc_filter_create(htc_amq_cuckoo(cf));
    htc_amq_filter_t amq = htc_amq_htc_filter(hf);
    htc_set_filter(t, &amq);

    int T = 4, OPS = 20000;
    pthread_t thr[8];
    worker_t wrk[8];
    for (int i = 0; i < T; i++) {
        wrk[i].t = t; wrk[i].id = i; wrk[i].ops = OPS; wrk[i].ok = 0;
        pthread_create(&thr[i], NULL, worker, &wrk[i]);
    }
    for (int i = 0; i < T; i++) {
        pthread_join(thr[i], NULL);
        assert(wrk[i].ok);
    }
    printf("OK (%zu entries)\n", htc_size(t));
    htc_set_filter(t, NULL);
    htc_filter_destroy(hf);
    cuckoo_filter_destroy(cf);
    htc_destroy(t);
    return 0;
}
