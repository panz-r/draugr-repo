/**
 * cqf_filter.h - Counting Quotient Filter (CQF)
 *
 * Implements the CQF from:
 *   Pandey, Bender, Johnson, and Patro.
 *   "A General-Purpose Counting Filter: Making Every Bit Count."
 *   SIGMOD 2017.
 *
 * Additional implementation details from:
 *   Bungeroth. "Implementation of a Rank and Select Based Quotient Filter."
 *   2018.
 *
 * **Performance:**
 *   - Rank: O(1) via block-level occ prefix sums.
 *   - Select: O(log num_blocks) via binary search on run prefix sums.
 *   - Merge: COMPAT_VERSION and rbits/num_slots checked.
 *     Hash domain NOT tracked — operates on stored fingerprints as-is.
 *     Use only for filters constructed with the same hash function/seed.
 *
 * Counter encoding (Scheme 4.1, Table 3):
 *   C=1:         x
 *   C=2:         x, x
 *   C>2 (x>0):   x, enc, x        (base 2^r-2, symbols skip 0 and x)
 *   C=3 (x=0):   0, 0, 0
 *   C>3 (x=0):   0, enc, 0, 0    (base 2^r-1)
 *
 * All fingerprint values supported, including remainder=0.
 *
 * Collision and counting semantics:
 *   - Counting at fingerprint level (AMQ behavior).
 *   - count_occurrences >= true count; never undercounts.
 *   - Counter overflow at UINT64_MAX: returns CQF_ERR_OVERFLOW.
 *
 * Merge compatibility:
 *   - Requires same compat_version, remainder_bits AND num_slots.
 *   - Hash domain NOT tracked; merge treats fingerprints as-is.
 *
 * Resize:
 *   - Exact multiset preservation (stored-fingerprint domain).
 *     After resize, lookup by original hash may differ because the
 *     quotient split changed (rbits preserved, num_slots changed).
 *     Use enumerated fingerprints for post-resize queries.
 *   - Failure-atomic: old filter survives allocation or capacity failure.
 *   - Down-resize returns NULL on insufficient capacity.
 */

#ifndef DRAUGR_CQF_FILTER_H
#define DRAUGR_CQF_FILTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CQF_DEFAULT_FINGERPRINT_BITS 10
#define CQF_MIN_FINGERPRINT_BITS  2
#define CQF_MAX_FINGERPRINT_BITS  32
#define CQF_SLOTS_PER_BLOCK       64

typedef enum {
    CQF_OK = 0,
    CQF_ERR_INVALID_PARAM,
    CQF_ERR_FULL,
    CQF_ERR_NOT_FOUND,
    CQF_ERR_ALLOC,
    CQF_ERR_OVERFLOW,
} cqf_err_t;

/* Current compat_version: 1 (initial).
 * Increment on any encoding/metadata change that would make merged
 * filters or serialized state incompatible. */
#define CQF_COMPAT_VERSION 1

typedef struct cqf_filter {
    uint64_t  num_slots;
    uint64_t  num_blocks;
    uint64_t  count;              /* total insertions (including duplicates) */
    uint64_t  distinct_count;     /* number of distinct keys */
    uint8_t   remainder_bits;
    uint8_t   quotient_bits;
    uint16_t  entries_per_set;
    uint16_t  num_sets;
    uint64_t  remainder_mask;
    uint32_t  compat_version;     /* must match for merge/serialize */
    uint64_t  hash_seed;          /* 0 = unknown/unseeded */
} cqf_filter_t;

cqf_filter_t *cqf_filter_create(size_t capacity, uint8_t fp_bits);
void cqf_filter_destroy(cqf_filter_t *cf);
cqf_err_t cqf_filter_reset(cqf_filter_t *cf);

cqf_err_t cqf_filter_insert(cqf_filter_t *cf, uint64_t hash);
bool cqf_filter_lookup(const cqf_filter_t *cf, uint64_t hash);

/** Validate internal CQF invariants. Returns true if valid. */
bool cqf_filter_validate(const cqf_filter_t *cf);
cqf_err_t cqf_filter_delete(cqf_filter_t *cf, uint64_t hash);

/* CQF-specific: get count for an item */
uint64_t cqf_filter_count_occurrences(const cqf_filter_t *cf, uint64_t hash);

/* Merge two CQFs into a new one. Neither input filter is modified. */
cqf_filter_t *cqf_filter_merge(const cqf_filter_t *cf1, const cqf_filter_t *cf2);

/* Resize: create new CQF with different capacity, returns new filter.
 * The input filter is destroyed. */
cqf_filter_t *cqf_filter_resize(cqf_filter_t *cf, size_t new_capacity);

/* Statistics */
static inline size_t cqf_filter_count(const cqf_filter_t *cf) {
    return (size_t)cf->count;
}
static inline size_t cqf_filter_distinct_count(const cqf_filter_t *cf) {
    return (size_t)cf->distinct_count;
}
static inline size_t cqf_filter_capacity(const cqf_filter_t *cf) {
    return (size_t)cf->num_slots;
}
static inline double cqf_filter_load_factor(const cqf_filter_t *cf) {
    return (double)cf->distinct_count / (double)cf->num_slots;
}
size_t cqf_filter_memory_bytes(const cqf_filter_t *cf);
static inline double cqf_filter_bits_per_item(const cqf_filter_t *cf) {
    if (cf->count == 0) return 0.0;
    double meta_bits = (double)(cf->num_blocks * (5 * 64 + 8) + 2 * 64);
    double data_bits = (double)cf->num_slots * cf->remainder_bits;
    return (meta_bits + data_bits) / (double)cf->count;
}
double cqf_filter_estimated_fpr(const cqf_filter_t *cf);

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_CQF_FILTER_H */
