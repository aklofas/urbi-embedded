#!/bin/sh
# check-gdb — smoke-test tools/gdb/urbi.py against a live inferior.
# Skips cleanly if gdb is missing. Builds a dedicated -O0 -g unit runner
# (readable symbols) via the unit-runner target, breaks at urbi_step where a
# UVM* is in scope with live strands, runs urbi-dump (which exercises
# urbi-strands + urbi-heap full cell walk + urbi-allocs + urbi-leaks +
# urbi-trace), and asserts no Python error leaked and the walkers produced
# output.
#   MEMDBG=1  build the runner with -DURBI_MEM_DEBUG=1 and additionally assert
#             the owner-tag walker (urbi-allocs) surfaces allocation sites.
set -e
if ! command -v gdb >/dev/null 2>&1; then
    echo "check-gdb: gdb not found — SKIP"
    exit 0
fi
if [ "${MEMDBG:-0}" = "1" ]; then
    RUNNER=build/host-gdb-mem/tests/unit/runner
    make -s TARGET=host-gdb-mem CFLAGS="-std=c99 -g -O0 -DURBI_MEM_DEBUG=1" unit-runner >/dev/null
else
    RUNNER=build/host-gdb/tests/unit/runner
    make -s TARGET=host-gdb CFLAGS="-std=c99 -g -O0" unit-runner >/dev/null
fi
[ -x "$RUNNER" ] || { echo "check-gdb: $RUNNER not built"; exit 1; }

OUT="$(gdb -batch -nx \
    -ex "source tools/gdb/urbi.py" \
    -ex "break urbi_step" \
    -ex "run" \
    -ex "urbi-dump vm" \
    -ex "kill" \
    -ex "quit" \
    "$RUNNER" 2>&1 || true)"

fail() {
    echo "check-gdb: FAIL — $1"
    echo "$OUT" | grep -v '^  PASS' | tail -30
    exit 1
}

if ! echo "$OUT" | grep -q "urbi-embedded GDB helpers loaded"; then fail "script did not load"; fi
if echo "$OUT" | grep -q "Python Exception"; then fail "Python exception in walker"; fi
if echo "$OUT" | grep -q "Error occurred in Python"; then fail "Python error in walker"; fi
if ! echo "$OUT" | grep -q "strand(s)"; then fail "urbi-strands produced no output"; fi
if ! echo "$OUT" | grep -q "gc_threshold"; then fail "urbi-heap produced no output"; fi
if ! echo "$OUT" | grep -q "live cells:"; then fail "urbi-heap full cell walk produced no output"; fi
if ! echo "$OUT" | grep -q "live handles:"; then fail "urbi-leaks produced no output"; fi
if [ "${MEMDBG:-0}" = "1" ]; then
    if ! echo "$OUT" | grep -q "alloc site"; then fail "urbi-allocs showed no owner data under MEM_DEBUG"; fi
fi
echo "check-gdb: walkers ran without error — OK (MEMDBG=${MEMDBG:-0})"
