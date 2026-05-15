/**
 * bench_htc.c — Performance benchmark for htc table (§28)
 *
 * Usage: ./bench_htc [options]
 *   -t N        thread count (default 1)
 *   -n N        operations per thread (default 100000)
 *   -b N        initial buckets (default 64)
 *   -l FLOAT    max load factor (default 0.75)
 *   -m MIX      operation mix: read, balanced, write, neg (default balanced)
 *   -k DIST     key distribution: seq, random (default seq)
 *   -f          enable cuckoo filter (default off)
 *   -c FLOAT    negative lookup fraction for 'neg' mix (default 0.5)
 *
 * Output: tab-separated columns for easy import into spreadsheets.
 */

#include "draugr/htc.h"
#include "draugr/cuckoo_filter.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

/* ─── Configuration ─────────────────────────────────────────── */
static int    opt_threads    = 1;
static int    opt_ops        = 100000;
static int    opt_buckets    = 64;
static double opt_load       = 0.75;
static char   opt_mix        = 'b'; /* r=read, b=balanced, w=write, n=neg */
static char   opt_key        = 's'; /* s=seq, r=random */
static int    opt_filter     = 0;
static double opt_neg_frac   = 0.5;

/* ─── Timer ─────────────────────────────────────────────────── */
static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e6 + ts.tv_nsec / 1e3;
}

/* ─── Per-thread state ──────────────────────────────────────── */
typedef struct {
    htc_table_t *t;
    int          id;
    int          ops;
    double       elapsed;
    uint64_t     seed;
} worker_t;

static uint64_t hash_seq(int i) {
    return (uint64_t)i * 0x9e3779b97f4a7c15ULL;
}

/* XORSHIFT PRNG for random keys */
static uint64_t xorshift64(uint64_t *s) {
    uint64_t x = *s;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *s = x;
    return x;
}

/* ─── Worker functions ──────────────────────────────────────── */
static void worker_read(worker_t *w) {
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = (opt_key == 'r')
            ? xorshift64(&w->seed) : hash_seq((int)(w->seed + i));
        uint64_t out;
        htc_find(w->t, h, &out);
    }
}

static void worker_balanced(worker_t *w) {
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = (opt_key == 'r')
            ? xorshift64(&w->seed) : hash_seq((int)(w->seed + i));
        int op = i & 3;
        if (op == 0) htc_insert(w->t, h, h >> 1);
        else if (op == 1) { uint64_t out; htc_find(w->t, h, &out); }
        else if (op == 2) htc_upsert(w->t, h, h >> 1);
        else htc_remove(w->t, h);
    }
}

static void worker_write(worker_t *w) {
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = (opt_key == 'r')
            ? xorshift64(&w->seed) : hash_seq((int)(w->seed + i));
        htc_insert(w->t, h, h >> 1);
    }
}

static void worker_neg(worker_t *w) {
    int neg_interval = (int)(1.0 / opt_neg_frac);
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = (opt_key == 'r')
            ? xorshift64(&w->seed) : hash_seq((int)(w->seed + i));
        if (i % neg_interval == 0) {
            htc_insert(w->t, h, h >> 1);
        } else {
            uint64_t out;
            htc_find(w->t, h, &out);
        }
    }
}

static void *worker_run(void *arg) {
    worker_t *w = (worker_t *)arg;
    w->seed = (uint64_t)w->id * 0x9e3779b97f4a7c15ULL + 1;
    double t0 = now_us();
    switch (opt_mix) {
    case 'r': worker_read(w); break;
    case 'b': worker_balanced(w); break;
    case 'w': worker_write(w); break;
    case 'n': worker_neg(w); break;
    }
    w->elapsed = now_us() - t0;
    return NULL;
}

