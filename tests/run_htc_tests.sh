#!/bin/sh
# run_htc_tests.sh — run all htc tests with required ARM workarounds
#
# On ARM systems, AddressSanitizer's leak detection (detect_leaks=1) can
# hang during initialization. Workaround: disable leak detection.
#
# Usage:
#   ./tests/run_htc_tests.sh              # run all test suites
#   ./tests/run_htc_tests.sh test_htc     # run specific suite
#
# No arguments: runs all suites in tests/build/tests.

set -e
export ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}detect_leaks=0"
BUILD_DIR="$(dirname "$0")/../build/tests"

if [ $# -gt 0 ]; then
    exec "$BUILD_DIR/$1"
fi

cd "$BUILD_DIR"

for t in test_htc test_migration test_edge_cases_a test_edge_cases_b \
         test_public_api test_stress test_ht test_ht_cache test_ht_arena \
         test_internal test_grow_shrink test_arena test_arena_concurrent \
         test_bugs test_oom; do
    echo "=== $t ==="
    ./$t
    echo
done
echo "All test suites completed."
