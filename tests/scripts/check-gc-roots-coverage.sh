#!/bin/sh
# T34 / v0.7.0 Wave 1: GC roots-coverage gate.
#
# Asserts every UVAL_* enum value declared in include/urbi/types.h is
# mentioned at least once across src/gc/*.{c,h}.  Closes the bug class
# that surfaced as the M4-era UVAL_OBJECT / UVAL_EVENT shading gap
# (fixed inline at v0.6.2 Phase 6) — a new UVAL_* kind landing without
# any GC-source consideration would have stayed latent until the first
# GC slice that touched a root of that kind.
#
# Implementation note: the heap-bearing check at mark time lives in
# src/gc/ugc_incremental.h's uvalue_is_heap() as an inline OR-chain
# (UVAL_CLOSURE || UVAL_OBJECT || UVAL_EVENT at v0.7.0).  Future
# heap-bearing UVAL_* additions MUST extend uvalue_is_heap; explicit
# non-heap UVAL_* additions should still appear in src/gc/ comments
# documenting why they need no shade arm.  Either pattern satisfies
# this gate.
set -eu

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
TYPES_H="$ROOT/include/urbi/types.h"
GC_DIR="$ROOT/src/gc"

# Extract UVAL_* enum member names from include/urbi/types.h.
# refactor-3 GATE-02: match ALL identifiers in the enum body — including
# unvalued members (`UVAL_FOO,` / trailing `UVAL_FOO`) which the previous
# `= N`-only regex could not see.
ENUM_VALUES=$(grep -E '^[[:space:]]+UVAL_[A-Z0-9_]+[[:space:]]*(=|,|/|$)' "$TYPES_H" \
              | sed -E 's/^[[:space:]]+(UVAL_[A-Z0-9_]+).*/\1/' \
              | LC_ALL=C sort -u)

if [ -z "$ENUM_VALUES" ]; then
    echo "FAIL: no UVAL_* enum members found in $TYPES_H" >&2
    exit 1
fi

# refactor-3 GATE-02: a token satisfies the gate ONLY if it is
#   (a) heap-bearing: appears inside uvalue_is_heap()'s function body in
#       src/gc/ugc_incremental.h, or
#   (b) explicitly waived: appears next to a structured `gc-no-shade:`
#       marker somewhere under src/gc/.
# A free-text comment mention no longer passes.
HEAP_TOKENS=$(awk '/^uvalue_is_heap\(/ {infn=1} infn {print} infn && /^}/ {infn=0}' \
              "$GC_DIR/ugc_incremental.h" \
              | grep -oE 'UVAL_[A-Z0-9_]+' | LC_ALL=C sort -u)
NOSHADE_TOKENS=$(grep -hoE 'gc-no-shade:[[:space:]]*UVAL_[A-Z0-9_]+' \
                 "$GC_DIR"/*.c "$GC_DIR"/*.h 2>/dev/null \
                 | grep -oE 'UVAL_[A-Z0-9_]+' | LC_ALL=C sort -u)

MISSING=""
for val in $ENUM_VALUES; do
    if ! echo "$HEAP_TOKENS" | grep -qx "$val" \
       && ! echo "$NOSHADE_TOKENS" | grep -qx "$val"; then
        MISSING="$MISSING $val"
    fi
done

if [ -n "$MISSING" ]; then
    echo "FAIL: src/gc/ has no reference to:$MISSING" >&2
    echo "" >&2
    echo "Each UVAL_* kind declared in include/urbi/types.h must either" >&2
    echo "appear inside uvalue_is_heap()'s body (src/gc/ugc_incremental.h," >&2
    echo "heap-bearing kinds) or carry a structured 'gc-no-shade: UVAL_X'" >&2
    echo "marker under src/gc/ (explicit non-heap waiver).  Free-text" >&2
    echo "mentions no longer satisfy this gate (refactor-3 GATE-02)." >&2
    exit 1
fi

count=$(echo "$ENUM_VALUES" | wc -l)
echo "PASS: GC roots coverage — all $count UVAL_* kinds are heap-bearing or gc-no-shade-waived"
exit 0
