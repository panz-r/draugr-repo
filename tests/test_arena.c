/**
 * test_arena.c — Tests for Draugr Arena v4 Allocator
 */

#include "draugr/arena.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int tests_run = 0;
static int tests_failed = 0;

#define TEST_ASSERT(cond, msg) do { \
    tests_run++; \
    if (!(cond)) { \
        printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        tests_failed++; \
    } \
} while(0)

#define TEST_ASSERT_PTR(ptr, msg) TEST_ASSERT((ptr) != NULL, msg)
#define TEST_ASSERT_EQ(a, b, msg) TEST_ASSERT((a) == (b), msg)
#define TEST_ASSERT_NEQ(a, b, msg) TEST_ASSERT((a) != (b), msg)

static void test_arena_create_destroy(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(0);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    for (int i = 0; i < 10; i++) {
        void *p = arena_alloc(a, 32);
        TEST_ASSERT_PTR(p, "arena_alloc(32) returned NULL");
        memset(p, 0xAA, 32);
    }

    arena_destroy(a);
}

static void test_arena_alloc_small(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    size_t sizes[] = {16, 32, 64, 128};
    for (int s = 0; s < 4; s++) {
        void *ptrs[100];
        for (int i = 0; i < 100; i++) {
            ptrs[i] = arena_alloc(a, sizes[s]);
            TEST_ASSERT_PTR(ptrs[i], "arena_alloc small returned NULL");
            memset(ptrs[i], (uint8_t)(i & 0xFF), sizes[s]);
        }
        for (int i = 0; i < 100; i++) {
            arena_free(a, ptrs[i], sizes[s]);
        }
    }

    arena_destroy(a);
}

static void test_arena_alloc_256(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    for (int i = 0; i < 50; i++) {
        void *ptr = arena_alloc(a, 256);
        TEST_ASSERT_PTR(ptr, "arena_alloc(256) returned NULL");
        memset(ptr, 0xBB, 256);
        arena_free(a, ptr, 256);
    }

    arena_destroy(a);
}

static void test_arena_alloc_large(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    for (int i = 0; i < 10; i++) {
        void *ptr = arena_alloc(a, 512);
        TEST_ASSERT_PTR(ptr, "arena_alloc(512) returned NULL");
        memset(ptr, 0xAA, 512);
        arena_free(a, ptr, 512);
    }

    for (int i = 0; i < 10; i++) {
        void *ptr = arena_alloc(a, 4096);
        TEST_ASSERT_PTR(ptr, "arena_alloc(4096) returned NULL");
        memset(ptr, 0xCC, 4096);
        arena_free(a, ptr, 4096);
    }

    arena_destroy(a);
}

static void test_arena_alloc_very_large(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    void *ptr = arena_alloc(a, 65536);
    TEST_ASSERT_PTR(ptr, "arena_alloc(65536) returned NULL");
    memset(ptr, 0xDD, 65536);
    arena_free(a, ptr, 65536);

    arena_destroy(a);
}

static void test_arena_realloc(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    void *ptr = arena_alloc(a, 64);
    TEST_ASSERT_PTR(ptr, "arena_alloc(64) returned NULL");
    memset(ptr, 0xAB, 64);

    void *ptr2 = arena_realloc(a, ptr, 64, 128);
    TEST_ASSERT_PTR(ptr2, "arena_realloc(64->128) returned NULL");

    void *ptr3 = arena_realloc(a, ptr2, 128, 256);
    TEST_ASSERT_PTR(ptr3, "arena_realloc(128->256) returned NULL");

    arena_free(a, ptr3, 256);
    arena_destroy(a);
}

static void test_arena_stats(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    size_t stats[ARENA_STAT_COUNT];
    int ret = arena_get_stats(a, stats);
    TEST_ASSERT_EQ(ret, 0, "arena_get_stats returned error");

    TEST_ASSERT(arena_capacity(a) > 0, "arena_capacity should be > 0");

    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = arena_alloc(a, 64);
        TEST_ASSERT_PTR(ptrs[i], "arena_alloc(64) returned NULL");
    }

    size_t sz_after = arena_size(a);
    TEST_ASSERT(sz_after > 0, "arena_size should be > 0 after allocations");

    for (int i = 0; i < 100; i++) {
        arena_free(a, ptrs[i], 64);
    }

    arena_destroy(a);
}

