#!/bin/sh
# tests/scripts/check-stdlib-fresh.sh
#
# Re-bakes src/stdlib/stdlib.u and diffs the result against the tracked
# src/stdlib/urbi_stdlib_bytecode.gen.c.  Fails on drift, so an overlay
# edit that was never baked cannot ship as stale bytecode.
#
# Requires tools/urbi-compile-stdlib to be built already (`make` first).
# Called by: make test-stdlib-bytecode-fresh.

set -eu
cd "$(dirname "$0")/../.."

BAKED="src/stdlib/urbi_stdlib_bytecode.gen.c"
GENERATED=$(mktemp -t urbi_stdlib_fresh.XXXXXX.c)
trap 'rm -f "$GENERATED"' EXIT

if [ ! -x "tools/urbi-compile-stdlib" ]; then
    echo "stdlib-fresh: tools/urbi-compile-stdlib not found — run 'make' first"
    exit 1
fi

./tools/urbi-compile-stdlib src/stdlib/stdlib.u "$GENERATED" >/dev/null 2>&1

if ! cmp -s "$BAKED" "$GENERATED"; then
    echo "stdlib bytecode drift — re-bake with:"
    echo "    ./tools/urbi-compile-stdlib src/stdlib/stdlib.u $BAKED"
    diff "$BAKED" "$GENERATED" | head -40
    exit 1
fi

echo "stdlib bytecode is fresh"
