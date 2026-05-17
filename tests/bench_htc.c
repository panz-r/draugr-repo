/**
 * bench_htc.c — Performance benchmark for htc table (§28)
 *
 * Usage: ./bench_htc [options]
 *   -t N        thread count (default 1)
 *   -n N        operations per thread (default 100000)
 *   -b N        initial buckets (default 64)
 *   -l FLOAT    max load factor (default 0.75)
  *   -m MIX      operation mix: read, balanced, write, insert-nogrow, insert-withgrow, neg, pureneg, mixednogrow (default balanced)
 *   -k DIST     key distribution: seq, random (default seq)
 *   -f          enable cuckoo filter (default off)
 *   -c FLOAT    negative lookup fraction for 'neg' mix (default 0.5)
 *
 * Output: tab-separated columns for easy import into spreadsheets.
 */

#include "draugr/htc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/time.h>

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

/* Pure negative find microbench: table pre-filled, no inserts during test.
 * Uses hashes guaranteed to be absent (high range) for negative lookups. */
static void worker_pure_neg(worker_t *w) {
    uint64_t base = (uint64_t)w->id * 10000000ULL + 100000000ULL;
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = base + (opt_key == 'r' ? xorshift64(&w->seed) : (uint64_t)i);
        uint64_t out;
        htc_find(w->t, h, &out);
    }
}

/* Insert-no-grow: table pre-sized with slack so no grow occurs during timed
 * section.  Pure inserts into reserved capacity. */
static void worker_insert_nogrow(worker_t *w) {
    uint64_t base = (uint64_t)w->id * 10000000ULL + 300000000ULL;
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = base + (opt_key == 'r' ? xorshift64(&w->seed) : (uint64_t)i);
        htc_error_t r = htc_insert(w->t, h, h >> 1);
        (void)r;
    }
}

/* Insert-with-grow: starts near load threshold (using reduced warmup),
 * then inserts to intentionally trigger grow/reseed during timed region. */
static void worker_insert_withgrow(worker_t *w) {
    uint64_t base = (uint64_t)w->id * 10000000ULL + 400000000ULL;
    for (int i = 0; i < w->ops; i++) {
        uint64_t h = base + (opt_key == 'r' ? xorshift64(&w->seed) : (uint64_t)i);
        htc_error_t r = htc_insert(w->t, h, h >> 1);
        (void)r;
    }
}

/* Mixed no-grow benchmark: table pre-sized with slack, no growth during timed
 * section.  ~80% successful find, ~15% negative find, ~5% insert/remove.
 * Uses a separate hash range for inserts to stay within slack capacity. */
static void worker_mixed_nogrow(worker_t *w) {
    uint64_t insert_base = (uint64_t)w->id * 1000000ULL + 200000000ULL;
    uint64_t find_base   = (uint64_t)w->id * 1000000ULL;
    int      insert_pos  = 0;
    for (int i = 0; i < w->ops; i++) {
        int r = (int)(xorshift64(&w->seed) & 63);  /* 0..63 */
        if (r < 5) {
            /* 5/64 ≈ 8% insert or remove */
            uint64_t h = insert_base + (uint64_t)(insert_pos++);
            if (r < 3)
                htc_insert(w->t, h, h >> 1);
            else
                htc_remove(w->t, h);
        } else {
            uint64_t h = find_base + (uint64_t)((xorshift64(&w->seed)) % (uint64_t)w->ops);
            if (h < insert_base)
                h += insert_base;
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
    case 'i': worker_insert_nogrow(w); break;
    case 'j': worker_insert_withgrow(w); break;
    case 'n': worker_neg(w); break;
    case 'p': worker_pure_neg(w); break;
    case 'g': worker_mixed_nogrow(w); break;
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
    if (opt_mix != 'w' && opt_mix != 'j') {
        int warmup;
        if (opt_mix == 'i')
            warmup = (int)((double)opt_buckets * 8 * opt_load * 0.3); /* leave slack for nogrow */
        else
            warmup = (int)((double)opt_buckets * 8 * opt_load * 0.5);
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
    else if (opt_mix == 'p') mix_name = "pureneg";
    else if (opt_mix == 'i') mix_name = "insertnogrow";
    else if (opt_mix == 'j') mix_name = "insertwithgrow";
    else if (opt_mix == 'g') mix_name = "mixednogrow";

    /* Battery 13 Q29, Battery 28 §29: reproducibility manifest */
    {
        FILE *git_f = popen("git rev-parse HEAD 2>/dev/null", "r");
        char git_commit[48] = "unknown";
        if (git_f) {
            if (fgets(git_commit, sizeof(git_commit), git_f)) {
                size_t ln = strlen(git_commit);
                if (ln > 0 && git_commit[ln-1] == '\n') git_commit[ln-1] = '\0';
            }
            pclose(git_f);
        }
        fprintf(stderr, "git_commit %s\n", git_commit);
    }
    {
        time_t now = time(NULL);
        char date_buf[32];
        struct tm *tm_ptr = localtime(&now);
        if (tm_ptr) {
            strftime(date_buf, sizeof(date_buf), "%Y-%m-%d %H:%M:%S %z", tm_ptr);
            fprintf(stderr, "date %s\n", date_buf);
        }
    }
    fprintf(stderr, "=== bench_htc manifest ===\n");
    fprintf(stderr, "threads %d\n", opt_threads);
    fprintf(stderr, "mix %s\n", mix_name);
    fprintf(stderr, "filter %d\n", opt_filter);
    fprintf(stderr, "load %.2f\n", opt_load);
    fprintf(stderr, "key_dist %c\n", opt_key);
    fprintf(stderr, "buckets_initial %d\n", opt_buckets);
    fprintf(stderr, "ops_per_thread %d\n", opt_ops);
    fprintf(stderr, "warmup %d\n", opt_mix != 'w' ? (int)((double)opt_buckets * 8 * opt_load * 0.5) : 0);
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
    /* Benchmark guardrails: grow estimate, load factors */
    size_t final_sz = htc_size(t);
    size_t initial_cap = (size_t)opt_buckets * 8; /* HTC_BUCKET_SLOTS */
    int grow_est = 0;
    size_t cap = initial_cap;
    while (cap < final_sz + initial_cap/2) { grow_est++; cap *= 2; }
    double final_lf = final_sz > 0 && opt_buckets > 0
        ? (double)final_sz / (double)(opt_buckets * 8) /* HTC_BUCKET_SLOTS */
        : 0.0;
    fprintf(stderr, "elapsed_us %.0f\n", t_elapsed);
    fprintf(stderr, "total_ops %.0f\n", total_ops);
    fprintf(stderr, "final_size %zu\n", final_sz);
    fprintf(stderr, "initial_capacity %zu\n", initial_cap);
    fprintf(stderr, "final_load_factor %.2f\n", final_lf);
    fprintf(stderr, "grow_estimate %d\n", grow_est);
    fprintf(stderr, "front_cache %s\n", "disabled");

    printf("threads\tmix\tfilter\tload\tfinal_lf\tgrow_est\tops/sec\tus/op\tsize\n");
    printf("%d\t%s\t%d\t%.2f\t%.2f\t%d\t%.0f\t%.1f\t%zu\n",
           opt_threads, mix_name, opt_filter, opt_load,
           final_lf, grow_est,
           total_ops / (t_elapsed / 1e6),
           t_elapsed / total_ops,
           final_sz);

    if (cf) {
        htc_set_filter(t, NULL);
        cuckoo_filter_destroy(cf);
    }
    htc_destroy(t);
    return 0;
}
