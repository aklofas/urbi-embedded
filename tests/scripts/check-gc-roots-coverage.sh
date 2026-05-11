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
ENUM_VALUES=$(grep -E "^[[:space:]]+UVAL_[A-Z_]+[[:space:]]*=" "$TYPES_H" \
              | sed -E 's/.*(UVAL_[A-Z_]+).*/\1/' \
              | LC_ALL=C sort -u)

if [ -z "$ENUM_VALUES" ]; then
    echo "FAIL: no UVAL_* enum members found in $TYPES_H" >&2
    exit 1
fi

# Collect every UVAL_* token referenced anywhere in src/gc/*.{c,h}.
GC_TOKENS=$(grep -hoE "UVAL_[A-Z_]+" "$GC_DIR"/*.c "$GC_DIR"/*.h 2>/dev/null \
            | LC_ALL=C sort -u)

MISSING=""
for val in $ENUM_VALUES; do
    if ! echo "$GC_TOKENS" | grep -qx "$val"; then
        MISSING="$MISSING $val"
    fi
done

if [ -n "$MISSING" ]; then
    echo "FAIL: src/gc/ has no reference to:$MISSING" >&2
    echo "" >&2
    echo "Each UVAL_* kind declared in include/urbi/types.h must appear" >&2
    echo "at least once under src/gc/ — either in uvalue_is_heap()'s" >&2
    echo "heap-bearing list (src/gc/ugc_incremental.h) or in a comment" >&2
    echo "explaining why no shade arm is needed.  The M4-era" >&2
    echo "UVAL_OBJECT / UVAL_EVENT shading gap closed at v0.6.2" >&2
    echo "Phase 6 was exactly this bug class — a heap-bearing kind" >&2
    echo "landed without updating uvalue_is_heap." >&2
    exit 1
fi

count=$(echo "$ENUM_VALUES" | wc -l)
echo "PASS: GC roots coverage — all $count UVAL_* kinds referenced under src/gc/"
exit 0
