#ifndef DRAUGR_PROPERTY_UTILS_H
#define DRAUGR_PROPERTY_UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <time.h>

#include "draugr/ht.h"
#include "draugr/ht_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Mock allocator (wrapped via --wrap) ──────────────────── */
extern void *__real_malloc(size_t);
extern void *__real_calloc(size_t, size_t);
extern void *__real_realloc(void *, size_t);
extern void  __real_free(void *);

void *__wrap_malloc(size_t);
void *__wrap_calloc(size_t, size_t);
void *__wrap_realloc(void *, size_t);
void  __wrap_free(void *);

void  alloc_mock_reset(void);
void  alloc_mock_set_max_alloc_calls(int);
void  alloc_mock_set_max_alloc_size(size_t);
void  alloc_mock_set_alloc_num_to_fail(int);
int   alloc_mock_get_count(void);
int   alloc_mock_get_fail_count(void);

/* ─── Hash functions ──────────────────────────────────────── */
uint64_t simple_hash(const void *key, size_t len, void *ctx);
uint64_t fixed_hash(const void *key, size_t len, void *ctx);
uint64_t hash_one(const void *key, size_t len, void *ctx);
uint64_t hash_zero(const void *key, size_t len, void *ctx);
uint64_t zero_hash(const void *key, size_t len, void *ctx);

/* ─── Reference model (simple ordered dictionary) ─────────── */
#define MODEL_MAX_ENTRIES 8192
#define MODEL_MAX_KEY_LEN 128
#define MODEL_MAX_VAL_LEN 512

typedef struct {
    char    keys[MODEL_MAX_ENTRIES][MODEL_MAX_KEY_LEN];
    size_t  key_lens[MODEL_MAX_ENTRIES];
    char    vals[MODEL_MAX_ENTRIES][MODEL_MAX_VAL_LEN];
    size_t  val_lens[MODEL_MAX_ENTRIES];
    size_t  count;
} model_t;

model_t      *model_create(void);
void          model_destroy(model_t *m);
size_t        model_size(model_t *m);
int           model_find(model_t *m, const char *key, size_t key_len);
void          model_set(model_t *m, const char *key, size_t key_len,
                        const char *value, size_t value_len);
bool          model_remove(model_t *m, const char *key, size_t key_len);
const char   *model_find_val(model_t *m, const char *key, size_t key_len,
                             size_t *out_val_len);

/* ─── Random utilities ────────────────────────────────────── */
uint32_t my_rand(void);
void     my_srand(uint32_t seed);
uint64_t splitmix64(uint64_t *state);

/* ─── Test counters & macros ──────────────────────────────── */
extern int tests_passed;
extern int tests_failed;

#define MODEL_CHECK(cond) do { \
    if (!(cond)) { \
        printf("  MODEL FAIL at line %d: %s\n", __LINE__, #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define PASS(msg) do { printf("  PASS: %s\n", msg); tests_passed++; } while(0)
#define BUG(msg)  do { printf("  BUG: %s\n", msg); tests_failed++; } while(0)

/* ─── Bare table slot inspector ───────────────────────────── */
void count_bare_slots(ht_bare_t *t, size_t *live, size_t *tomb, size_t *empty);

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_PROPERTY_UTILS_H */