static void test_arena_clear(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    for (int i = 0; i < 100; i++) {
        void *ptr = arena_alloc(a, 64);
        TEST_ASSERT_PTR(ptr, "arena_alloc(64) returned NULL");
    }

    arena_clear(a);

    for (int i = 0; i < 100; i++) {
        void *ptr = arena_alloc(a, 64);
        TEST_ASSERT_PTR(ptr, "arena_alloc(64) after clear returned NULL");
        arena_free(a, ptr, 64);
    }

    arena_destroy(a);
}

static void test_arena_stress(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(256 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    void *ptrs[1000];
    size_t sizes[1000];

    for (int i = 0; i < 1000; i++) {
        sizes[i] = (size_t)(rand() % 1024) + 1;
        ptrs[i] = arena_alloc(a, sizes[i]);
        TEST_ASSERT_PTR(ptrs[i], "stress allocation returned NULL");
    }

    for (int i = 0; i < 1000; i++) {
        arena_free(a, ptrs[i], sizes[i]);
    }

    arena_destroy(a);
}

static void test_arena_zero_size(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    void *ptr = arena_alloc(a, 0);
    TEST_ASSERT(ptr == NULL, "arena_alloc(0) should return NULL");

    arena_destroy(a);
}

static void test_arena_compact(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    arena_compact(a, ARENA_FREQ_WARM);
    arena_compact(a, ARENA_FREQ_WARM);
    arena_compact(a, ARENA_FREQ_COLD);

    arena_destroy(a);
}

static void test_arena_tcache_cycle(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(64 * 1024);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");

    void *ptrs[100];
    for (int i = 0; i < 100; i++) {
        ptrs[i] = arena_alloc(a, 32);
        TEST_ASSERT_PTR(ptrs[i], "tcache cycle alloc returned NULL");
    }
    for (int i = 0; i < 100; i++) {
        arena_free(a, ptrs[i], 32);
    }
    for (int i = 0; i < 100; i++) {
        ptrs[i] = arena_alloc(a, 32);
        TEST_ASSERT_PTR(ptrs[i], "tcache cycle re-alloc returned NULL");
    }
    for (int i = 0; i < 100; i++) {
        arena_free(a, ptrs[i], 32);
    }

    arena_destroy(a);
}

static void test_arena_multiple_destroy(void) {
    printf("Running: %s\n", __func__);

    struct arena *a = arena_create(0);
    TEST_ASSERT_PTR(a, "arena_create returned NULL");
    arena_destroy(a);

    a = arena_create(0);
    TEST_ASSERT_PTR(a, "arena_create 2 returned NULL");
    arena_destroy(a);

    a = arena_create(0);
    TEST_ASSERT_PTR(a, "arena_create 3 returned NULL");
    void *p = arena_alloc(a, 64);
    TEST_ASSERT_PTR(p, "alloc after multiple create returned NULL");
    arena_free(a, p, 64);
    arena_destroy(a);
}

static void test_arena_null_ops(void) {
    printf("Running: %s\n", __func__);

    TEST_ASSERT(arena_alloc(NULL, 64) == NULL, "arena_alloc(NULL) should return NULL");
    TEST_ASSERT(arena_realloc(NULL, NULL, 0, 64) == NULL, "arena_realloc(NULL) should return NULL");
    arena_free(NULL, NULL, 0);
    arena_clear(NULL);
    arena_compact(NULL, ARENA_FREQ_WARM);
    TEST_ASSERT(arena_get_stats(NULL, NULL) == -1, "arena_get_stats(NULL) should return -1");
    TEST_ASSERT(arena_size(NULL) == 0, "arena_size(NULL) should return 0");
    TEST_ASSERT(arena_capacity(NULL) == 0, "arena_capacity(NULL) should return 0");
}

int main(void) {
    printf("=== Arena v4 Tests ===\n\n");

    test_arena_create_destroy();
    test_arena_alloc_small();
    test_arena_alloc_256();
    test_arena_alloc_large();
    test_arena_alloc_very_large();
    test_arena_realloc();
    test_arena_stats();
    test_arena_clear();
    test_arena_stress();
    test_arena_zero_size();
    test_arena_compact();
    test_arena_tcache_cycle();
    test_arena_multiple_destroy();
    test_arena_null_ops();

    printf("\n=== Results ===\n");
    printf("Tests run: %d\n", tests_run);
    printf("Tests failed: %d\n", tests_failed);

    return tests_failed > 0 ? 1 : 0;
}
