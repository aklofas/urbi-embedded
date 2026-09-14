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
# Scope: the same directories `make test-chk` gates (CHK_GATE_DIRS), plus
# the same per-fixture exclusions.  Fixtures whose subsystem has not been
# re-founded yet fail for reasons that have nothing to do with memory
# safety, so sanitizing them would report noise; each directory joins
# this gate at the same moment it joins test-chk.

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

CHK_GATE_DIRS="${CHK_GATE_DIRS:-arithmetic closure function control}"
EXCLUSIONS=tests/chk/bringup-exclusions.txt

excluded() {
    [[ -f "$EXCLUSIONS" ]] || return 1
    sed 's/#.*//' "$EXCLUSIONS" | tr -d ' \t' | grep -qx "$1"
}

fixtures=()
for d in $CHK_GATE_DIRS; do
    while IFS= read -r f; do
        excluded "$f" || fixtures+=("$f")
    done < <(find "tests/chk/$d" -type f -name '*.chk' | sort)
done
echo "Discovered ${#fixtures[@]} fixtures in the gated directories ($CHK_GATE_DIRS)."

if [[ "${#fixtures[@]}" -eq 0 ]]; then
    echo "FAIL: zero fixtures discovered — corpus missing or find pattern broken" >&2
    exit 1
fi

failed=0
ran=0
skipped=0
placeholders=0

run_one() { # <label> <binary> <fixture>
    local label="$1" bin="$2" chk="$3" out rc
    out=$("$RUNNER" "$bin" "$chk" 2>&1); rc=$?
    case "$rc" in
        0) ran=$((ran + 1)) ;;
        3) skipped=$((skipped + 1)) ;;
        4) placeholders=$((placeholders + 1)) ;;
        *) echo "$label FAIL (rc=$rc): $chk"
           echo "$out" | sed 's/^/    /'
           failed=$((failed + 1)) ;;
    esac
}

for chk in "${fixtures[@]}"; do
    run_one "ASan"  "$ASAN_URBI"  "$chk"
    run_one "UBSan" "$UBSAN_URBI" "$chk"
done

echo "corpus-sanitize summary: $ran sanitized runs, $skipped SKIPped (preset-gated" \
     "— these fixtures get ZERO sanitizer coverage from this gate; refactor-3 CHK-04)," \
     "$placeholders placeholder runs, $failed failures"
if [[ "$failed" -gt 0 ]]; then
    echo "FAIL: $failed corpus-sanitize failures across ${#fixtures[@]} fixtures × 2 sanitizers"
    exit 1
fi
if [[ "$ran" -eq 0 ]]; then
    echo "FAIL: zero sanitized runs actually executed (all SKIP/placeholder) — gate provides no coverage" >&2
    exit 1
fi
echo "OK: ${#fixtures[@]} fixtures × 2 sanitizers — $ran runs clean ($skipped skipped, $placeholders placeholders)"
