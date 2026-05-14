#!/usr/bin/env bash
# test_aux_symbol_leak_detection.sh — Meta-test: verify check_aux_symbols.sh
# catches a deliberately-introduced aux symbol leak in a temporary archive.
#
# Strategy:
#   1. Compile a tiny C translation unit that defines a symbol named
#      urbi_aux_test_leak (matching the urbi_aux_* pattern).
#   2. Assemble a temporary .a archive containing that object.
#   3. Run scripts/check_aux_symbols.sh against the temporary archive.
#   4. Assert exit code == 1 (leak detected).
#   5. Clean up.
#
# If check_aux_symbols.sh returns 0 (PASS) for the poisoned archive, the
# gate itself is broken — this meta-test catches that regression.

set -euo pipefail

SCRIPT="./scripts/check_aux_symbols.sh"
if [ ! -x "$SCRIPT" ]; then
    echo "FAIL: $SCRIPT not found or not executable" >&2
    exit 1
fi

# --- Build a poisoned object with a deliberate urbi_aux_* symbol. ---
POISONED_SRC=$(mktemp -t urbi_aux_leak_XXXXXX.c)
POISONED_OBJ=$(mktemp -t urbi_aux_leak_XXXXXX.o)
POISONED_AR=$(mktemp -t urbi_aux_leak_XXXXXX.a)

cleanup() { rm -f "$POISONED_SRC" "$POISONED_OBJ" "$POISONED_AR"; }
trap cleanup EXIT

cat > "$POISONED_SRC" << 'EOF'
/* Deliberately-leaking aux symbol for meta-test. */
int urbi_aux_test_leak(void) { return 0; }
EOF

cc -c "$POISONED_SRC" -o "$POISONED_OBJ" 2>/dev/null
rm -f "$POISONED_AR"  # ar rcs will re-create; avoid format mismatch with the mktemp placeholder
ar rcs "$POISONED_AR" "$POISONED_OBJ"

# --- Run the gate against the poisoned archive. ---
# We expect exit code 1 (FAIL detected).
if "$SCRIPT" "$POISONED_AR" > /dev/null 2>&1; then
    echo "FAIL: check_aux_symbols.sh did NOT detect the deliberate leak" >&2
    exit 1
fi

echo "PASS: check_aux_symbols.sh correctly detected the deliberate aux leak"
