/**
 * test_reference_model.c — Cross-validates C implementation against
 * the Python reference model (Battery 25 Q1).
 *
 * Run:  ./test_reference_model && python3 htc_reference_model.py < trace.json
 *
 * This test generates a JSON operation trace, runs it through the C
 * implementation, and outputs the trace for the Python model to replay.
 * Both should produce the same results.
 */

#include "draugr/htc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void) {
    htc_table_t *t = htc_create(NULL);
    /* Generate JSON trace on stdout for Python model replay */
    /* Note: This test is for trace generation; comparison is manual or scripted */

    for (int i = 0; i < 10; i++) {
        htc_error_t r = htc_insert(t, (uint64_t)i, (uint64_t)(i * 10));
        printf("{\"op\":\"insert\",\"hash\":%d,\"result\":%d,\"size\":%zu}\n",
               i, (int)r, htc_size(t));
    }
    for (int i = 0; i < 10; i++) {
        uint64_t out = 0;
        htc_error_t r = htc_find(t, (uint64_t)i, &out);
        if (r == HTC_OK)
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d,\"value\":%lu}\n",
                   i, (int)r, out);
        else
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d}\n", i, (int)r);
    }
    for (int i = 0; i < 10; i += 2) {
        htc_error_t r = htc_remove(t, (uint64_t)i);
        printf("{\"op\":\"remove\",\"hash\":%d,\"result\":%d,\"size\":%zu}\n",
               i, (int)r, htc_size(t));
    }
    for (int i = 0; i < 10; i++) {
        uint64_t out = 0;
        htc_error_t r = htc_find(t, (uint64_t)i, &out);
        if (r == HTC_OK)
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d,\"value\":%lu}\n",
                   i, (int)r, out);
        else
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d}\n", i, (int)r);
    }

    htc_grow(t, false);
    for (int i = 0; i < 10; i++) {
        uint64_t out = 0;
        htc_error_t r = htc_find(t, (uint64_t)i, &out);
        if (r == HTC_OK)
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d,\"value\":%lu}\n",
                   i, (int)r, out);
        else
            printf("{\"op\":\"find\",\"hash\":%d,\"result\":%d}\n", i, (int)r);
    }

    htc_destroy(t);
    return 0;
}
