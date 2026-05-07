#!/bin/sh
# tests/scripts/check_loc_cap.sh — fail if any src/**/*.c exceeds the cap and is not on the exception list.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CAP="${URBI_LOC_CAP:-1000}"
EXCEPTIONS_FILE="$ROOT/CONTRIBUTING.md"
fails=0
while IFS= read -r f; do
    loc=$(wc -l < "$f")
    if [ "$loc" -gt "$CAP" ]; then
        rel="${f#$ROOT/}"
        if grep -qF "loc-cap-exception:$rel" "$EXCEPTIONS_FILE" 2>/dev/null; then
            printf '  EXEMPT %4d  %s\n' "$loc" "$rel"
        else
            printf '  FAIL   %4d  %s\n' "$loc" "$rel"
            fails=$((fails + 1))
        fi
    fi
done <<EOF
$(find "$ROOT/src" -name '*.c' -type f | LC_ALL=C sort)
EOF
if [ "$fails" -gt 0 ]; then
    echo "test-loc-cap: $fails files exceed the $CAP-LOC cap without a CONTRIBUTING.md exception" >&2
    exit 1
fi
echo "test-loc-cap: OK"
