#!/usr/bin/env bash
set -euo pipefail
CPPCHECK="${CPPCHECK:-cppcheck}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

OUT="${1:-build/cppcheck-out.txt}"
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
   --quiet \
   -Iinclude -Isrc \
   src/ 2>&1 | tee "$OUT"
set -e

# cppcheck format: <file>:<line>:<col>: <category>: <text> [<id>]
ERR_COUNT=$(grep -cE '^[^ ].*: (error|warning|style|performance|portability):' "$OUT" || true)
if [[ "$ERR_COUNT" -gt 0 ]]; then
    echo "FAIL: $ERR_COUNT cppcheck violations" >&2
    exit 1
fi
echo "OK: 0 cppcheck violations"
