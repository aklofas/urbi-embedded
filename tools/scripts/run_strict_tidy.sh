#!/usr/bin/env bash
# Run clang-tidy with the strict checklist over src/.
# Exit non-zero if any violation is reported.
set -euo pipefail

CLANG_TIDY="${CLANG_TIDY:-clang-tidy-18}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Use .clang-tidy.strict if present (parallel-config mode), else .clang-tidy.
CONFIG_FILE=".clang-tidy"
if [[ -f .clang-tidy.strict ]]; then
    CONFIG_FILE=".clang-tidy.strict"
fi

OUT="${1:-build/strict-tidy-out.txt}"
mkdir -p "$(dirname "$OUT")"

# shellcheck disable=SC2046
"$CLANG_TIDY" --config-file="$CONFIG_FILE" \
   --warnings-as-errors='*' \
   --quiet \
   $(find src -name '*.c' | sort) \
   -- -Iinclude -Isrc -std=c99 \
   2>&1 | tee "$OUT"

WARN_COUNT=$(grep -c 'warning:\|error:' "$OUT" || true)
if [[ "$WARN_COUNT" -gt 0 ]]; then
    echo "FAIL: $WARN_COUNT strict-tidy violations" >&2
    exit 1
fi
echo "OK: 0 strict-tidy violations"
