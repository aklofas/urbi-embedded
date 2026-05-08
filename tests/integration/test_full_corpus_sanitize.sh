#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# T126: Full-corpus sanitizer gate (Wave 5 spec §3.9 verification G4).
#
# Runs every tests/chk/**/*.chk fixture under three sanitizer regimes:
#   1. ASan  — heap/stack overflow + use-after-free
#   2. UBSan — undefined behavior (signed overflow, alignment, etc.)
#   3. valgrind memcheck (full leak-check) — uninitialized reads + leaks
#
# Solo-runs each regime to avoid bandwidth contention (per
# project_releasetest_perf.md: valgrind throughput collapses 10-20x
# under concurrent gcov / clang-tidy / cppcheck / fanalyzer).
#
# Skips the curated host-binary chk gate (already covered by `make test-chk`).
#
# Promotes from Wave-5's curated-subset sanitizer coverage to a standing
# all-fixtures gate.  Wave-5 hypothesis: prior phases closed the latent
# bugs so 148 × 3 = 444 runs all clean.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ASAN_URBI="build/host-asan/urbi"
UBSAN_URBI="build/host-ubsan/urbi"
HOST_URBI="build/host/urbi"
RUNNER="tests/integration/run_chk.sh"

# Sanity-check prerequisites
for bin in "$ASAN_URBI" "$UBSAN_URBI" "$HOST_URBI"; do
    if [[ ! -x "$bin" ]]; then
        echo "error: $bin not found; run 'make test-asan test-ubsan urbi-bin' first" >&2
        exit 2
    fi
done

mapfile -t fixtures < <(find tests/chk -type f -name '*.chk' | sort)
echo "Discovered ${#fixtures[@]} fixtures."

failed=0

for chk in "${fixtures[@]}"; do
    # ASan
    if ! "$RUNNER" "$ASAN_URBI" "$chk" >/dev/null 2>&1; then
        echo "ASan FAIL: $chk"
        failed=$((failed + 1))
    fi
    # UBSan
    if ! "$RUNNER" "$UBSAN_URBI" "$chk" >/dev/null 2>&1; then
        echo "UBSan FAIL: $chk"
        failed=$((failed + 1))
    fi
    # valgrind memcheck (full leak-check)
    if ! valgrind --error-exitcode=1 --leak-check=full --quiet \
            "$RUNNER" "$HOST_URBI" "$chk" >/dev/null 2>&1; then
        echo "valgrind FAIL: $chk"
        failed=$((failed + 1))
    fi
done

if [[ "$failed" -gt 0 ]]; then
    echo "FAIL: $failed corpus-sanitize failures across ${#fixtures[@]} fixtures × 3 sanitizers"
    exit 1
fi
echo "OK: ${#fixtures[@]} fixtures × 3 sanitizers = $((${#fixtures[@]} * 3)) runs all clean"
