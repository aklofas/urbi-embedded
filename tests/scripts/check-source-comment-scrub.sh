#!/usr/bin/env bash
# check-source-comment-scrub.sh — fail if C/H source files contain internal
# process-ID tokens (M<n>, T<n>, W<n>, FOUND-<n>, refactor-<n>) in comments.
#
# These identifiers are meaningful only in the private planning context and
# must not appear in the public source tree.  Strip the ID token; keep the
# rationale.  See docs/STYLE.md §"Comment quality standard".
#
# Scope: src/ include/ tools/ (tracked files only).
# Excluded: include/urbi/version.h (ABI history ledger — legitimately retains
#           milestone and task IDs alongside ABI-bump rationale).
# Excluded: tests/ (chk fixtures may legitimately cite legacy IDs).
#
# Inline escape: if a line contains `scrub-allow: <reason>` it is exempt.
# Example: /* W3-v0.10.4 — preserved ordering exemplar; scrub-allow: ABI ledger */
#
# Hardware names (Cortex-M0/M4/M7, ...) are not process IDs; occurrences of
# M<n> that are part of an ARM core name are ignored.

set -euo pipefail

cd "$(git rev-parse --show-toplevel)"

PATTERN='\b(M[0-9]+|T[0-9]+|W[0-9]+|FOUND-[0-9]+|refactor-[0-9]+)\b'

# Tracked files under src/ include/ tools/, excluding version.h
FILES=$(git ls-files src/ include/ tools/ \
    | grep -v '^include/urbi/version\.h$')

violations=0
while IFS= read -r file; do
    while IFS= read -r match; do
        # Skip lines with scrub-allow marker
        if echo "$match" | grep -qE 'scrub-allow'; then
            continue
        fi
        # ARM core names (Cortex-M7 etc.) are hardware, not process IDs:
        # re-test the line with them removed.
        stripped=$(echo "$match" | sed -E 's/(Cortex|cortex)-M[0-9]+//g')
        if ! echo "$stripped" | grep -qE "$PATTERN"; then
            continue
        fi
        echo "[src-comment-scrub] $match"
        violations=$((violations + 1))
    done < <(grep -nHE "$PATTERN" "$file" 2>/dev/null || true)
done <<< "$FILES"

if [ "$violations" -gt 0 ]; then
    echo ""
    echo "FAIL: $violations source-comment ID token violation(s)."
    echo "Strip the ID token, keep the rationale.  See docs/STYLE.md."
    echo "Tag intentional occurrences with 'scrub-allow: <reason>'."
    exit 1
fi

echo "OK: source-comment scrub clean."
