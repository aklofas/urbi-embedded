#!/usr/bin/env bash
# Bake-tool determinism smoke test.
#
# Run tools/urbi-compile-stdlib three times against the same input
# (src/stdlib/STDLIB_ORDER.txt + src/stdlib/*.u files) and assert that
# the three outputs are byte-identical.
#
# The bake-tool output is the contents of
# src/stdlib/urbi_stdlib_bytecode.gen.c, which gets baked into liburbi.a
# as a const byte array.  Any non-determinism here (timestamps,
# input-path strings, allocation-order-dependent constant ordering, etc.)
# would cause spurious wire-format-hash churn at every build, which would
# in turn invalidate the wire-format-hash CI gate.
#
# Wired into `make releasetest` via the test-bake-smoke target.

set -euo pipefail

TOOL=./tools/urbi-compile-stdlib
ORDER=src/stdlib/STDLIB_ORDER.txt
SRC=src/stdlib
OUT1=$(mktemp -t bake_smoke_run1.XXXXXX.c)
OUT2=$(mktemp -t bake_smoke_run2.XXXXXX.c)
OUT3=$(mktemp -t bake_smoke_run3.XXXXXX.c)

cleanup() { rm -f "$OUT1" "$OUT2" "$OUT3"; }
trap cleanup EXIT

if [ ! -x "$TOOL" ]; then
    echo "FAIL: $TOOL not built (run: make tools/urbi-compile-stdlib)" >&2
    exit 1
fi

"$TOOL" "$ORDER" "$SRC" "$OUT1" >/dev/null 2>&1
"$TOOL" "$ORDER" "$SRC" "$OUT2" >/dev/null 2>&1
"$TOOL" "$ORDER" "$SRC" "$OUT3" >/dev/null 2>&1

if ! cmp -s "$OUT1" "$OUT2"; then
    echo "FAIL: bake-tool output differs between run 1 and run 2" >&2
    diff -u "$OUT1" "$OUT2" >&2 || true
    exit 1
fi
if ! cmp -s "$OUT2" "$OUT3"; then
    echo "FAIL: bake-tool output differs between run 2 and run 3" >&2
    diff -u "$OUT2" "$OUT3" >&2 || true
    exit 1
fi

echo "PASS: bake tool deterministic across 3 runs"
