#include "draugr/htc_kv.h"
#include <stdio.h>
#include <string.h>
#include <assert.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    htc_kv_t *kv = htc_kv_create(NULL);
    assert(kv);
    assert(htc_kv_count(kv) == 0);

    const char *k1 = "hello", *v1 = "world";
    const char *k2 = "key2",  *v2 = "value2";
    const char *k3 = "hello"; /* same as k1 for duplicate test */

    assert(htc_kv_insert(kv, k1, strlen(k1)+1, v1, strlen(v1)+1));
    assert(htc_kv_count(kv) == 1);

    char buf[64];
    size_t blen = sizeof(buf);
    assert(htc_kv_find(kv, k1, strlen(k1)+1, buf, &blen));
    assert(strcmp(buf, v1) == 0);
    assert(blen == strlen(v1)+1);

    /* Second insert of same key should fail (duplicate hash) */
    assert(!htc_kv_insert(kv, k3, strlen(k3)+1, v1, strlen(v1)+1));

    assert(htc_kv_insert(kv, k2, strlen(k2)+1, v2, strlen(v2)+1));
    assert(htc_kv_count(kv) == 2);

    blen = sizeof(buf);
    assert(htc_kv_find(kv, k2, strlen(k2)+1, buf, &blen));
    assert(strcmp(buf, v2) == 0);

    /* Non-existent key */
    assert(!htc_kv_find(kv, "nope", 5, NULL, NULL));

    /* Remove */
    assert(htc_kv_remove(kv, k1, strlen(k1)+1));
    assert(htc_kv_count(kv) == 1);
    assert(!htc_kv_find(kv, k1, strlen(k1)+1, NULL, NULL));

    /* Insert copy */
    assert(htc_kv_insert_copy(kv, "copied", 7, "data", 5));
    assert(htc_kv_count(kv) == 2);

    printf("htc_kv PASS\n");
    htc_kv_destroy(kv);
    return 0;
}
