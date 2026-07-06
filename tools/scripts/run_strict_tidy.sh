#!/usr/bin/env bash
# Run clang-tidy with the strict checklist over src/.
# Exit non-zero if any violation is reported.
set -euo pipefail

CLANG_TIDY="${CLANG_TIDY:-clang-tidy-18}"
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# GATE-01: a missing tool must fail loudly, not "OK: 0 violations"
# (command-not-found output matches neither warning token).
command -v "$CLANG_TIDY" >/dev/null 2>&1 || {
    echo "FAIL: $CLANG_TIDY not found in PATH — the strict-tidy gate cannot run." >&2
    echo "      install: sudo apt-get install -y clang-tidy-18  (or set CLANG_TIDY=)" >&2
    exit 1
}

# Use .clang-tidy.strict if present (parallel-config mode), else .clang-tidy.
CONFIG_FILE=".clang-tidy"
if [[ -f .clang-tidy.strict ]]; then
    CONFIG_FILE=".clang-tidy.strict"
fi

OUT="${1:-build/strict-tidy-out.txt}"
mkdir -p "$(dirname "$OUT")"

# Run clang-tidy without letting `--warnings-as-errors` propagate via
# pipefail+set -e (which would terminate the script before our own
# count-and-summary block runs). Mirror the pattern from
# run_cppcheck.sh: drop set -e around the pipe, then count ourselves.
# shellcheck disable=SC2046
set +e
"$CLANG_TIDY" --config-file="$CONFIG_FILE" \
   --warnings-as-errors='*' \
   --quiet \
   $(find src -name '*.c' | sort) \
   -- -Iinclude -Isrc -std=c99 \
   2>&1 | tee "$OUT"
TOOL_RC=${PIPESTATUS[0]}
set -e

WARN_COUNT=$(grep -c 'warning:\|error:' "$OUT" || true)
if [ "$TOOL_RC" -ne 0 ] && [ "$WARN_COUNT" -eq 0 ]; then
    echo "run_strict_tidy: tool exited $TOOL_RC with no findings (crash?)" >&2
    exit "$TOOL_RC"
fi
if [[ "$WARN_COUNT" -gt 0 ]]; then
    echo "FAIL: $WARN_COUNT strict-tidy violations" >&2
    exit 1
fi
echo "OK: 0 strict-tidy violations"
