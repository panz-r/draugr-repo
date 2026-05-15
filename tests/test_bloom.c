#include "draugr/bloom_filter.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Basic insert + lookup with string keys */
    bloom_filter_t *bf = bloom_filter_create(1000, 0.01);
    assert(bf);

    const char *keys[] = {"hello", "world", "bloom", "filter", "test"};
    for (int i = 0; i < 5; i++)
        bloom_filter_insert(bf, keys[i], strlen(keys[i]));

    for (int i = 0; i < 5; i++)
        assert(bloom_filter_lookup(bf, keys[i], strlen(keys[i])));

    /* Test FPR on non-inserted keys */
    const char *nope = "nonexistent";
    int fp = 0;
    for (int i = 0; i < 10000; i++) {
        char tmp[32];
        snprintf(tmp, sizeof(tmp), "%s%d", nope, i);
        if (bloom_filter_lookup(bf, tmp, strlen(tmp)))
            fp++;
        bloom_filter_insert(bf, tmp, strlen(tmp));
    }
    printf("FPR: %d/10000 = %.2f%%\n", fp, fp / 100.0);

    /* Test reset */
    bloom_filter_reset(bf);
    assert(!bloom_filter_lookup(bf, "hello", 5));

    bloom_filter_destroy(bf);

    /* Test raw creation */
    bf = bloom_filter_create_raw(64000, 7);
    assert(bf);
    bloom_filter_destroy(bf);

    printf("bloom PASS\n");
    return 0;
}
