/**
 * rsqf_filter.h - Rank-and-Select Quotient Filter (RSQF)
 *
 * Implements the RSQF from:
 *   Pandey, Bender, Johnson, and Patro.
 *   "A General-Purpose Counting Filter: Making Every Bit Count."
 *   SIGMOD 2017.
 *
 * Additional implementation details from:
 *   Bungeroth. "Implementation of a Rank and Select Based Quotient Filter."
 *   2018.
 *
 * Performance (matching paper complexity claims):
 *   - rank_occ:     O(1)  via block-level occ prefix sums.
 *   - rank_select:  O(1)  via offset-guided block jump + bounded forward scan
 *                          (Pandey et al. §3.2, Bungeroth §3.2 Figure 3).
 *   - select_run:   O(log num_blocks) via binary search on run prefix sums
 *                          (fallback for non-quotient-mapped queries).
 *   - slot_is_free: O(1)  via physical occupancy bitset.
 *   - Prefix sums and offsets rebuilt incrementally after mutation;
 *     only blocks from the first changed position onward are recomputed.
 *     Wraparound shifts fall back to full recompute.
 *     Staleness does not affect correctness.
 *
 * Augmentation vs. the canonical RSQF:
 *   The paper stores a per-block `used` flag (1 bit per 64-slot block) and
 *   disambiguates unused slots by testing whether the slot's remainder value
 *   is zero.  We instead store a per-slot physical occupancy bitvector
 *   (phys_occ, 1 bit/slot).  This removes the ambiguity entirely: every slot
 *   is independently tracked, and remainder=0 is treated as a valid
 *   fingerprint without special sentinel logic.  The memory cost is 1 extra
 *   bit/slot (~0.3 % of total filter size at 10-bit fingerprints).
 *
 * Structure and invariants (matching the paper):
 *   - 64-slot blocks with occupieds + runends + prefix sums + offsets
 *   - Circular slot array (wraparound clusters FULLY supported)
 *   - Single hash function: fingerprint = (hash >> rbits) | (hash & mask)
 *   - Quotient = fingerprint >> rbits (top bits)
 *   - Remainder = fingerprint & remainder_mask (bottom rbits)
 *   - Sorted fingerprint order within each run (linear probing)
 *   - All fingerprint values supported, including remainder=0
 *
 * Memory layout:
 *   rsqf_filter_t
 *   ├── occupieds[num_blocks]     — 1.000 bits/slot
 *   ├── runends[num_blocks]       — 1.000 bits/slot
 *   ├── phys_occ[num_blocks]      — 1.000 bits/slot (augmentation)
 *   ├── occ_prefix[num_blocks+1]  — 1.000+ bits/slot
 *   ├── run_prefix[num_blocks+1]  — 1.000+ bits/slot
 *   ├── offsets[num_blocks]       — 0.125 bits/slot (padded to 8B)
 *   └── table[remainder_words]    — fp_bits bits/slot (bit-packed)
 *   Metadata overhead: ~5.125 bits/slot
 *
 * Collision and delete semantics:
 *   - Two distinct keys with the same (quotient, remainder) fingerprint
 *     are treated as duplicates (inherent AMQ behavior).
 *   - delete(hash) removes one fingerprint copy; may cause false-positive
 *     deletion for colliding fingerprints (identical to cuckoo filter).
 *   - No false negatives: lookup(hash) always returns true after insert.
 */

#ifndef DRAUGR_RSQF_FILTER_H
#define DRAUGR_RSQF_FILTER_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Configuration constants
 * ============================================================================ */

#define RSQF_DEFAULT_FINGERPRINT_BITS 10
#define RSQF_MIN_FINGERPRINT_BITS  2
#define RSQF_MAX_FINGERPRINT_BITS  32
#define RSQF_SLOTS_PER_BLOCK       64
#define RSQF_BLOCK_WORDS           (RSQF_SLOTS_PER_BLOCK / 64)
#define RSQF_SET_BYTES             64
#define RSQF_SET_WORDS             (RSQF_SET_BYTES / sizeof(uint64_t))

/* ============================================================================
 * Error codes
 * ============================================================================ */

typedef enum {
    RSQF_OK = 0,
    RSQF_ERR_INVALID_PARAM,
    RSQF_ERR_FULL,
    RSQF_ERR_NOT_FOUND,
    RSQF_ERR_ALLOC,
} rsqf_err_t;

/* ============================================================================
 * RSQF filter structure
 *
 * Memory layout (single allocation):
 *   rsqf_filter_t              — header
 *   occupieds[n]               — uint64_t[n]  (n = num_blocks)
 *   runends[n]                 — uint64_t[n]
 *   phys_occ[n]                — uint64_t[n]  (physical occupancy for rem=0)
 *   occ_prefix[n+1]            — uint64_t[n+1]  (block-level prefix sums of occ)
 *   run_prefix[n+1]            — uint64_t[n+1]  (block-level prefix sums of run)
 *   offsets[n]                 — uint8_t[n]  (padded to 8B alignment)
 *   table[remainder_words]     — uint64_t[m]  (bit-packed remainders)
 *   Metadata: ~5.125 bits/slot = (64+64+64+64+64+8)/64 + 2*64/(n*64)
 * ============================================================================ */

