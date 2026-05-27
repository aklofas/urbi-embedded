#!/usr/bin/env bash
# check-public-doc-scrub.sh — fail if tracked files mention workspace-private
# paths or tool-context filenames that should not appear in the public repo.
#
# Allowed exceptions: lines tagged with `<!-- scrub-allow: <reason> -->`
# or `# scrub-allow: <reason>` (script context).  See WORKFLOW.md §1.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

# Patterns that must not appear in tracked files
PATTERNS=(
    '/home/aklofas/'
    'docs/superpowers/'
    'REVIVAL\.md'
    'CLAUDE\.md'
    'AGENTS\.md'
    'GEMINI\.md'
    '\.cursor/'
    'Co-Authored-By: Claude'
    'Generated with Claude'
)

# Files this script runs over (tracked only; skip self and .gitignore)
FILES=$(git ls-files | grep -v -E '^(\.gitignore|tests/scripts/check-public-doc-scrub\.sh)$')

violations=0
for pattern in "${PATTERNS[@]}"; do
    while IFS= read -r match; do
        # Skip lines with scrub-allow marker (any comment style)
        if echo "$match" | grep -qE 'scrub-allow'; then
            continue
        fi
        echo "[scrub] $match"
        violations=$((violations + 1))
    done < <(echo "$FILES" | xargs -d '\n' grep -nH -E "$pattern" 2>/dev/null || true)
done

if [ "$violations" -gt 0 ]; then
    echo "FAIL: $violations public-doc scrub violation(s).  Tag intentional"
    echo "occurrences with 'scrub-allow: <reason>'.  See WORKFLOW.md §1."
    exit 1
fi

echo "OK: public-doc scrub clean."
