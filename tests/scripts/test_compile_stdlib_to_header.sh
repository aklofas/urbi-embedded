#!/usr/bin/env bash
# test_compile_stdlib_to_header.sh — TDD test for urbi-compile-stdlib --to-header.
#
# Verifies:
#   1. --to-header produces an output file.
#   2. The generated header compiles cleanly as a C translation unit.
#   3. The header declares the expected symbol (const uint8_t <name>[]) and
#      a corresponding <name>_size constant.
#   4. Determinism: two runs produce byte-identical output.
#
# Wired into make test-bake-smoke and tested standalone.

set -euo pipefail

TOOL=./tools/urbi-compile-stdlib

if [ ! -x "$TOOL" ]; then
    echo "FAIL: $TOOL not built (run: make tools/urbi-compile-stdlib)" >&2
    exit 1
fi

# --- Create a trivial urbiscript fixture. ---
SRC_FILE=$(mktemp -t urbi_trivial_XXXXXX.u)
OUT_H1=$(mktemp -t urbi_header_run1_XXXXXX.h)
OUT_H2=$(mktemp -t urbi_header_run2_XXXXXX.h)
OUT_OBJ=$(mktemp -t urbi_header_XXXXXX.o)
SYMBOL="test_bc"

cleanup() { rm -f "$SRC_FILE" "$OUT_H1" "$OUT_H2" "$OUT_OBJ"; }
trap cleanup EXIT

printf '42;\n' > "$SRC_FILE"

# --- Test 1: tool produces output file. ---
"$TOOL" --to-header -i "$SRC_FILE" -o "$OUT_H1" --symbol "$SYMBOL" 2>/dev/null
if [ ! -f "$OUT_H1" ]; then
    echo "FAIL: output file not created" >&2
    exit 1
fi
echo "PASS 1: output file created"

# --- Test 2: generated header compiles cleanly. ---
if ! cc -x c -c "$OUT_H1" -o "$OUT_OBJ" 2>/dev/null; then
    echo "FAIL: generated header does not compile cleanly" >&2
    cat "$OUT_H1" >&2
    exit 1
fi
echo "PASS 2: generated header compiles cleanly"

# --- Test 3: header declares expected symbol and size. ---
if ! grep -q "const uint8_t ${SYMBOL}\[\]" "$OUT_H1"; then
    echo "FAIL: header does not declare 'const uint8_t ${SYMBOL}[]'" >&2
    exit 1
fi
if ! grep -q "const size_t ${SYMBOL}_size" "$OUT_H1"; then
    echo "FAIL: header does not declare 'const size_t ${SYMBOL}_size'" >&2
    exit 1
fi
echo "PASS 3: header declares '${SYMBOL}' and '${SYMBOL}_size'"

# --- Test 4: determinism across two runs. ---
"$TOOL" --to-header -i "$SRC_FILE" -o "$OUT_H2" --symbol "$SYMBOL" 2>/dev/null
if ! cmp -s "$OUT_H1" "$OUT_H2"; then
    echo "FAIL: --to-header output differs between run 1 and run 2" >&2
    diff -u "$OUT_H1" "$OUT_H2" >&2 || true
    exit 1
fi
echo "PASS 4: deterministic across two runs"

echo "PASS: test_compile_stdlib_to_header.sh"
