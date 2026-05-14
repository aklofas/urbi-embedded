#!/usr/bin/env bash
# check_aux_symbols.sh — Assert that liburbi.a (core) contains NO urbi_aux_*
# symbols.  Aux symbols belong in liburbi_aux.a (separate archive).
#
# This is the inverse of the Wave-1 freestanding gate (test-freestanding.sh),
# which verifies that the freestanding BYTECODE_ONLY subset contains no hosted
# symbols.  This gate verifies that the core library contains no aux-layer
# symbols — enforcing the aux governance rule from CONTRIBUTING.md.
#
# Usage:
#   scripts/check_aux_symbols.sh [ARCHIVE]
#
# ARCHIVE defaults to build/host/liburbi.a.
#
# Exit codes:
#   0 — no aux symbols found in ARCHIVE (PASS)
#   1 — aux symbols detected (FAIL)

set -euo pipefail

ARCHIVE="${1:-build/host/liburbi.a}"

if [ ! -f "$ARCHIVE" ]; then
    echo "FAIL: $ARCHIVE not found (run: make first)" >&2
    exit 1
fi

# Collect any T (text / defined global) symbols named urbi_aux_*.
# nm output: <value> <type> <name>  (one symbol per line)
# We filter for lines where the symbol type is 'T' (global) or 't' (local)
# and the name starts with 'urbi_aux_'.
if nm "$ARCHIVE" 2>/dev/null | grep -E ' [Tt] urbi_aux_' > /tmp/aux_leak.txt 2>/dev/null; then
    echo "FAIL: aux symbols leaked into core $ARCHIVE:" >&2
    cat /tmp/aux_leak.txt >&2
    rm -f /tmp/aux_leak.txt
    exit 1
fi
rm -f /tmp/aux_leak.txt
echo "PASS: no aux symbols in core $ARCHIVE"