typedef struct rsqf_filter {
    uint64_t  num_slots;          /* 2^q total slots */
    uint64_t  num_blocks;         /* num_slots / 64 */
    uint64_t  count;              /* items currently stored */
    uint8_t   remainder_bits;     /* r = fingerprint bits per remainder */
    uint8_t   quotient_bits;      /* q = slots = 2^q */
    uint16_t  entries_per_set;    /* remainders per 64-byte set */
    uint16_t  num_sets;           /* total sets in table */
    uint64_t  remainder_mask;     /* (1 << remainder_bits) - 1 */
} rsqf_filter_t;

/* ============================================================================
 * Lifecycle
 * ============================================================================ */

/**
 * Create a new RSQF filter.
 * Single allocation: struct + metadata + table in one malloc.
 *
 * @param capacity  Expected maximum number of items
 * @param fp_bits   Fingerprint bits (1-32), 0 for default (10)
 * @return          Pointer to allocated filter, or NULL on failure
 */
rsqf_filter_t *rsqf_filter_create(size_t capacity, uint8_t fp_bits);

/**
 * Destroy an RSQF filter and free all memory.
 * @param rf  Filter to destroy (NULL is safe)
 */
void rsqf_filter_destroy(rsqf_filter_t *rf);

/**
 * Reset an RSQF filter to empty state (keeps allocation).
 * @param rf  Filter to reset (must not be NULL)
 * @return    RSQF_OK on success
 */
rsqf_err_t rsqf_filter_reset(rsqf_filter_t *rf);

/* ============================================================================
 * Core operations
 *
 * All operations take a uint64_t hash, not raw data.
 * The filter splits the hash into quotient (high bits) and remainder (low bits).
 * ============================================================================ */

/**
 * Insert an item into the filter.
 * @param rf    Filter (must not be NULL)
 * @param hash  64-bit hash of the item
 * @return      RSQF_OK on success, RSQF_ERR_FULL if filter is full
 */
rsqf_err_t rsqf_filter_insert(rsqf_filter_t *rf, uint64_t hash);

/**
 * Check if an item is possibly in the filter.
 * @param rf    Filter (must not be NULL)
 * @param hash  64-bit hash of the item
 * @return      true if possibly present, false if definitely not
 */
bool rsqf_filter_lookup(const rsqf_filter_t *rf, uint64_t hash);

/**
 * Validate internal RSQF invariants.
 * Checks metadata consistency, run structure, and occupied/runend correspondence.
 * Returns true if the filter passes all checks.
 */
bool rsqf_filter_validate(const rsqf_filter_t *rf);
bool rsqf_filter_validate_debug(const rsqf_filter_t *rf);

/**
 * Delete an item from the filter.
 * @param rf    Filter (must not be NULL)
 * @param hash  64-bit hash of the item
 * @return      RSQF_OK on success, RSQF_ERR_NOT_FOUND if not present
 */
rsqf_err_t rsqf_filter_delete(rsqf_filter_t *rf, uint64_t hash);

/**
 * Repair block offsets by recomputing all from scratch.
 * Offsets are performance-only metadata (select guidance); staleness
 * does not affect correctness but slows scans. Returns number of
 * offsets changed, or UINT64_MAX on error.
 */
uint64_t rsqf_filter_repair_offsets(rsqf_filter_t *rf);

/* ============================================================================
 * Statistics and properties
 * ============================================================================ */

static inline size_t rsqf_filter_count(const rsqf_filter_t *rf) {
    return (size_t)rf->count;
}

static inline size_t rsqf_filter_capacity(const rsqf_filter_t *rf) {
    return (size_t)rf->num_slots;
}

static inline double rsqf_filter_load_factor(const rsqf_filter_t *rf) {
    return (double)rf->count / (double)rf->num_slots;
}

size_t rsqf_filter_memory_bytes(const rsqf_filter_t *rf);

static inline double rsqf_filter_bits_per_item(const rsqf_filter_t *rf) {
    if (rf->count == 0) return 0.0;
    /* ~5.125 metadata bits/slot: occ(64) + run(64) + phys_occ(64) + occ_prefix(64) + run_prefix(64) + offsets(8)
     * per block, plus 2 word-length sentinels on prefix arrays. */
    double meta_bits = (double)(rf->num_blocks * (5 * 64 + 8) + 2 * 64);
    double data_bits = (double)rf->num_slots * rf->remainder_bits;
    return (meta_bits + data_bits) / (double)rf->count;
}

double rsqf_filter_estimated_fpr(const rsqf_filter_t *rf);

/* ============================================================================
 * Hash helpers (exposed for testing)
 * ============================================================================ */

uint64_t rsqf_quotient(uint64_t hash, uint8_t quotient_bits);
uint64_t rsqf_remainder(uint64_t hash, uint8_t remainder_bits);

#ifdef __cplusplus
}
#endif

#endif /* DRAUGR_RSQF_FILTER_H */
