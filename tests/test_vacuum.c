#include "draugr/vacuum_filter.h"
#include <stdio.h>
#include <assert.h>
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    vacuum_filter_t *vf = vacuum_filter_create(5000, 4, 10, 200);
    assert(vf);
    for (int i = 0; i < 2000; i++)
        assert(vacuum_filter_insert(vf, (uint64_t)i * 0x9e3779b97f4a7c15ULL) == VACUUM_OK);
    for (int i = 0; i < 2000; i++)
        assert(vacuum_filter_lookup(vf, (uint64_t)i * 0x9e3779b97f4a7c15ULL));
    int fp = 0;
    for (int i = 2000; i < 50000; i++)
        if (vacuum_filter_lookup(vf, (uint64_t)i * 0x9e3779b97f4a7c15ULL))
            fp++;
    printf("FPR: %d/48000 = %.4f%%\n", fp, fp / 480.0);
    for (int i = 0; i < 2000; i += 2)
        vacuum_filter_delete(vf, (uint64_t)i * 0x9e3779b97f4a7c15ULL);
    for (int i = 1; i < 2000; i += 2)
        assert(vacuum_filter_lookup(vf, (uint64_t)i * 0x9e3779b97f4a7c15ULL));
    printf("vacuum PASS\n");
    vacuum_filter_destroy(vf);
    return 0;
}
