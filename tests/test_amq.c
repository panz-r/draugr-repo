#include "draugr/htc.h"
#include <stdio.h>
#include <assert.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);

    /* Test with cuckoo filter */
    htc_config_t cfg = {64, 0.75, 0, HTC_CFG_DISABLE_FRONT_CACHE};
    htc_table_t *t = htc_create(&cfg);
    cuckoo_filter_t *cf = cuckoo_filter_create(5000, 4, 10, 200);
    htc_amq_filter_t amq = htc_amq_cuckoo(cf);
    htc_set_filter(t, &amq);
    for (int i = 0; i < 1000; i++) {
        assert(htc_insert(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, (uint64_t)i));
    }
    for (int i = 0; i < 1000; i++) {
        uint64_t out;
        assert(htc_find(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, &out) && out == (uint64_t)i);
    }
    printf("cuckoo OK\n");
    htc_set_filter(t, NULL);
    cuckoo_filter_destroy(cf);
    htc_destroy(t);

    /* Test with vacuum filter */
    t = htc_create(&cfg);
    vacuum_filter_t *vf = vacuum_filter_create(5000, 4, 10, 200);
    amq = htc_amq_vacuum(vf);
    htc_set_filter(t, &amq);
    for (int i = 0; i < 1000; i++) {
        assert(htc_insert(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, (uint64_t)i));
    }
    for (int i = 0; i < 1000; i++) {
        uint64_t out;
        assert(htc_find(t, (uint64_t)i * 0x9e3779b97f4a7c15ULL, &out) && out == (uint64_t)i);
    }
    printf("vacuum OK\n");
    htc_set_filter(t, NULL);
    vacuum_filter_destroy(vf);
    htc_destroy(t);
    printf("ALL PASS\n");
    return 0;
}
