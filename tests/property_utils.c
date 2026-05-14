#include "property_utils.h"

/* ─── Mock allocator state ────────────────────────────────── */
static struct {
    int max_alloc_calls;
    int alloc_count;
    int alloc_num_to_fail;
    bool fail_by_size;
    size_t fail_size_threshold;
    int fail_count;
    bool active;
} mock = {-1, 0, -1, false, 0, 0, false};

void alloc_mock_reset(void) {
    mock.max_alloc_calls = -1;
    mock.alloc_count = 0;
    mock.alloc_num_to_fail = -1;
    mock.fail_by_size = false;
    mock.fail_size_threshold = SIZE_MAX;
    mock.fail_count = 0;
    mock.active = false;
}

void alloc_mock_set_max_alloc_calls(int n) { mock.max_alloc_calls = n; mock.active = (n > 0); }
void alloc_mock_set_max_alloc_size(size_t t) { mock.fail_by_size = true; mock.fail_size_threshold = t; mock.active = true; }
void alloc_mock_set_alloc_num_to_fail(int n) { mock.alloc_num_to_fail = n; mock.active = (n >= 0); }
int  alloc_mock_get_count(void) { return mock.alloc_count; }
int  alloc_mock_get_fail_count(void) { return mock.fail_count; }

static bool should_fail(size_t size) {
    if (!mock.active) return false;
    if (mock.max_alloc_calls > 0 && mock.alloc_count >= mock.max_alloc_calls) return true;
    if (mock.fail_by_size && size >= mock.fail_size_threshold) return true;
    if (mock.alloc_num_to_fail >= 0 && mock.alloc_count == mock.alloc_num_to_fail) return true;
    return false;
}

void *__wrap_malloc(size_t size) {
    mock.alloc_count++;
    if (should_fail(size)) { mock.fail_count++; return NULL; }
    return __real_malloc(size);
}

void *__wrap_calloc(size_t nmemb, size_t size) {
    mock.alloc_count++;
    if (should_fail(nmemb * size)) { mock.fail_count++; return NULL; }
    return __real_calloc(nmemb, size);
}

void *__wrap_realloc(void *ptr, size_t size) {
    mock.alloc_count++;
    if (should_fail(size)) { mock.fail_count++; return NULL; }
    return __real_realloc(ptr, size);
}

void __wrap_free(void *ptr) { __real_free(ptr); }

/* ─── Hash functions ──────────────────────────────────────── */
uint64_t simple_hash(const void *key, size_t len, void *ctx) {
    (void)ctx;
    uint64_t h = 0xcbf29ce484222325ULL;
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < len; i++) { h ^= p[i]; h *= 0x100000001b3ULL; }
    return h;
}

uint64_t fixed_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx; return 42;
}

uint64_t hash_one(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx; return 1;
}

uint64_t hash_zero(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx; return 0;
}
uint64_t zero_hash(const void *key, size_t len, void *ctx) {
    (void)key; (void)len; (void)ctx; return 0;
}

/* ─── Reference model ─────────────────────────────────────── */
model_t *model_create(void) {
    return (model_t *)calloc(1, sizeof(model_t));
}

void model_destroy(model_t *m) { free(m); }

size_t model_size(model_t *m) { return m->count; }

int model_find(model_t *m, const char *key, size_t key_len) {
    size_t check_len = key_len < MODEL_MAX_KEY_LEN ? key_len : MODEL_MAX_KEY_LEN;
    for (size_t i = 0; i < m->count; i++)
        if (m->key_lens[i] == check_len && memcmp(m->keys[i], key, check_len) == 0)
            return (int)i;
    return -1;
}

void model_set(model_t *m, const char *key, size_t key_len,
               const char *value, size_t value_len) {
    /* Keys longer than MODEL_MAX_KEY_LEN are truncated for storage;
       the stored key_len reflects this so model_find stays consistent. */
    size_t store_klen = key_len < MODEL_MAX_KEY_LEN ? key_len : MODEL_MAX_KEY_LEN;
    size_t store_vlen = value_len < MODEL_MAX_VAL_LEN ? value_len : MODEL_MAX_VAL_LEN;
    int idx = model_find(m, key, store_klen);
    if (idx >= 0) {
        memcpy(m->vals[idx], value, store_vlen);
        m->val_lens[idx] = value_len;
    } else {
        if (m->count >= MODEL_MAX_ENTRIES) return;
        idx = (int)m->count++;
        memcpy(m->keys[idx], key, store_klen);
        m->key_lens[idx] = store_klen;
        memcpy(m->vals[idx], value, store_vlen);
        m->val_lens[idx] = value_len;
    }
}

bool model_remove(model_t *m, const char *key, size_t key_len) {
    int idx = model_find(m, key, key_len);
    if (idx < 0) return false;
    m->count--;
    for (size_t i = idx; i < m->count; i++) {
        memcpy(m->keys[i], m->keys[i + 1], MODEL_MAX_KEY_LEN);
        m->key_lens[i] = m->key_lens[i + 1];
        memcpy(m->vals[i], m->vals[i + 1], MODEL_MAX_VAL_LEN);
        m->val_lens[i] = m->val_lens[i + 1];
    }
    memset(m->keys[m->count], 0, MODEL_MAX_KEY_LEN);
    memset(m->vals[m->count], 0, MODEL_MAX_VAL_LEN);
    m->key_lens[m->count] = 0;
    m->val_lens[m->count] = 0;
    return true;
}

const char *model_find_val(model_t *m, const char *key, size_t key_len,
                           size_t *out_val_len) {
    int idx = model_find(m, key, key_len);
    if (idx < 0) return NULL;
    if (out_val_len) *out_val_len = m->val_lens[idx];
    return m->vals[idx];
}

/* ─── Random utilities ────────────────────────────────────── */
static uint32_t g_seed = 0;
static uint64_t g_seed2 = 0;

uint32_t my_rand(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    g_seed2 ^= g_seed2 << 17;
    g_seed2 ^= g_seed2 >> 31;
    g_seed2 ^= g_seed2 << 8;
    return g_seed ^ (uint32_t)(g_seed2 >> 32);
}

void my_srand(uint32_t seed) {
    g_seed = seed;
    g_seed2 = (uint64_t)seed << 32 | seed;
}

uint64_t splitmix64(uint64_t *state) {
    uint64_t z = (*state += 0x9e3779b97f4a7c15ULL);
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}

/* ─── Test counters ───────────────────────────────────────── */
int tests_passed = 0;
int tests_failed = 0;

/* ─── Slot inspector ──────────────────────────────────────── */
void count_bare_slots(ht_bare_t *t, size_t *live, size_t *tomb, size_t *empty) {
    *live = *tomb = *empty = 0;
    for (size_t i = 0; i < t->capacity; i++) {
        uint64_t hpd = t->hash_pd[i];
        if (hpd_live(hpd)) (*live)++;
        else if (hpd_tomb(hpd)) (*tomb)++;
        else (*empty)++;
    }
}
