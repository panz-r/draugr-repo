#!/bin/sh
# run_cbmc.sh — Run all CBMC formal verification models
# Usage: ./tests/run_cbmc.sh
# Requires: cbmc (https://www.cbmc.org/)

set -e
DIR="$(cd "$(dirname "$0")/.." && pwd)"
echo "=== CBMC verification suite ==="

echo ""
echo "--- Core model (2×2 cuckoo, no stash) ---"
cbmc -D__CBMC__ --unwind 4 --bounds-check --pointer-check \
     "$DIR/tests/cbmc_cuckoo_model.c" --function main
echo "PASS"

echo ""
echo "--- Extended model (stash, location, remap, flags, grow, failure atomicity, boundedness) ---"
cbmc -D__CBMC__ --unwind 10 --bounds-check --pointer-check \
     "$DIR/tests/cbmc_htc_extended.c" --function main
echo "PASS"

echo ""
echo "--- Concurrent model (two-thread interleaving: insert/find, remove/find, insert/insert) ---"
cbmc -D__CBMC__ --unwind 14 --bounds-check --pointer-check \
     "$DIR/tests/cbmc_htc_concurrent.c" --function main
echo "PASS"

echo ""
echo "--- Mutation test: skip stash (should find counterexample) ---"
cbmc -D__CBMC__ -DMUTATION_SKIP_STASH --unwind 6 --bounds-check \
     "$DIR/tests/cbmc_htc_extended.c" --function main 2>&1 && {
    echo "FAIL: mutation_skip_stash should have produced a counterexample"
    exit 1
} || echo "PASS (counterexample found as expected)"

echo ""
echo "--- Mutation test: allow duplicate (should find counterexample) ---"
cbmc -D__CBMC__ -DMUTATION_ALLOW_DUPLICATE --unwind 6 --bounds-check \
     "$DIR/tests/cbmc_htc_extended.c" --function main 2>&1 && {
    echo "FAIL: mutation_allow_duplicate should have produced a counterexample"
    exit 1
} || echo "PASS (counterexample found as expected)"

echo ""
echo "--- Mutation test: forget remap_inc (should find counterexample) ---"
cbmc -D__CBMC__ -DMUTATION_FORGET_REMAP_INC --unwind 6 --bounds-check \
     "$DIR/tests/cbmc_htc_extended.c" --function main 2>&1 && {
    echo "FAIL: mutation_forget_remap should have produced a counterexample"
    exit 1
} || echo "PASS (counterexample found as expected)"

echo ""
echo "=== All CBMC models verified successfully ==="
echo ""
echo "Coverage scope — CBMC verified:"
echo "  M1: sequential cuckoo core (2×2 slots, insert/find/remove/duplicate)"
echo "  M2: stash (max 1) verified against duplicate + reachability invariants"
echo "  M3: in_secondary/location awareness (every slot tagged with role)"
echo "  M4: remap_count skip (fast_find == full_scan_find)"
echo "  M5: delete flags (flags=DELETED as linearization point)"
echo "  M6: grow/reseed as abstract-map-preserving transformation"
echo "  M7: failure atomicity (failed ops leave abstract state unchanged)"
echo "  M8: boundedness (find scans ≤ NUM_BUCKETS buckets + ≤ STASH_MAX stash slots)"
echo "  C1: two-thread concurrency (insert||find, remove||find, insert||insert)"
echo ""
echo "Not yet CBMC verified:"
echo "  - Epochs/reclamation (dynamic retire list)"
echo "  - Front cache (thread-local ABA-safe cache)"
echo "  - Complex concurrency (>2 threads, grow+find race)"
echo "  - Failure atomicity (OOM in middle of BFS/migration)"
echo "  - Integration: real htc.c linked (cbmc_htc_harness.c exists but not in CI)"