/* ─── Main ──────────────────────────────────────────────────── */
int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Parse options */
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] != '-') continue;
        switch (argv[i][1]) {
        case 't': opt_threads = atoi(argv[++i]); break;
        case 'n': opt_ops = atoi(argv[++i]); break;
        case 'b': opt_buckets = atoi(argv[++i]); break;
        case 'l': opt_load = atof(argv[++i]); break;
        case 'm': opt_mix = argv[++i][0]; break;
        case 'k': opt_key = argv[++i][0]; break;
        case 'f': opt_filter = 1; break;
        case 'c': opt_neg_frac = atof(argv[++i]); break;
        }
    }

    /* Create table */
    htc_config_t cfg = {(uint32_t)opt_buckets, opt_load, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);

    /* Optional filter */
    cuckoo_filter_t *cf = NULL;
    if (opt_filter) {
        cf = cuckoo_filter_create((size_t)opt_threads * opt_ops, 4, 10, 200);
        htc_amq_filter_t amq = htc_amq_cuckoo(cf);
        htc_set_filter(t, &amq);
    }

    /* Warmup: pre-fill to target load */
    if (opt_mix != 'w') {
        int warmup = (int)((double)opt_buckets * 8 * opt_load * 0.5);
        for (int i = 0; i < warmup; i++)
            htc_insert(t, hash_seq(i + 1000000), (uint64_t)i);
    }

    /* Run threads */
    pthread_t threads[64];
    worker_t  workers[64];

    double t_start = now_us();
    for (int ti = 0; ti < opt_threads; ti++) {
        workers[ti].t = t;
        workers[ti].id = ti;
        workers[ti].ops = opt_ops;
        pthread_create(&threads[ti], NULL, worker_run, &workers[ti]);
    }

    double total_ops = 0;
    double max_time = 0;
    for (int ti = 0; ti < opt_threads; ti++) {
        pthread_join(threads[ti], NULL);
        total_ops += workers[ti].ops;
        if (workers[ti].elapsed > max_time)
            max_time = workers[ti].elapsed;
    }
    double t_elapsed = now_us() - t_start;

    /* Results — tab-separated data line */
    const char *mix_name = "read";
    if (opt_mix == 'b') mix_name = "bal";
    else if (opt_mix == 'w') mix_name = "write";
    else if (opt_mix == 'n') mix_name = "neg";

    /* Battery 13 Q29: reproducibility manifest */
    fprintf(stderr, "=== bench_htc manifest ===\n");
    fprintf(stderr, "threads %d\n", opt_threads);
    fprintf(stderr, "mix %s\n", mix_name);
    fprintf(stderr, "filter %d\n", opt_filter);
    fprintf(stderr, "load %.2f\n", opt_load);
    fprintf(stderr, "key_dist %c\n", opt_key);
    fprintf(stderr, "buckets_initial %d\n", opt_buckets);
    fprintf(stderr, "ops_per_thread %d\n", opt_ops);
    fprintf(stderr, "warmup %d\n", opt_mix != 'w' ? (int)((double)opt_buckets * 8 * opt_load * 0.5) : 0);
    fprintf(stderr, "front_cache %s\n", "disabled");
    fprintf(stderr, "stats %s\n",
#ifdef HTC_STATS
            "enabled"
#else
            "disabled"
#endif
           );
    fprintf(stderr, "debug %s\n",
#ifdef HTC_DEBUG
            "enabled"
#else
            "disabled"
#endif
           );
#if defined(__clang__)
    fprintf(stderr, "compiler clang\n");
    fprintf(stderr, "compiler_version %d.%d\n", __clang_major__, __clang_minor__);
#elif defined(__GNUC__) || defined(__GNUG__)
    fprintf(stderr, "compiler gcc\n");
    fprintf(stderr, "compiler_version %d.%d\n", __GNUC__, __GNUC_MINOR__);
#else
    fprintf(stderr, "compiler unknown\n");
#endif
#if defined(__aarch64__)
    fprintf(stderr, "arch aarch64\n");
#elif defined(__x86_64__) || defined(__amd64__)
    fprintf(stderr, "arch x86_64\n");
#else
    fprintf(stderr, "arch unknown\n");
#endif
    fprintf(stderr, "elapsed_us %.0f\n", t_elapsed);
    fprintf(stderr, "total_ops %.0f\n", total_ops);
    fprintf(stderr, "final_size %zu\n", htc_size(t));

    printf("threads\tmix\tfilter\tload\tops/sec\tus/op\tsize\n");
    printf("%d\t%s\t%d\t%.2f\t%.0f\t%.1f\t%zu\n",
           opt_threads, mix_name, opt_filter, opt_load,
           total_ops / (t_elapsed / 1e6),
           t_elapsed / total_ops,
           htc_size(t));

    if (cf) {
        htc_set_filter(t, NULL);
        cuckoo_filter_destroy(cf);
    }
    htc_destroy(t);
    return 0;
}
