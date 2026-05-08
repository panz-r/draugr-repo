#include <stddef.h>
#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define HAVE_BUILTIN_CLZ 1
#endif

size_t next_pow2(size_t n) {
    if (n == 0) return 1;

#if defined(HAVE_BUILTIN_CLZ)
    if (sizeof(size_t) == 8) {
        return n == 1 ? 1 : (size_t)1 << (64 - __builtin_clzl(n - 1));
    } else {
        return n == 1 ? 1 : (size_t)1 << (32 - __builtin_clz(n - 1));
    }
#else
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    if (sizeof(size_t) == 8) n |= n >> 32;
    n++;
    return n;
#endif
}