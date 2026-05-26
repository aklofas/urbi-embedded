#!/bin/sh
# tests/scripts/check-stdlib-fresh.sh
#
# Regenerates the stdlib bytecode at the current host flavor and diffs
# against the checked-in src/stdlib/urbi_stdlib_bytecode.gen.c.  Fails
# on drift — forces re-bake to be tracked at every stdlib .u change.
#
# Requires: tools/urbi-compile-stdlib must already be built (make first).
# Called by: make test-stdlib-bytecode-fresh (RELEASETEST_PHASE1).

set -eu

BAKED="src/stdlib/urbi_stdlib_bytecode.gen.c"
GENERATED="/tmp/urbi_stdlib_bytecode.gen.c.fresh.$$"

if [ ! -x "tools/urbi-compile-stdlib" ]; then
    echo "stdlib-fresh: tools/urbi-compile-stdlib not found — run 'make' first"
    exit 1
fi

./tools/urbi-compile-stdlib \
    src/stdlib/STDLIB_ORDER.txt \
    src/stdlib \
    "$GENERATED"

if ! diff -q "$BAKED" "$GENERATED" >/dev/null 2>&1; then
    echo "stdlib bytecode drift detected — run 'make bake-clean' to refresh"
    diff "$BAKED" "$GENERATED" | head -40
    rm -f "$GENERATED"
    exit 1
fi

rm -f "$GENERATED"
echo "stdlib bytecode is fresh"
