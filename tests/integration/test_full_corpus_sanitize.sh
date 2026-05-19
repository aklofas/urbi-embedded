#!/usr/bin/env bash
# SPDX-License-Identifier: BSD-3-Clause
# T126: Full-corpus sanitizer gate (Wave 5 spec §3.9 verification G4).
#
# Runs every tests/chk/**/*.chk fixture under two sanitizer regimes:
#   1. ASan  — heap/stack overflow + use-after-free
#   2. UBSan — undefined behavior (signed overflow, alignment, etc.)
#
# valgrind memcheck is INTENTIONALLY OMITTED from the .chk corpus per
# the project's "Not valgrind-wrapped" rationale (Makefile:88-90):
#   "urbi itself is memory-clean, and wrapping the sh+awk+sed pipeline
#    adds noise, not signal."
# The pipeline-wrapper-bash itself leaks ~520 bytes via yyparse on every
# fixture, drowning any real urbi-side leak signal.  Unit-test-binary
# valgrind coverage is provided by `make test-valgrind` (releasetest
# Phase 2 alongside this target).
#
# Solo-runs each regime to avoid bandwidth contention (per
# project_releasetest_perf.md: sanitizer throughput collapses under
# concurrent gcov / clang-tidy / cppcheck / fanalyzer).
#
# Promotes from Wave-5's curated-subset sanitizer coverage to a standing
# all-fixtures gate.  Wave-5 hypothesis: prior phases closed the latent
# bugs so 148 × 2 = 296 runs all clean.

set -uo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ASAN_URBI="build/host-asan/urbi"
UBSAN_URBI="build/host-ubsan/urbi"
RUNNER="tests/integration/run_chk.sh"

# Sanity-check prerequisites
for bin in "$ASAN_URBI" "$UBSAN_URBI"; do
    if [[ ! -x "$bin" ]]; then
        echo "error: $bin not found; run 'make test-asan test-ubsan' first" >&2
        exit 2
    fi
done

# tests/chk/repl/*.chk are NDJSON fixtures (v0.9.1 Phase 8) driven in-process
# by tests/unit/test_repl_chk_corpus.c, not by run_chk.sh which expects
# urbiscript-REPL input.  The in-process driver is itself built with -fsanitize
# in the host-asan / host-ubsan variants of `make test`, so the REPL corpus is
# already covered by both sanitizers there.  Exclude here to match `test-chk`.
mapfile -t fixtures < <(find tests/chk -path tests/chk/repl -prune -o \
                             -type f -name '*.chk' -print | sort)
echo "Discovered ${#fixtures[@]} fixtures (tests/chk/repl excluded)."

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
done

if [[ "$failed" -gt 0 ]]; then
    echo "FAIL: $failed corpus-sanitize failures across ${#fixtures[@]} fixtures × 2 sanitizers"
    exit 1
fi
echo "OK: ${#fixtures[@]} fixtures × 2 sanitizers = $((${#fixtures[@]} * 2)) runs all clean"
