#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tests/scripts/capture_bytecode_hashes.sh
# Capture sha256 of compiled bytecode disassembly for every tests/chk/**/*.chk
# fixture.  Used as the byte-identical-codegen oracle for the v0.5.4-decompose
# wave.
#
# Usage:
#   ./tests/scripts/capture_bytecode_hashes.sh [output-file]
#
# The binary must be built first (make).
# Each fixture's code lines are extracted (comment lines, blank lines, and
# expected-output [frame] lines are stripped), joined on semicolons, and fed
# to 'urbi --dump-bytecode'.  Fixtures that intentionally test error paths
# cannot be compiled to bytecode; they are recorded as COMPILE_ERROR and
# remain stable across pure refactor waves.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
URBI="${URBI_BIN:-$ROOT/build/host/urbi}"
OUT="${1:-$ROOT/tests/golden/bytecode-hashes.txt}"
[ -x "$URBI" ] || { echo "missing $URBI; run 'make' first" >&2; exit 1; }
: > "$OUT"
find "$ROOT/tests/chk" -name '*.chk' -type f | LC_ALL=C sort | while IFS= read -r chk; do
    rel="${chk#$ROOT/}"
    tmp=$(mktemp /tmp/urbi_chk_XXXXXX.u)
    # Extract code lines: skip comment lines, blank lines, and [frame] output lines.
    awk '/^\[/ || /^[[:space:]]*#/ || /^[[:space:]]*$/ { next } { print }' \
        "$chk" | tr '\n' ';' | sed 's/;;*/;/g;s/;$//' > "$tmp"
    disasm=$("$URBI" --dump-bytecode "$tmp" 2>/dev/null) && bc_ok=1 || bc_ok=0
    if [ "$bc_ok" -eq 1 ] && [ -n "$disasm" ]; then
        h=$(printf '%s' "$disasm" | sha256sum | awk '{print $1}')
    else
        h="COMPILE_ERROR"
    fi
    printf '%s  %s\n' "$h" "$rel" >> "$OUT"
    rm -f "$tmp"
done
wc -l "$OUT"
