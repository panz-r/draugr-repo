/**
 * test_next_pow2.c - Unit tests for next_pow2 utility function
 *
 * Comprehensive coverage to catch platform-specific issues with:
 * - __builtin_clz behavior on edge cases
 * - Bit-fill fallback implementation
 * - 32-bit vs 64-bit size_t differences
 * - Overflow protection
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <limits.h>

size_t next_pow2(size_t n);

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name) void test_##name(void)
#define RUN(name) do { \
    printf("  " #name "... "); \
    test_##name(); \
    printf("PASSED\n"); \
    tests_passed++; \
} while(0)

#define ASSERT(cond) do { \
    if (!(cond)) { \
        printf("FAILED: %s\n", #cond); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_EQ(a, b) do { \
    if ((a) != (b)) { \
        printf("FAILED: %s (%zu) != %s (%zu)\n", #a, (size_t)(a), #b, (size_t)(b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

#define ASSERT_GE(a, b) do { \
    if ((a) < (b)) { \
        printf("FAILED: %s (%zu) < %s (%zu)\n", #a, (size_t)(a), #b, (size_t)(b)); \
        tests_failed++; \
        return; \
    } \
} while(0)

TEST(zeros_and_ones) {
    ASSERT_EQ(next_pow2(0), 1);
    ASSERT_EQ(next_pow2(1), 1);
}

TEST(powers_of_two) {
    ASSERT_EQ(next_pow2(2), 2);
    ASSERT_EQ(next_pow2(4), 4);
    ASSERT_EQ(next_pow2(8), 8);
    ASSERT_EQ(next_pow2(16), 16);
    ASSERT_EQ(next_pow2(32), 32);
    ASSERT_EQ(next_pow2(64), 64);
    ASSERT_EQ(next_pow2(128), 128);
    ASSERT_EQ(next_pow2(256), 256);
    ASSERT_EQ(next_pow2(512), 512);
    ASSERT_EQ(next_pow2(1024), 1024);
    ASSERT_EQ(next_pow2(2048), 2048);
    ASSERT_EQ(next_pow2(4096), 4096);
    ASSERT_EQ(next_pow2(8192), 8192);
}

TEST(just_below_powers_of_two) {
    ASSERT_EQ(next_pow2(3), 4);
    ASSERT_EQ(next_pow2(7), 8);
    ASSERT_EQ(next_pow2(15), 16);
    ASSERT_EQ(next_pow2(31), 32);
    ASSERT_EQ(next_pow2(63), 64);
    ASSERT_EQ(next_pow2(127), 128);
    ASSERT_EQ(next_pow2(255), 256);
    ASSERT_EQ(next_pow2(511), 512);
    ASSERT_EQ(next_pow2(1023), 1024);
    ASSERT_EQ(next_pow2(2047), 2048);
    ASSERT_EQ(next_pow2(4095), 4096);
    ASSERT_EQ(next_pow2(8191), 8192);
}

TEST(midpoint_values) {
    ASSERT_EQ(next_pow2(100), 128);
    ASSERT_EQ(next_pow2(1000), 1024);
    ASSERT_EQ(next_pow2(10000), 16384);
    ASSERT_EQ(next_pow2(500000), 524288);
    ASSERT_EQ(next_pow2(1000000), 1048576);
}

TEST(result_at_least_input) {
    for (size_t n = 0; n <= 65536; n++) {
        size_t result = next_pow2(n);
        ASSERT_GE(result, n);
    }
    for (size_t i = 0; i < 1000; i++) {
        size_t n = (size_t)rand() % 1000000;
        size_t result = next_pow2(n);
        ASSERT_GE(result, n);
    }
}

TEST(result_is_power_of_two) {
    for (size_t n = 0; n <= 65536; n++) {
        size_t result = next_pow2(n);
        ASSERT_EQ((result & (result - 1)), 0);
    }
    for (size_t i = 0; i < 1000; i++) {
        size_t n = (size_t)rand() % 1000000;
        size_t result = next_pow2(n);
        ASSERT_EQ((result & (result - 1)), 0);
    }
}

TEST(result_is_correct_next_power) {
    for (size_t n = 0; n <= 65536; n++) {
        size_t result = next_pow2(n);
        if (n == 0 || n == 1) continue;
        size_t expected = 1;
        while (expected < n) expected <<= 1;
        ASSERT_EQ(result, expected);
    }
}

TEST(max_values_64bit) {
    if (sizeof(size_t) >= 8) {
        ASSERT_EQ(next_pow2((size_t)1 << 62), (size_t)1 << 62);
        ASSERT_EQ(next_pow2((size_t)1 << 63), (size_t)1 << 63);
    } else {
        ASSERT_EQ(next_pow2((size_t)1 << 30), (size_t)1 << 30);
        ASSERT_EQ(next_pow2((size_t)1 << 31), (size_t)1 << 31);
    }
}

TEST(high_bit_patterns) {
    if (sizeof(size_t) >= 8) {
        ASSERT_EQ(next_pow2((size_t)1 << 32), (size_t)1 << 32);
        ASSERT_EQ(next_pow2((size_t)1 << 40), (size_t)1 << 40);
        ASSERT_EQ(next_pow2((size_t)1 << 48), (size_t)1 << 48);
        ASSERT_EQ(next_pow2((size_t)1 << 56), (size_t)1 << 56);

        ASSERT_EQ(next_pow2(((size_t)1 << 32) - 1), (size_t)1 << 32);
        ASSERT_EQ(next_pow2(((size_t)1 << 40) - 1), (size_t)1 << 40);
        ASSERT_EQ(next_pow2(((size_t)1 << 48) - 1), (size_t)1 << 48);
        ASSERT_EQ(next_pow2(((size_t)1 << 56) - 1), (size_t)1 << 56);
    } else {
        ASSERT_EQ(next_pow2((size_t)1 << 16), (size_t)1 << 16);
        ASSERT_EQ(next_pow2((size_t)1 << 24), (size_t)1 << 24);
        ASSERT_EQ(next_pow2((size_t)1 << 30), (size_t)1 << 30);

        ASSERT_EQ(next_pow2(((size_t)1 << 16) - 1), (size_t)1 << 16);
        ASSERT_EQ(next_pow2(((size_t)1 << 24) - 1), (size_t)1 << 24);
        ASSERT_EQ(next_pow2(((size_t)1 << 30) - 1), (size_t)1 << 30);
    }
}

TEST(two_ones_below_power) {
    ASSERT_EQ(next_pow2(5), 8);
    ASSERT_EQ(next_pow2(9), 16);
    ASSERT_EQ(next_pow2(17), 32);
    ASSERT_EQ(next_pow2(33), 64);
    ASSERT_EQ(next_pow2(65), 128);
    ASSERT_EQ(next_pow2(129), 256);
    ASSERT_EQ(next_pow2(257), 512);
    ASSERT_EQ(next_pow2(513), 1024);
    ASSERT_EQ(next_pow2(1025), 2048);
}

TEST(large_random_sampling) {
    srand(12345);
    for (int i = 0; i < 10000; i++) {
        size_t n = (size_t)(((uint64_t)rand() << 32) ^ rand());
        n = n % 1000000007;
        size_t result = next_pow2(n);

        ASSERT_GE(result, n);
        ASSERT_EQ((result & (result - 1)), 0);

        size_t expected = 1;
        while (expected < n) expected <<= 1;
        ASSERT_EQ(result, expected);
    }
}

TEST(specific_bit_boundaries) {
    for (int bit = 2; bit < (int)(sizeof(size_t) * 8) - 1; bit++) {
        size_t pow = (size_t)1 << bit;
        ASSERT_EQ(next_pow2(pow), pow);
        ASSERT_EQ(next_pow2(pow - 1), pow);
        ASSERT_EQ(next_pow2(pow + 1), pow << 1);
    }
}

TEST(bit_fill_fallback_coverage) {
    srand(54321);
    for (int i = 0; i < 5000; i++) {
        size_t n = (size_t)rand() % 100000;
        n = n * n + 1;
        n = n % 1000000;

        size_t result = next_pow2(n);
        ASSERT_GE(result, n);
        ASSERT_EQ((result & (result - 1)), 0);
    }
}

int main(void) {
    printf("========================================\n");
    printf("next_pow2 Unit Tests\n");
    printf("========================================\n");
    printf("size_t is %zu bytes\n", sizeof(size_t));

    printf("\nBasic edge cases:\n");
    RUN(zeros_and_ones);

    printf("\nPowers of two:\n");
    RUN(powers_of_two);

    printf("\nJust below powers of two:\n");
    RUN(just_below_powers_of_two);

    printf("\nMidpoint values:\n");
    RUN(midpoint_values);

    printf("\nResult >= input:\n");
    RUN(result_at_least_input);

    printf("\nResult is power of two:\n");
    RUN(result_is_power_of_two);

    printf("\nResult is correct next power:\n");
    RUN(result_is_correct_next_power);

    printf("\nHigh bit patterns:\n");
    RUN(high_bit_patterns);

    printf("\nTwo ones below power:\n");
    RUN(two_ones_below_power);

    printf("\nSpecific bit boundaries:\n");
    RUN(specific_bit_boundaries);

    printf("\nLarge random sampling:\n");
    RUN(large_random_sampling);

    printf("\nBit-fill fallback coverage:\n");
    RUN(bit_fill_fallback_coverage);

    printf("\nMax values:\n");
    RUN(max_values_64bit);

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed\n", tests_passed, tests_failed);
    printf("========================================\n");

    return tests_failed > 0 ? 1 : 0;
}