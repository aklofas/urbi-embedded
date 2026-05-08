#!/bin/sh
# SPDX-License-Identifier: BSD-3-Clause
# tests/scripts/capture_wire_format_hashes.sh
# Capture sha256 of on-disk wire-format bytes for every tests/chk/**/*.chk
# fixture. Complements capture_bytecode_hashes.sh (which hashes the
# disassembled mnemonic text — stable across opcode renumber + version-byte
# advance, but blind to genuine wire-format breaks). Use this gate to detect
# wire-format changes (header byte, opcode-shape table, nested[]/ic_names
# round-trip, varint encoding) that the disasm-text gate cannot observe.
#
# Filed as a Wave-4 deferral in v0.5.6-bytecode (REVIVAL §14 row
# S-bytecode-v1.5 caveat); landed at v0.5.7-fixes Phase 22 T131.
#
# Usage:
#   ./tests/scripts/capture_wire_format_hashes.sh [output-file]
#
# The binary must be built first (make).
# Each fixture's code lines are extracted (comment lines, blank lines, and
# expected-output [frame] lines are stripped), joined on semicolons, and fed
# to 'urbi --dump-wire-format'. Fixtures that intentionally test error paths
# cannot be compiled to bytecode; they are recorded as COMPILE_ERROR and
# remain stable across pure refactor waves.
set -eu
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
URBI="${URBI_BIN:-$ROOT/build/host/urbi}"
OUT="${1:-$ROOT/tests/golden/wire-format-hashes.txt}"
[ -x "$URBI" ] || { echo "missing $URBI; run 'make' first" >&2; exit 1; }
: > "$OUT"
find "$ROOT/tests/chk" -name '*.chk' -type f | LC_ALL=C sort | while IFS= read -r chk; do
    rel="${chk#$ROOT/}"
    tmp=$(mktemp /tmp/urbi_chk_XXXXXX.u)
    # Extract code lines: skip comment lines, blank lines, and [frame] output lines.
    awk '/^\[/ || /^[[:space:]]*#/ || /^[[:space:]]*$/ { next } { print }' \
        "$chk" | tr '\n' ';' | sed 's/;;*/;/g;s/;$//' > "$tmp"
    # Wire-format output contains NULs; shell command substitution truncates
    # at the first NUL.  Pipe directly to sha256sum to preserve all bytes.
    wire_bin=$(mktemp /tmp/urbi_wire_XXXXXX.bin)
    if "$URBI" --dump-wire-format "$tmp" > "$wire_bin" 2>/dev/null && \
       [ -s "$wire_bin" ]; then
        h=$(sha256sum "$wire_bin" | awk '{print $1}')
    else
        h="COMPILE_ERROR"
    fi
    rm -f "$wire_bin"
    printf '%s  %s\n' "$h" "$rel" >> "$OUT"
    rm -f "$tmp"
done
wc -l "$OUT"
