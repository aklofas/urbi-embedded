#!/usr/bin/env bash
set -euo pipefail
CPPCHECK="${CPPCHECK:-cppcheck}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# GATE-01: a missing tool must fail loudly, not "OK: 0 violations".
command -v "$CPPCHECK" >/dev/null 2>&1 || {
    echo "FAIL: $CPPCHECK not found in PATH — the strict-cppcheck gate cannot run." >&2
    echo "      install: sudo apt-get install -y cppcheck  (or set CPPCHECK=)" >&2
    exit 1
}

OUT="${1:-build/cppcheck-out.txt}"
shift || true
# The sources to scan come from the Makefile ($(SRC)) so the gate
# never drifts from the build.  The old runtime core is parked in the
# tree but not compiled, and scanning it against the new public
# header produces only noise.
SCAN=("$@")
[ "${#SCAN[@]}" -gt 0 ] || SCAN=(src/)
mkdir -p "$(dirname "$OUT")"

# Run cppcheck without --error-exitcode so we observe the full output
# (cppcheck would exit-2 on first finding under set -e). We grep the
# tee'd output for the diagnostic categories and exit ourselves.
set +e
"$CPPCHECK" \
   --enable=all \
   --inconclusive \
   --suppressions-list=.cppcheck.suppressions \
   --suppress=missingIncludeSystem \
   --inline-suppr \
   --quiet \
   -Iinclude -Isrc \
   "${SCAN[@]}" 2>&1 | tee "$OUT"
TOOL_RC=${PIPESTATUS[0]}
set -e

# cppcheck format: <file>:<line>:<col>: <category>: <text> [<id>]
ERR_COUNT=$(grep -cE '^[^ ].*: (error|warning|style|performance|portability):' "$OUT" || true)
if [ "$TOOL_RC" -ne 0 ] && [ "$ERR_COUNT" -eq 0 ]; then
    echo "run_cppcheck: tool exited $TOOL_RC with no findings (crash?)" >&2
    exit "$TOOL_RC"
fi
if [ "$ERR_COUNT" -gt 0 ]; then
    echo "FAIL: $ERR_COUNT cppcheck violations" >&2
    exit 1
fi
echo "OK: 0 cppcheck violations"
